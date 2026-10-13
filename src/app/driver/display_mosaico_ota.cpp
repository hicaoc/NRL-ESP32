// ESP-Mosaico OTA (system update) detail page: server/firmware info, release
// picker, check/install buttons and a progress bar. Built into the shared
// content container from the Settings page; same detail-page framework as the
// sensor pages (hidden dock, non-scrollable, up-swipe/Back to exit).
//
// State-machine semantics follow the S31 OTA UI (display_s31.cpp): the
// service flags checking/updating are authoritative; the *_pending flags
// bridge the request/response gap so the status line never flashes idle
// while a request is in flight.

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "display_mosaico_ota.h"

#include "../../lib/nrl_version.h"
#include "../../services/ota_service.h"

#include <esp_heap_caps.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

// Keep in sync with the AMOLED Dark theme in display_mosaico.cpp.
constexpr uint32_t kColorCard     = 0x12151B;
constexpr uint32_t kColorBorder   = 0x232A35;
constexpr uint32_t kColorAccent   = 0x22D3EE;
constexpr uint32_t kColorText     = 0xF2F6FA;
constexpr uint32_t kColorSub      = 0x8A97A8;
constexpr uint32_t kColorGood     = 0x4ADE80;
constexpr uint32_t kColorWarn     = 0xF5B453;
constexpr uint32_t kColorBad      = 0xF87171;
constexpr uint32_t kColorBtnPress = 0x1B2A33;

constexpr int kPageW = 480;

NrlOtaStatus *s_status = nullptr;  // ~4 KB: SPIRAM, never on the stack
size_t s_selected_index = 0;
bool s_check_pending = false;
bool s_update_pending = false;
uint32_t s_check_baseline_ms = 0;

lv_obj_t *s_lbl_url = nullptr;
lv_obj_t *s_lbl_selected = nullptr;
lv_obj_t *s_bar = nullptr;
lv_obj_t *s_btn_check = nullptr;
lv_obj_t *s_btn_install = nullptr;
lv_obj_t *s_lbl_status = nullptr;

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int font_px, uint32_t color)
{
    return MosaicoUi_MakeLabel(parent, text, MosaicoUi_Font(font_px), color);
}

const char *tr(const char *text)
{
    return MosaicoUi_Tr(text);
}

NrlOtaStatus *otaSnapshot()
{
    if (s_status == nullptr) {
        s_status = static_cast<NrlOtaStatus *>(
            heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_status == nullptr) {
            s_status = static_cast<NrlOtaStatus *>(
                heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_8BIT));
        }
    }
    if (s_status == nullptr) {
        return nullptr;
    }
    memset(s_status, 0, sizeof(*s_status));
    OtaService_GetStatus(s_status);
    return s_status;
}

void backEvent(lv_event_t *)
{
    MosaicoUi_ExitOtaDetail();
}

void setDisabled(lv_obj_t *obj, bool disabled)
{
    if (obj == nullptr || lv_obj_has_state(obj, LV_STATE_DISABLED) == disabled) {
        return;
    }
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
}

lv_obj_t *makeButton(lv_obj_t *parent, int x, int y, int w, int h,
                     const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtnPress), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = makeLabel(btn, text, 20, kColorText);
    lv_obj_center(label);
    return btn;
}

void checkEvent(lv_event_t *)
{
    const NrlOtaStatus *ota = otaSnapshot();
    if (ota == nullptr) {
        return;
    }
    if (!OtaService_CheckNow()) {
        lv_label_set_text(s_lbl_status, tr("Check failed: OTA server not configured."));
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(kColorBad), 0);
        return;
    }
    s_check_baseline_ms = ota->last_check_ms;
    s_check_pending = true;
    s_update_pending = false;
    MosaicoOtaDetail_Refresh();
}

