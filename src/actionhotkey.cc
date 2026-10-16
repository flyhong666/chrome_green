#include "actionhotkey.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"
#include "inputhook.h"
#include "uia.h"
#include "utils.h"

namespace {

struct ActionHotkey {
  UINT modifiers = 0;
  UINT vk = 0;
};

ActionHotkey open_new_window;
ActionHotkey open_url_group;
ActionHotkey open_clipboard;

// Exact modifier match: every modifier in `modifiers` must be held, and no
// extra modifier (beyond those specified) may be held.
bool CheckModifiers(UINT modifiers) {
  const bool shift_ok = !(modifiers & MOD_SHIFT) || IsKeyPressed(VK_SHIFT);
  const bool ctrl_ok = !(modifiers & MOD_CONTROL) || IsKeyPressed(VK_CONTROL);
  const bool alt_ok = !(modifiers & MOD_ALT) || IsKeyPressed(VK_MENU);
  const bool win_ok =
      !(modifiers & MOD_WIN) || IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN);

  const bool no_extra_shift =
      (modifiers & MOD_SHIFT) || !IsKeyPressed(VK_SHIFT);
  const bool no_extra_ctrl =
      (modifiers & MOD_CONTROL) || !IsKeyPressed(VK_CONTROL);
  const bool no_extra_alt = (modifiers & MOD_ALT) || !IsKeyPressed(VK_MENU);
  const bool no_extra_win = (modifiers & MOD_WIN) ||
                            (!IsKeyPressed(VK_LWIN) && !IsKeyPressed(VK_RWIN));

  return shift_ok && ctrl_ok && alt_ok && win_ok && no_extra_shift &&
         no_extra_ctrl && no_extra_alt && no_extra_win;
}

void OpenNewWindow() {
  ExecuteCommand(IDC_NEW_WINDOW);
}

// Read the clipboard as plain text. Returns false when the clipboard holds no
// CF_UNICODETEXT (e.g. an image or an empty clipboard).
bool ReadClipboardText(std::wstring* out) {
  out->clear();
  if (!::OpenClipboard(nullptr)) {
    return false;
  }
  HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
  if (handle) {
    if (const wchar_t* text =
            static_cast<const wchar_t*>(::GlobalLock(handle))) {
      out->assign(text);
      ::GlobalUnlock(handle);
    }
  }
  ::CloseClipboard();
  return !out->empty();
}

bool IsAsciiSpace(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' ||
         c == L'\f';
}

// A URL token starts with a scheme we can navigate to, or with "www.".
bool IsUrlStart(std::wstring_view text, size_t pos) {
  auto starts_with = [text, pos](std::wstring_view prefix) {
    return text.compare(pos, prefix.size(), prefix) == 0;
  };
  return starts_with(L"http://") || starts_with(L"https://") ||
         starts_with(L"ftp://") || starts_with(L"www.");
}

// Characters that commonly trail a URL inside prose. Trimming them keeps
// "see http://a.com." from becoming a link with a stray dot.
bool IsUrlTerminator(wchar_t c) {
  return IsAsciiSpace(c) || c == L',' || c == L';' || c == L'，' || c == L'；' ||
         c == L'。' || c == L'、' || c == L'\'' || c == L'"' || c == L'（' ||
         c == L'）' || c == L'(' || c == L')' || c == L'【' || c == L'】' ||
         c == L'[' || c == L']' || c == L'<' || c == L'>' || c == L'「' ||
         c == L'」' || c == L'《' || c == L'》' || c == L'{' || c == L'}';
}

std::wstring Trim(std::wstring_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && IsAsciiSpace(text[begin])) {
    ++begin;
  }
  while (end > begin && IsAsciiSpace(text[end - 1])) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

// One openable item plus the offset it was found at, so links and leftover
// text can be merged back into document order.
struct ClipItem {
  size_t pos = 0;
  bool is_search = false;
  std::wstring text;
};

