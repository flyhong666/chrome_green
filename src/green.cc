#include "green.h"

#include <windows.h>

#include <lmaccess.h>
#include <processthreadsapi.h>
#include <shlwapi.h>

#include <winstring.h>
#include <inspectable.h>

#include "detours.h"

#include "config.h"
#include "utils.h"

namespace {

// Static function pointers for original functions
static auto RawUpdateProcThreadAttribute = UpdateProcThreadAttribute;
static auto RawCryptProtectData = CryptProtectData;
static auto RawCryptUnprotectData = CryptUnprotectData;
static auto RawGetVolumeInformationW = GetVolumeInformationW;
static auto RawLogonUserW = LogonUserW;
static auto RawIsOS = IsOS;

// Enumeration for process creation mitigation policy
enum ProcessCreationMitigationPolicy : DWORD64 {
  BlockNonMicrosoftBinariesAlwaysOn = 0x00000001ui64 << 44,
  Win32kSystemCallDisableAlwaysOn = 0x00000001ui64 << 28
};

BOOL WINAPI FakeGetComputerName(_Out_ LPTSTR lpBuffer,
                                _Inout_ LPDWORD lpnSize) {
  return false;
}

// Mirrors Chromium's portability hack: when lpVolumeSerialNumber is requested
// we return FALSE so the volume serial is never exposed. Reference:
// https://source.chromium.org/chromium/chromium/src/+/main:rlz/win/lib/machine_id_win.cc;l=41;drc=3dd5eb19b88fb0246061e21fc6098830bead0edb
//
// When lpVolumeSerialNumber is null we forward to RawGetVolumeInformationW,
// because other code paths still need real volume info (e.g.
// lpMaximumComponentLength). Reference:
// https://source.chromium.org/chromium/chromium/src/+/main:base/files/file_util_win.cc;drc=5b01e9f5bff328ba66e415103ca50ae940328fde;l=1071
//
// Returns FALSE if lpVolumeSerialNumber is non-null; otherwise the
// RawGetVolumeInformationW result.
BOOL WINAPI FakeGetVolumeInformation(_In_opt_ LPCTSTR lpRootPathName,
                                     _Out_opt_ LPTSTR lpVolumeNameBuffer,
                                     _In_ DWORD nVolumeNameSize,
                                     _Out_opt_ LPDWORD lpVolumeSerialNumber,
                                     _Out_opt_ LPDWORD lpMaximumComponentLength,
                                     _Out_opt_ LPDWORD lpFileSystemFlags,
                                     _Out_opt_ LPTSTR lpFileSystemNameBuffer,
                                     _In_ DWORD nFileSystemNameSize) {
  if (lpVolumeSerialNumber != nullptr) {
    return false;
  } else {
    return RawGetVolumeInformationW(
        lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize,
        lpVolumeSerialNumber, lpMaximumComponentLength, lpFileSystemFlags,
        lpFileSystemNameBuffer, nFileSystemNameSize);
  }
}

BOOL WINAPI MyUpdateProcThreadAttribute(
    __inout LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    __in DWORD dwFlags,
    __in DWORD_PTR Attribute,
    __in_bcount_opt(cbSize) PVOID lpValue,
    __in SIZE_T cbSize,
    __out_bcount_opt(cbSize) PVOID lpPreviousValue,
    __in_opt PSIZE_T lpReturnSize) {
  if (Attribute == PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY &&
      cbSize >= sizeof(DWORD64)) {
    // https://source.chromium.org/chromium/chromium/src/+/main:sandbox/win/src/process_mitigations.cc;l=362;drc=4c2fec5f6699ffeefd93137d2bf8c03504c6664c
    PDWORD64 policy_value_1 = &(static_cast<PDWORD64>(lpValue))[0];
    *policy_value_1 &= ~static_cast<DWORD64>(
        ProcessCreationMitigationPolicy::BlockNonMicrosoftBinariesAlwaysOn);
    if (config.IsWin32K()) {
      *policy_value_1 &= ~static_cast<DWORD64>(
          ProcessCreationMitigationPolicy::Win32kSystemCallDisableAlwaysOn);
    }
  }
  return RawUpdateProcThreadAttribute(lpAttributeList, dwFlags, Attribute,
                                      lpValue, cbSize, lpPreviousValue,
                                      lpReturnSize);
}

BOOL WINAPI
MyCryptProtectData(_In_ DATA_BLOB* pDataIn,
                   _In_opt_ LPCWSTR szDataDescr,
                   _In_opt_ DATA_BLOB* pOptionalEntropy,
                   _Reserved_ PVOID pvReserved,
                   _In_opt_ CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                   _In_ DWORD dwFlags,
                   _Out_ DATA_BLOB* pDataOut) {
  pDataOut->cbData = pDataIn->cbData;
  pDataOut->pbData =
      static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, pDataOut->cbData));
  memcpy(pDataOut->pbData, pDataIn->pbData, pDataOut->cbData);
  return true;
}