void stepEvent(lv_event_t *event)
{
    const bool newer = static_cast<bool>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(event)));
    const NrlOtaStatus *ota = otaSnapshot();
    if (ota == nullptr || ota->release_count == 0u) {
        return;
    }
    if (newer) {
        if (s_selected_index > 0u) {
            --s_selected_index;
        }
    } else if (s_selected_index + 1u < ota->release_count) {
        ++s_selected_index;
    }
    MosaicoOtaDetail_Refresh();
}

void installEvent(lv_event_t *)
{
    const NrlOtaStatus *ota = otaSnapshot();
    if (ota == nullptr || ota->release_count == 0u ||
        s_selected_index >= ota->release_count) {
        lv_label_set_text(s_lbl_status, tr("Install failed: check available versions first."));
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(kColorBad), 0);
        return;
    }
    const char *version = ota->releases[s_selected_index].version;
    if (strcmp(version, NRL_FIRMWARE_VERSION) == 0) {
        lv_label_set_text(s_lbl_status, tr("Selected version is already installed."));
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(kColorWarn), 0);
        return;
    }
    if (!OtaService_UpdateVersion(version)) {
        lv_label_set_text(s_lbl_status, tr("Install failed: check available versions first."));
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(kColorBad), 0);
        return;
    }
    s_update_pending = true;
    s_check_pending = false;
    MosaicoOtaDetail_Refresh();
}

} // namespace