// Scan the whole clipboard text for URLs, keeping them in document order and
// trimming prose punctuation from each end. Also records the spans occupied by
// links so the caller can recover the text in between.
//
// The recorded span always ends at the *raw* scan boundary (before trimming),
// so punctuation stripped off the link's end does not reappear in the leftover
// text; the link text itself is the trimmed form.
std::vector<ClipItem> ExtractLinks(std::wstring_view text,
                                   std::vector<std::pair<size_t, size_t>>*
                                       link_spans) {
  std::vector<ClipItem> links;
  size_t i = 0;
  while (i < text.size()) {
    if (!IsUrlStart(text, i)) {
      ++i;
      continue;
    }

    size_t raw_end = i;
    while (raw_end < text.size() && !IsUrlTerminator(text[raw_end])) {
      ++raw_end;
    }

    // Trailing punctuation may sit inside [i, raw_end) when the URL is
    // followed directly by e.g. "。" — strip it.
    size_t trimmed = raw_end;
    while (trimmed > i) {
      const wchar_t last = text[trimmed - 1];
      if (last == L'.' || last == L',' || last == L';' || last == L'。' ||
          last == L'，' || last == L'；' || last == L'、' || last == L':' ||
          last == L'！' || last == L'?') {
        --trimmed;
        continue;
      }
      break;
    }

    // Trimming can eat the scheme itself ("www." -> "www"), which would open a
    // junk tab. Require the remainder to still look like a URL.
    std::wstring_view candidate = text.substr(i, trimmed - i);
    if (!candidate.empty() && IsUrlStart(candidate, 0)) {
      links.push_back({i, false, std::wstring(candidate)});
      if (link_spans) {
        link_spans->emplace_back(i, raw_end);
      }
    }
    i = (raw_end > i) ? raw_end : i + 1;
  }
  return links;
}

// Leftover prose between (and around) the extracted links, split on line
// breaks. Empty fragments are dropped.
std::vector<ClipItem> ExtractTexts(std::wstring_view text,
                                   const std::vector<std::pair<size_t, size_t>>&
                                       link_spans) {
  std::vector<ClipItem> parts;
  size_t cursor = 0;
  for (const auto& span : link_spans) {
    if (span.first > cursor) {
      size_t seg_begin = cursor;
      size_t i = cursor;
      while (i <= span.first) {
        if (i == span.first || text[i] == L'\n' || text[i] == L'\r') {
          std::wstring piece = Trim(text.substr(seg_begin, i - seg_begin));
          if (!piece.empty()) {
            parts.push_back({seg_begin, true, piece});
          }
          // Skip CRLF as one break.
          if (i == span.first) {
            break;
          }
          if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            ++i;
          }
          seg_begin = i + 1;
        }
        ++i;
      }
    }
    cursor = span.second;
  }
  if (cursor < text.size()) {
    size_t seg_begin = cursor;
    for (size_t i = cursor; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == L'\n' || text[i] == L'\r') {
        std::wstring piece = Trim(text.substr(seg_begin, i - seg_begin));
        if (!piece.empty()) {
          parts.push_back({seg_begin, true, piece});
        }
        if (i == text.size()) {
          break;
        }
        if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
          ++i;
        }
        seg_begin = i + 1;
      }
    }
  }
  return parts;
}

// Merge links and texts back into the order they appeared in the clipboard, so
// tabs open in the same sequence the user copied. Items are returned as-is so
// each keeps its is_search flag.
std::vector<ClipItem> MergeInDocumentOrder(std::vector<ClipItem> links,
                                           std::vector<ClipItem> texts) {
  links.insert(links.end(), texts.begin(), texts.end());
  std::stable_sort(links.begin(), links.end(),
                   [](const ClipItem& a, const ClipItem& b) {
                     return a.pos < b.pos;
                   });
  std::vector<ClipItem> out;
  out.reserve(links.size());
  for (auto& item : links) {
    if (!item.text.empty()) {
      out.push_back(std::move(item));
    }
  }
  return out;
}

// A link (navigated directly) or a search query (built into a search URL).
struct ClipTarget {
  bool is_search = false;
  std::wstring text;
};

// Hard cap on how many tabs one hotkey press may open. OpenTargets sleeps
// ~120ms per tab and runs on the keyboard hook thread, so an unbounded batch
// (a clipboard holding hundreds of links) would stall that thread for
// minutes. Anything past the cap is dropped.
constexpr size_t kMaxClipTargets = 20;

// windows.h defines min()/max() as macros, which also mangles std::min.
constexpr size_t ReserveCount(size_t n) {
  return n < kMaxClipTargets ? n : kMaxClipTargets;
}