BOOL WINAPI
MyCryptUnprotectData(_In_ DATA_BLOB* pDataIn,
                     _Out_opt_ LPWSTR* ppszDataDescr,
                     _In_opt_ DATA_BLOB* pOptionalEntropy,
                     _Reserved_ PVOID pvReserved,
                     _In_opt_ CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                     _In_ DWORD dwFlags,
                     _Out_ DATA_BLOB* pDataOut) {
  if (RawCryptUnprotectData(pDataIn, ppszDataDescr, pOptionalEntropy,
                            pvReserved, pPromptStruct, dwFlags, pDataOut)) {
    return true;
  }

  pDataOut->cbData = pDataIn->cbData;
  pDataOut->pbData =
      static_cast<BYTE*>(LocalAlloc(LMEM_FIXED, pDataOut->cbData));
  memcpy(pDataOut->pbData, pDataIn->pbData, pDataOut->cbData);
  return true;
}

// show_password: bypass Windows Hello so Chrome's password reauth falls back to
// the legacy path, where MyLogonUserW / MyIsOS short-circuit it. No dependency
// on the version-specific WinRT header UserConsentVerifierInterop.h: we hand-roll
// a minimal COM object and only need its RequestVerificationForWindowAsync to fail.
//
// Verified against Windows SDK 10.0.22621.0 / 10.0.26100.0:
//   IID_IUserConsentVerifierInterop = 39E050C3-4E74-441A-8DC0-B81104DF949C
//   RoGetActivationFactory is the entry Chromium's GetActivationFactory calls.
typedef HRESULT(WINAPI* FnRoGetActivationFactory)(HSTRING activatableClassId,
                                                  REFIID iid,
                                                  void** factory);
static FnRoGetActivationFactory RawRoGetActivationFactory = nullptr;

static const IID kIidUserConsentVerifierInterop = {
    0x39E050C3, 0x4E74, 0x441A,
    {0x8D, 0xC0, 0xB8, 0x11, 0x04, 0xDF, 0x94, 0x9C}};

// IUserConsentVerifierInterop is a single-method WinRT interop interface that
// inherits IInspectable. We declare just enough of it to implement a fake
// factory, avoiding the version-specific UserConsentVerifierInterop.h header.
// Verified against SDK 10.0.22621.0 / 10.0.26100.0.
struct IUserConsentVerifierInterop : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RequestVerificationForWindowAsync(
      HWND app_window, HSTRING message, REFIID riid, void** async_operation) = 0;
};

// Minimal factory implementation. Only RequestVerificationForWindowAsync
// matters; returning E_FAIL makes Chromium take the legacy reauth path.
class FakeUserConsentFactory : public IUserConsentVerifierInterop {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IInspectable) ||
        IsEqualIID(riid, kIidUserConsentVerifierInterop)) {
      *ppv = static_cast<IInspectable*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* trust) override {
    *trust = BaseTrust;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RequestVerificationForWindowAsync(
      HWND app_window,
      HSTRING message,
      REFIID riid,
      void** async_operation) override {
    return E_FAIL;
  }
};

static FakeUserConsentFactory g_fake_ucv_factory;