void MosaicoOtaDetail_Build(lv_obj_t *content)
{
    // Widgets from a previous build were destroyed by lv_obj_clean().
    s_lbl_url = nullptr;
    s_lbl_selected = nullptr;
    s_bar = nullptr;
    s_btn_check = nullptr;
    s_btn_install = nullptr;
    s_lbl_status = nullptr;

    lv_obj_t *page = MosaicoUi_BuildDetailChrome(content, "System Update", backEvent);

    // Firmware version + OTA server (read-only; this board has no text input
    // UI, so the built-in default URL is only displayed).
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_gesture_bubble(card, true);
    lv_obj_set_pos(card, 16, 56);
    lv_obj_set_size(card, kPageW - 32, 88);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *fw_cap = makeLabel(card, tr("Firmware"), 14, kColorSub);
    lv_obj_set_pos(fw_cap, 0, 2);
    lv_obj_t *fw = makeLabel(card, "v" NRL_FIRMWARE_VERSION, 16, kColorAccent);
    lv_obj_set_width(fw, 200);
    lv_obj_set_style_text_align(fw, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(fw, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *url_cap = makeLabel(card, tr("OTA Server"), 14, kColorSub);
    lv_obj_set_pos(url_cap, 0, 36);
    s_lbl_url = makeLabel(card, "--", 16, kColorText);
    lv_obj_set_width(s_lbl_url, 300);
    lv_obj_set_style_text_align(s_lbl_url, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_url, LV_LABEL_LONG_DOT);
    lv_obj_align(s_lbl_url, LV_ALIGN_TOP_RIGHT, 0, 32);

    // Release picker: Older / Newer step through the manifest list.
    makeButton(page, 16, 160, 64, 52, LV_SYMBOL_LEFT, stepEvent,
               reinterpret_cast<void *>(static_cast<intptr_t>(0)));
    makeButton(page, kPageW - 80, 160, 64, 52, LV_SYMBOL_RIGHT, stepEvent,
               reinterpret_cast<void *>(static_cast<intptr_t>(1)));
    s_lbl_selected = makeLabel(page, tr("No versions available"), 20, kColorAccent);
    lv_obj_set_width(s_lbl_selected, kPageW - 176);
    lv_obj_set_style_text_align(s_lbl_selected, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_selected, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_lbl_selected, 88, 174);

    s_bar = lv_bar_create(page);
    lv_obj_set_scrollable(s_bar, false);
    lv_obj_set_gesture_bubble(s_bar, true);
    lv_obj_set_pos(s_bar, 16, 236);
    lv_obj_set_size(s_bar, kPageW - 32, 16);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(kColorBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_btn_check = makeButton(page, 16, 272, 216, 56, tr("Check Update"), checkEvent, nullptr);
    s_btn_install = makeButton(page, 248, 272, 216, 56, tr("Install"), installEvent, nullptr);

    s_lbl_status = makeLabel(page, "", 16, kColorSub);
    lv_obj_set_width(s_lbl_status, kPageW - 32);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_status, 16, 344);

    MosaicoOtaDetail_Refresh();
}

void MosaicoOtaDetail_Refresh(void)
{
    if (s_lbl_status == nullptr) {
        return;
    }
    const NrlOtaStatus *ota = otaSnapshot();
    if (ota == nullptr) {
        lv_label_set_text(s_lbl_status, tr("OTA status unavailable: out of memory."));
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(kColorBad), 0);
        return;
    }
    // A completed check clears the pending flag (either the service flag or a
    // newer last_check_ms proves the request was consumed).
    if (ota->checking) {
        s_check_pending = false;
    } else if (s_check_pending && ota->last_check_ms != s_check_baseline_ms) {
        s_check_pending = false;
    }
    if (ota->updating) {
        s_update_pending = false;
    } else if (s_update_pending && ota->last_error[0] != '\0') {
        s_update_pending = false;
    }

    lv_label_set_text(s_lbl_url, ota->server_url[0] != '\0' ? ota->server_url : "--");

    char text[160];
    if (ota->release_count == 0u) {
        s_selected_index = 0u;
        lv_label_set_text(s_lbl_selected, tr("No versions available"));
    } else {
        if (s_selected_index >= ota->release_count) {
            s_selected_index = 0u;
        }
        snprintf(text, sizeof(text), "%s", ota->releases[s_selected_index].version);
        lv_label_set_text(s_lbl_selected, text);
    }

    uint32_t status_color = kColorSub;
    if (ota->updating) {
        if (ota->update_size > 0u) {
            snprintf(text, sizeof(text), tr("Installing firmware... %u%%"),
                     static_cast<unsigned>(ota->update_percent));
        } else {
            snprintf(text, sizeof(text), "%s", tr("Installing firmware..."));
        }
        status_color = kColorWarn;
    } else if (ota->checking) {
        snprintf(text, sizeof(text), "%s", tr("Checking for updates..."));
        status_color = kColorWarn;
    } else if (s_update_pending) {
        snprintf(text, sizeof(text), "%s", tr("Firmware install requested..."));
        status_color = kColorWarn;
    } else if (s_check_pending) {
        snprintf(text, sizeof(text), "%s", tr("Update check requested..."));
        status_color = kColorWarn;
    } else if (ota->last_error[0] != '\0') {
        snprintf(text, sizeof(text), tr("OTA error: %s"), ota->last_error);
        status_color = kColorBad;
    } else if (ota->release_count == 0u) {
        snprintf(text, sizeof(text), "%s",
                 tr("Tap Check Update to query available versions."));
    } else {
        snprintf(text, sizeof(text), tr("Latest: %s  |  %u version(s) available"),
                 ota->latest_version[0] != '\0' ? ota->latest_version
                                                : ota->releases[0].version,
                 static_cast<unsigned>(ota->release_count));
        status_color = (ota->latest_version[0] != '\0' &&
                        strcmp(ota->latest_version, NRL_FIRMWARE_VERSION) != 0)
                           ? kColorGood
                           : kColorSub;
    }
    lv_label_set_text(s_lbl_status, text);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(status_color), 0);

    lv_bar_set_value(s_bar, ota->updating ? ota->update_percent : 0, LV_ANIM_OFF);

    const bool busy = ota->checking || ota->updating ||
                      s_check_pending || s_update_pending;
    setDisabled(s_btn_check, busy);
    setDisabled(s_btn_install, busy || ota->release_count == 0u ||
                (s_selected_index < ota->release_count &&
                 strcmp(ota->releases[s_selected_index].version,
                        NRL_FIRMWARE_VERSION) == 0));
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