// Build the list of things to open from `text`, per `mode`:
// 0 off; 1 URLs only; 2 whole clipboard as one search; 3 URLs + each leftover
// text chunk as a search.
std::vector<ClipTarget> BuildClipTargets(std::wstring_view text, int mode) {
  if (mode == 0 || text.empty()) {
    return {};
  }
  std::vector<ClipTarget> out;

  // Plain text: the whole clipboard is one search query, URLs included.
  if (mode == 2) {
    std::wstring query = Trim(text);
    if (query.empty()) {
      return {};
    }
    out.push_back({true, query});
    return out;
  }

  std::vector<std::pair<size_t, size_t>> spans;
  std::vector<ClipItem> links = ExtractLinks(text, &spans);

  if (mode == 1) {
    out.reserve(ReserveCount(links.size()));
    for (const auto& link : links) {
      if (out.size() >= kMaxClipTargets) {
        break;
      }
      out.push_back({false, link.text});
    }
    return out;
  }

  // mode 3: links as-is, each leftover text chunk as a search query.
  std::vector<ClipItem> texts = ExtractTexts(text, spans);
  for (auto& item : texts) {
    item.is_search = true;
  }
  std::vector<ClipItem> items = MergeInDocumentOrder(std::move(links),
                                                     std::move(texts));
  out.reserve(ReserveCount(items.size()));
  for (const auto& item : items) {
    if (out.size() >= kMaxClipTargets) {
      break;
    }
    out.push_back({item.is_search, item.text});
  }
  return out;
}

std::wstring GetSelfExePath() {
  wchar_t exe_path[MAX_PATH] = {};
  if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
    return {};
  }
  return exe_path;
}

// Percent-encode a UTF-8 string for a query string (RFC 3986 unreserved set
// stays literal, everything else including spaces is escaped byte by byte).
std::wstring PercentEncode(const std::wstring& text) {
  static const char kHex[] = "0123456789ABCDEF";
  const std::string utf8 = WStringToUtf8(text);
  std::wstring encoded;
  encoded.reserve(utf8.size() * 3);
  for (const unsigned char c : utf8) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      encoded.push_back(static_cast<wchar_t>(c));
    } else {
      encoded.push_back(L'%');
      encoded.push_back(static_cast<wchar_t>(kHex[c >> 4]));
      encoded.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
    }
  }
  return encoded;
}

bool ReadTextFile(const std::wstring& path, std::string* out) {
  out->clear();
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size;
  if (!::GetFileSizeEx(file, &size) || size.QuadPart > 64 * 1024 * 1024) {
    ::CloseHandle(file);
    return false;
  }
  char buffer[1 << 16];
  DWORD got = 0;
  while (::ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    out->append(buffer, got);
  }
  ::CloseHandle(file);
  return true;
}

// Value of a `--key=value` switch (`key` must include the trailing '='), or "".
// Split on unquoted whitespace so switches whose value lives inside a quoted
// argument survive; portable.cc quotes the whole `--user-data-dir=...` token
// when the path contains a space (e.g. "Program Files").
std::wstring GetCmdArgValue(const wchar_t* key) {
  const std::wstring k(key);
  const wchar_t* cmd = ::GetCommandLineW();
  if (!cmd) {
    return {};
  }
  std::wstring cur;
  bool in_quotes = false;
  bool have_cur = false;
  std::wstring found;
  bool done = false;
  for (const wchar_t* p = cmd;; ++p) {
    const wchar_t c = *p;
    if (c == L'"') {
      in_quotes = !in_quotes;
      have_cur = true;
    } else if (!in_quotes && (c == L' ' || c == L'\t')) {
      if (have_cur) {
        if (!done && cur.size() > k.size() &&
            cur.compare(0, k.size(), k) == 0) {
          found = cur.substr(k.size());
          done = true;
        }
        cur.clear();
        have_cur = false;
      }
    } else if (c == L'\0') {
      if (have_cur && !done && cur.size() > k.size() &&
          cur.compare(0, k.size(), k) == 0) {
        found = cur.substr(k.size());
      }
      break;
    } else {
      cur += c;
      have_cur = true;
    }
  }
  return found;
}