HRESULT WINAPI MyRoGetActivationFactory(HSTRING activatableClassId,
                                        REFIID iid,
                                        void** factory) {
  // IUserConsentVerifierInterop is requested exclusively for the
  // Windows.Security.Credentials.UI.UserConsentVerifier runtime class, so an
  // IID match is enough to identify the Windows Hello reauth factory.
  if (IsEqualIID(iid, kIidUserConsentVerifierInterop)) {
    *factory = static_cast<IInspectable*>(&g_fake_ucv_factory);
    g_fake_ucv_factory.AddRef();
    return S_OK;
  }
  return RawRoGetActivationFactory(activatableClassId, iid, factory);
}

}  // namespace

BOOL WINAPI MyLogonUserW(LPCWSTR lpszUsername,
                         LPCWSTR lpszDomain,
                         LPCWSTR lpszPassword,
                         DWORD dwLogonType,
                         DWORD dwLogonProvider,
                         PHANDLE phToken) {
  // Chromium's blank-password probe (password_manager_util_win.cc ->
  // CheckBlankPasswordWithPrefs) is the only caller that passes an empty
  // password with LOGON32_LOGON_INTERACTIVE. Forwarding it reaches LSA and
  // logs a 4625 per call, eventually locking the account. Chromium treats
  // ERROR_ACCOUNT_RESTRICTION as "password is blank", so fake that and never
  // touch the real logon path. This keeps show_password working with zero 4625.
  const bool is_blank_probe =
      dwLogonType == LOGON32_LOGON_INTERACTIVE &&
      (lpszPassword == nullptr || lpszPassword[0] == L'\0');
  if (is_blank_probe) {
    SetLastError(ERROR_ACCOUNT_RESTRICTION);
    return false;
  }
  return RawLogonUserW(lpszUsername, lpszDomain, lpszPassword, dwLogonType,
                       dwLogonProvider, phToken);
}

BOOL WINAPI MyIsOS(DWORD dwOS) {
  DWORD ret = RawIsOS(dwOS);
  // Chrome's password reauth checks whether the machine is domain-joined;
  // report "not a domain member" so it takes the local-account path.
  if (dwOS == OS_DOMAINMEMBER) {
    return false;
  }
  return ret;
}

// MyNetUserGetInfo hook removed: its password-age rewrite was a no-op that only
// amplified the reauth probe frequency (see MyLogonUserW for the real fix).

void MakeGreen() {
  auto RawGetComputerNameW = GetComputerNameW;

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  // kernel32.dll
  DetourAttach(reinterpret_cast<LPVOID*>(&RawGetComputerNameW),
               reinterpret_cast<void*>(FakeGetComputerName));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawGetVolumeInformationW),
               reinterpret_cast<void*>(FakeGetVolumeInformation));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawUpdateProcThreadAttribute),
               reinterpret_cast<void*>(MyUpdateProcThreadAttribute));

  // components/os_crypt/os_crypt_win.cc
  // crypt32.dll
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCryptProtectData),
               reinterpret_cast<void*>(MyCryptProtectData));
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCryptUnprotectData),
               reinterpret_cast<void*>(MyCryptUnprotectData));

  if (config.IsShowPassword()) {
    // advapi32.dll
    DetourAttach(reinterpret_cast<LPVOID*>(&RawLogonUserW),
                 reinterpret_cast<void*>(MyLogonUserW));
    // shlwapi.dll
    DetourAttach(reinterpret_cast<LPVOID*>(&RawIsOS),
                 reinterpret_cast<void*>(MyIsOS));
    // combase.dll: bypass Windows Hello so reauth falls back to the legacy
    // path where MyLogonUserW short-circuits it.
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (combase) {
      RawRoGetActivationFactory =
          reinterpret_cast<FnRoGetActivationFactory>(
              GetProcAddress(combase, "RoGetActivationFactory"));
      if (RawRoGetActivationFactory) {
        DetourAttach(reinterpret_cast<LPVOID*>(&RawRoGetActivationFactory),
                     reinterpret_cast<void*>(MyRoGetActivationFactory));
      }
    }
  }

  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"MakeGreen failed: {}", status);
  }
}
