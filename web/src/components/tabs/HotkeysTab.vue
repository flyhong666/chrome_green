<template>
  <div class="card">
    <div class="card-header">
      <h2 class="card-title">{{ t("grp_hotkeys") }}</h2>
      <p class="card-description">{{ t("hotkeys_desc") }}</p>
    </div>
    <div class="card-content space-y-0">
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("translate_key") }}</label>
          <p class="setting-desc">{{ t("translate_key_desc") }}</p>
        </div>
        <div class="w-64 max-w-full">
          <KeyInput
            v-model="settings.translate_key"
            :placeholder="t('translate_key_desc')"
            @change="saveSettings" />
        </div>
      </div>
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("boss_key") }}</label>
          <p class="setting-desc">{{ t("boss_key_desc") }}</p>
        </div>
        <div class="w-64 max-w-full">
          <KeyInput
            v-model="settings.boss_key"
            :placeholder="t('boss_key_desc')"
            @change="saveSettings" />
        </div>
      </div>
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("action_open_new_window") }}</label>
          <p class="setting-desc">{{ t("action_open_new_window_desc") }}</p>
        </div>
        <div class="w-64 max-w-full">
          <KeyInput
            v-model="settings.open_new_window"
            :placeholder="t('action_open_new_window_ph')"
            @change="saveSettings" />
        </div>
      </div>
      <div class="setting-row !items-start">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("action_open_url_group") }}</label>
          <p class="setting-desc">{{ t("action_open_url_group_desc") }}</p>
        </div>
        <div class="w-80 max-w-full space-y-2">
          <KeyInput
            v-model="settings.open_url_group"
            :placeholder="t('action_open_url_group_ph')"
            @change="saveSettings" />
          <textarea
            v-model="settings.url_group"
            spellcheck="false"
            @blur="saveSettings"
            class="input min-h-[88px] w-full resize-y font-mono text-xs leading-relaxed"
            :placeholder="t('url_group_desc')"></textarea>
        </div>
      </div>
      <div class="setting-row">
        <div class="setting-label-group">
          <label class="setting-label">{{ t("action_open_clipboard") }}</label>
          <p class="setting-desc">{{ t("action_open_clipboard_desc") }}</p>
        </div>
        <div class="w-80 max-w-full space-y-2">
          <KeyInput
            v-model="settings.open_clipboard"
            :placeholder="t('action_open_clipboard_ph')"
            @change="saveSettings" />
          <div class="flex gap-2">
            <div class="w-48 shrink-0">
              <CustomSelect
                v-model="settings.clipboard_mode"
                :options="clipboardModeOptions"
                @change="() => saveSettings()" />
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from "vue";
import KeyInput from "../KeyInput.vue";
import CustomSelect from "../CustomSelect.vue";
import { t } from "../../i18n.js";
import { useStore } from "../../store.js";

const { settings, saveSettings } = useStore();

const clipboardModeOptions = computed(() => [
  { value: 0, label: t("clipboard_mode_off") },
  { value: 1, label: t("clipboard_mode_links") },
  { value: 2, label: t("clipboard_mode_search") },
  { value: 3, label: t("clipboard_mode_both") },
]);
</script>