// The profile directory Chrome is actually running with. Same resolution as
// the jump-list code: --user-data-dir / --profile-directory switches, else the
// default profile under the default user-data dir.
std::wstring ResolveProfileDir() {
  std::wstring user_data = GetCmdArgValue(L"--user-data-dir=");
  if (user_data.empty()) {
    wchar_t local[MAX_PATH] = {0};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) {
      return {};
    }
    user_data = std::wstring(local) + L"\\Google\\Chrome\\User Data";
  }
  std::wstring profile = GetCmdArgValue(L"--profile-directory=");
  if (profile.empty()) {
    profile = L"Default";
  }
  return user_data + L"\\" + profile;
}

// Chrome persists its engine template under
// default_search_provider_data.mirrored_template_url_data.url, but only after
// the user overrides it on chrome://settings/search. Untouched installs write
// no such key, so fall back to builtin Google.
constexpr wchar_t kFallbackSearchUrl[] =
    L"https://www.google.com/search?q={searchTerms}";

// Google-family engines persist a *parameterized* template, e.g.
//   {google:baseURL}search?q={searchTerms}&{google:RLZ}{google:language}
// The {google:*} placeholders are internal substitution variables that only
// Chromium's TemplateURL can expand, so a URL built from them literally would
// carry raw braces and fail to load. Expand the two we can resolve ({google:baseURL}
// from the engine's `keyword`, {inputEncoding}) and drop the rest; a template
// that ends up without {searchTerms} is unusable and falls back.
std::wstring NormalizeTemplate(std::wstring tmpl,
                               const std::wstring& keyword) {
  if (tmpl.find(L"{google:baseURL}") == std::wstring::npos) {
    return tmpl;  // standard {searchTerms}-only template, use as-is
  }
  if (!keyword.empty()) {
    tmpl.replace(tmpl.find(L"{google:baseURL}"),
                 std::wstring(L"{google:baseURL}").size(),
                 L"https://www." + keyword + L"/");
  }
  size_t pos = tmpl.find(L"{inputEncoding}");
  if (pos != std::wstring::npos) {
    tmpl.replace(pos, std::wstring(L"{inputEncoding}").size(), L"UTF-8");
  }
  // Strip any remaining {google:*} / {image*} placeholders.
  size_t begin = tmpl.find(L'{');
  while (begin != std::wstring::npos) {
    const size_t end = tmpl.find(L'}', begin);
    if (end == std::wstring::npos) {
      tmpl.erase(begin);
      break;
    }
    tmpl.erase(begin, end - begin + 1);
    begin = tmpl.find(L'{', begin);
  }
  return tmpl;
}

std::wstring GetSearchUrlTemplate() {
  const std::wstring profile = ResolveProfileDir();
  if (profile.empty()) {
    return kFallbackSearchUrl;
  }
  std::string content;
  if (!ReadTextFile(profile + L"\\Preferences", &content)) {
    return kFallbackSearchUrl;
  }

  // Walk to the value: the template is the "url" inside
  // default_search_provider_data -> mirrored_template_url_data.
  static const char kAnchor[] = "\"mirrored_template_url_data\"";
  size_t pos = content.find(kAnchor);
  if (pos == std::string::npos) {
    return kFallbackSearchUrl;
  }

  // Read a sibling string value: "key": "<value>" after `from`.
  auto read_sibling = [&content](size_t from, const char* key) {
    size_t p = content.find(key, from);
    if (p == std::string::npos) {
      return std::string();
    }
    p = content.find(':', p);
    if (p == std::string::npos) {
      return std::string();
    }
    p = content.find('"', p);
    if (p == std::string::npos) {
      return std::string();
    }
    const size_t end = content.find('"', p + 1);
    if (end == std::string::npos) {
      return std::string();
    }
    return content.substr(p + 1, end - p - 1);
  };

  const std::string url = read_sibling(pos, "\"url\"");
  if (url.empty() || url.find('{') == std::string::npos) {
    return kFallbackSearchUrl;
  }
  const std::wstring keyword = Utf8ToWstring(read_sibling(pos, "\"keyword\""));

  std::wstring tmpl = NormalizeTemplate(Utf8ToWstring(url), keyword);
  if (tmpl.find(L"{searchTerms}") == std::wstring::npos) {
    return kFallbackSearchUrl;
  }
  return tmpl;
}

// Build the search URL for `query` from Chrome's own engine template,
// substituting the percent-encoded query for the {searchTerms} placeholder.
std::wstring BuildSearchUrl(const std::wstring& query,
                            const std::wstring& tmpl) {
  constexpr std::wstring_view kTerms = L"{searchTerms}";
  const size_t pos = tmpl.find(kTerms);
  if (pos == std::wstring::npos) {
    return {};  // unusable template, drop the target
  }
  std::wstring out = tmpl;
  out.replace(pos, kTerms.size(), PercentEncode(query));
  return out;
}

// Hand URLs to the running Chrome as command-line args (one tab per arg). The
// search template is read once per batch. Chrome routes command-line URLs to a
// regular window even when an incognito one is focused, so `--incognito` is
// added explicitly when the foreground window is private.
void OpenTargets(const std::vector<ClipTarget>& targets) {
  const std::wstring exe_path = GetSelfExePath();
  if (exe_path.empty()) {
    return;
  }

  HWND fg = ::GetForegroundWindow();
  const bool incognito = IsIncognitoWindow(fg);

  // Read the template once per batch, not per search item.
  std::wstring tmpl = GetSearchUrlTemplate();
  for (const auto& target : targets) {
    if (target.text.empty()) {
      continue;
    }
    std::wstring arg =
        target.is_search ? BuildSearchUrl(target.text, tmpl) : target.text;
    if (arg.empty()) {
      continue;  // unusable search template
    }
    if (incognito) {
      arg = L"--incognito " + arg;
    }
    ::ShellExecuteW(nullptr, L"open", exe_path.c_str(), arg.c_str(), nullptr,
                    SW_SHOWNORMAL);
    // Small delay so Chrome processes each URL sequentially and avoids
    // creating multiple new windows when launched in rapid succession.
    ::Sleep(120);
  }
}

// Open each configured URL in its own new tab. Incognito following is inherited
// from OpenTargets, which opens in the foreground window's mode.
void OpenUrlGroup() {
  std::vector<ClipTarget> targets;
  for (const auto& url : config.GetUrlGroup()) {
    targets.push_back({false, url});
  }
  OpenTargets(targets);
}

// Open the clipboard content as new tabs, according to `clipboard_mode`.
void OpenClipboard() {
  std::wstring clip;
  if (!ReadClipboardText(&clip)) {
    return;  // silent no-op
  }
  OpenTargets(BuildClipTargets(clip, config.GetClipboardMode()));
}

bool ActionHotkeyHandler(WPARAM wParam, LPARAM lParam) {
  // Ignore key-up events.
  if (lParam & 0x80000000) {
    return false;
  }

  // These actions only make sense when a Chrome window is in the foreground.
  if (!IsChromeWindow(GetForegroundWindow())) {
    return false;
  }

  if (open_new_window.vk != 0 && wParam == open_new_window.vk &&
      CheckModifiers(open_new_window.modifiers)) {
    OpenNewWindow();
    return true;
  }

  if (open_url_group.vk != 0 && wParam == open_url_group.vk &&
      CheckModifiers(open_url_group.modifiers)) {
    OpenUrlGroup();
    return true;
  }

  if (open_clipboard.vk != 0 && wParam == open_clipboard.vk &&
      CheckModifiers(open_clipboard.modifiers)) {
    OpenClipboard();
    return true;
  }

  return false;
}

void ParseActionHotkey(const std::wstring& str, ActionHotkey& out) {
  if (str.empty()) {
    return;
  }
  // no_repeat=false so MOD_NOREPEAT is not added (matches keymapping parsing).
  const UINT parsed = ParseHotkeys(str, /*no_repeat=*/false);
  out.modifiers = LOWORD(parsed);
  out.vk = HIWORD(parsed);
}

}  // namespace

void ActionHotkeys() {
  ParseActionHotkey(config.GetOpenNewWindowHotkey(), open_new_window);
  ParseActionHotkey(config.GetOpenUrlGroupHotkey(), open_url_group);
  ParseActionHotkey(config.GetOpenClipboardHotkey(), open_clipboard);

  if (open_new_window.vk != 0 || open_url_group.vk != 0 ||
      open_clipboard.vk != 0) {
    RegisterKeyboardHandler(ActionHotkeyHandler, HandlerPriority::kHigh);
    DebugLog(
        L"ActionHotkeys: registered (new_window={}, url_group={}, "
        L"clipboard={})",
        open_new_window.vk, open_url_group.vk, open_clipboard.vk);
  }
}
