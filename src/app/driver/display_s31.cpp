#include "display.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/nrl_bt_hfp.h"
#include "../../lib/nrl_net_compat.h"
#include "../../lib/nrl_psram.h"
#include "../../lib/nrl_version.h"
#include "../../lib/nrl_wifi.h"
#include "../../lib/wifi_config_portal.h"
#include "../../media/cover_decoder.h"
#include "../../services/ai_assistant.h"
#include "../../services/aprs_service.h"
#include "../../services/config_notify.h"
#include "../../services/cw_service.h"
#include "../../services/display_notice.h"
#include "../../services/espnow_link.h"
#include "../../services/map_tiles.h"
#include "../../services/music_player.h"
#include "../../services/music_playlist.h"
#include "../../services/nanny.h"
#include "../../services/ota_service.h"
#include "../../services/radio_favorites.h"
#include "../../services/storage_service.h"
#include "../../services/signaling_service.h"
#include "../../services/sstv_service.h"
#include "../../services/video_call.h"
#include "external_radio.h"
#include "fonts/lv_font_cjk.h"
#include "game_tetris.h"
#include "i2c1.h"
#include "status_io.h"

#include <bsp/camera.h>

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char *TAG = "LCD_S31";

namespace {

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;
constexpr uint32_t kRefreshIntervalMs = 500u;
constexpr uint32_t kVolumeSaveDelayMs = 2000u;
constexpr uint16_t kVolumeLongPressRepeatMs = 80u;

constexpr uint32_t kColorBg = 0x070B11;
constexpr uint32_t kColorPanel = 0x0B1220;
constexpr uint32_t kColorPanel2 = 0x101A2A;
constexpr uint32_t kColorBorder = 0x24364D;
constexpr uint32_t kColorText = 0xE6EDF3;
constexpr uint32_t kColorSub = 0x7D95AA;
constexpr uint32_t kColorDim = 0x4C6378;
constexpr uint32_t kColorTime = 0xBFE9F5;
constexpr uint32_t kColorAccent = 0x22D3EE;
constexpr uint32_t kColorGood = 0x4ADE80;
constexpr uint32_t kColorWarn = 0xF5B453;
constexpr uint32_t kColorBad = 0xF87171;
constexpr uint32_t kColorTx = 0xFF6B6B;
constexpr uint32_t kColorDuplex = 0xA78BFA;
constexpr const char *kCallsignPlaceholder = "----------";
constexpr size_t kStationFieldChars = 10u;
constexpr size_t kWifiOptionCount = 12u;  // max scanned SSIDs listed in the dropdown
constexpr size_t kMusicListMaxRows = 48u; // keep LVGL list layout bounded on large libraries
NRL_PSRAM_BSS uint8_t s_video_local_jpeg[VIDEO_MAX_JPEG_BYTES];
NRL_PSRAM_BSS uint8_t s_video_remote_jpeg[VIDEO_MAX_JPEG_BYTES];

enum class Page : uint8_t {
    Provisioning,
    Home,
    Config,
    Apps,
    Wifi,
    Station,
    Audio,
    Bt,
    Music,
    Radio,
    Nanny,
    Smb,
    EspNow,
    Ctcss,
    Mdc,
    Dtmf,
    Video,
    Game,
    Ai,
    About,
    Aprs,
    AprsMsg,
    AprsGateway,
    Cw,
    CwScore,
    Map,
    Sstv,
};

enum class Action : intptr_t {
    Config = 1,
    Home,
    Wifi,
    Station,
    Audio,
    Bt,
    ScanBt,
    VolumeDown,
    VolumeUp,
    CallsignSsidDown,
    CallsignSsidUp,
    ResetWifi,
    ScanWifi,
    SaveWifi,
    SaveStation,
    SaveAudio,
    ResetAudio,
    Music,
    MusicToggle,
    MusicNext,
    MusicPrev,
    MusicRescan,
    MusicMode,
    Radio,
    Nanny,
    Smb,
    Apps,
    EspNow,
    SaveSmb,
    ClearSmb,
    SaveNanny,
    BeaconOff,
    SaveRadio,
    RadioPlay,
    RadioStop,
    RadioFavAdd,
    RadioFavDel,
    RadioFavPrev,
    RadioFavNext,
    Video,
    VideoTx,
    Game,
    Ai,
    About,
    SaveOtaUrl,
    CheckOta,
    OtaOlder,
    OtaNewer,
    InstallOta,
    Aprs,
    AprsBeacon,
    AprsNrlTx,
    AprsNrlRx,
    AprsMsgOpen,
    AprsMsgSend,
    AprsGatewayOpen,
    AprsFwdRfIs,
    AprsFwdIsRf,
    AprsFwdNrlIs,
    AprsFwdIsNrl,
    AprsFwdRfNrl,
    AprsFwdNrlRf,
    Cw,
    CwDelete,
    CwSend,
    CwPractice,
    CwMode,
    CwReplay,
    CwScore,
    CwScoreSet,
    CwScoreAdapt,
    Ctcss,
    Mdc,
    Dtmf,
    SaveMdc,
    SaveDtmf,
    Provisioning,
    Map,
    MapZoomIn,
    MapZoomOut,
    Sstv,
    SstvTxMode,
    SstvFilePrev,
    SstvFileNext,
    SstvSend,
    SstvRxSource,
    SstvRxToggle,
    SstvSave,
    SstvCam,
    SstvLog,
    SstvLogPrev,
    SstvLogNext,
    SstvLogView,
    SstvLogBack,
};

enum class AudioControl : intptr_t {
    Speaker = 1,
    Mic,
    Aec,
    Noise,
    MicHpf,
    EspNow,
    EspNowRx,
    EspNowOpus,
    OpusCodec,
    CtcssRxMic,
    CtcssRxNrl,
    MdcRxMic,
    MdcRxNrl,
    MdcTxNrl,
    MdcTxSpeaker,
    DtmfRxMic,
    DtmfRxNrl,
    DtmfTxNrl,
    DtmfTxSpeaker,
};

bool s_ready = false;
bool s_provisioning_mode = false;
Page s_page = Page::Home;
uint32_t s_last_refresh_ms = 0u;
uint32_t s_volume_change_ms = 0u;
bool s_time_sync_started = false;
bool s_volume_dirty = false;
volatile bool s_wifi_scan_running = false;
volatile bool s_wifi_scan_complete = false;
volatile bool s_wifi_scan_ok = false;
TaskHandle_t s_wifi_scan_task = nullptr;
// Async enter/up/rescan of the display's browse session started by a list
// tap; pollMusicScan watches PLAYLIST_ClientIsScanning(DISPLAY) and
// repopulates the list when it finishes.
bool s_music_dir_scan_pending = false;
// PLAYLIST_ClientScanRevision(DISPLAY) the visible list was built from. Any
// later scan of the display session bumps the revision and makes the
// on-screen row indices stale.
uint32_t s_music_list_rev = 0u;
// Current page of the music list; resets to 0 whenever the listing revision
// changes (directory change, rescan, external scan).
size_t s_music_page = 0u;
uint32_t s_music_page_rev = 0u;

esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t *s_disp = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
lv_indev_t *s_touch_indev = nullptr;

lv_obj_t *s_lbl_caption = nullptr;
lv_obj_t *s_lbl_rx_codec = nullptr;
char s_shown_rx_codec[12] = {};
lv_obj_t *s_lbl_dmrid = nullptr;
char s_shown_dmrid[20] = {};
lv_obj_t *s_lbl_callsign = nullptr;
lv_obj_t *s_lbl_signaling = nullptr;
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_time = nullptr;
lv_obj_t *s_lbl_local_station = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_bt_top = nullptr;
lv_obj_t *s_list_bt = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_remote_station = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_server = nullptr;
lv_obj_t *s_lbl_detail = nullptr;
lv_obj_t *s_lbl_form_status = nullptr;
lv_obj_t *s_lbl_provision_ip = nullptr;
lv_obj_t *s_lbl_provision_ssid = nullptr;
lv_obj_t *s_notice_panel = nullptr;
lv_obj_t *s_lbl_notice = nullptr;
lv_obj_t *s_bar_notice_progress = nullptr;
char s_shown_notice[96] = {};
uint32_t s_notice_sequence = 0u;
uint32_t s_notice_color = 0xFFFFFFFFu;
lv_obj_t *s_dd_wifi = nullptr;
lv_obj_t *s_ta_wifi_ssid = nullptr;
lv_obj_t *s_ta_wifi_pass = nullptr;
lv_obj_t *s_ta_callsign = nullptr;
lv_obj_t *s_ta_callsign_ssid = nullptr;
lv_obj_t *s_ta_server_host = nullptr;
lv_obj_t *s_ta_server_port = nullptr;
lv_obj_t *s_ta_mdc_unit_id = nullptr;
lv_obj_t *s_ta_mdc_opcode = nullptr;
lv_obj_t *s_ta_mdc_argument = nullptr;
lv_obj_t *s_ta_dtmf_digits = nullptr;
lv_obj_t *s_slider_speaker = nullptr;
lv_obj_t *s_slider_mic = nullptr;
lv_obj_t *s_lbl_speaker_value = nullptr;
lv_obj_t *s_lbl_mic_value = nullptr;
lv_obj_t *s_sw_aec = nullptr;
lv_obj_t *s_sw_noise = nullptr;
lv_obj_t *s_sw_mic_hpf = nullptr;
lv_obj_t *s_sw_bt = nullptr;
lv_obj_t *s_sw_wifi = nullptr;
lv_obj_t *s_lbl_bt_status = nullptr;
lv_obj_t *s_keyboard = nullptr;

// About / OTA page widgets. Releases are already newest-first in the OTA
// service; the inline Older/Newer controls avoid another large LVGL dropdown
// on the RGB panel.
lv_obj_t *s_ta_ota_url = nullptr;
lv_obj_t *s_lbl_ota_release = nullptr;
lv_obj_t *s_btn_ota_check = nullptr;
lv_obj_t *s_btn_ota_install = nullptr;
size_t s_ota_selected_index = 0u;
bool s_ota_check_pending = false;
bool s_ota_update_pending = false;
uint32_t s_ota_check_baseline_ms = 0u;
char s_shown_ota_release[NRL_OTA_VERSION_MAX + 32u] = {};
char s_shown_ota_status[224] = {};
uint32_t s_ota_status_color = 0xFFFFFFFFu;
// NrlOtaStatus contains the complete release manifest (~4 KB). Keep the UI
// snapshot off nrl_main_loop's 6 KB stack; it is reused by all About-page
// actions and lives in PSRAM when available.
NrlOtaStatus *s_ota_ui_status = nullptr;

// LVGL calls these probes from deep inside lv_timer_handler(). Logging there
// takes the ESP log mutex and can push nrl_main_loop past its stack guard.
// Callbacks only record telemetry; Display_Poll prints it after LVGL returns.
uint32_t s_pending_present_ms = 0u;
uint32_t s_pending_touch_ms = 0u;
bool s_big_invalidate_pending = false;
int32_t s_big_invalidate_w = 0;
int32_t s_big_invalidate_h = 0;
int32_t s_big_invalidate_x = 0;
int32_t s_big_invalidate_y = 0;
uint32_t s_big_invalidate_count = 0u; // per 10 s digest window
uint32_t s_invalidate_count = 0u;     // every invalidation, per digest window
uint64_t s_invalidate_px = 0u;        // summed invalidated area, per window

// Media / nanny config pages (SMB share, beacon scheduler, net radio).
lv_obj_t *s_ta_smb_server = nullptr;
lv_obj_t *s_ta_smb_share = nullptr;
lv_obj_t *s_ta_smb_user = nullptr;
lv_obj_t *s_ta_smb_pass = nullptr;
lv_obj_t *s_ta_beacon_path = nullptr;
lv_obj_t *s_ta_beacon_interval = nullptr;
lv_obj_t *s_ta_radio_url = nullptr;
lv_obj_t *s_ta_radio_name = nullptr;
lv_obj_t *s_dd_radio_fav = nullptr;
lv_obj_t *s_dd_music_target = nullptr;
lv_obj_t *s_sw_espnow = nullptr;
lv_obj_t *s_lbl_espnow_status = nullptr;
// TX-codec tags in the top-right corner of the Home PTT buttons.
lv_obj_t *s_lbl_ptt_codec = nullptr;
lv_obj_t *s_lbl_eptt_codec = nullptr;
char s_shown_ptt_codec[8] = {};
char s_shown_eptt_codec[8] = {};
// Home PTT button objects: kept so refreshHome() can mirror the physical PTT
// state onto the on-screen button (add/remove LV_STATE_PRESSED).
lv_obj_t *s_btn_ptt = nullptr;
lv_obj_t *s_btn_eptt = nullptr;
bool s_ptt_shown_pressed = false;
bool s_eptt_shown_pressed = false;
lv_obj_t *s_lbl_cpu = nullptr;
char s_shown_cpu[16] = {};
uint32_t s_clr_cpu = 0xFFFFFFFFu;
void *s_fb_bench = nullptr;                       // fb0, for the FB benchmark
volatile bool s_force_invalidate = false;         // repaint after the benchmark
char s_shown_espnow_status[128] = {};
// Whether the Home page was built with the split (NRL + ESP-NOW) PTT; when
// the ESP-NOW enable state changes, refreshHome rebuilds the page.
bool s_home_espnow_split = false;
// Config generation this UI last rendered/produced. When another config
// writer (web portal, AT console) bumps CONFIG_NOTIFY past this, refresh()
// rebuilds the visible form page so its widgets show the new values.
uint32_t s_cfg_gen_seen = 0u;

// Music player page widgets. The list is rebuilt on page entry / rescan;
// the now-playing labels update from refresh().
lv_obj_t *s_lbl_music_title = nullptr;
lv_obj_t *s_lbl_music_artist = nullptr;
lv_obj_t *s_lbl_music_album = nullptr;
lv_obj_t *s_lbl_music_state = nullptr;
lv_obj_t *s_lbl_music_format = nullptr;
lv_obj_t *s_btn_music_toggle_label = nullptr;
lv_obj_t *s_btn_music_mode_label = nullptr;
lv_obj_t *s_list_music = nullptr;
lv_obj_t *s_img_music_cover = nullptr;
lv_obj_t *s_lbl_music_icon = nullptr;
char s_shown_music_path[256] = {}; // matches the player/playlist path limit
char s_shown_music_format[40] = {};
bool s_shown_music_playing = false;

// Decoded album art for the now-playing card. The bitmap and its LVGL
// descriptor outlive page rebuilds (the widget re-attaches to them);
// replaced on track change in refreshMusic.
CoverBitmap s_music_cover_bmp = {};
lv_image_dsc_t s_music_cover_dsc = {};
constexpr uint16_t kMusicCoverDim = 152;
TaskHandle_t s_music_cover_task = nullptr;
SemaphoreHandle_t s_music_cover_lock = nullptr;
uint32_t s_music_cover_req_seq = 0;
uint32_t s_music_cover_pending_seq = 0;
char s_music_cover_req_path[256] = {};
uint8_t *s_music_cover_req_jpeg = nullptr;
size_t s_music_cover_req_jpeg_size = 0;
CoverBitmap s_music_cover_pending = {};
char s_music_cover_pending_path[256] = {};
char s_music_cover_loaded_path[256] = {};

// Video-call page: remote JPEG frames decode through the same cover decoder
// into this bitmap; s_video_shown_seq tracks the last rendered frame.
// AI assistant page (xiaozhi push-to-talk).
lv_obj_t *s_lbl_ai_status = nullptr;
lv_obj_t *s_btn_ai_talk_label = nullptr;
lv_obj_t *s_btn_ai_talk = nullptr;
lv_obj_t *s_sw_ai = nullptr;
char s_shown_ai_status[224] = {};

// APRS page: master switch, status line and the stations-heard list.
lv_obj_t *s_sw_aprs = nullptr;
lv_obj_t *s_lbl_aprs_status = nullptr;
lv_obj_t *s_lbl_aprs_list = nullptr;
char s_shown_aprs_status[128] = {};
// APRS manual-message page: draft fields survive page switches.
lv_obj_t *s_ta_aprs_dest = nullptr;
lv_obj_t *s_ta_aprs_text = nullptr;
char s_aprs_msg_dest[12] = {};
char s_aprs_msg_text[72] = {};
char s_shown_aprs_list[704] = {};
uint32_t s_aprs_seen_revision = 0u;
uint32_t s_aprs_last_refresh_ms = 0u;

lv_obj_t *s_img_video = nullptr;
lv_obj_t *s_lbl_video_status = nullptr;
lv_obj_t *s_lbl_cw_rx = nullptr;
lv_obj_t *s_lbl_cw_rx_code = nullptr;
lv_obj_t *s_lbl_cw_tx = nullptr;
lv_obj_t *s_lbl_cw_tx_code = nullptr;
lv_obj_t *s_lbl_cw_metrics = nullptr;
uint32_t s_cw_revision = UINT32_MAX;
// CW touch input mode on the key zone(s): false = straight key (tap/hold),
// true = single-paddle keyer (hold a zone for an automatic element stream).
bool s_cw_paddle_mode = false;

// Map page (slippy tiles + APRS station overlay). The viewport is the area
// below the top bar; a 4x3 grid of 256 px tiles covers it at any sub-tile
// pan offset (3x2 would leave a gap: 3*256 < 800).
constexpr int kMapCols = 4;
constexpr int kMapRows = 3;
constexpr int kMapTilePx = 256;
constexpr int kMapTop = 56;                 // below the top bar
constexpr int kMapViewH = kHeight - kMapTop; // 424
constexpr uint8_t kMapZoomMin = 3u;
constexpr uint8_t kMapZoomMax = 17u;
constexpr uint8_t kMapZoomDefault = 12u;
constexpr size_t kMapMarkerMax = 24u;
lv_obj_t *s_map_view = nullptr;             // clipping container for tiles + markers
lv_obj_t *s_map_tiles[kMapCols * kMapRows] = {};
lv_image_dsc_t s_map_dsc[kMapCols * kMapRows] = {};
int32_t s_map_tile_x[kMapCols * kMapRows] = {}; // tile each widget currently shows
int32_t s_map_tile_y[kMapCols * kMapRows] = {};
bool s_map_tile_filled[kMapCols * kMapRows] = {};
lv_obj_t *s_map_markers[kMapMarkerMax] = {};
lv_obj_t *s_map_marker_labels[kMapMarkerMax] = {};
lv_obj_t *s_map_center_dot = nullptr;
lv_obj_t *s_map_lbl_zoom = nullptr;
double s_map_cx = 0.0; // viewport center in global pixels at s_map_zoom
double s_map_cy = 0.0;
uint8_t s_map_zoom = kMapZoomDefault;
bool s_map_centered = false; // initial center picked once (GPS/default/station)
uint32_t s_map_tile_rev = 0u;
uint32_t s_map_station_rev = UINT32_MAX;

// SSTV page (TX a JPEG from the TF card, live RX view from the service's
// decoder frame buffer).
constexpr size_t kSstvFileMax = 16u;
constexpr size_t kSstvNameChars = 40u;
lv_obj_t *s_sstv_lbl_file = nullptr;
lv_obj_t *s_sstv_lbl_tx = nullptr;
lv_obj_t *s_sstv_lbl_rx1 = nullptr;
lv_obj_t *s_sstv_lbl_rx2 = nullptr;
lv_obj_t *s_sstv_lbl_img = nullptr;
lv_obj_t *s_sstv_mode_label = nullptr; // label children of toggle buttons
lv_obj_t *s_sstv_src_label = nullptr;
lv_obj_t *s_sstv_rx_toggle_label = nullptr;
lv_obj_t *s_img_sstv = nullptr;
lv_image_dsc_t s_sstv_dsc = {};
// TX-source preview in the same right-side window: camera captures and
// TF-card images decode to this bitmap; a new RX frame takes it back.
CoverBitmap s_sstv_tx_bmp = {};
lv_image_dsc_t s_sstv_tx_dsc = {};
bool s_sstv_tx_preview = false;
SSTV_Mode s_sstv_tx_mode = SSTV_MODE_ROBOT36;
SstvRxSource s_sstv_rx_source = SSTV_SOURCE_MIC;
char s_sstv_files[kSstvFileMax][kSstvNameChars] = {};
size_t s_sstv_file_count = 0u;
size_t s_sstv_file_index = 0u;
uint32_t s_sstv_img_rev = 0u;
char s_sstv_msg[96] = {};       // transient save/status message
uint32_t s_sstv_msg_ms = 0u;

// RX log viewer (sub-page of the SSTV page): saved frames at
// /sdcard/sstv/sstv_rx_*.jpg, static rebuild-on-action like the CW score page.
constexpr size_t kSstvLogMax = 32u;
uint8_t s_sstv_view = 0u; // 0 = main SSTV page, 1 = RX log list, 2 = log viewer
NRL_PSRAM_BSS char s_sstv_log_files[kSstvLogMax][kSstvNameChars] = {};
size_t s_sstv_log_count = 0u;
size_t s_sstv_log_index = 0u;
CoverBitmap s_sstv_log_bmp = {};
lv_image_dsc_t s_sstv_log_dsc = {};
lv_obj_t *s_img_sstv_log = nullptr;
lv_obj_t *s_btn_video_tx_label = nullptr;
volatile bool s_video_tx_toggle_busy = false; // camera open/close in a helper task
CoverBitmap s_video_bmp = {};
lv_image_dsc_t s_video_dsc = {};
constexpr uint16_t kVideoFrameDim = 456; // fits the 800x480 page layout

// Local camera self-view (small PIP in the right column).
lv_obj_t *s_img_video_local = nullptr;
CoverBitmap s_video_local_bmp = {};
lv_image_dsc_t s_video_local_dsc = {};
constexpr uint16_t kVideoLocalDim = 144; // scaled decode; fits the PIP panel

// Video decode worker: JPEG -> RGB565 for both the remote frame and the
// local self-view runs in this task instead of inside Display_Poll, so the
// LVGL/touch latency stays flat while a call is up. Core 1, priority 3:
// core 0 already carries the WiFi stack, the audio bridge, the camera TX
// burst task and LVGL during a call -- a decode starved for cycles there
// stretched a single jpeg_dec_process() past the task-watchdog window and
// IDLE0 panicked. Core 1 runs the (higher-priority) music player and BT at
// most, so decoding uses its leftover cycles and simply drops to a lower
// frame rate under load (Acquire/Copy always hand out only the newest
// frame). Handoff to the UI is a pair of pending bitmaps guarded by
// s_video_view_lock; refreshVideo adopts them with a non-blocking take.
TaskHandle_t s_video_view_task = nullptr;
volatile bool s_video_view_run = false;
SemaphoreHandle_t s_video_view_lock = nullptr;
CoverBitmap s_video_pending_remote = {};
CoverBitmap s_video_pending_local = {};

// Montserrat with a CJK font as fallback: ASCII keeps the Latin design,
// Chinese tags/filenames render from the CJK engine. Initialised in
// Display_Init (lv_font_montserrat_* are const, so mutable copies).
// The fallback is switchable at runtime between the built-in bitmap subset
// (lv_font_cjk_*) and FreeType vector rendering from a TTF on the TF card,
// for side-by-side comparison (Display_SetCjkFontEngine).
lv_font_t s_font_ui_16;
lv_font_t s_font_ui_20;
int s_cjk_font_engine = DISPLAY_CJK_FONT_BITMAP;
#if LV_USE_FREETYPE
constexpr const char *kCjkTtfPath = "/sdcard/fonts/cjk.ttf";
bool s_freetype_inited = false;
lv_font_t *s_ft_font_16 = nullptr;
lv_font_t *s_ft_font_20 = nullptr;
#endif

char s_shown_caption[24] = {};
char s_shown_callsign[16] = {};
char s_shown_ssid[16] = {};
char s_shown_time[16] = {};
char s_shown_local_station[24] = {};
char s_shown_wifi[32] = {};
char s_shown_vol[16] = {};
char s_shown_remote_station[160] = {};
char s_shown_ip[96] = {};
char s_shown_server[96] = {};
char s_shown_signaling[96] = {};
char s_shown_detail[512] = {};
char s_shown_bt_top[24] = {};
char s_wifi_option_ssids[kWifiOptionCount][33] = {};

// Cached text colours / styles. In LVGL FULL render mode any invalidation
// re-renders the whole 800x480 framebuffer (~80-106 ms), so re-applying a colour
// or style every refresh tick -- even when unchanged -- forces a costly full
// redraw and makes the UI feel laggy. These caches let the refresh helpers skip
// the setter (and the invalidation) when the value hasn't changed. Sentinel
// 0xFFFFFFFF / -1 means "unknown" and is reset on every page rebuild.
constexpr uint32_t kColorUnset = 0xFFFFFFFFu;
uint32_t s_clr_callsign = kColorUnset;
uint32_t s_clr_server = kColorUnset;
uint32_t s_clr_remote_station = kColorUnset;
uint32_t s_clr_bt_top = kColorUnset;
int s_ls_callsign = -1;  // callsign letter-spacing

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool initPanel()
{
    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_cfg.timings.pclk_hz = 26 * 1000 * 1000;
    panel_cfg.timings.h_res = kWidth;
    panel_cfg.timings.v_res = kHeight;
    panel_cfg.timings.hsync_pulse_width = 1;
    panel_cfg.timings.hsync_back_porch = 40;
    panel_cfg.timings.hsync_front_porch = 20;
    panel_cfg.timings.vsync_pulse_width = 1;
    panel_cfg.timings.vsync_back_porch = 10;
    panel_cfg.timings.vsync_front_porch = 5;
    panel_cfg.timings.flags.pclk_active_neg = true;
    panel_cfg.data_width = 16;
    panel_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    panel_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    // Two full framebuffers in PSRAM, double-buffered with LVGL FULL-refresh (see
    // initLvgl). The GDMA always streams one complete framebuffer straight from
    // PSRAM while LVGL renders the whole next frame into the other, so nothing
    // writes the buffer being scanned and there is NO bounce buffer / CPU refill
    // ISR for a codec I2C write to starve. This is the example's multi-framebuffer
    // technique and is what finally stops the volume-change drift.
    panel_cfg.num_fbs = 2;
    panel_cfg.dma_burst_size = 128;
    panel_cfg.hsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_HSYNC);
    panel_cfg.vsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_VSYNC);
    panel_cfg.de_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_DE);
    panel_cfg.pclk_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_PCLK);
    panel_cfg.disp_gpio_num = GPIO_NUM_NC;
    panel_cfg.data_gpio_nums[0] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA0);
    panel_cfg.data_gpio_nums[1] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA1);
    panel_cfg.data_gpio_nums[2] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA2);
    panel_cfg.data_gpio_nums[3] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA3);
    panel_cfg.data_gpio_nums[4] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA4);
    panel_cfg.data_gpio_nums[5] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA5);
    panel_cfg.data_gpio_nums[6] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA6);
    panel_cfg.data_gpio_nums[7] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA7);
    panel_cfg.data_gpio_nums[8] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA8);
    panel_cfg.data_gpio_nums[9] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA9);
    panel_cfg.data_gpio_nums[10] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA10);
    panel_cfg.data_gpio_nums[11] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA11);
    panel_cfg.data_gpio_nums[12] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA12);
    panel_cfg.data_gpio_nums[13] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA13);
    panel_cfg.data_gpio_nums[14] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA14);
    panel_cfg.data_gpio_nums[15] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA15);
    panel_cfg.flags.fb_in_psram = true;

    if (esp_lcd_new_rgb_panel(&panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel create failed");
        return false;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel init failed");
        return false;
    }
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
}

uint32_t lvglTick()
{
    return millis();
}

void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    // DIRECT render mode: LVGL renders only the changed areas, but straight into
    // one of the panel's two PSRAM framebuffers (px_map is that whole buffer).
    // Present it once per frame -- on the last flush -- by handing the framebuffer
    // back to the RGB panel, which flips to it at VSYNC (no copy, since px_map is a
    // known framebuffer). LVGL keeps both buffers in sync by also re-rendering the
    // previous frame's dirty areas. Rendering touches only the back buffer, so the
    // partial-write-into-the-live-framebuffer drift FULL mode avoided stays avoided.
    if (lv_display_flush_is_last(disp)) {
        const int64_t t0 = esp_timer_get_time();
        esp_lcd_panel_draw_bitmap(panel, 0, 0, kWidth, kHeight, px_map);
        const uint32_t present_ms =
            static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
        if (present_ms > 50u) {
            // Telemetry: splits "software render slow" from "panel present
            // blocked" inside the lv_timer_handler duration numbers.
            if (present_ms > s_pending_present_ms) {
                s_pending_present_ms = present_ms;
            }
        }
    }
    lv_display_flush_ready(disp);
}

// Telemetry: DIRECT render mode should only redraw small dirty areas, yet
// ~140 ms full-screen-sized render passes keep showing up. Log any
// invalidation covering >1/4 of the screen so the offending widget's
// coordinates give it away.
void invalidateProbe(lv_event_t *e)
{
    const lv_area_t *a = static_cast<const lv_area_t *>(lv_event_get_param(e));
    if (a == nullptr) {
        return;
    }
    const int32_t w = a->x2 - a->x1 + 1;
    const int32_t h = a->y2 - a->y1 + 1;
    ++s_invalidate_count;
    s_invalidate_px += static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    if (static_cast<int64_t>(w) * h >= (static_cast<int64_t>(kWidth) * kHeight) / 4) {
        s_big_invalidate_w = w;
        s_big_invalidate_h = h;
        s_big_invalidate_x = a->x1;
        s_big_invalidate_y = a->y1;
        s_big_invalidate_pending = true;
    }
}

void flushLvglTelemetry()
{
    if (s_pending_present_ms > 0u) {
        const uint32_t present_ms = s_pending_present_ms;
        s_pending_present_ms = 0u;
        ESP_LOGW(TAG, "panel present took %lums", static_cast<unsigned long>(present_ms));
    }
    if (s_pending_touch_ms > 0u) {
        const uint32_t touch_ms = s_pending_touch_ms;
        s_pending_touch_ms = 0u;
        ESP_LOGW(TAG, "touch read blocked %lums", static_cast<unsigned long>(touch_ms));
    }
    if (s_big_invalidate_pending) {
        s_big_invalidate_pending = false;
        ++s_big_invalidate_count;
        static uint32_t s_last_log_ms = 0u;
        const uint32_t now = millis();
        if (now - s_last_log_ms > 1000u) {
            s_last_log_ms = now;
            ESP_LOGW(TAG, "big invalidate %ldx%ld @(%ld,%ld)",
                     static_cast<long>(s_big_invalidate_w),
                     static_cast<long>(s_big_invalidate_h),
                     static_cast<long>(s_big_invalidate_x),
                     static_cast<long>(s_big_invalidate_y));
        }
    }
}

bool initLvgl()
{
    lv_init();

    // Use the RGB panel's own two framebuffers and let LVGL double-buffer in
    // DIRECT mode: LVGL renders only the changed areas into the off-screen
    // framebuffer, the flush hands that whole buffer to the panel (which swaps it
    // in at VSYNC), and the GDMA scans it straight from PSRAM. No bounce buffer and
    // no partial writes into the live framebuffer, so a codec I2C write can no
    // longer starve a refill and drift the image. DIRECT (vs FULL) avoids
    // re-rendering the entire 800x480 screen on every small change (e.g. the clock
    // tick), which in FULL mode cost ~80-106 ms per refresh and made the UI laggy.
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK) {
        ESP_LOGE(TAG, "get frame buffers failed");
        return false;
    }
    s_fb_bench = fb0;

    s_disp = lv_display_create(kWidth, kHeight);
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "display create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, lvglFlush);
    const size_t fb_bytes = static_cast<size_t>(kWidth) * kHeight * 2u;
    lv_display_set_buffers(s_disp, fb0, fb1, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_add_event_cb(s_disp, invalidateProbe, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_tick_set_cb(lvglTick);
    return true;
}

void touchRead(lv_indev_t *, lv_indev_data_t *data)
{
    if (s_touch == nullptr || data == nullptr) {
        return;
    }
    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t count = 0;
    const int64_t t0 = esp_timer_get_time();
    esp_lcd_touch_read_data(s_touch);
    const uint32_t dt_ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    if (dt_ms > 30u) {
        // Telemetry: the GT1151 shares the I2C bus with the audio codec; a
        // long read here means another task parked on the bus mutex.
        if (dt_ms > s_pending_touch_ms) {
            s_pending_touch_ms = dt_ms;
        }
    }
    const esp_err_t err = esp_lcd_touch_get_data(s_touch, points, &count, 1);
    if (err == ESP_OK && count > 0) {
        data->point.x = static_cast<int16_t>(points[0].x);
        data->point.y = static_cast<int16_t>(points[0].y);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool initTouch()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!I2C_MasterGetBus(&bus)) {
        ESP_LOGW(TAG, "touch I2C unavailable");
        return false;
    }

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = kWidth;
    touch_cfg.y_max = kHeight;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = GPIO_NUM_NC;

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS;
    io_cfg.scl_speed_hz = 400000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 16;
    io_cfg.lcd_param_bits = 0;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.transaction_timeout_ms = 100;

    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_touch_io) != ESP_OK) {
        ESP_LOGW(TAG, "touch IO create failed");
        return false;
    }
    if (esp_lcd_touch_new_i2c_gt1151(s_touch_io, &touch_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "GT1151 create failed");
        return false;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        return false;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, touchRead);
    lv_indev_set_long_press_repeat_time(s_touch_indev, kVolumeLongPressRepeatMs);
    ESP_LOGI(TAG, "GT1151 touch ready");
    return true;
}

// ---- UI language (i18n) -----------------------------------------------------
// English is the source language; `tr()` looks a string up in the table and
// returns the Chinese text when 中文 is selected (falling back to English for
// anything untranslated, so dynamic/technical strings pass through). The
// helpers (button / fieldLabel / formStatus) translate automatically, so page
// code mostly stays written in plain English. Persisted in NVS ("ui"/"lang");
// switching rebuilds the current page. The CJK glyphs come from the GB2312
// level-1 bitmap font that already backs the music tags.

int s_lang = 0; // 0 = English, 1 = 中文

struct TrEntry {
    const char *en;
    const char *zh;
};

const TrEntry kTr[] = {
    // Navigation / common buttons
    {"Back", "返回"},
    {"Save", "保存"},
    {"Config", "设置"},
    {"Apps", "应用"},
    {"Music", "音乐"},
    {"Radio", "电台"},
    {"Video", "视频"},
    {"Tetris", "方块"},
    {"Map", "地图"},
    {"Send", "发送"},
    {"Save", "保存"},
    {"Start", "开始"},
    {"Stop", "停止"},
    {"no TF card", "未插 TF 卡"},
    {"no .jpg in /sstv", "/sstv 目录没有 .jpg"},
    {"no JPEG on TF card", "TF 卡上没有 JPEG"},
    {"TX busy / failed", "发送忙/失败"},
    {"saved %s", "已保存 %s"},
    {"save failed (no frame/card)", "保存失败（无图像/无卡）"},
    {"TX preparing...", "发送准备中..."},
    {"TX sending %u%%", "发送中 %u%%"},
    {"TX done", "发送完成"},
    {"TX error: %s", "发送错误：%s"},
    {"TX idle (%s)", "发送空闲（%s）"},
    {"RX off", "接收关闭"},
    {"camera busy (video TX)", "摄像头忙（视频通话中）"},
    {"camera capture failed", "拍照失败"},
    {"camera TX queue failed", "照片发送排队失败"},
    {"no saved frames", "没有已保存的图像"},
    {"decode failed", "解码失败"},
    {"Up", "上一个"},
    {"Down", "下一个"},
    {"View", "查看"},
    {"Station", "台站"},
    {"Audio", "音频"},
    {"BT", "蓝牙"},
    {"Nanny", "保姆"},
    {"NAS", "共享"},
    {"Scan", "扫描"},
    {"Reset", "重置"},
    {"Rescan", "重扫"},
    {"Clear", "清除"},
    {"Beacon Off", "关闭信标"},
    {"Cam ON", "开摄像头"},
    {"Cam OFF", "关摄像头"},
    {"Hold to Talk", "按住说话"},
    {"Language", "语言"},
    {"About", "关于"},
    {"Firmware Version", "固件版本"},
    {"OTA Server URL", "OTA 服务器地址"},
    {"Available Version", "可用版本"},
    {"Older", "旧版本"},
    {"Newer", "新版本"},
    {"Save URL", "保存地址"},
    {"Check Update", "检查更新"},
    {"Install", "安装"},
    {"English\n中文", "English\n中文"},
    {"VOL-", "音量-"},
    {"VOL+", "音量+"},
    // PTT stays English (universal ham term); left untranslated on purpose.
    // Home-page status captions (top bar)
    {"STANDBY", "待机"},
    {"FULL DUPLEX", "全双工"},
    {"TRANSMITTING", "发射中"},
    {"RECEIVING", "接收中"},
    {"ESP-NOW TX", "ESP-NOW 发射"},
    {"ESP-NOW RX", "ESP-NOW 接收"},
    // WiFi page
    {"WiFi SSID", "WiFi 名称"},
    {"Password", "密码"},
    {"Nearby WiFi", "附近 WiFi"},
    {"Scan, select or type SSID, then save.", "扫描后选择或直接输入名称, 然后保存。"},
    {"Scanning WiFi...", "正在扫描 WiFi..."},
    {"Found %u WiFi networks.", "找到 %u 个 WiFi 网络。"},
    {"Scan failed.", "扫描失败。"},
    {"Scan failed: task create failed.", "扫描失败: 任务创建失败。"},
    {"(scanning)", "(扫描中)"},
    {"Select WiFi...", "选择 WiFi..."},
    {"SSID: %s\nMode: %s\nSTA IP: %s\nConfig AP: %s\n\nReset WiFi clears saved network credentials.",
     "名称: %s\n模式: %s\n联网地址: %s\n配置热点: %s\n\n重置将清除已保存的 WiFi 凭据。"},
    // Station page
    {"Callsign", "呼号"},
    {"Server Host / IP", "服务器地址 / IP"},
    {"Port", "端口"},
    {"Edit station and server settings, then save.", "修改台站和服务器设置后保存。"},
    {"Callsign: %s\nSSID: %u\nServer: %s:%u\n\nEdit station and server settings on the Station page.",
     "呼号: %s\nSSID: %u\n服务器: %s:%u\n\n在台站页修改呼号和服务器设置。"},
    // Audio page
    {"Speaker", "扬声器"},
    {"Speaker (headset)", "扬声器 (耳机)"},
    {"Mic", "麦克风"},
    {"Adjust audio settings, then save.", "调整音频设置后保存。"},
    {"Adjust audio, then save. Playback applies from the next track.", "调整音频后保存，播放设置下一曲生效。"},
    {"Changed. Tap Save to persist.", "已修改, 点保存生效。"},
    {"Reset failed.", "重置失败。"},
    {"Speaker volume: %d%%\nMic volume: %u\nAEC: %s\nNoise reduction: %s",
     "扬声器音量: %d%%\n麦克风音量: %u\n回声消除: %s\nAI 降噪: %s"},
    // Bluetooth page
    {"Bluetooth headset", "蓝牙耳机"},
    {"Turn on, Scan, then tap a headset. Saved ones reconnect automatically.",
     "先开启并扫描, 点击耳机连接; 已保存的会自动重连。"},
    {"Headsets: tap to connect, long-press a saved one to delete",
     "点击耳机连接, 长按已保存条目删除"},
    {"Scanning for headsets...", "正在扫描耳机..."},
    {"Turn Bluetooth on first.", "请先打开蓝牙。"},
    {"Removed saved headset.", "已删除保存的耳机。"},
    // Apps page / playback target
    {"Playback Target (music / beacon / radio)", "播放目标 (音乐 / 信标 / 电台)"},
    {"Music Output Device", "音乐输出设备"},
    {"Local speaker\nNRL network\nLocal + network", "本地扬声器\nNRL 网络\n本地 + 网络"},
    {"Onboard speaker\nBT headset (A2DP)", "板载扬声器\n蓝牙耳机 (A2DP)"},
    {"Applies from the next track.", "下一曲目起生效。"},
    {"Playback target saved (applies from the next track).", "播放目标已保存 (下一曲目起生效)。"},
    {"Music output saved (applies from the next track).", "输出设备已保存 (下一曲目起生效)。"},
    // Music page
    {"Playing", "播放中"},
    {"Stopped", "已停止"},
    {"No tracks. Put files in /sdcard/music and Rescan.",
     "没有歌曲。把文件放进 TF 卡 music 目录后重扫。"},
    {"No TF card mounted.", "未挂载 TF 卡。"},
    // Net radio page
    {"Favorite Stations", "收藏电台"},
    {"Station Name", "电台名称"},
    {"Stream URL (http:// or https://)", "直播流地址 (http:// 或 https://)"},
    {"(no favorites)", "(暂无收藏)"},
    {"My station", "我的电台"},
    {"Pick a favorite or type a URL, then tap Play. + saves to favorites.",
     "选择收藏或输入地址后点播放; + 存入收藏。"},
    {"Playing: %s", "播放中: %s"},
    {"Station URL saved.", "电台地址已保存。"},
    {"Set a station URL first.", "请先填写电台地址。"},
    {"Tuning in...", "正在连接电台..."},
    {"Play failed.", "播放失败。"},
    {"Stopped.", "已停止。"},
    {"Save failed: URL must start with http:// or https://.",
     "保存失败: 地址必须以 http:// 或 https:// 开头。"},
    {"Add failed: URL must start with http:// or https://.",
     "添加失败: 地址必须以 http:// 或 https:// 开头。"},
    {"Favorites list is full.", "收藏已满。"},
    {"Favorite saved.", "已存入收藏。"},
    {"Favorite deleted.", "已删除收藏。"},
    {"Delete failed.", "删除失败。"},
    {"No favorites to delete.", "没有可删除的收藏。"},
    {"No favorites to switch. Add one with +.", "没有收藏可切换, 用 + 添加。"},
    // Nanny page
    {"Beacon File Path", "信标文件路径"},
    {"Interval (min)", "间隔 (分钟)"},
    {"Beacon armed.", "信标已启用。"},
    {"Beacon disabled.", "信标已关闭。"},
    {"Beacon armed. Save applies changes; Beacon Off disarms.",
     "信标已启用。保存应用修改; 关闭信标停用。"},
    {"Beacon off. Set file path + minutes, then Save.",
     "信标未启用。填写文件路径和分钟数后保存。"},
    {"Save failed: set the beacon file path.", "保存失败: 请填写信标文件路径。"},
    {"Save failed: interval must be 1-1440 minutes.", "保存失败: 间隔须为 1-1440 分钟。"},
    // SMB page
    {"Server (NAS / PC)", "服务器 (NAS / 电脑)"},
    {"Share Name", "共享名"},
    {"Username (empty = guest)", "用户名 (留空为来宾)"},
    {"SMB saved. Mounting in background...", "已保存, 后台挂载中..."},
    {"SMB config cleared.", "已清除共享配置。"},
    {"Save failed: server and share are required.", "保存失败: 服务器和共享名必填。"},
    // ESP-NOW page
    {"ESP-NOW enable failed (WiFi not started).", "开启失败 (WiFi 未启动)。"},
    {"ESP-NOW Intercom", "ESP-NOW 对讲"},
    {"RX", "接收"},
    {"Opus", "Opus"},
    {"ESP-NOW intercom on.", "ESP-NOW 对讲已开启。"},
    {"ESP-NOW intercom off.", "ESP-NOW 对讲已关闭。"},
    {"ESP-NOW receive on.", "ESP-NOW 接收已开启。"},
    {"ESP-NOW receive muted.", "ESP-NOW 接收已静默。"},
    {"ESP-NOW TX: Opus 16k wideband.", "ESP-NOW 发射: Opus 16k 宽带。"},
    {"ESP-NOW TX: G.711 8k.", "ESP-NOW 发射: G.711 8k。"},
    {"Opus enable failed (out of memory); staying on G.711.",
     "Opus 开启失败 (内存不足), 保持 G.711。"},
    {"Off-grid intercom with nearby devices. When on, the home screen "
     "gains a dedicated ESP-NOW PTT below the network PTT.",
     "与附近设备脱网对讲。开启后, 主页会在网络 PTT 下方增加独立的 ESP-NOW PTT。"},
    {"TX voice: Opus 16k wideband (type 8).", "NRL 发射: Opus 16k 宽带 (类型 8)。"},
    {"TX voice: G.711 8k (type 1).", "NRL 发射: G.711 8k (类型 1)。"},
    {"State: %s\nLast peer heard: %s%s", "状态: %s\n最近听到: %s%s"},
    // Video page
    {"Remote video", "对方画面"},
    {"Local camera", "本机画面"},
    // AI page
    {"xiaozhi AI Assistant", "小智 AI 助手"},
    {"Enable assistant", "启用助手"},
    {"Listening... release to send", "正在录音... 松开发送"},
    {"Not connected", "未连接"},
    {"Enable the assistant first", "请先启用助手"},
    {"Connecting...", "连接中..."},
    {"Assistant disabled.", "已停用助手。"},
    {"Set the server URL first (web portal Media page or AT+AI=wss://...,token).",
     "请先配置服务器地址 (Web 配置页媒体标签, 或 AT+AI=wss://...,token)。"},
    {"Server URL / token: web portal Media page, or AT+AI=wss://...,token",
     "服务器地址/令牌: Web 配置页媒体标签, 或 AT+AI=wss://...,token"},
    // Shared
    {"OTA CHECKING...", "正在检查 OTA..."},
    {"OTA DOWNLOADING...", "正在下载 OTA 固件..."},
    {"OTA UPDATING...", "正在升级 OTA 固件..."},
    {"OTA Updating %u%%", "OTA 升级 %u%%"},
    {"OTA UPLOADING...", "正在上传 OTA 固件..."},
    {"OTA CHECK FAILED", "OTA 检查失败"},
    {"OTA UPDATE FAILED", "OTA 升级失败"},
    {"OTA COMPLETE - REBOOTING", "OTA 完成，正在重启"},
    {"FIRMWARE IS UP TO DATE", "固件已是最新版本"},
    {"AT COMMAND APPLIED", "AT 指令已执行"},
    {"AT COMMAND FAILED", "AT 指令执行失败"},
    {"Settings updated from web/serial.", "设置已从 Web/串口更新。"},
    {"Set and save the OTA server URL.", "请设置并保存 OTA 服务器地址。"},
    {"Tap Check Update to query available versions.", "点击检查更新以查询可用版本。"},
    {"Checking for updates...", "正在检查更新..."},
    {"Update check requested...", "已请求检查更新..."},
    {"Installing firmware...", "正在安装固件..."},
    {"Installing firmware... %u%%", "正在安装固件... %u%%"},
    {"Firmware install requested...", "已请求安装固件..."},
    {"OTA server URL saved.", "OTA 服务器地址已保存。"},
    {"Save failed: out of memory.", "保存失败：内存不足。"},
    {"Check failed: out of memory.", "检查失败：内存不足。"},
    {"Install failed: out of memory.", "安装失败：内存不足。"},
    {"OTA status unavailable: out of memory.", "无法读取 OTA 状态：内存不足。"},
    {"Save failed: URL must start with http:// or https://.",
     "保存失败：地址必须以 http:// 或 https:// 开头。"},
    {"Check failed: configure the OTA server first.", "检查失败：请先配置 OTA 服务器。"},
    {"Install failed: check available versions first.", "安装失败：请先检查可用版本。"},
    {"Selected version is already installed.", "所选版本已经安装。"},
    {"OTA error: %s", "OTA 错误：%s"},
    {"Latest: %s  |  %u version(s) available", "最新：%s  |  共 %u 个可用版本"},
    {"Selected: %s", "已选择：%s"},
    {"No versions available", "没有可用版本"},
};

const char *tr(const char *text)
{
    if (s_lang == 0 || text == nullptr) {
        return text;
    }
    for (size_t i = 0; i < sizeof(kTr) / sizeof(kTr[0]); ++i) {
        if (strcmp(kTr[i].en, text) == 0) {
            return kTr[i].zh;
        }
    }
    return text;
}

void loadUiLang()
{
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t lang = 0;
    if (nvs_get_u8(nvs, "lang", &lang) == ESP_OK && lang <= 1u) {
        s_lang = lang;
    }
    nvs_close(nvs);
}

void setUiLang(const int lang)
{
    s_lang = (lang != 0) ? 1 : 0;
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "lang", static_cast<uint8_t>(s_lang));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// ---- widget helpers ---------------------------------------------------------

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    // Route the standard UI fonts through their CJK-fallback twins so
    // translated text renders; pure-ASCII output is unaffected.
    if (font == &lv_font_montserrat_16) {
        font = &s_font_ui_16;
    } else if (font == &lv_font_montserrat_20) {
        font = &s_font_ui_20;
    }
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    return obj;
}

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorPanel), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 14, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void action(lv_event_t *event);
void volumeEvent(lv_event_t *event);
void softPttEvent(lv_event_t *event);
void espnowPttEvent(lv_event_t *event);
void wifiEnableEvent(lv_event_t *event);
void textAreaEvent(lv_event_t *event);
void wifiOptionEvent(lv_event_t *event);
void audioSliderEvent(lv_event_t *event);
void audioSwitchEvent(lv_event_t *event);
void musicTargetEvent(lv_event_t *event);
void musicOutputEvent(lv_event_t *event);
void languageEvent(lv_event_t *event);
void updateAudioValueLabels();
void refresh();
void refreshOtaPage();
void refreshAprsPage();
void buildAprs();
void buildAprsGateway();
void rebuildCurrentPage();
void buildProvisioning();
void refreshProvisioning();
void stopVideoView();
lv_obj_t *styledDropdown(lv_obj_t *parent, int x, int y, int w);

NrlOtaStatus *otaUiSnapshot()
{
    if (s_ota_ui_status == nullptr) {
        s_ota_ui_status = static_cast<NrlOtaStatus *>(
            heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_ota_ui_status == nullptr) {
            s_ota_ui_status = static_cast<NrlOtaStatus *>(
                heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_8BIT));
        }
    }
    if (s_ota_ui_status == nullptr) {
        return nullptr;
    }
    memset(s_ota_ui_status, 0, sizeof(*s_ota_ui_status));
    OtaService_GetStatus(s_ota_ui_status);
    return s_ota_ui_status;
}

// Set the per-page form status line (no-op when the page has none).
void formStatus(const char *text, uint32_t color)
{
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, tr(text));
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(color), 0);
    }
}

lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, Action id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    void *user_data = reinterpret_cast<void *>(static_cast<intptr_t>(id));
    if (id == Action::VolumeDown || id == Action::VolumeUp) {
        lv_obj_add_event_cb(btn, volumeEvent, LV_EVENT_PRESSED, user_data);
        lv_obj_add_event_cb(btn, volumeEvent, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    } else {
        lv_obj_add_event_cb(btn, action, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *txt = label(btn, &lv_font_montserrat_20, kColorText);
    lv_obj_center(txt);
    lv_label_set_text(txt, tr(text));
    return btn;
}

lv_obj_t *textArea(lv_obj_t *parent,
                   int x,
                   int y,
                   int w,
                   const char *placeholder,
                   const char *text,
                   uint32_t max_len,
                   bool password,
                   const char *accepted_chars,
                   bool number_keyboard)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 46);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kColorText), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, text ? text : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_password_mode(ta, password);
    if (accepted_chars != nullptr) {
        lv_textarea_set_accepted_chars(ta, accepted_chars);
    }
    lv_obj_add_event_cb(ta, textAreaEvent, LV_EVENT_ALL,
                        reinterpret_cast<void *>(static_cast<intptr_t>(number_keyboard ? 1 : 0)));
    return ta;
}

void fieldLabel(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *obj = label(parent, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(obj, x, y);
    lv_label_set_text(obj, tr(text));
}

lv_obj_t *valueLabel(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *obj = label(parent, &lv_font_montserrat_16, kColorAccent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(obj, tr("--"));
    return obj;
}

lv_obj_t *slider(lv_obj_t *parent, int x, int y, int w, int min, int max, int value, AudioControl id)
{
    lv_obj_t *obj = lv_slider_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, 16);
    lv_slider_set_range(obj, min, max);
    lv_slider_set_value(obj, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(obj, audioSliderEvent, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));
    return obj;
}

lv_obj_t *switchControl(lv_obj_t *parent, int x, int y, const char *text, bool checked, AudioControl id)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 64, 34);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, audioSwitchEvent, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    lv_obj_t *txt = label(parent, &lv_font_montserrat_20, kColorText);
    lv_obj_set_pos(txt, x + 78, y + 6);
    lv_label_set_text(txt, tr(text));
    return sw;
}

void createKeyboard(lv_obj_t *scr)
{
    s_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_keyboard, kWidth, 184);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

bool setLabel(lv_obj_t *obj, char *cache, size_t cache_size, const char *text)
{
    if (obj == nullptr || strncmp(cache, text, cache_size) == 0) {
        return false;
    }
    snprintf(cache, cache_size, "%.*s", static_cast<int>(cache_size - 1u), text);
    lv_label_set_text(obj, text);
    return true;
}

// Apply a text colour only when it differs from the cached value, to avoid the
// invalidation (and full-screen re-render) that lv_obj_set_style_text_color
// triggers every time it is called.
void setLabelColor(lv_obj_t *obj, uint32_t &cache, uint32_t color)
{
    if (obj == nullptr || cache == color) {
        return;
    }
    cache = color;
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

void formatStationBadge(char *out, size_t out_size, const char *call, unsigned ssid)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    const size_t field_chars = (out_size - 1u < kStationFieldChars) ? out_size - 1u : kStationFieldChars;
    if (call == nullptr || call[0] == '\0') {
        snprintf(out, out_size, "%s", kCallsignPlaceholder);
        out[field_chars] = '\0';
        return;
    }

    char raw[24] = {};
    snprintf(raw, sizeof(raw), "%s-%u", call, ssid);
    size_t i = 0u;
    for (; i < field_chars && raw[i] != '\0'; ++i) {
        out[i] = raw[i];
    }
    for (; i < field_chars; ++i) {
        out[i] = ' ';
    }
    out[field_chars] = '\0';
}

void clearScreen()
{
    // A page (re)build renders current config; consume any pending change.
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
    // Stop the game's lv_timer before its widgets are deleted below.
    GAME_TETRIS_Teardown();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    memset(s_shown_caption, 0, sizeof(s_shown_caption));
    memset(s_shown_dmrid, 0, sizeof(s_shown_dmrid));
    memset(s_shown_callsign, 0, sizeof(s_shown_callsign));
    memset(s_shown_ssid, 0, sizeof(s_shown_ssid));
    memset(s_shown_time, 0, sizeof(s_shown_time));
    memset(s_shown_local_station, 0, sizeof(s_shown_local_station));
    memset(s_shown_wifi, 0, sizeof(s_shown_wifi));
    memset(s_shown_vol, 0, sizeof(s_shown_vol));
    memset(s_shown_remote_station, 0, sizeof(s_shown_remote_station));
    memset(s_shown_ip, 0, sizeof(s_shown_ip));
    memset(s_shown_server, 0, sizeof(s_shown_server));
    memset(s_shown_signaling, 0, sizeof(s_shown_signaling));
    memset(s_shown_detail, 0, sizeof(s_shown_detail));
    memset(s_shown_bt_top, 0, sizeof(s_shown_bt_top));
    // Labels are recreated on rebuild, so drop the colour/style caches too.
    s_clr_callsign = kColorUnset;
    s_clr_server = kColorUnset;
    s_clr_remote_station = kColorUnset;
    s_clr_bt_top = kColorUnset;
    s_ls_callsign = -1;
    s_lbl_caption = nullptr;
    s_lbl_rx_codec = nullptr;
    s_shown_rx_codec[0] = '\0';
    s_lbl_dmrid = nullptr;
    s_lbl_callsign = nullptr;
    s_lbl_signaling = nullptr;
    s_lbl_ssid = nullptr;
    s_lbl_time = nullptr;
    s_lbl_local_station = nullptr;
    s_lbl_wifi = nullptr;
    s_lbl_bt_top = nullptr;
    s_list_bt = nullptr;
    s_lbl_vol = nullptr;
    s_lbl_remote_station = nullptr;
    s_lbl_ip = nullptr;
    s_lbl_server = nullptr;
    s_lbl_detail = nullptr;
    s_lbl_form_status = nullptr;
    s_lbl_cw_rx = nullptr;
    s_lbl_cw_rx_code = nullptr;
    s_lbl_cw_tx = nullptr;
    s_lbl_cw_tx_code = nullptr;
    s_lbl_cw_metrics = nullptr;
    s_cw_revision = UINT32_MAX;
    s_map_view = nullptr;
    for (size_t i = 0u; i < sizeof(s_map_tiles) / sizeof(s_map_tiles[0]); ++i) {
        s_map_tiles[i] = nullptr;
    }
    for (size_t i = 0u; i < kMapMarkerMax; ++i) {
        s_map_markers[i] = nullptr;
        s_map_marker_labels[i] = nullptr;
    }
    s_map_center_dot = nullptr;
    s_map_lbl_zoom = nullptr;
    s_sstv_lbl_file = nullptr;
    s_sstv_lbl_tx = nullptr;
    s_sstv_lbl_rx1 = nullptr;
    s_sstv_lbl_rx2 = nullptr;
    s_sstv_lbl_img = nullptr;
    s_sstv_mode_label = nullptr;
    s_sstv_src_label = nullptr;
    s_sstv_rx_toggle_label = nullptr;
    s_img_sstv = nullptr;
    s_lbl_provision_ip = nullptr;
    s_lbl_provision_ssid = nullptr;
    s_notice_panel = nullptr;
    s_lbl_notice = nullptr;
    s_bar_notice_progress = nullptr;
    s_shown_notice[0] = '\0';
    s_notice_sequence = 0u;
    s_notice_color = kColorUnset;
    s_dd_wifi = nullptr;
    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        s_wifi_option_ssids[i][0] = '\0';
    }
    s_ta_wifi_ssid = nullptr;
    s_ta_wifi_pass = nullptr;
    s_ta_callsign = nullptr;
    s_ta_callsign_ssid = nullptr;
    s_ta_server_host = nullptr;
    s_ta_server_port = nullptr;
    s_ta_mdc_unit_id = nullptr;
    s_ta_mdc_opcode = nullptr;
    s_ta_mdc_argument = nullptr;
    s_ta_dtmf_digits = nullptr;
    s_slider_speaker = nullptr;
    s_slider_mic = nullptr;
    s_lbl_speaker_value = nullptr;
    s_lbl_mic_value = nullptr;
    s_sw_aec = nullptr;
    s_sw_noise = nullptr;
    s_sw_mic_hpf = nullptr;
    s_sw_bt = nullptr;
    s_sw_wifi = nullptr;
    s_lbl_bt_status = nullptr;
    s_keyboard = nullptr;
    s_ta_ota_url = nullptr;
    s_lbl_ota_release = nullptr;
    s_btn_ota_check = nullptr;
    s_btn_ota_install = nullptr;
    s_ota_selected_index = 0u;
    s_ota_check_pending = false;
    s_ota_update_pending = false;
    s_ota_check_baseline_ms = 0u;
    s_shown_ota_release[0] = '\0';
    s_shown_ota_status[0] = '\0';
    s_ota_status_color = kColorUnset;
    s_ta_smb_server = nullptr;
    s_ta_smb_share = nullptr;
    s_ta_smb_user = nullptr;
    s_ta_smb_pass = nullptr;
    s_ta_beacon_path = nullptr;
    s_ta_beacon_interval = nullptr;
    s_ta_radio_url = nullptr;
    s_ta_radio_name = nullptr;
    s_dd_radio_fav = nullptr;
    s_dd_music_target = nullptr;
    s_sw_espnow = nullptr;
    s_lbl_espnow_status = nullptr;
    memset(s_shown_espnow_status, 0, sizeof(s_shown_espnow_status));
    s_lbl_ptt_codec = nullptr;
    s_lbl_eptt_codec = nullptr;
    s_shown_ptt_codec[0] = '\0';
    s_shown_eptt_codec[0] = '\0';
    s_btn_ptt = nullptr;
    s_btn_eptt = nullptr;
    s_ptt_shown_pressed = false;
    s_eptt_shown_pressed = false;
    s_lbl_cpu = nullptr;
    memset(s_shown_cpu, 0, sizeof(s_shown_cpu));
    s_clr_cpu = kColorUnset;
    s_lbl_music_title = nullptr;
    s_lbl_music_artist = nullptr;
    s_lbl_music_album = nullptr;
    s_lbl_music_state = nullptr;
    s_lbl_music_format = nullptr;
    s_shown_music_format[0] = '\0';
    s_btn_music_toggle_label = nullptr;
    s_btn_music_mode_label = nullptr;
    s_list_music = nullptr;
    s_img_music_cover = nullptr;
    s_lbl_music_icon = nullptr;
    s_shown_music_path[0] = '\0';
    s_shown_music_playing = false;
    // Video page teardown: park the decode worker before its target widgets
    // disappear (frames keep flowing in video_call; re-entering the page
    // restarts the worker and repaints from the newest frame).
    stopVideoView();
    s_img_video = nullptr;
    s_img_video_local = nullptr;
    s_lbl_video_status = nullptr;
    s_btn_video_tx_label = nullptr;
    s_lbl_ai_status = nullptr;
    s_btn_ai_talk_label = nullptr;
    s_btn_ai_talk = nullptr;
    s_sw_ai = nullptr;
    s_shown_ai_status[0] = '\0';
    s_sw_aprs = nullptr;
    s_lbl_aprs_status = nullptr;
    s_lbl_aprs_list = nullptr;
    s_shown_aprs_status[0] = '\0';
    s_ta_aprs_dest = nullptr;
    s_ta_aprs_text = nullptr;
    s_shown_aprs_list[0] = '\0';
}

void topBar(lv_obj_t *scr)
{
    constexpr int kTopLeft = 20;
    constexpr int kTopGap = 14;
    constexpr int kTopTimeW = 108;
    constexpr int kTopStationW = 140;
    constexpr int kTopVolW = 82;
    constexpr int kTopWifiW = 88;
    constexpr int kTopBtW = 40;
    constexpr int kTopCpuW = 96;
    // Order, left to right: local callsign-SSID, time, incoming caller, volume,
    // WiFi signal, Bluetooth, per-core CPU load.
    constexpr int kTopLocalX = kTopLeft;
    constexpr int kTopTimeX = kTopLocalX + kTopStationW + kTopGap;
    constexpr int kTopRemoteX = kTopTimeX + kTopTimeW + kTopGap;
    constexpr int kTopVolX = kTopRemoteX + kTopStationW + kTopGap;
    constexpr int kTopWifiX = kTopVolX + kTopVolW + kTopGap;
    constexpr int kTopBtX = kTopWifiX + kTopWifiW + 8;
    constexpr int kTopCpuX = kTopBtX + kTopBtW + 8;

    lv_obj_t *bar = panel(scr, 0, 0, kWidth, 56);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A111B), 0);

    s_lbl_time = label(bar, &lv_font_montserrat_20, kColorTime);
    lv_obj_set_width(s_lbl_time, kTopTimeW);
    lv_obj_align(s_lbl_time, LV_ALIGN_LEFT_MID, kTopTimeX, 0);
    lv_obj_set_style_text_align(s_lbl_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_time, tr("--:--:--"));

    s_lbl_local_station = label(bar, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_local_station, kTopStationW);
    lv_obj_align(s_lbl_local_station, LV_ALIGN_LEFT_MID, kTopLocalX, 0);
    lv_obj_set_style_text_align(s_lbl_local_station, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_local_station, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_local_station, kCallsignPlaceholder);

    s_lbl_vol = label(bar, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_vol, kTopVolW);
    lv_obj_align(s_lbl_vol, LV_ALIGN_LEFT_MID, kTopVolX, 0);
    lv_obj_set_style_text_align(s_lbl_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    // Music font (CJK fallback): when idle this badge doubles as the
    // now-playing ticker, which must render Chinese titles.
    s_lbl_remote_station = label(bar, &s_font_ui_20, kColorDim);
    lv_obj_set_width(s_lbl_remote_station, kTopStationW);
    lv_obj_align(s_lbl_remote_station, LV_ALIGN_LEFT_MID, kTopRemoteX, 0);
    lv_obj_set_style_text_align(s_lbl_remote_station, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_remote_station, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_remote_station, kCallsignPlaceholder);

    s_lbl_wifi = label(bar, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_wifi, kTopWifiW);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, kTopWifiX, 0);
    lv_obj_set_style_text_align(s_lbl_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI " --");

    // Bluetooth state lives next to WiFi: hidden when off, grey when on but
    // unlinked, green + headset glyph once a headset is linked. Updated in
    // refresh() (starts empty -- Bluetooth defaults off).
    s_lbl_bt_top = label(bar, &lv_font_montserrat_16, kColorDim);
    lv_obj_set_width(s_lbl_bt_top, kTopBtW);
    lv_obj_align(s_lbl_bt_top, LV_ALIGN_LEFT_MID, kTopBtX, 0);
    lv_obj_set_style_text_align(s_lbl_bt_top, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_bt_top, tr(""));

    // Per-core CPU load ("cpu0/cpu1"), updated by refreshCpu().
    s_lbl_cpu = label(bar, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_cpu, kTopCpuW);
    lv_obj_align(s_lbl_cpu, LV_ALIGN_LEFT_MID, kTopCpuX, 0);
    lv_obj_set_style_text_align(s_lbl_cpu, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_cpu, tr("--/--"));
}

void buildProvisioning()
{
    clearScreen();
    s_page = Page::Provisioning;
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = label(scr, &s_font_ui_20, kColorAccent);
    lv_obj_set_width(title, 752);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 24, 28);
    lv_label_set_text(title, "设备尚未配网 / WiFi Setup");

    lv_obj_t *box = panel(scr, 42, 80, 716, 270);
    lv_obj_t *state = label(box, &s_font_ui_20, kColorWarn);
    lv_obj_set_width(state, 688);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(state, "正在等待网络设置，可通过 WiFi 或蓝牙完成配网");

    lv_obj_t *wifi = label(box, &s_font_ui_20, kColorText);
    lv_obj_set_pos(wifi, 18, 56);
    lv_label_set_text(wifi, "方式 1：连接设备 WiFi 热点，浏览器访问");

    s_lbl_provision_ip = label(box, &lv_font_montserrat_20, kColorGood);
    lv_obj_set_pos(s_lbl_provision_ip, 470, 56);
    lv_label_set_text(s_lbl_provision_ip, "192.168.4.1");

    s_lbl_provision_ssid = label(box, &lv_font_montserrat_20, kColorGood);
    lv_obj_set_width(s_lbl_provision_ssid, 688);
    lv_obj_set_style_text_align(s_lbl_provision_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_provision_ssid, 0, 98);
    lv_label_set_text(s_lbl_provision_ssid, "NRL-ESP32-XXXXXX");

    lv_obj_t *wechat = label(box, &s_font_ui_20, kColorText);
    lv_obj_set_pos(wechat, 18, 142);
    lv_label_set_text(wechat, "方式 2：微信小程序「NRL互联」打开设置，使用蓝牙配网");

    lv_obj_t *direct = label(box, &s_font_ui_16, kColorSub);
    lv_obj_set_width(direct, 688);
    lv_obj_set_style_text_align(direct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(direct, 0, 194);
    lv_label_set_text(direct, "也可以直接点击下方按钮，在本机屏幕选择并设置 WiFi");

    button(scr, 210, 374, 380, 76, "屏幕设置 WiFi", Action::Wifi);
}

void refreshProvisioning()
{
    if (s_lbl_provision_ip == nullptr || s_lbl_provision_ssid == nullptr) {
        return;
    }
    char ssid[32] = {};
    WifiConfigPortal_GetApSsid(ssid, sizeof(ssid));
    lv_label_set_text(s_lbl_provision_ssid, ssid);
    char ip[16] = "192.168.4.1";
    const uint32_t ap_ip = nrlWifiApIp();
    if (ap_ip != 0u) {
        nrlIpToString(ap_ip, ip, sizeof(ip));
    }
    lv_label_set_text(s_lbl_provision_ip, ip);
}

void buildHome()
{
    clearScreen();
    s_page = Page::Home;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // Left: callsign / status, with the network IP underneath.
    lv_obj_t *left = panel(scr, 22, 78, 458, 270);
    s_lbl_caption = label(left, &lv_font_montserrat_20, kColorDim);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_caption, tr("STANDBY"));

    // Incoming-stream codec tag (OPUS / G.711), top-right opposite the
    // caption; only shown while an NRL voice stream is being received.
    s_lbl_rx_codec = label(left, &lv_font_montserrat_20, kColorAccent);
    lv_obj_align(s_lbl_rx_codec, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(s_lbl_rx_codec, "");

    // The active NRL2 voice packet's optional 24-bit DMR ID. Keep it on a
    // dedicated row immediately above the caller so it never replaces the
    // receive-state caption or the callsign.
    s_lbl_dmrid = label(left, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_dmrid, 430);
    lv_obj_align(s_lbl_dmrid, LV_ALIGN_LEFT_MID, 0, -58);
    lv_label_set_text(s_lbl_dmrid, "");

    // Incoming caller's callsign-SSID at the largest crisp built-in font (48px),
    // centred in the freed space so it reads as the focal element.
    s_lbl_callsign = label(left, &lv_font_montserrat_48, kColorText);
    lv_obj_set_width(s_lbl_callsign, 430);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_LEFT_MID, 0, -14);
    lv_label_set_text(s_lbl_callsign, kCallsignPlaceholder);

    // Last decoded signaling (CW/MDC/CTCSS/DTMF) shown below the callsign.
    s_lbl_signaling = label(left, &lv_font_montserrat_16, kColorAccent);
    lv_obj_set_width(s_lbl_signaling, 430);
    lv_obj_align(s_lbl_signaling, LV_ALIGN_LEFT_MID, 0, 30);
    lv_label_set_long_mode(s_lbl_signaling, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_lbl_signaling, "");

    // Bottom row of the main panel: local IP on the left, server IP on the right.
    s_lbl_ip = label(left, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(s_lbl_ip, 215);
    lv_obj_align(s_lbl_ip, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, LV_SYMBOL_WIFI " ---");

    s_lbl_server = label(left, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_server, 215);
    lv_obj_align(s_lbl_server, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_align(s_lbl_server, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_server, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_server, tr("---"));

    // Right: hold-to-talk PTT. With ESP-NOW enabled the area splits into two
    // stacked PTTs -- top keys the NRL network, bottom is the dedicated
    // ESP-NOW intercom PTT (purple when pressed to tell them apart).
    const bool espnow_split = ESPNOW_LINK_IsEnabled();
    s_home_espnow_split = espnow_split;
    auto make_ptt = [&scr](int y, int h, void (*cb)(lv_event_t *), uint32_t pressed_color) {
        lv_obj_t *ptt = lv_button_create(scr);
        lv_obj_set_pos(ptt, 502, y);
        lv_obj_set_size(ptt, 276, h);
        lv_obj_set_style_radius(ptt, 12, 0);
        lv_obj_set_style_bg_color(ptt, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_bg_color(ptt, lv_color_hex(pressed_color), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(ptt, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_border_width(ptt, 1, 0);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_PRESS_LOST, nullptr);
        return ptt;
    };
    // TX-codec tag in a PTT button's top-right corner; text is kept current by
    // refreshHome so codec changes from any config surface show up live.
    auto make_codec_tag = [](lv_obj_t *ptt, uint32_t color) {
        lv_obj_t *tag = label(ptt, &lv_font_montserrat_20, color);
        lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -6, 2);
        lv_label_set_text(tag, "");
        return tag;
    };
    if (!espnow_split) {
        lv_obj_t *ptt = make_ptt(78, 270, softPttEvent, 0x7A1F1F);
        lv_obj_t *ptt_lbl = label(ptt, &lv_font_montserrat_48, kColorText);
        lv_obj_center(ptt_lbl);
        lv_label_set_text(ptt_lbl, tr("PTT"));
        s_lbl_ptt_codec = make_codec_tag(ptt, kColorSub);
        s_btn_ptt = ptt;
    } else {
        lv_obj_t *ptt = make_ptt(78, 128, softPttEvent, 0x7A1F1F);
        lv_obj_t *ptt_lbl = label(ptt, &lv_font_montserrat_48, kColorText);
        lv_obj_center(ptt_lbl);
        lv_label_set_text(ptt_lbl, tr("PTT"));
        s_lbl_ptt_codec = make_codec_tag(ptt, kColorSub);
        s_btn_ptt = ptt;

        lv_obj_t *eptt = make_ptt(220, 128, espnowPttEvent, 0x4C1D95);
        lv_obj_t *eptt_lbl = label(eptt, &lv_font_montserrat_20, kColorDuplex);
        lv_obj_center(eptt_lbl);
        lv_obj_set_style_text_align(eptt_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(eptt_lbl, tr("ESP-NOW\nPTT"));
        s_lbl_eptt_codec = make_codec_tag(eptt, kColorDuplex);
        s_btn_eptt = eptt;
    }

    button(scr, 22, 372, 170, 78, "VOL-", Action::VolumeDown);
    button(scr, 208, 372, 184, 78, "Config", Action::Config);
    button(scr, 408, 372, 184, 78, "Apps", Action::Apps);
    button(scr, 608, 372, 170, 78, "VOL+", Action::VolumeUp);
}

void buildApps()
{
    clearScreen();
    s_page = Page::Apps;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    // Pure app launcher: 3x3 grid. Audio output routing lives on the
    // Config > Audio settings page, so nothing else crowds this grid.
    button(scr, 24, 84, 240, 84, "Music", Action::Music);
    button(scr, 272, 84, 240, 84, "Radio", Action::Radio);
    button(scr, 520, 84, 240, 84, "Video", Action::Video);
    button(scr, 24, 176, 240, 84, "AI", Action::Ai);
    button(scr, 272, 176, 240, 84, "Tetris", Action::Game);
    button(scr, 520, 176, 240, 84, "APRS", Action::Aprs);
    button(scr, 24, 268, 240, 84, "CW", Action::Cw);
    button(scr, 272, 268, 240, 84, "Map", Action::Map);
    button(scr, 520, 268, 240, 84, "SSTV", Action::Sstv);

    button(scr, 24, 372, 230, 76, "Back", Action::Home);
}

void setLabelTextIfChanged(lv_obj_t *label_obj, const char *text)
{
    if (label_obj == nullptr || text == nullptr) return;
    const char *shown = lv_label_get_text(label_obj);
    if (shown == nullptr || strcmp(shown, text) != 0) {
        lv_label_set_text(label_obj, text);
    }
}

void refreshCwPage()
{
    CwSnapshot cw{};
    CW_SERVICE_GetSnapshot(&cw);
    if (cw.revision == s_cw_revision || s_lbl_cw_rx == nullptr) return;
    s_cw_revision = cw.revision;
    char text[160];
    snprintf(text, sizeof(text), "%s", cw.rx_letters[0] != '\0' ? cw.rx_letters : "-");
    setLabelTextIfChanged(s_lbl_cw_rx, text);
    setLabelTextIfChanged(s_lbl_cw_rx_code, cw.rx_code[0] != '\0' ? cw.rx_code : "-");
    snprintf(text, sizeof(text), "%s%s%s%s", cw.tx_letters,
             cw.current_pattern[0] != '\0' ? "  [" : "",
             cw.current_pattern, cw.current_pattern[0] != '\0' ? "]" : "");
    setLabelTextIfChanged(s_lbl_cw_tx, text[0] != '\0' ? text : "-");
    snprintf(text, sizeof(text), "%s%s", cw.tx_code, cw.current_pattern);
    setLabelTextIfChanged(s_lbl_cw_tx_code, text[0] != '\0' ? text : "-");
    if (cw.practice_mode == CW_PRACTICE_RX) {
        char feedback[24];
        if (cw.copy_revealed != '\0') {
            snprintf(feedback, sizeof(feedback), "= %c  %s", cw.copy_revealed,
                     cw.copy_last_correct ? "correct" : "wrong");
        } else {
            snprintf(feedback, sizeof(feedback), "listening...");
        }
        snprintf(text, sizeof(text),
                 "RX-COPY level %u/36   %u WPM   accuracy %u%% (%u)   %s%s",
                 static_cast<unsigned>(cw.koch_unlocked), static_cast<unsigned>(cw.wpm),
                 static_cast<unsigned>(cw.accuracy_percent),
                 static_cast<unsigned>(cw.practice_attempts), feedback,
                 cw.sending ? "   SENDING" : "");
    } else if (cw.practice_mode == CW_PRACTICE_TX) {
        snprintf(text, sizeof(text), "TRAIN: key %c   %u WPM   accuracy %u%% (%u)   timing %u%%%s",
                 cw.practice_target, static_cast<unsigned>(cw.wpm),
                 static_cast<unsigned>(cw.accuracy_percent),
                 static_cast<unsigned>(cw.practice_attempts),
                 static_cast<unsigned>(cw.timing_percent), cw.sending ? "   SENDING" : "");
    } else {
        snprintf(text, sizeof(text), "%u WPM   accuracy %u%%%s",
                 static_cast<unsigned>(cw.wpm), static_cast<unsigned>(cw.accuracy_percent),
                 cw.sending ? "   SENDING" : "");
    }
    setLabelTextIfChanged(s_lbl_cw_metrics, text);
}

// Key zone events: straight-key mode measures press duration (tap=dit,
// hold=dah); paddle mode runs the keyer stream while a zone is held.
// PRESS_LOST counts as release so the sidetone can never stick on.
void cwZonePressed(lv_event_t *e)
{
    if (s_cw_paddle_mode) {
        CW_SERVICE_PaddleStart(static_cast<CwElement>(
            reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    } else {
        CW_SERVICE_KeyDown();
    }
}

void cwZoneReleased(lv_event_t *)
{
    if (s_cw_paddle_mode) {
        CW_SERVICE_PaddleStop();
    } else {
        CW_SERVICE_KeyUp();
    }
}

void buildCw()
{
    clearScreen();
    s_page = Page::Cw;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    lv_obj_t *rx_box = panel(scr, 24, 76, 752, 100);
    fieldLabel(rx_box, 0, 0, "RECEIVE / 接收");
    s_lbl_cw_rx = label(rx_box, &lv_font_montserrat_28, kColorText);
    lv_obj_set_pos(s_lbl_cw_rx, 0, 24);
    lv_obj_set_width(s_lbl_cw_rx, 720);
    lv_label_set_long_mode(s_lbl_cw_rx, LV_LABEL_LONG_CLIP);
    s_lbl_cw_rx_code = label(rx_box, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(s_lbl_cw_rx_code, 0, 62);
    lv_obj_set_width(s_lbl_cw_rx_code, 720);
    lv_label_set_long_mode(s_lbl_cw_rx_code, LV_LABEL_LONG_CLIP);

    lv_obj_t *tx_box = panel(scr, 24, 186, 752, 144);
    fieldLabel(tx_box, 0, 0, "TRANSMIT / 发射");
    s_lbl_cw_tx = label(tx_box, &lv_font_montserrat_28, kColorText);
    lv_obj_set_pos(s_lbl_cw_tx, 0, 24);
    lv_obj_set_width(s_lbl_cw_tx, 720);
    lv_label_set_long_mode(s_lbl_cw_tx, LV_LABEL_LONG_CLIP);
    s_lbl_cw_tx_code = label(tx_box, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(s_lbl_cw_tx_code, 0, 62);
    lv_obj_set_width(s_lbl_cw_tx_code, 720);
    lv_label_set_long_mode(s_lbl_cw_tx_code, LV_LABEL_LONG_CLIP);
    s_lbl_cw_metrics = label(tx_box, &lv_font_montserrat_16, kColorGood);
    lv_obj_set_pos(s_lbl_cw_metrics, 0, 100);
    lv_obj_set_width(s_lbl_cw_metrics, 720);

    button(scr, 14, 342, 100, 56, "Back", Action::Apps);
    button(scr, 122, 342, 100, 56, "Delete", Action::CwDelete);
    button(scr, 230, 342, 100, 56, "Send", Action::CwSend);
    CwSnapshot cw{};
    CW_SERVICE_GetSnapshot(&cw);
    button(scr, 338, 342, 110, 56,
           cw.practice_mode == CW_PRACTICE_TX ? "Train TX" :
           cw.practice_mode == CW_PRACTICE_RX ? "Train RX" : "Train",
           Action::CwPractice);
    button(scr, 456, 342, 120, 56, s_cw_paddle_mode ? "Mode:PDL" : "Mode:KEY",
           Action::CwMode);
    button(scr, 584, 342, 100, 56, "Replay", Action::CwReplay);
    button(scr, 692, 342, 94, 56, "Score", Action::CwScore);

    auto cw_zone = [scr](int x, int w, const char *text, CwElement element) {
        lv_obj_t *zone = lv_button_create(scr);
        lv_obj_set_pos(zone, x, 406);
        lv_obj_set_size(zone, w, 66);
        lv_obj_set_style_radius(zone, 8, 0);
        lv_obj_set_style_bg_color(zone, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_bg_color(zone, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(zone, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_border_width(zone, 1, 0);
        lv_obj_add_event_cb(zone, cwZonePressed, LV_EVENT_PRESSED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(element)));
        lv_obj_add_event_cb(zone, cwZoneReleased, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(zone, cwZoneReleased, LV_EVENT_PRESS_LOST, nullptr);
        lv_obj_t *txt = label(zone, &lv_font_montserrat_28, kColorText);
        lv_obj_center(txt);
        lv_label_set_text(txt, text);
    };
    if (s_cw_paddle_mode) {
        cw_zone(24, 370, ". DIT  (HOLD)", CW_ELEMENT_DIT);
        cw_zone(406, 370, "- DAH  (HOLD)", CW_ELEMENT_DAH);
    } else {
        cw_zone(24, 752, "KEY: TAP = DIT   HOLD = DAH", CW_ELEMENT_DIT);
    }
    refreshCwPage();
}

// Per-letter practice score grid: cell color encodes copy accuracy (green
// >=90%, amber >=70%, red below, dim = untried); charset and adaptive-speed
// toggles. Static page -- reopen to refresh.
void buildCwScore()
{
    clearScreen();
    s_page = Page::CwScore;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    static const char *const kSetNames[] = {"KOCH", "LETTERS", "DIGITS", "CUSTOM"};
    CwSnapshot cw{};
    CW_SERVICE_GetSnapshot(&cw);
    const uint8_t charset = cw.charset <= CW_CHARSET_CUSTOM ? cw.charset : 0u;

    CwLetterStat stats[36];
    const size_t count = CW_SERVICE_GetLetterStats(stats, 36u);
    uint32_t total_attempts = 0u;
    uint32_t total_correct = 0u;
    for (size_t i = 0u; i < count; ++i) {
        total_attempts += stats[i].attempts;
        total_correct += stats[i].correct;
    }

    lv_obj_t *summary = label(scr, &lv_font_montserrat_20, kColorText);
    lv_obj_set_pos(summary, 24, 56);
    char line[160];
    snprintf(line, sizeof(line),
             "CW SCORE  %s Lv%u/36   WPM %u%s   ACC %u%% (%u)   TIM %u%%",
             kSetNames[charset], static_cast<unsigned>(cw.koch_unlocked),
             static_cast<unsigned>(cw.wpm), cw.adaptive_wpm ? "(A)" : "",
             total_attempts > 0u ? static_cast<unsigned>(100u * total_correct / total_attempts)
                                 : 100u,
             static_cast<unsigned>(total_attempts),
             static_cast<unsigned>(cw.timing_percent));
    lv_label_set_text(summary, line);

    for (size_t i = 0u; i < count; ++i) {
        const int col = static_cast<int>(i % 9u);
        const int row = static_cast<int>(i / 9u);
        lv_obj_t *cell = lv_obj_create(scr);
        lv_obj_set_pos(cell, 24 + col * 84, 92 + row * 72);
        lv_obj_set_size(cell, 80, 64);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        uint32_t color = 0x20262E; // untried
        char acc_text[12] = "-";
        if (stats[i].attempts > 0u) {
            const unsigned acc = (100u * stats[i].correct) / stats[i].attempts;
            color = acc >= 90u ? 0x1E6B34 : acc >= 70u ? 0x8A6D1A : 0x7A2A2A;
            snprintf(acc_text, sizeof(acc_text), "%u%%", acc);
        }
        lv_obj_set_style_bg_color(cell, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_t *letter = label(cell, &lv_font_montserrat_28, kColorText);
        lv_obj_align(letter, LV_ALIGN_TOP_MID, 0, 2);
        char letter_text[2] = {stats[i].letter, '\0'};
        lv_label_set_text(letter, letter_text);
        lv_obj_t *acc = label(cell, &lv_font_montserrat_16, kColorSub);
        lv_obj_align(acc, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_label_set_text(acc, acc_text);
    }

    char set_label[24];
    char adapt_label[24];
    snprintf(set_label, sizeof(set_label), "Set: %s", kSetNames[charset]);
    snprintf(adapt_label, sizeof(adapt_label), "Adaptive: %s", cw.adaptive_wpm ? "ON" : "OFF");
    button(scr, 24, 406, 150, 56, set_label, Action::CwScoreSet);
    button(scr, 186, 406, 170, 56, adapt_label, Action::CwScoreAdapt);
    button(scr, 652, 406, 124, 56, "Back", Action::Cw);
}

void buildConfig()
{
    clearScreen();
    s_page = Page::Config;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    // Keep every settings entry visible in a uniform 4 x 3 grid. The compact
    // 80 px rows leave the fixed bottom bar exclusively for Back and Language.
    button(scr, 24, 84, 180, 80, "WiFi", Action::Wifi);
    button(scr, 214, 84, 180, 80, "NRL", Action::Station);
    button(scr, 404, 84, 180, 80, "Audio", Action::Audio);
    button(scr, 594, 84, 180, 80, "BT", Action::Bt);

    button(scr, 24, 174, 180, 80, "CTCSS", Action::Ctcss);
    button(scr, 214, 174, 180, 80, "MDC", Action::Mdc);
    button(scr, 404, 174, 180, 80, "DTMF", Action::Dtmf);
    button(scr, 594, 174, 180, 80, "ESP-NOW", Action::EspNow);

    button(scr, 24, 264, 180, 80, "Nanny", Action::Nanny);
    button(scr, 214, 264, 180, 80, "NAS", Action::Smb);
    button(scr, 404, 264, 180, 80, "About", Action::About);
    button(scr, 24, 372, 230, 76, "Back", Action::Home);

    // Language selector: switching persists and rebuilds this page in place.
    fieldLabel(scr, 470, 388, "Language");
    lv_obj_t *dd_lang = styledDropdown(scr, 574, 378, 200);
    lv_dropdown_set_options(dd_lang, "English\n中文");
    lv_dropdown_set_selected(dd_lang, static_cast<uint32_t>(s_lang));
    lv_obj_add_event_cb(dd_lang, languageEvent, LV_EVENT_VALUE_CHANGED, nullptr);
}

void buildAbout()
{
    clearScreen();
    s_page = Page::About;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    const NrlOtaStatus *ota = otaUiSnapshot();

    lv_obj_t *box = panel(scr, 24, 82, 750, 270);
    fieldLabel(box, 0, 0, "Firmware Version");
    lv_obj_t *version = label(box, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(version, 180, -4);
    lv_obj_set_width(version, 520);
    lv_label_set_long_mode(version, LV_LABEL_LONG_DOT);
    lv_label_set_text(version, NRL_FIRMWARE_BANNER);

    fieldLabel(box, 0, 38, "OTA Server URL");
    s_ta_ota_url = textArea(box, 0, 60, 710, "http://ota.example.com",
                            ota != nullptr ? ota->server_url : "",
                            NRL_OTA_URL_MAX - 1u, false, nullptr, false);

    fieldLabel(box, 0, 120, "Available Version");
    button(box, 160, 112, 112, 46, "Older", Action::OtaOlder);
    s_lbl_ota_release = label(box, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(s_lbl_ota_release, 284, 123);
    lv_obj_set_width(s_lbl_ota_release, 282);
    lv_obj_set_style_text_align(s_lbl_ota_release, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_ota_release, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ota_release, tr("No versions available"));
    button(box, 578, 112, 112, 46, "Newer", Action::OtaNewer);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 0, 178);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_label_set_long_mode(s_lbl_form_status, LV_LABEL_LONG_WRAP);

    button(scr, 24, 372, 150, 76, "Back", Action::Config);
    button(scr, 190, 372, 170, 76, "Save URL", Action::SaveOtaUrl);
    s_btn_ota_check = button(scr, 376, 372, 180, 76, "Check Update", Action::CheckOta);
    s_btn_ota_install = button(scr, 572, 372, 202, 76, "Install", Action::InstallOta);
    createKeyboard(scr);
    refreshOtaPage();
}

void sanitizeOptionText(const char *src, char *dst, size_t dst_size)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; ++i) {
        const char ch = src[i];
        dst[out++] = (ch == '\r' || ch == '\n') ? ' ' : ch;
    }
    dst[out] = '\0';
}

void setWifiDropdownOptionsFromCache()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    NrlWifiScanResult results[16] = {};
    const size_t got = nrlWifiScanGetCache(results, sizeof(results) / sizeof(results[0]));

    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        s_wifi_option_ssids[i][0] = '\0';
    }

    size_t used = 0;
    if (cfg != nullptr && cfg->wifi_ssid[0] != '\0') {
        sanitizeOptionText(cfg->wifi_ssid, s_wifi_option_ssids[used], sizeof(s_wifi_option_ssids[used]));
        ++used;
    }

    for (size_t i = 0; i < got && used < kWifiOptionCount; ++i) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < used; ++j) {
            if (strncmp(results[i].ssid, s_wifi_option_ssids[j], sizeof(results[i].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        sanitizeOptionText(results[i].ssid, s_wifi_option_ssids[used], sizeof(s_wifi_option_ssids[used]));
        ++used;
    }

    if (s_dd_wifi != nullptr) {
        // Build a newline-separated option list for the dropdown (scrollable, so
        // it can show every scanned network, not just the first few).
        char options[kWifiOptionCount * 34] = {};
        size_t pos = 0;
        for (size_t i = 0; i < used; ++i) {
            if (i > 0 && pos < sizeof(options) - 1) {
                options[pos++] = '\n';
            }
            const int n = snprintf(options + pos, sizeof(options) - pos, "%s",
                                   s_wifi_option_ssids[i]);
            if (n > 0) {
                pos += static_cast<size_t>(n);
            }
        }
        if (used == 0) {
            snprintf(options, sizeof(options), "%s", "(no networks)");
        }
        lv_dropdown_set_options(s_dd_wifi, options);
    }
    ESP_LOGI(TAG, "WiFi dropdown updated: scanned=%u shown=%u",
             static_cast<unsigned>(got), static_cast<unsigned>(used));
}

void buildWifi()
{
    clearScreen();
    s_page = Page::Wifi;
    lv_obj_t *scr = lv_screen_active();
    if (!s_provisioning_mode) {
        topBar(scr);
    }
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 6, "Nearby WiFi");

    // Master Wi-Fi radio switch (moved here from the BT page). Off frees the
    // shared radio for Bluetooth A2DP music, at the cost of the network
    // radio-voice link. Defaults ON. Sits on the header row; the dropdown and
    // the rows below are shifted down so nothing overlaps the 34 px switch.
    fieldLabel(box, 500, 6, "Wi-Fi radio");
    s_sw_wifi = lv_switch_create(box);
    lv_obj_set_pos(s_sw_wifi, 646, 0);
    lv_obj_set_size(s_sw_wifi, 64, 34);
    {
        const ExternalRadioConfig *wcfg = EXTERNAL_RADIO_GetConfig();
        if (wcfg == nullptr || wcfg->wifi_enabled) {
            lv_obj_add_state(s_sw_wifi, LV_STATE_CHECKED);
        }
    }
    lv_obj_set_style_bg_color(s_sw_wifi, lv_color_hex(kColorDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sw_wifi, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sw_wifi, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sw_wifi, wifiEnableEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    // Scrollable dropdown lists every scanned network; picking one fills the SSID
    // field below.
    s_dd_wifi = lv_dropdown_create(box);
    lv_obj_set_pos(s_dd_wifi, 0, 44);
    lv_obj_set_size(s_dd_wifi, 710, 42);
    lv_obj_set_style_radius(s_dd_wifi, 6, 0);
    lv_obj_set_style_bg_color(s_dd_wifi, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(s_dd_wifi, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_dd_wifi, 1, 0);
    lv_obj_set_style_text_color(s_dd_wifi, lv_color_hex(kColorText), 0);
    lv_dropdown_set_text(s_dd_wifi, "Select WiFi...");
    lv_dropdown_set_options(s_dd_wifi, tr("(scanning)"));
    lv_obj_add_event_cb(s_dd_wifi, wifiOptionEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    // Style the popped-up option list so it is readable on the dark theme.
    lv_obj_t *dd_list = lv_dropdown_get_list(s_dd_wifi);
    if (dd_list != nullptr) {
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_text_color(dd_list, lv_color_hex(kColorText), 0);
        lv_obj_set_style_border_color(dd_list, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_max_height(dd_list, 220, 0);
    }
    setWifiDropdownOptionsFromCache();

    fieldLabel(box, 0, 82, "WiFi SSID");
    s_ta_wifi_ssid = textArea(box, 0, 106, 340, "SSID", cfg ? cfg->wifi_ssid : "", 32, false, nullptr, false);
    fieldLabel(box, 370, 82, "Password");
    s_ta_wifi_pass = textArea(box, 370, 106, 340, "Password", cfg ? cfg->wifi_password : "", 64, true, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    lv_label_set_text(s_lbl_form_status, tr("Scan, select or type SSID, then save."));

    button(scr, 24, 372, 230, 76, "Back",
           s_provisioning_mode ? Action::Provisioning : Action::Config);
    button(scr, 278, 372, 146, 76, "Scan", Action::ScanWifi);
    button(scr, 442, 372, 146, 76, "Save", Action::SaveWifi);
    button(scr, 606, 372, 168, 76, "Reset", Action::ResetWifi);
    createKeyboard(scr);
}

void buildStation()
{
    clearScreen();
    s_page = Page::Station;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 0, "Callsign");
    s_ta_callsign = textArea(box, 0, 24, 250, "CALL", cfg ? cfg->callsign : "", 6, false,
                             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", false);
    fieldLabel(box, 280, 0, "SSID");
    char ssid[8] = {};
    snprintf(ssid, sizeof(ssid), "%u", cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0);
    s_ta_callsign_ssid = textArea(box, 280, 24, 110, "0", ssid, 3, false, "0123456789", true);

    // NRL TX voice codec lives with the other NRL settings; applies + persists
    // immediately (RX auto-detects, no Save round-trip needed). On the SSID row
    // where there is room -- after the Port field it gets clipped.
    switchControl(box, 470, 24, "Opus", NRLAudioBridge_GetVoiceCodec() == 1u,
                  AudioControl::OpusCodec);

    fieldLabel(box, 0, 82, "Server Host / IP");
    s_ta_server_host = textArea(box, 0, 106, 430, "server host", cfg ? cfg->server_host : "", 64, false,
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_", false);
    fieldLabel(box, 460, 82, "Port");
    char port[8] = {};
    snprintf(port, sizeof(port), "%u", cfg ? static_cast<unsigned>(cfg->server_port) : 0);
    s_ta_server_port = textArea(box, 460, 106, 120, "0", port, 5, false, "0123456789", true);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 176);
    lv_label_set_text(s_lbl_form_status, tr("Edit station and server settings, then save."));

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveStation);
    createKeyboard(scr);
}

void buildAudio()
{
    clearScreen();
    s_page = Page::Audio;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    // Compact vertical rhythm so the playback-target selectors moved in from
    // the Apps page fit without wrapping: sliders at 62 px pitch, switches,
    // then two dropdowns side by side, status line at the panel bottom.
    lv_obj_t *box = panel(scr, 24, 86, 750, 272);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // When a headset is online the Speaker slider tracks/controls its volume;
    // otherwise it controls the onboard codec line-out.
    const int bt_pct = NRL_BtHfp_IsConnected() ? NRL_BtHfp_GetVolumePercent() : -1;
    const int speaker_pct = (bt_pct >= 0)
                                ? bt_pct
                                : (cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0);

    fieldLabel(box, 0, 0, (bt_pct >= 0) ? "Speaker (headset)" : "Speaker");
    s_lbl_speaker_value = valueLabel(box, 620, 0, 90);
    s_slider_speaker = slider(box, 0, 30, 710, 0, 100, speaker_pct, AudioControl::Speaker);

    fieldLabel(box, 0, 62, "Mic");
    s_lbl_mic_value = valueLabel(box, 620, 62, 90);
    s_slider_mic = slider(box, 0, 92, 710, 0, 255, cfg ? cfg->mic_volume : 0, AudioControl::Mic);

    s_sw_aec = switchControl(box, 0, 132, "AEC", cfg && cfg->aec_enabled, AudioControl::Aec);
    s_sw_noise = switchControl(box, 190, 132, "AI Noise", cfg && cfg->ai_noise_enabled, AudioControl::Noise);
    s_sw_mic_hpf = switchControl(box, 380, 132, "Mic HPF", cfg && cfg->mic_hpf_enabled, AudioControl::MicHpf);
    // The NRL TX codec switch lives on the NRL (Station) page; the ESP-NOW
    // codec switch on the ESP-NOW page.

    // Shared playback target: one setting for everything the music player
    // outputs (music / nanny beacon / net radio) plus the output device.
    fieldLabel(box, 0, 176, "Playback Target (music / beacon / radio)");
    s_dd_music_target = styledDropdown(box, 0, 198, 340);
    lv_dropdown_set_options(s_dd_music_target, tr("Local speaker\nNRL network\nLocal + network"));
    const int target = MUSIC_GetTarget();
    lv_dropdown_set_selected(s_dd_music_target,
                             (target >= MUSIC_TARGET_LOCAL && target <= MUSIC_TARGET_BOTH)
                                 ? static_cast<uint32_t>(target)
                                 : 0u);
    lv_obj_add_event_cb(s_dd_music_target, musicTargetEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    fieldLabel(box, 360, 176, "Music Output Device");
    lv_obj_t *dd_output = styledDropdown(box, 360, 198, 350);
    lv_dropdown_set_options(dd_output, tr("Onboard speaker\nBT headset (A2DP)"));
    lv_dropdown_set_selected(dd_output,
                             (MUSIC_GetOutput() == MUSIC_OUTPUT_BT) ? 1u : 0u);
    lv_obj_add_event_cb(dd_output, musicOutputEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 248);
    lv_label_set_text(s_lbl_form_status,
                      tr("Adjust audio, then save. Playback applies from the next track."));
    updateAudioValueLabels();

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Reset", Action::ResetAudio);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveAudio);
}

// Dark-theme dropdown matching the WiFi page's (shared styling for the
// playback-target selector).
lv_obj_t *styledDropdown(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 42);
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), 0);
    // CJK-capable font on the selected-value box AND the popup list, so
    // translated options render instead of tofu (乱码).
    lv_obj_set_style_text_font(dd, &s_font_ui_20, 0);
    lv_obj_t *dd_list = lv_dropdown_get_list(dd);
    if (dd_list != nullptr) {
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_text_color(dd_list, lv_color_hex(kColorText), 0);
        lv_obj_set_style_text_font(dd_list, &s_font_ui_20, 0);
        lv_obj_set_style_border_color(dd_list, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_max_height(dd_list, 220, 0);
    }
    return dd;
}

void buildSmb()
{
    clearScreen();
    s_page = Page::Smb;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char server[64] = {};
    char share[64] = {};
    char user[32] = {};
    char pass[64] = {};
    (void)STORAGE_SmbGetConfig(server, sizeof(server), share, sizeof(share),
                               user, sizeof(user), pass, sizeof(pass));

    fieldLabel(box, 0, 0, "Server (NAS / PC)");
    s_ta_smb_server = textArea(box, 0, 24, 340, "192.168.1.10", server, 63, false, nullptr, false);
    fieldLabel(box, 370, 0, "Share Name");
    s_ta_smb_share = textArea(box, 370, 24, 340, "music", share, 63, false, nullptr, false);
    fieldLabel(box, 0, 82, "Username (empty = guest)");
    s_ta_smb_user = textArea(box, 0, 106, 340, "guest", user, 31, false, nullptr, false);
    fieldLabel(box, 370, 82, "Password");
    s_ta_smb_pass = textArea(box, 370, 106, 340, "Password", pass, 63, true, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    char status[96] = {};
    STORAGE_SmbDescribe(status, sizeof(status));
    char text[128];
    snprintf(text, sizeof(text), "SMB: %s", status);
    lv_label_set_text(s_lbl_form_status, text);

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Clear", Action::ClearSmb);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveSmb);
    createKeyboard(scr);
}

void buildNanny()
{
    clearScreen();
    s_page = Page::Nanny;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char path[128] = {};
    uint32_t interval = 0;
    const bool armed = NANNY_GetBeacon(path, sizeof(path), &interval);

    fieldLabel(box, 0, 0, "Beacon File Path");
    s_ta_beacon_path = textArea(box, 0, 24, 460, "/sdcard/beacon/id.wav", path, 127, false, nullptr, false);
    fieldLabel(box, 490, 0, "Interval (min)");
    char interval_text[8] = {};
    if (armed) {
        snprintf(interval_text, sizeof(interval_text), "%lu", static_cast<unsigned long>(interval));
    }
    s_ta_beacon_interval = textArea(box, 490, 24, 140, "30", interval_text, 4, false, "0123456789", true);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 100);
    lv_label_set_text(s_lbl_form_status,
                      armed ? "Beacon armed. Save applies changes; Beacon Off disarms."
                            : "Beacon off. Set file path + minutes, then Save.");

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Beacon Off", Action::BeaconOff);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveNanny);
    createKeyboard(scr);
}

// Copy favorite `index` into the name/URL fields (no retune).
void loadRadioFavForm(const size_t index)
{
    char name[RADIO_FAV_NAME_SIZE] = {};
    char url[RADIO_FAV_URL_SIZE] = {};
    if (!RADIO_FAV_Get(index, name, sizeof(name), url, sizeof(url))) {
        return;
    }
    if (s_ta_radio_name != nullptr) {
        lv_textarea_set_text(s_ta_radio_name, name);
    }
    if (s_ta_radio_url != nullptr) {
        lv_textarea_set_text(s_ta_radio_url, url);
    }
}

void radioFavSelectEvent(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target_obj(event);
    loadRadioFavForm(lv_dropdown_get_selected(dd));
}

void buildRadio()
{
    clearScreen();
    s_page = Page::Radio;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char url[256] = {};
    MUSIC_GetRadioUrl(url, sizeof(url));
    const int fav_match = RADIO_FAV_IndexOfUrl(url);
    char name[RADIO_FAV_NAME_SIZE] = {};
    if (fav_match >= 0) {
        (void)RADIO_FAV_Get(static_cast<size_t>(fav_match), name, sizeof(name), nullptr, 0);
    }

    // Favorite stations: pick from the dropdown to load a station into the
    // fields, < / > retune directly, +Fav stores the fields, Del removes the
    // selected entry. The same list is served to the web portal and AT.
    fieldLabel(box, 0, 0, "Favorite Stations");
    s_dd_radio_fav = styledDropdown(box, 0, 24, 380);
    const size_t fav_count = RADIO_FAV_Count();
    if (fav_count == 0u) {
        lv_dropdown_set_options(s_dd_radio_fav, tr("(no favorites)"));
    } else {
        char options[RADIO_FAV_MAX * RADIO_FAV_NAME_SIZE + 8] = {};
        size_t used = 0;
        for (size_t i = 0; i < fav_count; ++i) {
            char fav_name[RADIO_FAV_NAME_SIZE] = {};
            (void)RADIO_FAV_Get(i, fav_name, sizeof(fav_name), nullptr, 0);
            char clean[RADIO_FAV_NAME_SIZE];
            sanitizeOptionText(fav_name, clean, sizeof(clean));
            const int written = snprintf(options + used, sizeof(options) - used,
                                         "%s%s", (i > 0u) ? "\n" : "", clean);
            if (written <= 0 || used + static_cast<size_t>(written) >= sizeof(options)) {
                break;
            }
            used += static_cast<size_t>(written);
        }
        lv_dropdown_set_options(s_dd_radio_fav, options);
        lv_obj_add_event_cb(s_dd_radio_fav, radioFavSelectEvent, LV_EVENT_VALUE_CHANGED, nullptr);
        const int current = (fav_match >= 0) ? fav_match : RADIO_FAV_CurrentIndex();
        if (current >= 0 && static_cast<size_t>(current) < fav_count) {
            lv_dropdown_set_selected(s_dd_radio_fav, static_cast<uint32_t>(current));
        }
    }
    button(box, 396, 24, 82, 42, LV_SYMBOL_PREV, Action::RadioFavPrev);
    button(box, 488, 24, 82, 42, LV_SYMBOL_NEXT, Action::RadioFavNext);
    button(box, 580, 24, 66, 42, "+", Action::RadioFavAdd);
    button(box, 656, 24, 66, 42, LV_SYMBOL_TRASH, Action::RadioFavDel);

    fieldLabel(box, 0, 80, "Station Name");
    s_ta_radio_name = textArea(box, 0, 104, 280, "My station", name,
                               RADIO_FAV_NAME_SIZE - 1, false, nullptr, false);
    lv_obj_set_style_text_font(s_ta_radio_name, &s_font_ui_20, 0);
    fieldLabel(box, 310, 80, "Stream URL (http:// or https://)");
    s_ta_radio_url = textArea(box, 310, 104, 412, "http://...", url,
                              RADIO_FAV_URL_SIZE - 1, false, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 164);
    const char *playing_path = MUSIC_CurrentPath();
    if (MUSIC_IsPlaying() && strncmp(playing_path, "http", 4) == 0) {
        char text[192];
        snprintf(text, sizeof(text), tr("Playing: %s"), playing_path);
        lv_label_set_text(s_lbl_form_status, text);
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorGood), 0);
    } else {
        lv_label_set_text(s_lbl_form_status,
                          tr("Pick a favorite or type a URL, then tap Play. + saves to favorites."));
    }

    button(scr, 24, 372, 190, 76, "Back", Action::Apps);
    button(scr, 230, 372, 170, 76, "Save", Action::SaveRadio);
    button(scr, 416, 372, 170, 76, LV_SYMBOL_PLAY, Action::RadioPlay);
    button(scr, 602, 372, 172, 76, LV_SYMBOL_STOP, Action::RadioStop);
    createKeyboard(scr);
}

void refreshEspNowPage()
{
    if (s_lbl_espnow_status == nullptr) {
        return;
    }
    char peer[16] = {};
    ESPNOW_LINK_GetLastPeer(peer, sizeof(peer));
    char text[128];
    snprintf(text, sizeof(text), tr("State: %s\nLast peer heard: %s%s"),
             ESPNOW_LINK_IsEnabled() ? "ON" : "OFF",
             peer[0] != '\0' ? peer : "(none)",
             ESPNOW_LINK_IsReceiving() ? "  " LV_SYMBOL_VOLUME_MAX : "");
    if (setLabel(s_lbl_espnow_status, s_shown_espnow_status,
                 sizeof(s_shown_espnow_status), text)) {
        lv_obj_set_style_text_color(s_lbl_espnow_status,
                                    lv_color_hex(ESPNOW_LINK_IsReceiving() ? kColorDuplex : kColorText), 0);
    }
}

void buildEspNow()
{
    clearScreen();
    s_page = Page::EspNow;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    s_sw_espnow = switchControl(box, 0, 8, "ESP-NOW Intercom",
                                ESPNOW_LINK_IsEnabled(), AudioControl::EspNow);
    // Independent receive switch (default ON): hear intercom voice even while
    // TX stays off.
    (void)switchControl(box, 300, 8, "RX", ESPNOW_LINK_IsRxEnabled(),
                        AudioControl::EspNowRx);
    // ESP-NOW TX codec, independent of the NRL codec on the Station page.
    (void)switchControl(box, 470, 8, "Opus", ESPNOW_LINK_GetTxCodec() == 1u,
                        AudioControl::EspNowOpus);

    s_lbl_espnow_status = label(box, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_espnow_status, 710);
    lv_obj_set_pos(s_lbl_espnow_status, 0, 70);
    refreshEspNowPage();

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    lv_label_set_text(s_lbl_form_status,
                      "Off-grid intercom with nearby devices. When on, the home screen "
                      "gains a dedicated ESP-NOW PTT below the network PTT.");

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
}

void buildCtcss()
{
    clearScreen();
    s_page = Page::Ctcss;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 78, 750, 274);
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);

    switchControl(box, 0, 4, "CTCSS MIC RX", cfg.ctcss_rx_mic, AudioControl::CtcssRxMic);
    switchControl(box, 370, 4, "CTCSS NRL RX", cfg.ctcss_rx_nrl, AudioControl::CtcssRxNrl);
    fieldLabel(box, 0, 78, "Detected standard PL tone");
    char result[96] = {};
    SIGNALING_GetLastResult(result, sizeof(result));
    s_lbl_form_status = label(box, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 0, 112);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_label_set_text(s_lbl_form_status,
                      strncmp(result, "CTCSS ", 6u) == 0 ? result : "No CTCSS tone detected yet.");
    button(scr, 24, 372, 230, 76, "Back", Action::Config);
}

void buildMdc()
{
    clearScreen();
    s_page = Page::Mdc;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 78, 750, 274);
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);

    switchControl(box, 0, 4, "MDC MIC RX", cfg.mdc_rx_mic, AudioControl::MdcRxMic);
    switchControl(box, 370, 4, "MDC NRL RX", cfg.mdc_rx_nrl, AudioControl::MdcRxNrl);
    switchControl(box, 0, 58, "MDC NRL TX", cfg.mdc_tx_nrl, AudioControl::MdcTxNrl);
    switchControl(box, 370, 58, "MDC SPK TX", cfg.mdc_tx_speaker, AudioControl::MdcTxSpeaker);

    char unit_id[5] = {};
    char opcode[3] = {};
    char argument[3] = {};
    snprintf(unit_id, sizeof(unit_id), "%04X", cfg.mdc_unit_id);
    snprintf(opcode, sizeof(opcode), "%02X", cfg.mdc_opcode);
    snprintf(argument, sizeof(argument), "%02X", cfg.mdc_argument);
    fieldLabel(box, 0, 116, "Unit ID (HEX)");
    s_ta_mdc_unit_id = textArea(box, 0, 140, 190, "0001", unit_id, 4, false,
                                "0123456789ABCDEFabcdef", false);
    fieldLabel(box, 220, 116, "Opcode (HEX)");
    s_ta_mdc_opcode = textArea(box, 220, 140, 150, "01", opcode, 2, false,
                               "0123456789ABCDEFabcdef", false);
    fieldLabel(box, 400, 116, "Argument (HEX)");
    s_ta_mdc_argument = textArea(box, 400, 140, 150, "00", argument, 2, false,
                                 "0123456789ABCDEFabcdef", false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 0, 204);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_label_set_text(s_lbl_form_status, "Edit MDC packet fields, then save.");
    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveMdc);
    createKeyboard(scr);
}

void buildDtmf()
{
    clearScreen();
    s_page = Page::Dtmf;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 78, 750, 274);
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);

    switchControl(box, 0, 4, "DTMF MIC RX", cfg.dtmf_rx_mic, AudioControl::DtmfRxMic);
    switchControl(box, 370, 4, "DTMF NRL RX", cfg.dtmf_rx_nrl, AudioControl::DtmfRxNrl);
    switchControl(box, 0, 58, "DTMF NRL TX", cfg.dtmf_tx_nrl, AudioControl::DtmfTxNrl);
    switchControl(box, 370, 58, "DTMF SPK TX", cfg.dtmf_tx_speaker, AudioControl::DtmfTxSpeaker);

    fieldLabel(box, 0, 116, "DTMF ID (1-16 digits)");
    s_ta_dtmf_digits = textArea(box, 0, 140, 550, "123#", cfg.dtmf_digits, 16, false,
                                "0123456789ABCDabcd*#", false);
    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 0, 204);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_label_set_text(s_lbl_form_status, "Edit the DTMF ID, then save.");
    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveDtmf);
    createKeyboard(scr);
}

const char *musicBasename(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr) ? slash + 1 : path;
}

void populateMusicList();
void showMusicListStatus(const char *text, uint32_t color);

// True when the display session's tables were rebuilt after the visible list
// was populated: row indices no longer match the screen, so repaint instead
// of acting on a stale index.
bool musicListStale()
{
    if (PLAYLIST_ClientScanRevision(PLAYLIST_CLIENT_DISPLAY) == s_music_list_rev) {
        return false;
    }
    populateMusicList();
    s_last_refresh_ms = 0;
    return true;
}

void musicListEvent(lv_event_t *event)
{
    if (musicListStale()) {
        return;
    }
    const size_t index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    (void)PLAYLIST_ClientPlayIndex(PLAYLIST_CLIENT_DISPLAY, index);
    s_last_refresh_ms = 0;
}

void musicFavoriteEvent(lv_event_t *event)
{
    if (musicListStale()) {
        return;
    }
    const size_t index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const char *path = PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_DISPLAY, index);
    if (PLAYLIST_ToggleFavorite(path)) {
        populateMusicList();
    }
    s_last_refresh_ms = 0;
}

void musicDirEvent(lv_event_t *event)
{
    if (musicListStale()) {
        return;
    }
    const size_t index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    // Async only: a synchronous scan on the UI task inside this LVGL event,
    // and an SMB opendir on a dead connection froze lv_timer_handler for the
    // whole 5 s directory-open timeout.
    if (PLAYLIST_ClientEnterDirAsync(PLAYLIST_CLIENT_DISPLAY, index)) {
        s_music_dir_scan_pending = true;
        showMusicListStatus("Scanning...", kColorWarn);
    }
    s_last_refresh_ms = 0;
}

void musicUpEvent(lv_event_t *)
{
    if (PLAYLIST_ClientUpAsync(PLAYLIST_CLIENT_DISPLAY)) {
        s_music_dir_scan_pending = true;
        showMusicListStatus("Scanning...", kColorWarn);
    }
    s_last_refresh_ms = 0;
}

void musicPageEvent(lv_event_t *event)
{
    const int delta = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int next = static_cast<int>(s_music_page) + delta;
    s_music_page = (next > 0) ? static_cast<size_t>(next) : 0u;
    populateMusicList();
    s_last_refresh_ms = 0;
}

void populateMusicList()
{
    if (s_list_music == nullptr) {
        return;
    }
    s_music_list_rev = PLAYLIST_ClientScanRevision(PLAYLIST_CLIENT_DISPLAY);
    if (s_music_page_rev != s_music_list_rev) {
        // New listing (directory change, rescan, external scan): back to page 1.
        s_music_page_rev = s_music_list_rev;
        s_music_page = 0u;
    }
    lv_obj_clean(s_list_music);

    const bool at_root = PLAYLIST_ClientAtRoot(PLAYLIST_CLIENT_DISPLAY);
    size_t rows_used = 0;
    if (!at_root) {
        lv_obj_t *up = lv_list_add_button(s_list_music, LV_SYMBOL_LEFT, "..");
        lv_obj_set_style_bg_color(up, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_text_color(up, lv_color_hex(kColorSub), 0);
        lv_obj_add_event_cb(up, musicUpEvent, LV_EVENT_CLICKED, nullptr);
        ++rows_used;
    }

    const size_t dir_count = PLAYLIST_ClientDirCount(PLAYLIST_CLIENT_DISPLAY);
    const size_t count = PLAYLIST_ClientCount(PLAYLIST_CLIENT_DISPLAY);
    if (count == 0u && dir_count == 0u) {
        // Render the hint in place (the ".." row above stays usable); a
        // full-list status wipe here stranded users inside empty directories.
        const char *msg = !PLAYLIST_ClientLastScanOk(PLAYLIST_CLIENT_DISPLAY)
                              ? "Directory unavailable (connection lost)."
                              : (at_root ? "No storage mounted."
                                         : "No tracks or subdirectories.");
        lv_obj_t *empty = lv_list_add_text(s_list_music, tr(msg));
        lv_obj_set_style_text_color(empty, lv_color_hex(kColorSub), 0);
        return;
    }
    // Radio contention: SMB (network TCP bulk) can't stream while Bluetooth holds
    // the single shared radio, so hide network tracks while BT is on -- only local
    // SD/USB are playable then. (Turn Bluetooth off to play SMB music.)
    const bool bt_on = NRL_BtHfp_IsEnabled();
    const char *smb_mp = STORAGE_SmbMountPoint();
    const size_t smb_len = (smb_mp != nullptr) ? strlen(smb_mp) : 0u;

    // BT-hidden tracks are excluded from paging entirely.
    size_t visible_tracks = 0u;
    for (size_t i = 0; i < count; ++i) {
        const char *path = PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_DISPLAY, i);
        if (bt_on && smb_len > 0u && path != nullptr &&
            strncmp(path, smb_mp, smb_len) == 0) {
            continue;
        }
        ++visible_tracks;
    }
    const size_t total_items = dir_count + visible_tracks;

    // Paginate: kMusicListMaxRows rows at most, minus the ".." row and (when
    // paged) the two page-nav rows. Directories with hundreds of entries
    // were previously cut off at row 48 with no way to reach the rest.
    size_t page_cap = kMusicListMaxRows - (at_root ? 0u : 1u);
    const bool paged = total_items > page_cap;
    if (paged) {
        page_cap -= 2u;
    }
    const size_t pages = (total_items + page_cap - 1u) / page_cap;
    if (s_music_page >= pages) {
        s_music_page = pages - 1u;
    }
    const size_t item_begin = s_music_page * page_cap;
    const size_t item_end =
        (item_begin + page_cap < total_items) ? item_begin + page_cap : total_items;

    size_t items_rendered = 0u;
    size_t ordinal = 0u;
    for (size_t i = 0; i < dir_count; ++i, ++ordinal) {
        if (ordinal < item_begin || ordinal >= item_end) {
            continue;
        }
        const char *name = PLAYLIST_ClientGetDirName(PLAYLIST_CLIENT_DISPLAY, i);
        lv_obj_t *btn = lv_list_add_button(s_list_music, LV_SYMBOL_DIRECTORY,
                                           (name != nullptr && name[0] != '\0') ? name : "(dir)");
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(kColorText), 0);
        lv_obj_add_event_cb(btn, musicDirEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        ++items_rendered;
        ++rows_used;
    }
    for (size_t i = 0; i < count; ++i) {
        const char *path = PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_DISPLAY, i);
        if (bt_on && smb_len > 0u && path != nullptr &&
            strncmp(path, smb_mp, smb_len) == 0) {
            continue;  // SMB track hidden while BT is on
        }
        if (ordinal >= item_begin && ordinal < item_end) {
            char row[128];
            snprintf(row, sizeof(row), "%s%s",
                     PLAYLIST_IsFavorite(path) ? "* " : "",
                     musicBasename(path));
            lv_obj_t *btn = lv_list_add_button(s_list_music, LV_SYMBOL_AUDIO, row);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(kColorText), 0);
            lv_obj_add_event_cb(btn, musicListEvent, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
            lv_obj_add_event_cb(btn, musicFavoriteEvent, LV_EVENT_LONG_PRESSED,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
            ++items_rendered;
            ++rows_used;
        }
        ++ordinal;
    }
    if (items_rendered == 0u) {
        lv_obj_t *empty = lv_list_add_text(
            s_list_music,
            tr(bt_on ? "Network music is hidden while Bluetooth is on. Turn Bluetooth off to play SMB."
                     : "No tracks or subdirectories."));
        lv_obj_set_style_text_color(empty, lv_color_hex(kColorSub), 0);
    }
    if (paged) {
        if (s_music_page > 0u) {
            char text[40];
            snprintf(text, sizeof(text), "Prev page (%u/%u)",
                     static_cast<unsigned>(s_music_page),
                     static_cast<unsigned>(pages));
            lv_obj_t *prev = lv_list_add_button(s_list_music, LV_SYMBOL_UP, text);
            lv_obj_set_style_bg_color(prev, lv_color_hex(kColorPanel2), 0);
            lv_obj_set_style_text_color(prev, lv_color_hex(kColorSub), 0);
            lv_obj_add_event_cb(prev, musicPageEvent, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
        }
        if (s_music_page + 1u < pages) {
            char text[40];
            snprintf(text, sizeof(text), "Next page (%u/%u)",
                     static_cast<unsigned>(s_music_page + 2u),
                     static_cast<unsigned>(pages));
            lv_obj_t *next = lv_list_add_button(s_list_music, LV_SYMBOL_DOWN, text);
            lv_obj_set_style_bg_color(next, lv_color_hex(kColorPanel2), 0);
            lv_obj_set_style_text_color(next, lv_color_hex(kColorSub), 0);
            lv_obj_add_event_cb(next, musicPageEvent, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<intptr_t>(1)));
        }
    }
}

void showMusicListStatus(const char *text, const uint32_t color)
{
    if (s_list_music == nullptr) {
        return;
    }
    lv_obj_clean(s_list_music);
    lv_obj_t *line = lv_list_add_text(s_list_music, tr(text));
    lv_obj_set_style_text_color(line, lv_color_hex(color), 0);
}

void startMusicRescan()
{
    // Async on the display's own session: a synchronous scan on a stale NAS
    // froze the LVGL task for the whole SMB directory-open timeout.
    if (PLAYLIST_ClientScanAsync(PLAYLIST_CLIENT_DISPLAY)) {
        s_music_dir_scan_pending = true;
    }
    showMusicListStatus("Scanning...", kColorWarn);
    lv_timer_handler();
}

void pollMusicScan()
{
    // Completion of an async enter/up/rescan of the display session (its
    // result only becomes visible once the session's scan finishes).
    if (s_music_dir_scan_pending && !PLAYLIST_ClientIsScanning(PLAYLIST_CLIENT_DISPLAY)) {
        s_music_dir_scan_pending = false;
        if (s_page == Page::Music) {
            // populateMusicList renders the empty-directory hint itself now;
            // overwriting it with a full-list status line would wipe the
            // ".." row and trap the user in the empty directory.
            populateMusicList();
        }
        s_last_refresh_ms = 0;
    }
    // Display-session scans that never set s_music_dir_scan_pending are
    // followed via the session's scan revision.
    if (s_page == Page::Music && !s_music_dir_scan_pending &&
        !PLAYLIST_ClientIsScanning(PLAYLIST_CLIENT_DISPLAY) && s_list_music != nullptr &&
        PLAYLIST_ClientScanRevision(PLAYLIST_CLIENT_DISPLAY) != s_music_list_rev) {
        populateMusicList();
        s_last_refresh_ms = 0;
    }
}

// True for common album-art filenames (case-insensitive): cover/folder/album/
// front + .jpg/.jpeg. Matching by listing the directory once (below) means we
// find the file whatever its case, without probing capitalised variants.
bool isCoverFileName(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr ||
        (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0)) {
        return false;
    }
    const size_t stem_len = static_cast<size_t>(dot - name);
    static const char *const kStems[] = {"cover", "folder", "album", "front"};
    for (const char *stem : kStems) {
        if (strlen(stem) == stem_len && strncasecmp(name, stem, stem_len) == 0) {
            return true;
        }
    }
    return false;
}

// Album-art fallback: a cover image file sitting next to the track
// (cover.jpg / folder.jpg / album.jpg / front.jpg). JPEG only; the cover
// decoder has no PNG path.
bool loadFolderCover(const char *track_path, CoverBitmap *out)
{
    if (track_path == nullptr || track_path[0] == '\0') {
        return false;
    }
    const char *slash = strrchr(track_path, '/');
    if (slash == nullptr) {
        return false;
    }
    const size_t dir_len = static_cast<size_t>(slash - track_path);

    // Heap scratch, NOT stack: loadFolderCover may decode several JPEG probes,
    // and the decoder is itself stack-heavy. Keep the path scratch off the
    // caller stack; on S31 this normally runs in the cover worker task.
    constexpr size_t kPathCap = 512;
    if (dir_len == 0u || dir_len >= kPathCap) {
        return false;
    }
    char *dir_path = static_cast<char *>(malloc(kPathCap));
    char *cover_path = static_cast<char *>(malloc(kPathCap));
    if (dir_path == nullptr || cover_path == nullptr) {
        free(dir_path);
        free(cover_path);
        return false;
    }
    memcpy(dir_path, track_path, dir_len);
    dir_path[dir_len] = '\0';
    cover_path[0] = '\0';

    // One directory listing instead of six blind fopen() probes: on an SMB share
    // each fopen is a network round-trip, so the old six-name probe stalled the
    // UI for ~6 RTTs of "stuck entering music". opendir + a single readdir pass
    // finds a cover file (case-insensitive) in one listing; then we open the
    // one match.
    DIR *dir = opendir(dir_path);
    if (dir != nullptr) {
        for (struct dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
            if (entry->d_type == DT_DIR || entry->d_name[0] == '.') {
                continue;
            }
            if (!isCoverFileName(entry->d_name)) {
                continue;
            }
            const int n = snprintf(cover_path, kPathCap, "%s/%s", dir_path, entry->d_name);
            if (n > 0 && static_cast<size_t>(n) < kPathCap) {
                break;
            }
            cover_path[0] = '\0'; // pathologically long path; keep scanning
        }
        closedir(dir);
    }
    free(dir_path);
    if (cover_path[0] == '\0') {
        free(cover_path);
        return false;
    }

    constexpr size_t kFolderCoverMaxBytes = 1024 * 1024;
    FILE *file = fopen(cover_path, "rb");
    free(cover_path);
    if (file == nullptr) {
        return false;
    }
    bool ok = false;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size > 0 && static_cast<size_t>(size) <= kFolderCoverMaxBytes &&
            fseek(file, 0, SEEK_SET) == 0) {
            uint8_t *data = static_cast<uint8_t *>(
                heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM));
            if (data != nullptr) {
                if (fread(data, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
                    ok = COVER_DecodeJpeg(data, static_cast<size_t>(size), kMusicCoverDim, out);
                }
                heap_caps_free(data);
            }
        }
    }
    fclose(file);
    return ok;
}

void setMusicCoverPlaceholder()
{
    if (s_img_music_cover == nullptr || s_lbl_music_icon == nullptr) {
        return;
    }
    lv_image_set_src(s_img_music_cover, nullptr);
    lv_obj_add_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
    COVER_Free(&s_music_cover_bmp);
}

void applyMusicCoverBitmap(CoverBitmap *bmp)
{
    if (s_img_music_cover == nullptr || s_lbl_music_icon == nullptr || bmp == nullptr ||
        bmp->rgb565 == nullptr) {
        return;
    }

    lv_image_set_src(s_img_music_cover, nullptr);
    COVER_Free(&s_music_cover_bmp);
    s_music_cover_bmp = *bmp;
    memset(bmp, 0, sizeof(*bmp));

    memset(&s_music_cover_dsc, 0, sizeof(s_music_cover_dsc));
    s_music_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_music_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_music_cover_dsc.header.w = s_music_cover_bmp.width;
    s_music_cover_dsc.header.h = s_music_cover_bmp.height;
    s_music_cover_dsc.header.stride = static_cast<uint32_t>(s_music_cover_bmp.width) * 2u;
    s_music_cover_dsc.data = s_music_cover_bmp.rgb565;
    s_music_cover_dsc.data_size = s_music_cover_bmp.bytes;

    lv_image_set_src(s_img_music_cover, &s_music_cover_dsc);
    lv_obj_remove_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
}

void musicCoverTask(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char path[sizeof(s_music_cover_req_path)] = {};
        uint8_t *jpeg = nullptr;
        size_t jpeg_size = 0;
        uint32_t seq = 0;

        xSemaphoreTake(s_music_cover_lock, portMAX_DELAY);
        seq = s_music_cover_req_seq;
        snprintf(path, sizeof(path), "%s", s_music_cover_req_path);
        jpeg = s_music_cover_req_jpeg;
        jpeg_size = s_music_cover_req_jpeg_size;
        s_music_cover_req_jpeg = nullptr;
        s_music_cover_req_jpeg_size = 0;
        xSemaphoreGive(s_music_cover_lock);

        CoverBitmap bmp = {};
        bool have_cover = false;
        if (jpeg != nullptr && jpeg_size > 0u) {
            have_cover = COVER_DecodeJpeg(jpeg, jpeg_size, kMusicCoverDim, &bmp);
        }
        if (jpeg != nullptr) {
            heap_caps_free(jpeg);
        }
        if (!have_cover) {
            have_cover = loadFolderCover(path, &bmp);
        }

        xSemaphoreTake(s_music_cover_lock, portMAX_DELAY);
        if (seq == s_music_cover_req_seq && path[0] != '\0') {
            COVER_Free(&s_music_cover_pending);
            if (have_cover) {
                s_music_cover_pending = bmp;
                memset(&bmp, 0, sizeof(bmp));
            }
            snprintf(s_music_cover_pending_path, sizeof(s_music_cover_pending_path), "%s", path);
            s_music_cover_pending_seq = seq;
        }
        xSemaphoreGive(s_music_cover_lock);
        COVER_Free(&bmp);
    }
}

void ensureMusicCoverTask()
{
    if (s_music_cover_lock == nullptr) {
        s_music_cover_lock = xSemaphoreCreateMutex();
        if (s_music_cover_lock == nullptr) {
            return;
        }
    }
    if (s_music_cover_task == nullptr) {
        if (xTaskCreatePinnedToCore(musicCoverTask, "music_cover", 6144,
                                    nullptr, 3, &s_music_cover_task, 0) != pdPASS) {
            s_music_cover_task = nullptr;
        }
    }
}

void requestMusicCoverDecode(const char *path, const MediaTrackInfo *track)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    ensureMusicCoverTask();
    if (s_music_cover_lock == nullptr || s_music_cover_task == nullptr) {
        return;
    }

    uint8_t *cover_copy = nullptr;
    size_t cover_size = 0;
    if (track != nullptr && track->cover_type == MEDIA_COVER_JPEG &&
        track->cover_data != nullptr && track->cover_size > 0u) {
        cover_copy = static_cast<uint8_t *>(
            heap_caps_malloc(track->cover_size, MALLOC_CAP_SPIRAM));
        if (cover_copy != nullptr) {
            memcpy(cover_copy, track->cover_data, track->cover_size);
            cover_size = track->cover_size;
        }
    }
    if (cover_size > 0u) {
        // The copy above is what the decode worker uses; free the embedded
        // original (up to 512 KB of PSRAM) instead of holding it for the
        // rest of the track -- playback-time PSRAM is fully booked.
        MUSIC_ReleaseTrackCover();
    }

    xSemaphoreTake(s_music_cover_lock, portMAX_DELAY);
    ++s_music_cover_req_seq;
    snprintf(s_music_cover_req_path, sizeof(s_music_cover_req_path), "%s", path);
    if (s_music_cover_req_jpeg != nullptr) {
        heap_caps_free(s_music_cover_req_jpeg);
    }
    s_music_cover_req_jpeg = cover_copy;
    s_music_cover_req_jpeg_size = cover_size;
    xSemaphoreGive(s_music_cover_lock);

    xTaskNotifyGive(s_music_cover_task);
}

void adoptMusicCover()
{
    if (s_img_music_cover == nullptr || s_music_cover_lock == nullptr) {
        return;
    }

    CoverBitmap bmp = {};
    char path[sizeof(s_music_cover_pending_path)] = {};
    uint32_t seq = 0;
    xSemaphoreTake(s_music_cover_lock, portMAX_DELAY);
    if (s_music_cover_pending_seq != 0u) {
        seq = s_music_cover_pending_seq;
        s_music_cover_pending_seq = 0;
        snprintf(path, sizeof(path), "%s", s_music_cover_pending_path);
        bmp = s_music_cover_pending;
        memset(&s_music_cover_pending, 0, sizeof(s_music_cover_pending));
        s_music_cover_pending_path[0] = '\0';
    }
    xSemaphoreGive(s_music_cover_lock);

    if (seq == 0u) {
        return;
    }
    if (strcmp(path, MUSIC_CurrentPath()) != 0) {
        COVER_Free(&bmp);
        return;
    }
    if (bmp.rgb565 != nullptr) {
        applyMusicCoverBitmap(&bmp);
    } else {
        setMusicCoverPlaceholder();
    }
    snprintf(s_music_cover_loaded_path, sizeof(s_music_cover_loaded_path), "%s", path);
}

void refreshMusicCover(const MediaTrackInfo *track, const char *path)
{
    if (path == nullptr || path[0] == '\0' ||
        strcmp(path, s_music_cover_loaded_path) == 0) {
        return;
    }
    setMusicCoverPlaceholder();
    s_music_cover_loaded_path[0] = '\0';
    requestMusicCoverDecode(path, track);
}

void refreshMusic()
{
    const bool playing = MUSIC_IsPlaying();
    const char *path = MUSIC_CurrentPath();
    adoptMusicCover();

    // Stream format line: the decoder only knows rate/depth/channels a
    // moment after playback starts (no track/state change fires then), so
    // poll it every pass; setLabel no-ops while the text is unchanged.
    if (s_lbl_music_format != nullptr) {
        char text[40] = "";
        uint32_t rate = 0;
        uint8_t bits = 0;
        uint8_t channels = 0;
        if (playing && MUSIC_GetStreamInfo(&rate, &bits, &channels)) {
            snprintf(text, sizeof(text), "%lu.%lukHz %ubit %uch",
                     static_cast<unsigned long>(rate / 1000u),
                     static_cast<unsigned long>((rate % 1000u) / 100u),
                     static_cast<unsigned>(bits),
                     static_cast<unsigned>(channels));
        }
        setLabel(s_lbl_music_format, s_shown_music_format, sizeof(s_shown_music_format), text);
    }

    const bool track_changed = strncmp(path, s_shown_music_path, sizeof(s_shown_music_path)) != 0;
    if (!track_changed && playing == s_shown_music_playing) {
        return;
    }
    snprintf(s_shown_music_path, sizeof(s_shown_music_path), "%s", path);
    s_shown_music_playing = playing;

    const MediaTrackInfo *track = MUSIC_GetTrackInfo();
    // Reload the cover art only when the *actual* track path changes -- not on
    // every screen entry. buildMusic() forces track_changed (via s_shown_music_path
    // = "\1") to repaint the text labels, but the cover reload runs
    // refreshMusicCover -> loadFolderCover(), which does up to 6 sequential
    // fopen() probes for cover files. When the library is on an SMB share those
    // are 6 network round-trips on the UI thread -- seconds of "stuck entering
    // music" every time. The bitmap is already cached (and re-attached by
    // buildMusic), so gate the reload on a separate path so re-entering the same
    // track is instant.
    if (track_changed) {
        refreshMusicCover(track, path);
    }
    if (s_lbl_music_title != nullptr) {
        const char *title = (track != nullptr && track->title[0] != '\0')
                                ? track->title
                                : musicBasename(path);
        lv_label_set_text(s_lbl_music_title, (title[0] != '\0') ? title : "--");
    }
    if (s_lbl_music_artist != nullptr) {
        lv_label_set_text(s_lbl_music_artist,
                          (track != nullptr && track->artist[0] != '\0') ? track->artist : "");
    }
    if (s_lbl_music_album != nullptr) {
        lv_label_set_text(s_lbl_music_album,
                          (track != nullptr && track->album[0] != '\0') ? track->album : "");
    }
    if (s_lbl_music_state != nullptr) {
        lv_label_set_text(s_lbl_music_state, tr(playing ? "Playing" : "Stopped"));
        lv_obj_set_style_text_color(s_lbl_music_state,
                                    lv_color_hex(playing ? kColorGood : kColorSub), 0);
    }
    if (s_btn_music_toggle_label != nullptr) {
        lv_label_set_text(s_btn_music_toggle_label, playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    }
    if (s_btn_music_mode_label != nullptr) {
        lv_label_set_text(s_btn_music_mode_label,
                          PLAYLIST_GetRepeatMode() == PLAYLIST_REPEAT_ONE ? "One" : "List");
    }
}

void buildMusic()
{
    clearScreen();
    s_page = Page::Music;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // Left: now-playing card -- cover art (or placeholder icon) on top,
    // track labels underneath.
    lv_obj_t *card = panel(scr, 24, 78, 300, 274);
    s_lbl_music_icon = label(card, &lv_font_montserrat_48, kColorDim);
    lv_obj_align(s_lbl_music_icon, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(s_lbl_music_icon, LV_SYMBOL_AUDIO);

    s_img_music_cover = lv_image_create(card);
    lv_obj_align(s_img_music_cover, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    if (s_music_cover_bmp.rgb565 != nullptr) {
        // Returning to the page mid-track: re-attach the existing bitmap.
        lv_image_set_src(s_img_music_cover, &s_music_cover_dsc);
        lv_obj_remove_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
    }

    s_lbl_music_title = label(card, &s_font_ui_20, kColorText);
    lv_obj_set_width(s_lbl_music_title, 270);
    lv_obj_align(s_lbl_music_title, LV_ALIGN_TOP_LEFT, 0, 162);
    lv_label_set_long_mode(s_lbl_music_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_title, tr("--"));

    s_lbl_music_artist = label(card, &s_font_ui_16, kColorSub);
    lv_obj_set_width(s_lbl_music_artist, 270);
    lv_obj_align(s_lbl_music_artist, LV_ALIGN_TOP_LEFT, 0, 196);
    lv_label_set_long_mode(s_lbl_music_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_artist, tr(""));

    s_lbl_music_album = label(card, &s_font_ui_16, kColorDim);
    lv_obj_set_width(s_lbl_music_album, 270);
    lv_obj_align(s_lbl_music_album, LV_ALIGN_TOP_LEFT, 0, 222);
    lv_label_set_long_mode(s_lbl_music_album, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_album, tr(""));

    s_lbl_music_state = label(card, &lv_font_montserrat_16, kColorSub);
    lv_obj_align(s_lbl_music_state, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_label_set_text(s_lbl_music_state, tr("Stopped"));

    // Stream format (rate / bit depth / channels), filled in by refreshMusic
    // once the decoder reports it.
    s_lbl_music_format = label(card, &lv_font_montserrat_16, kColorDim);
    lv_obj_align(s_lbl_music_format, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    lv_label_set_text(s_lbl_music_format, tr(""));

    // Right: scrollable track list.
    s_list_music = lv_list_create(scr);
    lv_obj_set_pos(s_list_music, 340, 78);
    lv_obj_set_size(s_list_music, 436, 274);
    lv_obj_set_style_bg_color(s_list_music, lv_color_hex(kColorPanel), 0);
    lv_obj_set_style_border_color(s_list_music, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_text_font(s_list_music, &s_font_ui_16, 0);
    populateMusicList();

    // First visit ever: nothing scans the display session at boot, so the
    // page would sit blank until the user finds Rescan. Kick an async
    // sources-root scan (SMB/SD/USB) on the first entry instead.
    if (PLAYLIST_ClientScanRevision(PLAYLIST_CLIENT_DISPLAY) == 0u &&
        !PLAYLIST_ClientIsScanning(PLAYLIST_CLIENT_DISPLAY)) {
        startMusicRescan();
    }

    button(scr, 24, 372, 120, 76, "Back", Action::Apps);
    button(scr, 158, 372, 96, 76, LV_SYMBOL_PREV, Action::MusicPrev);
    lv_obj_t *toggle = button(scr, 268, 372, 96, 76,
                              MUSIC_IsPlaying() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY,
                              Action::MusicToggle);
    s_btn_music_toggle_label = lv_obj_get_child(toggle, 0);
    button(scr, 378, 372, 96, 76, LV_SYMBOL_NEXT, Action::MusicNext);
    lv_obj_t *mode = button(scr, 488, 372, 120, 76,
                            PLAYLIST_GetRepeatMode() == PLAYLIST_REPEAT_ONE ? "One" : "List",
                            Action::MusicMode);
    s_btn_music_mode_label = lv_obj_get_child(mode, 0);
    button(scr, 622, 372, 154, 76, "Rescan", Action::MusicRescan);

    // Prime the now-playing card.
    s_shown_music_path[0] = '\1'; // force refreshMusic to repaint
    refreshMusic();
}

void musicToggle()
{
    if (MUSIC_IsPlaying()) {
        MUSIC_Stop();
        return;
    }
    // Replay the current track by path: the playback queue is decoupled from
    // any browse session, so no session index addresses it.
    const char *path = MUSIC_CurrentPath();
    if (path != nullptr && path[0] != '\0') {
        (void)MUSIC_PlayFile(path);
    } else {
        (void)PLAYLIST_Next();
    }
}

void musicModeToggle()
{
    (void)PLAYLIST_ToggleRepeatMode();
    if (s_btn_music_mode_label != nullptr) {
        lv_label_set_text(s_btn_music_mode_label,
                          PLAYLIST_GetRepeatMode() == PLAYLIST_REPEAT_ONE ? "One" : "List");
    }
}

// ---- Video call page --------------------------------------------------------

void videoViewTask(void *)
{
    uint8_t *remote_jpeg = s_video_remote_jpeg;
    uint8_t *local_jpeg = s_video_local_jpeg;
    uint32_t remote_seq = 0;
    uint32_t local_seq = 0;

    while (s_video_view_run) {
        const int64_t iter_start_us = esp_timer_get_time();
        bool worked = false;

        size_t jpeg_size = 0;
        if (VIDEO_AcquireFrame(remote_jpeg, VIDEO_MAX_JPEG_BYTES, &jpeg_size, &remote_seq)) {
            CoverBitmap bmp = {};
            CoverBitmap hw = {};
            // Hardware JPEG decode into the decoder's persistent full-res
            // buffer, then scale out immediately (borrowed semantics): the UI
            // adopts an owned panel-sized bitmap and the 1.8 MB buffer is
            // reused for the next frame. Software downscale stays the
            // fallback when the hardware is unavailable or rejects the frame.
            if (COVER_DecodeJpegHw(remote_jpeg, jpeg_size, &hw) && hw.rgb565 != nullptr) {
                (void)COVER_ScaleRgb565(hw.rgb565, hw.width, hw.height, hw.stride,
                                        kVideoFrameDim, &bmp);
            }
            if (bmp.rgb565 == nullptr) {
                (void)COVER_DecodeJpeg(remote_jpeg, jpeg_size, kVideoFrameDim, &bmp);
            }
            if (bmp.rgb565 != nullptr) {
                xSemaphoreTake(s_video_view_lock, portMAX_DELAY);
                COVER_Free(&s_video_pending_remote); // UI missed one; drop it
                s_video_pending_remote = bmp;
                xSemaphoreGive(s_video_view_lock);
            }
            worked = true;
        }

        size_t local_size = 0;
        if (local_jpeg != nullptr &&
            VIDEO_CopyLocalFrame(local_jpeg, VIDEO_MAX_JPEG_BYTES, &local_size, &local_seq)) {
            CoverBitmap bmp = {};
            CoverBitmap hw = {};
            // Same camera stream as the TX side: hardware decode keeps the
            // self-view off the CPU (a software 1/8 decode runs ~180 ms and
            // pinned CPU1 at ~95% duty, starving IDLE1 into the task WDT).
            if (COVER_DecodeJpegHw(local_jpeg, local_size, &hw) && hw.rgb565 != nullptr) {
                (void)COVER_ScaleRgb565(hw.rgb565, hw.width, hw.height, hw.stride,
                                        kVideoLocalDim, &bmp);
            }
            if (bmp.rgb565 == nullptr) {
                (void)COVER_DecodeJpeg(local_jpeg, local_size, kVideoLocalDim, &bmp);
            }
            if (bmp.rgb565 != nullptr) {
                xSemaphoreTake(s_video_view_lock, portMAX_DELAY);
                COVER_Free(&s_video_pending_local);
                s_video_pending_local = bmp;
                xSemaphoreGive(s_video_view_lock);
            }
            worked = true;
        }

        // Frames arrive at ~5 fps per direction; poll fast right after work
        // (the other direction is often ready too), lazily otherwise. A slow
        // software-decode fallback must not pin the core: once an iteration
        // costs >100 ms, delay at least as long as it ran so the decode duty
        // cycle stays <= 50% and the IDLE task keeps feeding the watchdog
        // (newest-frame-only handoff already drops whatever the lower frame
        // rate misses). A 720p software decode takes seconds, so the cap must
        // cover that -- a 400 ms cap still starved IDLE1 into the task WDT.
        const int64_t iter_ms = (esp_timer_get_time() - iter_start_us) / 1000;
        uint32_t delay_ms = worked ? 10u : 40u;
        if (worked && iter_ms > 100) {
            delay_ms = static_cast<uint32_t>(iter_ms > 4000 ? 4000 : iter_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    s_video_view_task = nullptr;
    vTaskDelete(nullptr);
}

void stopVideoView()
{
    s_video_view_run = false;
    for (int i = 0; i < 100 && s_video_view_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_video_view_lock != nullptr) {
        xSemaphoreTake(s_video_view_lock, portMAX_DELAY);
        COVER_Free(&s_video_pending_remote);
        COVER_Free(&s_video_pending_local);
        xSemaphoreGive(s_video_view_lock);
    }
    // Return the ~1.8 MB persistent hardware-decode buffer to the PSRAM heap;
    // the next video session lazily re-allocates it.
    COVER_HwDecoderRelease();
}

void startVideoView()
{
    if (s_video_view_lock == nullptr) {
        s_video_view_lock = xSemaphoreCreateMutex();
        if (s_video_view_lock == nullptr) {
            return;
        }
    }
    if (s_video_view_task != nullptr) {
        return;
    }
    s_video_view_run = true;
    if (xTaskCreatePinnedToCore(videoViewTask, "video_view", 6144, nullptr, 3,
                                &s_video_view_task, 1) != pdPASS) {
        s_video_view_run = false;
        s_video_view_task = nullptr;
        ESP_LOGE(TAG, "video view task create failed");
    }
}

// Adopt a decoded bitmap from the worker into the shown bitmap + LVGL image.
// UI-task only (lv_image_set_src is not thread-safe). Hardware-decoded frames
// are full resolution (e.g. 1280x720) and get scaled down here to fit the
// panel; software-decoded frames already match the layout (zoom stays 100%).
void adoptVideoBitmap(lv_obj_t *img, CoverBitmap *shown, lv_image_dsc_t *dsc, CoverBitmap *pending)
{
    lv_image_set_src(img, nullptr);
    COVER_Free(shown);
    *shown = *pending;
    memset(pending, 0, sizeof(*pending));
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = shown->width;
    dsc->header.h = shown->height;
    // Hardware JPEG output is padded to 16-pixel rows (stride > width*2).
    dsc->header.stride = (shown->stride != 0)
                             ? static_cast<uint32_t>(shown->stride)
                             : static_cast<uint32_t>(shown->width) * 2u;
    dsc->data = shown->rgb565;
    dsc->data_size = shown->bytes;
    lv_image_set_src(img, dsc);

    // Fit the (possibly full-resolution) bitmap into the parent panel,
    // keeping aspect. LVGL zoom is 8.8 fixed point: 256 == 100%.
    const lv_obj_t *parent = lv_obj_get_parent(img);
    if (parent != nullptr && shown->width > 0 && shown->height > 0) {
        const int32_t pw = lv_obj_get_content_width(parent);
        const int32_t ph = lv_obj_get_content_height(parent);
        const uint32_t zoom_x = (static_cast<uint32_t>(pw) * 256u) / shown->width;
        const uint32_t zoom_y = (static_cast<uint32_t>(ph) * 256u) / shown->height;
        const uint32_t zoom = (zoom_x < zoom_y) ? zoom_x : zoom_y;
        lv_image_set_scale(img, static_cast<uint32_t>((zoom > 256u) ? 256u : zoom));
    }
}

void refreshVideo()
{
    if (s_lbl_video_status != nullptr) {
        char status[64];
        snprintf(status, sizeof(status), "TX %s | RX %s",
                 VIDEO_TxEnabled() ? "ON" : "off",
                 VIDEO_Receiving() ? "receiving" : "idle");
        setLabel(s_lbl_video_status, s_shown_detail, sizeof(s_shown_detail), status);
    }
    if (s_btn_video_tx_label != nullptr) {
        lv_label_set_text(s_btn_video_tx_label, tr(VIDEO_TxEnabled() ? "Cam OFF" : "Cam ON"));
    }

    // Camera off: drop the stale self-view so the "Local camera" hint shows.
    if (s_img_video_local != nullptr && !VIDEO_TxEnabled() &&
        s_video_local_bmp.rgb565 != nullptr) {
        lv_image_set_src(s_img_video_local, nullptr);
        COVER_Free(&s_video_local_bmp);
    }

    // Pick up freshly decoded frames; never block the UI task (the worker
    // holds the lock only for a pointer swap). Adopting both in one pass
    // lets a single LVGL render cover remote + local.
    if (s_video_view_lock == nullptr ||
        xSemaphoreTake(s_video_view_lock, 0) != pdTRUE) {
        return;
    }
    if (s_img_video != nullptr && s_video_pending_remote.rgb565 != nullptr) {
        adoptVideoBitmap(s_img_video, &s_video_bmp, &s_video_dsc, &s_video_pending_remote);
    }
    if (s_img_video_local != nullptr && s_video_pending_local.rgb565 != nullptr &&
        VIDEO_TxEnabled()) {
        adoptVideoBitmap(s_img_video_local, &s_video_local_bmp, &s_video_local_dsc,
                         &s_video_pending_local);
    }
    xSemaphoreGive(s_video_view_lock);
}

void buildVideo()
{
    clearScreen();
    s_page = Page::Video;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // Remote frame view (left) + status/controls/self-view (right).
    lv_obj_t *frame_panel = panel(scr, 24, 78, 500, 274);
    s_img_video = lv_image_create(frame_panel);
    lv_obj_center(s_img_video);
    lv_obj_t *hint = label(frame_panel, &lv_font_montserrat_16, kColorDim);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(hint, tr("Remote video"));
    lv_obj_move_background(hint);

    s_lbl_video_status = label(scr, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_video_status, 548, 96);
    lv_obj_set_width(s_lbl_video_status, 228);
    lv_label_set_text(s_lbl_video_status, tr("TX off | RX idle"));

    lv_obj_t *toggle = button(scr, 548, 140, 226, 76, VIDEO_TxEnabled() ? "Cam OFF" : "Cam ON",
                              Action::VideoTx);
    s_btn_video_tx_label = lv_obj_get_child(toggle, 0);

    // Local camera self-view under the toggle (VGA 4:3 scaled to <=144 px).
    lv_obj_t *local_panel = panel(scr, 548, 232, 226, 140);
    lv_obj_t *local_hint = label(local_panel, &lv_font_montserrat_16, kColorDim);
    lv_obj_center(local_hint);
    lv_label_set_text(local_hint, tr("Local camera"));
    s_img_video_local = lv_image_create(local_panel);
    lv_obj_center(s_img_video_local);

    button(scr, 24, 372, 230, 76, "Back", Action::Apps);

    startVideoView();
    refreshVideo();
}

void videoTxToggleTask(void *)
{
    // bsp_camera_open() detects the sensor over I2C and allocates three
    // ~900 KB V4L2 buffers -- over a second of work that must not run on
    // the LVGL/UI task (it froze lv_timer_handler for >1 s).
    if (!VIDEO_SetTxEnabled(!VIDEO_TxEnabled())) {
        ESP_LOGW(TAG, "camera start failed");
    }
    s_video_tx_toggle_busy = false;
    s_last_refresh_ms = 0;
    vTaskDelete(nullptr);
}

void videoTxToggle()
{
    if (s_video_tx_toggle_busy) {
        return; // camera open/close still in flight
    }
    s_video_tx_toggle_busy = true;
    if (xTaskCreate(videoTxToggleTask, "video_tx_sw", 6144, nullptr, 4, nullptr) != pdPASS) {
        s_video_tx_toggle_busy = false;
        ESP_LOGW(TAG, "video toggle task create failed");
    }
}

// ---- AI assistant page (xiaozhi) --------------------------------------------

void refreshAiPage()
{
    if (s_lbl_ai_status != nullptr) {
        char status[224];
        AI_Describe(status, sizeof(status));
        setLabel(s_lbl_ai_status, s_shown_ai_status, sizeof(s_shown_ai_status), status);
    }
    // While a listen turn is up the button doubles as the level indicator.
    if (s_btn_ai_talk_label != nullptr && s_btn_ai_talk != nullptr) {
        const bool listening = AI_IsListening();
        lv_label_set_text(s_btn_ai_talk_label,
                          tr(listening ? "Listening... release to send" : "Hold to Talk"));
        lv_obj_set_style_bg_color(s_btn_ai_talk,
                                  lv_color_hex(listening ? 0x1D634E : kColorPanel2), 0);
    }
}

// Push-to-talk: press starts a listen turn (mic streams to the server),
// release ends it and the reply plays through the speaker.
void aiTalkEvent(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        if (!AI_StartListen()) {
            if (s_btn_ai_talk_label != nullptr) {
                lv_label_set_text(s_btn_ai_talk_label,
                                  AI_IsEnabled() ? "Not connected" : "Enable the assistant first");
            }
            return;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        AI_StopListen();
    }
    refreshAiPage();
}

void aiEnableEvent(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    const bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!AI_SetEnabled(want)) {
        // Enabling without a configured URL fails: snap the switch back.
        if (want) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        formStatus("Set the server URL first (web portal Media page or AT+AI=wss://...,token).",
                   kColorBad);
    } else {
        formStatus(want ? "Connecting..." : "Assistant disabled.", kColorSub);
    }
    s_last_refresh_ms = 0;
}

void buildAi()
{
    clearScreen();
    s_page = Page::Ai;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    fieldLabel(box, 0, 0, "xiaozhi AI Assistant");
    s_lbl_ai_status = label(box, &s_font_ui_16, kColorText);
    lv_obj_set_width(s_lbl_ai_status, 710);
    lv_obj_set_pos(s_lbl_ai_status, 0, 26);
    lv_label_set_long_mode(s_lbl_ai_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_ai_status, tr("--"));

    fieldLabel(box, 0, 96, "Enable assistant");
    s_sw_ai = lv_switch_create(box);
    lv_obj_set_pos(s_sw_ai, 0, 120);
    lv_obj_set_size(s_sw_ai, 64, 34);
    if (AI_IsEnabled()) {
        lv_obj_add_state(s_sw_ai, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sw_ai, aiEnableEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 176);
    lv_label_set_text(s_lbl_form_status,
                      tr("Server URL / token: web portal Media page, or AT+AI=wss://...,token"));

    button(scr, 24, 372, 190, 76, "Back", Action::Apps);
    // Big push-to-talk button; needs its own press/release wiring instead of
    // the shared CLICKED handler.
    s_btn_ai_talk = button(scr, 230, 372, 544, 76, "Hold to Talk", Action::Ai);
    lv_obj_remove_event_cb(s_btn_ai_talk, action);
    lv_obj_add_event_cb(s_btn_ai_talk, aiTalkEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_btn_ai_talk, aiTalkEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_btn_ai_talk, aiTalkEvent, LV_EVENT_PRESS_LOST, nullptr);
    s_btn_ai_talk_label = lv_obj_get_child(s_btn_ai_talk, 0);

    refreshAiPage();
}

// ---- APRS -------------------------------------------------------------------

void aprsEnableEvent(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    const bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!APRS_SERVICE_SetEnabled(want)) {
        if (want) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        formStatus("APRS service unavailable.", kColorBad);
    } else {
        formStatus(want ? "APRS enabled." : "APRS disabled.", kColorSub);
    }
    s_aprs_last_refresh_ms = 0u;
    s_last_refresh_ms = 0;
}

// Rebuild the status line and station list only when a packet arrived or the
// age column needs a tick -- Aprs is a FULL-render surface, so every
// lv_label_set_text here repaints the whole screen.
void refreshAprsPage()
{
    if (s_lbl_aprs_status == nullptr || s_lbl_aprs_list == nullptr) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const uint32_t revision = APRS_SERVICE_GetStationRevision();
    if (revision == s_aprs_seen_revision && s_aprs_last_refresh_ms != 0u &&
        now - s_aprs_last_refresh_ms < 3000u) {
        return;
    }
    s_aprs_seen_revision = revision;
    s_aprs_last_refresh_ms = now;

    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);
    char status[sizeof(s_shown_aprs_status)];
    snprintf(status, sizeof(status),
             "%s | IS %s | RF tx %s rx %s | GPS %s | rx %lu tx %lu",
             cfg.enabled ? "ON" : "OFF",
             APRS_SERVICE_IsNetConnected() ? "linked" : "--",
             cfg.rf_tx_enabled ? "on" : "off",
             cfg.rf_rx_enabled ? "on" : "off",
             APRS_SERVICE_GpsHasFix() ? "fix" : "--",
             static_cast<unsigned long>(APRS_SERVICE_GetRxCount()),
             static_cast<unsigned long>(APRS_SERVICE_GetTxCount()));
    if (strcmp(status, s_shown_aprs_status) != 0) {
        snprintf(s_shown_aprs_status, sizeof(s_shown_aprs_status), "%s", status);
        lv_label_set_text(s_lbl_aprs_status, status);
    }

    AprsStationInfo stations[8];
    const size_t count = APRS_SERVICE_GetStations(stations, 8);
    char list[sizeof(s_shown_aprs_list)];
    size_t used = 0;
    if (count == 0u) {
        snprintf(list, sizeof(list), "%s",
                 cfg.enabled ? "No stations heard yet." : "APRS is off.");
    } else {
        list[0] = '\0';
        for (size_t i = 0; i < count && used < sizeof(list) - 8u; ++i) {
            const AprsStationInfo &s = stations[i];
            char dist[16] = "--";
            if (!isnan(s.distance_km)) {
                snprintf(dist, sizeof(dist), "%.1fkm", static_cast<double>(s.distance_km));
            }
            char spd[20] = "";
            if (!isnan(s.speed_kmh)) {
                snprintf(spd, sizeof(spd), "  %.0fkm/h", static_cast<double>(s.speed_kmh));
            } else if (!isnan(s.derived_speed_kmh)) {
                snprintf(spd, sizeof(spd), "  ~%.0fkm/h",
                         static_cast<double>(s.derived_speed_kmh));
            }
            char alt[16] = "";
            if (!isnan(s.altitude_m)) {
                snprintf(alt, sizeof(alt), "  %.0fm", static_cast<double>(s.altitude_m));
            }
            char age[12];
            if (s.age_s < 60u) {
                snprintf(age, sizeof(age), "%lus", static_cast<unsigned long>(s.age_s));
            } else {
                snprintf(age, sizeof(age), "%lum", static_cast<unsigned long>(s.age_s / 60u));
            }
            used += static_cast<size_t>(snprintf(
                list + used, sizeof(list) - used, "%s%-9s  %8.4f %9.4f%s%s  %s  %s  %.20s",
                i > 0 ? "\n" : "", s.name, static_cast<double>(s.lat),
                static_cast<double>(s.lon), alt, spd, s.via_rf ? "RF" : "IS", age,
                s.comment));
        }
    }
    if (strcmp(list, s_shown_aprs_list) != 0) {
        snprintf(s_shown_aprs_list, sizeof(s_shown_aprs_list), "%s", list);
        lv_label_set_text(s_lbl_aprs_list, list);
    }
}

// ---- map page ---------------------------------------------------------------
// Slippy-Map viewport: a kMapCols x kMapRows grid of 256 px lv_image widgets
// fed by MAP_TILES (TF card first, HTTP download otherwise). Dragging pans
// (sub-tile offsets move the widgets), +/- buttons zoom, APRS stations plot
// as dots with callsign labels. Tiles/widgets live in a clipping container
// below the top bar; missing tiles show the widget's dark placeholder.

void layoutMapTiles();
void layoutMapMarkers();

void clampMapCenter()
{
    const double world = ldexp(256.0, s_map_zoom);
    while (s_map_cx < 0.0) {
        s_map_cx += world;
    }
    while (s_map_cx >= world) {
        s_map_cx -= world;
    }
    if (s_map_cy < 0.0) {
        s_map_cy = 0.0;
    }
    if (s_map_cy > world) {
        s_map_cy = world;
    }
}

void mapSetCenterLonLat(double lon, double lat)
{
    MAP_TILES_LonLatToPixel(lon, lat, s_map_zoom, &s_map_cx, &s_map_cy);
    clampMapCenter();
}

// Position every widget from the current center and (re)bind tile sources.
// A widget re-queries its tile while it shows the placeholder, so tiles that
// finish downloading appear without any explicit invalidation.
void layoutMapTiles()
{
    if (s_map_tiles[0] == nullptr) {
        return;
    }
    const double left = s_map_cx - kWidth / 2.0;
    const double top = s_map_cy - kMapViewH / 2.0;
    const int32_t zlimit = 1 << s_map_zoom;
    const int32_t tx0 = static_cast<int32_t>(floor(left / kMapTilePx));
    const int32_t ty0 = static_cast<int32_t>(floor(top / kMapTilePx));
    const int frac_x = static_cast<int>(floor(left - static_cast<double>(tx0) * kMapTilePx));
    const int frac_y = static_cast<int>(floor(top - static_cast<double>(ty0) * kMapTilePx));
    for (int r = 0; r < kMapRows; ++r) {
        for (int c = 0; c < kMapCols; ++c) {
            const int i = r * kMapCols + c;
            lv_obj_set_pos(s_map_tiles[i], c * kMapTilePx - frac_x, r * kMapTilePx - frac_y);
            int32_t tx = (tx0 + c) % zlimit; // longitude wraps around the world
            if (tx < 0) {
                tx += zlimit;
            }
            const int32_t ty = ty0 + r;
            const bool valid = ty >= 0 && ty < zlimit;
            if (valid && s_map_tile_x[i] == tx && s_map_tile_y[i] == ty &&
                s_map_tile_filled[i]) {
                continue; // already showing this tile
            }
            const uint8_t *pixels = valid ? MAP_TILES_Get(s_map_zoom, tx, ty) : nullptr;
            s_map_tile_x[i] = tx;
            s_map_tile_y[i] = ty;
            s_map_tile_filled[i] = pixels != nullptr;
            if (pixels != nullptr) {
                memset(&s_map_dsc[i], 0, sizeof(s_map_dsc[i]));
                s_map_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
                s_map_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
                s_map_dsc[i].header.w = kMapTilePx;
                s_map_dsc[i].header.h = kMapTilePx;
                s_map_dsc[i].header.stride = kMapTilePx * 2u;
                s_map_dsc[i].data = pixels;
                s_map_dsc[i].data_size = kMapTilePx * kMapTilePx * 2u;
                lv_image_set_src(s_map_tiles[i], &s_map_dsc[i]);
            } else {
                lv_image_set_src(s_map_tiles[i], nullptr); // dark placeholder
            }
        }
    }
}

void layoutMapMarkers()
{
    if (s_map_markers[0] == nullptr) {
        return;
    }
    // Static: the station struct carries a 220-byte comment, so a snapshot
    // array would blow the UI task's stack.
    NRL_PSRAM_BSS static AprsStationInfo stations[kMapMarkerMax];
    const size_t count = APRS_SERVICE_GetStations(stations, kMapMarkerMax);
    const double left = s_map_cx - kWidth / 2.0;
    const double top = s_map_cy - kMapViewH / 2.0;
    size_t used = 0u;
    for (size_t i = 0u; i < count && used < kMapMarkerMax; ++i) {
        double px = 0.0;
        double py = 0.0;
        MAP_TILES_LonLatToPixel(stations[i].lon, stations[i].lat, s_map_zoom, &px, &py);
        const int sx = static_cast<int>(px - left);
        const int sy = static_cast<int>(py - top);
        // Skip stations outside the viewport (the label sticks out right/up).
        if (sx < -64 || sx >= kWidth + 8 || sy < -16 || sy >= kMapViewH + 8) {
            continue;
        }
        lv_obj_set_pos(s_map_markers[used], sx - 4, sy - 4);
        lv_obj_set_pos(s_map_marker_labels[used], sx + 7, sy - 10);
        setLabelTextIfChanged(s_map_marker_labels[used], stations[i].name);
        lv_obj_remove_flag(s_map_markers[used], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_map_marker_labels[used], LV_OBJ_FLAG_HIDDEN);
        ++used;
    }
    for (size_t i = used; i < kMapMarkerMax; ++i) {
        lv_obj_add_flag(s_map_markers[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_map_marker_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Drag panning: the pressed tile widget gets PRESSING on every indev read;
// the vect is the pixel delta since the previous read.
void mapPanEvent(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == nullptr) {
        return;
    }
    lv_point_t vect = {};
    lv_indev_get_vect(indev, &vect);
    if (vect.x == 0 && vect.y == 0) {
        return;
    }
    s_map_cx -= vect.x;
    s_map_cy -= vect.y;
    clampMapCenter();
    layoutMapTiles();
    layoutMapMarkers();
}

void mapZoomStep(int delta)
{
    const int next = static_cast<int>(s_map_zoom) + delta;
    if (next < kMapZoomMin || next > kMapZoomMax) {
        return;
    }
    // Keep the geographic center across the scale change.
    double lon = 0.0;
    double lat = 0.0;
    MAP_TILES_PixelToLonLat(s_map_cx, s_map_cy, s_map_zoom, &lon, &lat);
    s_map_zoom = static_cast<uint8_t>(next);
    mapSetCenterLonLat(lon, lat);
    for (size_t i = 0u; i < sizeof(s_map_tiles) / sizeof(s_map_tiles[0]); ++i) {
        s_map_tile_x[i] = INT32_MIN; // force tile reassignment
        s_map_tile_filled[i] = false;
    }
    layoutMapTiles();
    layoutMapMarkers();
    char text[8];
    snprintf(text, sizeof(text), "z%u", static_cast<unsigned>(s_map_zoom));
    setLabelTextIfChanged(s_map_lbl_zoom, text);
}

void refreshMapPage()
{
    const uint32_t tile_rev = MAP_TILES_Revision();
    const uint32_t station_rev = APRS_SERVICE_GetStationRevision();
    if (tile_rev == s_map_tile_rev && station_rev == s_map_station_rev) {
        return;
    }
    s_map_tile_rev = tile_rev;
    s_map_station_rev = station_rev;
    layoutMapTiles();
    layoutMapMarkers();
}

void buildMap()
{
    clearScreen();
    s_page = Page::Map;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // First visit: center on the GPS fix, else the configured APRS default
    // position, else the first heard station, else the world origin.
    if (!s_map_centered) {
        double lat = 0.0;
        double lon = 0.0;
        bool have = APRS_SERVICE_GetOwnPosition(&lat, &lon, nullptr);
        if (!have) {
            // No live fix: the getter already fell back to the configured
            // default; 0/0 means none is configured.
            have = lat != 0.0 || lon != 0.0;
        }
        if (!have) {
            AprsStationInfo first = {};
            if (APRS_SERVICE_GetStations(&first, 1u) > 0u) {
                lat = first.lat;
                lon = first.lon;
                have = true;
            }
        }
        s_map_zoom = kMapZoomDefault;
        mapSetCenterLonLat(lon, lat);
        s_map_centered = true;
    }

    s_map_view = lv_obj_create(scr);
    lv_obj_remove_style_all(s_map_view);
    lv_obj_set_pos(s_map_view, 0, kMapTop);
    lv_obj_set_size(s_map_view, kWidth, kMapViewH);
    lv_obj_set_style_bg_color(s_map_view, lv_color_hex(kColorPanel), 0);
    lv_obj_set_style_bg_opa(s_map_view, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_map_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_map_view, LV_OBJ_FLAG_OVERFLOW_VISIBLE); // clip panning tiles

    for (int i = 0; i < kMapCols * kMapRows; ++i) {
        lv_obj_t *img = lv_image_create(s_map_view);
        lv_obj_set_size(img, kMapTilePx, kMapTilePx);
        lv_obj_set_style_bg_color(img, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE); // receives the drag PRESSING
        lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(img, mapPanEvent, LV_EVENT_PRESSING, nullptr);
        s_map_tiles[i] = img;
        s_map_tile_x[i] = INT32_MIN;
        s_map_tile_filled[i] = false;
    }

    // Own-position reticle at the viewport center.
    s_map_center_dot = lv_obj_create(s_map_view);
    lv_obj_remove_style_all(s_map_center_dot);
    lv_obj_set_size(s_map_center_dot, 9, 9);
    lv_obj_set_style_radius(s_map_center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_map_center_dot, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(s_map_center_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_map_center_dot, kWidth / 2 - 4, kMapViewH / 2 - 4);

    for (size_t i = 0u; i < kMapMarkerMax; ++i) {
        lv_obj_t *dot = lv_obj_create(s_map_view);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 9, 9);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kColorBad), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_map_markers[i] = dot;

        lv_obj_t *tag = label(s_map_view, &lv_font_montserrat_16, kColorText);
        lv_obj_set_style_bg_color(tag, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(tag, 2, 0);
        lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
        s_map_marker_labels[i] = tag;
    }

    // Required visible attribution for tile.openstreetmap.org. Keep it above
    // the tile widgets while allowing drag gestures to pass through.
    lv_obj_t *attribution = label(s_map_view, &lv_font_montserrat_14, kColorText);
    lv_label_set_text(attribution,
                      "(c) OpenStreetMap contributors\nopenstreetmap.org/copyright");
    lv_obj_set_style_text_align(attribution, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(attribution, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(attribution, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(attribution, 2, 0);
    lv_obj_align(attribution, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_remove_flag(attribution, LV_OBJ_FLAG_CLICKABLE);

    button(scr, 724, 70, 64, 56, "+", Action::MapZoomIn);
    button(scr, 724, 134, 64, 56, "-", Action::MapZoomOut);
    s_map_lbl_zoom = label(scr, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_pos(s_map_lbl_zoom, 724, 202);
    lv_obj_set_width(s_map_lbl_zoom, 64);
    lv_obj_set_style_text_align(s_map_lbl_zoom, LV_TEXT_ALIGN_CENTER, 0);
    char text[8];
    snprintf(text, sizeof(text), "z%u", static_cast<unsigned>(s_map_zoom));
    lv_label_set_text(s_map_lbl_zoom, text);
    button(scr, 12, 420, 88, 52, "Back", Action::Apps);

    layoutMapTiles();
    layoutMapMarkers();
    s_map_tile_rev = MAP_TILES_Revision();
    s_map_station_rev = APRS_SERVICE_GetStationRevision();
}

// ---- SSTV page ----------------------------------------------------------------
// TX: pick a JPEG from the TF card /sstv directory and send it over the air (side tone
// on the speaker). RX: the service's demodulator taps mic or NRL downlink and
// writes lines into a PSRAM frame buffer shown live; the widget only gets
// invalidated when the image revision bumps.

void refreshSstvPage();

const char *sstvModeName(SSTV_Mode mode)
{
    return mode == SSTV_MODE_ROBOT36 ? "ROBOT36" : mode == SSTV_MODE_MARTIN_M1 ? "MARTIN1"
                                                                              : "PD120";
}

void scanSstvFiles()
{
    s_sstv_file_count = 0u;
    s_sstv_file_index = 0u;
    if (!STORAGE_SdMounted()) {
        return;
    }
    char directory[96];
    if (!SSTV_SERVICE_GetImageDirectory(directory, sizeof(directory))) {
        return;
    }
    DIR *dir = opendir(directory);
    if (dir == nullptr) {
        return;
    }
    while (s_sstv_file_count < kSstvFileMax) {
        const struct dirent *ent = readdir(dir);
        if (ent == nullptr) {
            break;
        }
        const size_t len = strlen(ent->d_name);
        if (len < 5u || len >= kSstvNameChars) {
            continue;
        }
        const bool jpg = strcasecmp(ent->d_name + len - 4u, ".jpg") == 0;
        const bool jpeg = len >= 6u && strcasecmp(ent->d_name + len - 5u, ".jpeg") == 0;
        if (!jpg && !jpeg) {
            continue;
        }
        snprintf(s_sstv_files[s_sstv_file_count], kSstvNameChars, "%s", ent->d_name);
        ++s_sstv_file_count;
    }
    closedir(dir);
}

// Decode a JPEG into the right-side preview window (320x256, centered by the
// image widget's default align). Hardware decode first -- the camera frames
// are full-resolution 720p and the software path takes seconds; the scaled
// bitmap is small (~160 KB) and owned here until replaced.
void showSstvPreview(const uint8_t *jpeg, const size_t jpeg_size)
{
    if (jpeg == nullptr || jpeg_size == 0u || s_img_sstv == nullptr) {
        return;
    }
    CoverBitmap hw = {};
    CoverBitmap bmp = {};
    bool ok = COVER_DecodeJpegHw(jpeg, jpeg_size, &hw) && hw.rgb565 != nullptr &&
              COVER_FitRgb565(hw.rgb565, hw.width, hw.height, hw.stride, 320u, 256u, &bmp);
    if (!ok) {
        ok = COVER_DecodeJpeg(jpeg, jpeg_size, 256u, &bmp);
    }
    if (!ok) {
        return; // keep whatever is on screen
    }
    COVER_Free(&s_sstv_tx_bmp);
    s_sstv_tx_bmp = bmp;
    memset(&s_sstv_tx_dsc, 0, sizeof(s_sstv_tx_dsc));
    s_sstv_tx_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_sstv_tx_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_sstv_tx_dsc.header.w = s_sstv_tx_bmp.width;
    s_sstv_tx_dsc.header.h = s_sstv_tx_bmp.height;
    s_sstv_tx_dsc.header.stride = static_cast<uint32_t>(s_sstv_tx_bmp.width) * 2u;
    s_sstv_tx_dsc.data = s_sstv_tx_bmp.rgb565;
    s_sstv_tx_dsc.data_size = s_sstv_tx_bmp.bytes;
    lv_image_set_src(s_img_sstv, &s_sstv_tx_dsc);
    s_sstv_tx_preview = true;
}

// Preview the currently selected /sdcard/sstv file in the TX IMAGE list.
void showSstvFilePreview()
{
    if (s_sstv_file_count == 0u || !STORAGE_SdMounted()) {
        return;
    }
    char directory[96];
    char path[160];
    if (!SSTV_SERVICE_GetImageDirectory(directory, sizeof(directory))) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", directory, s_sstv_files[s_sstv_file_index]);
    FILE *file = fopen(path, "rb");
    if (file == nullptr || fseek(file, 0, SEEK_END) != 0) {
        if (file != nullptr) {
            fclose(file);
        }
        return;
    }
    const long size = ftell(file);
    if (size <= 0 || size > 768 * 1024) { // preview cap keeps PSRAM sane
        fclose(file);
        return;
    }
    uint8_t *jpeg = static_cast<uint8_t *>(heap_caps_malloc(
        static_cast<size_t>(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg != nullptr && fseek(file, 0, SEEK_SET) == 0 &&
        fread(jpeg, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
        showSstvPreview(jpeg, static_cast<size_t>(size));
    }
    if (jpeg != nullptr) {
        heap_caps_free(jpeg);
    }
    fclose(file);
}

void stepSstvFile(int delta)
{
    if (s_sstv_file_count == 0u) {
        return;
    }
    s_sstv_file_index = (s_sstv_file_index + s_sstv_file_count + static_cast<size_t>(delta)) %
                        s_sstv_file_count;
    showSstvFilePreview();
    refreshSstvPage();
}

void sendSstvFromPage()
{
    if (s_sstv_file_count == 0u || !STORAGE_SdMounted()) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("no JPEG on TF card"));
        s_sstv_msg_ms = millis();
        refreshSstvPage();
        return;
    }
    char directory[96];
    if (!SSTV_SERVICE_GetImageDirectory(directory, sizeof(directory))) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("no JPEG on TF card"));
        s_sstv_msg_ms = millis();
        refreshSstvPage();
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", directory,
             s_sstv_files[s_sstv_file_index]);
    if (!SSTV_SERVICE_SendJpeg(path, s_sstv_tx_mode)) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("TX busy / failed"));
        s_sstv_msg_ms = millis();
    }
    refreshSstvPage();
}

void saveSstvFromPage()
{
    char path[96] = {};
    if (SSTV_SERVICE_SaveRxJpeg(path, sizeof(path))) {
        const char *base = strrchr(path, '/');
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), tr("saved %s"), base != nullptr ? base + 1 : path);
    } else {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("save failed (no frame/card)"));
    }
    s_sstv_msg_ms = millis();
    refreshSstvPage();
}

// Capture one OV3660 frame and queue it as the TX image. Runs on the UI
// task and blocks about a second while the sensor's AEC/AWB settles.
void camSstvFromPage()
{
    if (VIDEO_TxEnabled()) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("camera busy (video TX)"));
        s_sstv_msg_ms = millis();
        refreshSstvPage();
        return;
    }
    SstvSnapshot snap{};
    SSTV_SERVICE_GetSnapshot(&snap);
    if (snap.state == SSTV_STATE_PREPARING || snap.state == SSTV_STATE_SENDING) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("TX busy / failed"));
        s_sstv_msg_ms = millis();
        refreshSstvPage();
        return;
    }
    if (snap.rx_active) {
        (void)SSTV_SERVICE_StopRx();
    }
    bsp_camera_config_t cfg = BSP_CAMERA_DEFAULT_CONFIG();
    // Match the camera parameters already proven stable by video calling.
    cfg.width = 1280;
    cfg.height = 720;
    cfg.pixel_format = BSP_CAMERA_PIXEL_FORMAT_JPEG;
    cfg.xclk_freq_hz = 10 * 1000 * 1000;
    bsp_camera_t *cam = nullptr;
    bool sent = false;
    bool captured = false;
    const esp_err_t open_err = bsp_camera_open(&cfg, &cam);
    if (open_err == ESP_OK && cam != nullptr) {
        (void)bsp_camera_set_jpeg_quality(cam, 10);
        const esp_err_t start_err = bsp_camera_start(cam);
        if (start_err == ESP_OK) {
            for (int i = 0; i < 8 && !sent; ++i) {
                bsp_camera_frame_t frame = {};
                const esp_err_t frame_err = bsp_camera_get_frame(cam, &frame);
                if (frame_err == ESP_OK && frame.data != nullptr && frame.size > 0u) {
                    captured = true;
                    if (i >= 5) { // first frames: exposure/white-balance settling
                        ESP_LOGI(TAG, "SSTV camera frame: index=%d bytes=%u", i,
                                 static_cast<unsigned>(frame.size));
                        // Show the captured frame in the right-side preview
                        // window before returning the V4L2 buffer.
                        showSstvPreview(static_cast<const uint8_t *>(frame.data), frame.size);
                        sent = SSTV_SERVICE_SendJpegBuffer(
                            static_cast<const uint8_t *>(frame.data), frame.size,
                            s_sstv_tx_mode);
                    }
                    (void)bsp_camera_return_frame(cam, &frame);
                } else {
                    ESP_LOGW(TAG, "SSTV camera frame failed: index=%d err=%s data=%p size=%u",
                             i, esp_err_to_name(frame_err), frame.data,
                             static_cast<unsigned>(frame.size));
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            (void)bsp_camera_stop(cam);
        } else {
            ESP_LOGE(TAG, "SSTV camera start failed: %s", esp_err_to_name(start_err));
        }
        (void)bsp_camera_close(cam);
    } else {
        ESP_LOGE(TAG, "SSTV camera open failed: %s handle=%p", esp_err_to_name(open_err), cam);
    }
    if (!sent) {
        snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s",
                 tr(captured ? "camera TX queue failed" : "camera capture failed"));
        s_sstv_msg_ms = millis();
    }
    refreshSstvPage();
}

// ---- RX log (saved frames) ---------------------------------------------------

void scanSstvLogFiles()
{
    s_sstv_log_count = 0u;
    if (!STORAGE_SdMounted()) {
        return;
    }
    char directory[96];
    if (!SSTV_SERVICE_GetImageDirectory(directory, sizeof(directory))) {
        return;
    }
    DIR *dir = opendir(directory);
    if (dir == nullptr) {
        return;
    }
    while (s_sstv_log_count < kSstvLogMax) {
        const struct dirent *ent = readdir(dir);
        if (ent == nullptr) {
            break;
        }
        const size_t len = strlen(ent->d_name);
        if (len < 13u || len >= kSstvNameChars) {
            continue;
        }
        if (strncasecmp(ent->d_name, "sstv_rx_", 8u) != 0 ||
            strcasecmp(ent->d_name + len - 4u, ".jpg") != 0) {
            continue;
        }
        snprintf(s_sstv_log_files[s_sstv_log_count], kSstvNameChars, "%s", ent->d_name);
        ++s_sstv_log_count;
    }
    closedir(dir);
    // Insertion sort; the tick suffix is zero-padded by snprintf only per
    // value, so this is rough chronological order -- good enough for a log.
    for (size_t i = 1u; i < s_sstv_log_count; ++i) {
        char key[kSstvNameChars];
        snprintf(key, sizeof(key), "%s", s_sstv_log_files[i]);
        size_t j = i;
        while (j > 0u && strcmp(s_sstv_log_files[j - 1u], key) > 0) {
            snprintf(s_sstv_log_files[j], kSstvNameChars, "%s", s_sstv_log_files[j - 1u]);
            --j;
        }
        snprintf(s_sstv_log_files[j], kSstvNameChars, "%s", key);
    }
    if (s_sstv_log_index >= s_sstv_log_count) {
        s_sstv_log_index = 0u;
    }
}

void buildSstvLog()
{
    clearScreen();
    s_page = Page::Sstv;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    scanSstvLogFiles();

    lv_obj_t *box = panel(scr, 24, 76, 752, 288);
    fieldLabel(box, 0, 0, "RX LOG (/sdcard/sstv/sstv_rx_*.jpg)");
    constexpr size_t kRows = 8u;
    if (s_sstv_log_count == 0u) {
        lv_obj_t *lbl = label(box, &lv_font_montserrat_20, kColorSub);
        lv_obj_set_pos(lbl, 0, 40);
        lv_label_set_text(lbl, tr("no saved frames"));
    } else {
        const size_t start = s_sstv_log_index >= kRows ? s_sstv_log_index - kRows + 1u : 0u;
        for (size_t r = 0u; r < kRows && start + r < s_sstv_log_count; ++r) {
            const size_t i = start + r;
            lv_obj_t *row = label(box, &lv_font_montserrat_20,
                                  i == s_sstv_log_index ? kColorAccent : kColorText);
            lv_obj_set_pos(row, 0, 28 + static_cast<int>(r) * 27);
            char line[56];
            snprintf(line, sizeof(line), "%s %s", i == s_sstv_log_index ? ">" : " ",
                     s_sstv_log_files[i]);
            lv_label_set_text(row, line);
        }
    }
    button(scr, 24, 384, 120, 64, "Up", Action::SstvLogPrev);
    button(scr, 152, 384, 120, 64, "Down", Action::SstvLogNext);
    button(scr, 280, 384, 120, 64, "View", Action::SstvLogView);
    button(scr, 610, 384, 170, 64, "Back", Action::SstvLogBack);
}

void buildSstvLogView()
{
    clearScreen();
    s_page = Page::Sstv;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    bool shown = false;
    if (s_sstv_log_count > 0u && STORAGE_SdMounted()) {
        char directory[96];
        char path[160];
        const bool directory_ok =
            SSTV_SERVICE_GetImageDirectory(directory, sizeof(directory));
        if (directory_ok) {
            snprintf(path, sizeof(path), "%s/%s", directory,
                     s_sstv_log_files[s_sstv_log_index]);
        }
        FILE *file = directory_ok ? fopen(path, "rb") : nullptr;
        if (file != nullptr && fseek(file, 0, SEEK_END) == 0) {
            const long size = ftell(file);
            uint8_t *jpeg = static_cast<uint8_t *>(heap_caps_malloc(
                static_cast<size_t>(size > 0 ? size : 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (jpeg != nullptr && size > 0 && fseek(file, 0, SEEK_SET) == 0 &&
                fread(jpeg, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
                shown = COVER_DecodeJpeg(jpeg, static_cast<size_t>(size), 480u,
                                         &s_sstv_log_bmp);
            }
            if (jpeg != nullptr) {
                heap_caps_free(jpeg);
            }
            fclose(file);
        } else if (file != nullptr) {
            fclose(file);
        }
    }
    if (shown && s_sstv_log_bmp.rgb565 != nullptr) {
        s_img_sstv_log = lv_image_create(scr);
        lv_obj_set_size(s_img_sstv_log, s_sstv_log_bmp.width, s_sstv_log_bmp.height);
        lv_obj_set_pos(s_img_sstv_log, (kWidth - s_sstv_log_bmp.width) / 2, 76);
        memset(&s_sstv_log_dsc, 0, sizeof(s_sstv_log_dsc));
        s_sstv_log_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_sstv_log_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_sstv_log_dsc.header.w = s_sstv_log_bmp.width;
        s_sstv_log_dsc.header.h = s_sstv_log_bmp.height;
        s_sstv_log_dsc.header.stride = static_cast<uint32_t>(s_sstv_log_bmp.width) * 2u;
        s_sstv_log_dsc.data = s_sstv_log_bmp.rgb565;
        s_sstv_log_dsc.data_size = s_sstv_log_bmp.bytes;
        lv_image_set_src(s_img_sstv_log, &s_sstv_log_dsc);
        lv_obj_t *name = label(scr, &lv_font_montserrat_16, kColorSub);
        lv_obj_set_pos(name, 24, 360);
        lv_label_set_text(name, s_sstv_log_files[s_sstv_log_index]);
    } else {
        lv_obj_t *lbl = label(scr, &lv_font_montserrat_20, kColorSub);
        lv_obj_set_pos(lbl, 24, 96);
        lv_label_set_text(lbl, tr("decode failed"));
    }
    button(scr, 610, 396, 170, 68, "Back", Action::SstvLogBack);
}

void refreshSstvPage()
{
    if (s_sstv_lbl_tx == nullptr) {
        return;
    }
    SstvSnapshot snap{};
    SSTV_SERVICE_GetSnapshot(&snap);
    char text[160];

    if (!STORAGE_SdMounted()) {
        snprintf(text, sizeof(text), "%s", tr("no TF card"));
    } else if (s_sstv_file_count == 0u) {
        snprintf(text, sizeof(text), "%s", tr("no .jpg in /sstv"));
    } else {
        snprintf(text, sizeof(text), "%u/%u %s", static_cast<unsigned>(s_sstv_file_index + 1u),
                 static_cast<unsigned>(s_sstv_file_count), s_sstv_files[s_sstv_file_index]);
    }
    setLabelTextIfChanged(s_sstv_lbl_file, text);

    if (snap.state == SSTV_STATE_PREPARING) {
        snprintf(text, sizeof(text), "%s", tr("TX preparing..."));
    } else if (snap.state == SSTV_STATE_SENDING) {
        snprintf(text, sizeof(text), tr("TX sending %u%%"),
                 static_cast<unsigned>(snap.progress_percent));
    } else if (snap.state == SSTV_STATE_DONE) {
        snprintf(text, sizeof(text), "%s", tr("TX done"));
    } else if (snap.state == SSTV_STATE_ERROR) {
        snprintf(text, sizeof(text), tr("TX error: %s"), snap.error);
    } else {
        snprintf(text, sizeof(text), tr("TX idle (%s)"), sstvModeName(s_sstv_tx_mode));
    }
    setLabelTextIfChanged(s_sstv_lbl_tx, text);

    if (!snap.rx_active) {
        snprintf(text, sizeof(text), "%s", tr("RX off"));
    } else {
        const char *state = snap.rx_state == SSTV_RX_VIS ? "VIS" :
                            snap.rx_state == SSTV_RX_LINES ? "RX" :
                            snap.rx_state == SSTV_RX_DONE ? "DONE" : "LISTEN";
        snprintf(text, sizeof(text), "RX %s  %s  q%u%%", state,
                 snap.rx_state >= SSTV_RX_LINES
                     ? (snap.rx_mode == SSTV_MODE_ROBOT36 ? "ROBOT36" : "MARTIN1")
                     : "-",
                 static_cast<unsigned>(snap.rx_quality));
    }
    setLabelTextIfChanged(s_sstv_lbl_rx1, text);

    // Transient message (save result, send error) wins for a few seconds.
    if (s_sstv_msg[0] != '\0' && millis() - s_sstv_msg_ms < 4000u) {
        snprintf(text, sizeof(text), "%s", s_sstv_msg);
    } else {
        s_sstv_msg[0] = '\0';
        snprintf(text, sizeof(text), "lines %u/%u", static_cast<unsigned>(snap.rx_lines),
                 static_cast<unsigned>(snap.rx_lines_total));
    }
    setLabelTextIfChanged(s_sstv_lbl_rx2, text);

    setLabelTextIfChanged(s_sstv_mode_label, sstvModeName(s_sstv_tx_mode));
    setLabelTextIfChanged(s_sstv_src_label,
                          s_sstv_rx_source == SSTV_SOURCE_MIC ? "Src:MIC" : "Src:NRL");
    setLabelTextIfChanged(s_sstv_rx_toggle_label, snap.rx_active ? tr("Stop") : tr("Start"));

    if (snap.rx_revision != s_sstv_img_rev) {
        s_sstv_img_rev = snap.rx_revision;
        if (s_img_sstv != nullptr) {
            if (s_sstv_tx_preview && s_sstv_dsc.data != nullptr) {
                // A fresh RX frame arrived: hand the window back to the live
                // decoder buffer (the preview bitmap stays for later reuse).
                lv_image_set_src(s_img_sstv, &s_sstv_dsc);
                s_sstv_tx_preview = false;
            }
            lv_obj_invalidate(s_img_sstv); // decoder wrote new lines in place
        }
    }
    snprintf(text, sizeof(text), "frame %u/%u", static_cast<unsigned>(snap.rx_lines),
             static_cast<unsigned>(snap.rx_lines_total));
    setLabelTextIfChanged(s_sstv_lbl_img, text);
}

void buildSstv()
{
    COVER_Free(&s_sstv_log_bmp); // the log viewer owns one decoded frame
    s_img_sstv_log = nullptr;
    COVER_Free(&s_sstv_tx_bmp); // the preview window's bitmap is rebuilt below
    s_sstv_tx_preview = false;
    if (s_sstv_view == 1u) {
        buildSstvLog();
        return;
    }
    if (s_sstv_view == 2u) {
        buildSstvLogView();
        return;
    }
    clearScreen();
    s_page = Page::Sstv;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    lv_obj_t *tx = panel(scr, 24, 76, 420, 176);
    fieldLabel(tx, 0, 0, "TX IMAGE (TF -> air)");
    s_sstv_lbl_file = label(tx, &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_sstv_lbl_file, 0, 24);
    lv_obj_set_width(s_sstv_lbl_file, 380);
    lv_label_set_long_mode(s_sstv_lbl_file, LV_LABEL_LONG_DOT);
    button(tx, 0, 56, 64, 44, "<", Action::SstvFilePrev);
    button(tx, 72, 56, 64, 44, ">", Action::SstvFileNext);
    button(tx, 144, 56, 72, 44, "CAM", Action::SstvCam);
    lv_obj_t *mode_btn = button(tx, 224, 56, 96, 44, "ROBOT36", Action::SstvTxMode);
    s_sstv_mode_label = lv_obj_get_child(mode_btn, 0);
    button(tx, 328, 56, 60, 44, "Send", Action::SstvSend);
    s_sstv_lbl_tx = label(tx, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_sstv_lbl_tx, 0, 112);
    lv_obj_set_width(s_sstv_lbl_tx, 380);

    lv_obj_t *rx = panel(scr, 24, 264, 420, 192);
    fieldLabel(rx, 0, 0, "RX RECEIVE");
    lv_obj_t *src_btn = button(rx, 0, 24, 100, 44, "Src:MIC", Action::SstvRxSource);
    s_sstv_src_label = lv_obj_get_child(src_btn, 0);
    lv_obj_t *toggle_btn = button(rx, 108, 24, 100, 44, "Start", Action::SstvRxToggle);
    s_sstv_rx_toggle_label = lv_obj_get_child(toggle_btn, 0);
    button(rx, 216, 24, 84, 44, "Save", Action::SstvSave);
    button(rx, 308, 24, 80, 44, "Log", Action::SstvLog);
    s_sstv_lbl_rx1 = label(rx, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_sstv_lbl_rx1, 0, 80);
    lv_obj_set_width(s_sstv_lbl_rx1, 380);
    s_sstv_lbl_rx2 = label(rx, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_sstv_lbl_rx2, 0, 108);
    lv_obj_set_width(s_sstv_lbl_rx2, 380);

    // Live frame view. The buffer is owned by the service and mutated by the
    // decoder; the descriptor is bound once here.
    const uint16_t *frame = SSTV_SERVICE_RxImage();
    s_img_sstv = lv_image_create(scr);
    lv_obj_set_pos(s_img_sstv, 460, 76);
    lv_obj_set_size(s_img_sstv, 320, 256);
    lv_obj_set_style_bg_color(s_img_sstv, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_opa(s_img_sstv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_img_sstv, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_img_sstv, 1, 0);
    if (frame != nullptr) {
        memset(&s_sstv_dsc, 0, sizeof(s_sstv_dsc));
        s_sstv_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_sstv_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_sstv_dsc.header.w = 320;
        s_sstv_dsc.header.h = 256;
        s_sstv_dsc.header.stride = 320u * 2u;
        s_sstv_dsc.data = reinterpret_cast<const uint8_t *>(frame);
        s_sstv_dsc.data_size = 320u * 256u * 2u;
        lv_image_set_src(s_img_sstv, &s_sstv_dsc);
    }
    s_sstv_img_rev = SSTV_SERVICE_RxImageRevision();
    s_sstv_lbl_img = label(scr, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_sstv_lbl_img, 460, 340);
    lv_obj_set_width(s_sstv_lbl_img, 320);

    button(scr, 610, 396, 170, 68, "Back", Action::Apps);
    scanSstvFiles();
    refreshSstvPage();
    showSstvFilePreview(); // prime the right-side window with the TX image
}

void buildAprs()
{
    clearScreen();
    s_page = Page::Aprs;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    lv_obj_t *box = panel(scr, 24, 82, 750, 270);
    fieldLabel(box, 0, 0, "APRS Transceiver");
    s_sw_aprs = lv_switch_create(box);
    lv_obj_set_pos(s_sw_aprs, 640, -6);
    lv_obj_set_size(s_sw_aprs, 64, 34);
    if (APRS_SERVICE_IsEnabled()) {
        lv_obj_add_state(s_sw_aprs, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sw_aprs, aprsEnableEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lbl_aprs_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_aprs_status, 0, 30);
    lv_obj_set_width(s_lbl_aprs_status, 710);
    lv_label_set_long_mode(s_lbl_aprs_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_aprs_status, "--");

    s_lbl_aprs_list = label(box, &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_lbl_aprs_list, 0, 60);
    lv_obj_set_width(s_lbl_aprs_list, 710);
    lv_label_set_long_mode(s_lbl_aprs_list, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_lbl_aprs_list, "--");

    s_lbl_form_status = label(scr, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 24, 452);
    lv_obj_set_width(s_lbl_form_status, 750);
    lv_label_set_long_mode(s_lbl_form_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_form_status,
                      tr("Server / position: web portal APRS page or AT+APRS."));

    button(scr, 24, 372, 118, 76, "Back", Action::Apps);
    button(scr, 150, 372, 126, 76, "Beacon", Action::AprsBeacon);
    button(scr, 284, 372, 92, 76, "Msg", Action::AprsMsgOpen);
    button(scr, 384, 372, 84, 76, "GW", Action::AprsGatewayOpen);

    // NRL network audio toggles: the button label mirrors the live state.
    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);
    char toggle_text[24];
    lv_obj_t *btn_nrl_tx = button(scr, 476, 372, 140, 76, "", Action::AprsNrlTx);
    snprintf(toggle_text, sizeof(toggle_text), "NRL TX:%s",
             cfg.nrl_tx_enabled ? "on" : "off");
    lv_label_set_text(lv_obj_get_child(btn_nrl_tx, 0), toggle_text);
    lv_obj_t *btn_nrl_rx = button(scr, 624, 372, 150, 76, "", Action::AprsNrlRx);
    snprintf(toggle_text, sizeof(toggle_text), "NRL RX:%s",
             cfg.nrl_rx_enabled ? "on" : "off");
    lv_label_set_text(lv_obj_get_child(btn_nrl_rx, 0), toggle_text);

    s_aprs_seen_revision = APRS_SERVICE_GetStationRevision() - 1u; // force fill
    s_aprs_last_refresh_ms = 0u;
    refreshAprsPage();
}

void buildAprsMsg()
{
    clearScreen();
    s_page = Page::AprsMsg;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    lv_obj_t *box = panel(scr, 24, 82, 750, 270);
    fieldLabel(box, 0, 0, "Send APRS Message");

    fieldLabel(box, 0, 34, "Addressee callsign (A-Z 0-9 -)");
    s_ta_aprs_dest = textArea(box, 0, 58, 320, "BI4UMD-9", s_aprs_msg_dest, 9,
                              false, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "abcdefghijklmnopqrstuvwxyz0123456789-",
                              false);

    fieldLabel(box, 344, 34, "Message text (max 67 bytes)");
    s_ta_aprs_text = textArea(box, 344, 58, 396, "Hello", s_aprs_msg_text, 67,
                              false, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 0, 130);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_label_set_long_mode(s_lbl_form_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_form_status,
                      tr("Goes out over APRS-IS when linked, and as AFSK audio on every enabled path (RF speaker / NRL network)."));

    button(scr, 24, 372, 230, 76, "Back", Action::Aprs);
    button(scr, 544, 372, 230, 76, "Send", Action::AprsMsgSend);
    createKeyboard(scr);
}

// APRS gateway page: one toggle button per forwarding direction. The label
// mirrors the live state; tapping rebuilds the page and reports the change.
void buildAprsGateway()
{
    clearScreen();
    s_page = Page::AprsGateway;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    lv_obj_t *box = panel(scr, 24, 82, 750, 270);
    fieldLabel(box, 0, 0, "APRS Gateway Forwarding");

    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);

    struct FwdBtn {
        int x;
        int y;
        AprsFwdDir dir;
        const char *name;
        Action act;
    };
    static const FwdBtn fwd_btns[6] = {
        {0, 34, APRS_FWD_RF_TO_IS, "RF->IS", Action::AprsFwdRfIs},
        {255, 34, APRS_FWD_IS_TO_RF, "IS->RF", Action::AprsFwdIsRf},
        {510, 34, APRS_FWD_NRL_TO_IS, "NRL->IS", Action::AprsFwdNrlIs},
        {0, 114, APRS_FWD_IS_TO_NRL, "IS->NRL", Action::AprsFwdIsNrl},
        {255, 114, APRS_FWD_RF_TO_NRL, "RF->NRL", Action::AprsFwdRfNrl},
        {510, 114, APRS_FWD_NRL_TO_RF, "NRL->RF", Action::AprsFwdNrlRf},
    };
    char text[24];
    for (size_t i = 0; i < 6u; ++i) {
        lv_obj_t *btn = button(box, fwd_btns[i].x, fwd_btns[i].y, 230, 70, "",
                               fwd_btns[i].act);
        snprintf(text, sizeof(text), "%s:%s", fwd_btns[i].name,
                 cfg.fwd[fwd_btns[i].dir] ? "on" : "off");
        lv_label_set_text(lv_obj_get_child(btn, 0), text);
    }

    lv_obj_t *hint = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(hint, 0, 200);
    lv_obj_set_width(hint, 710);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint,
                      tr("AFSK relay directions also need the matching TX route on (RF Transmit / NRL Transmit)."));

    s_lbl_form_status = label(scr, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 24, 452);
    lv_obj_set_width(s_lbl_form_status, 750);
    lv_label_set_long_mode(s_lbl_form_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_form_status, "--");

    button(scr, 24, 372, 230, 76, "Back", Action::Aprs);
}

void aprsFwdToggle(AprsFwdDir dir, const char *name)
{
    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);
    if (APRS_SERVICE_SetFwdEnabled(dir, !cfg.fwd[dir])) {
        buildAprsGateway();
        char msg[64];
        snprintf(msg, sizeof(msg), "%s gateway %s.", name,
                 cfg.fwd[dir] ? "off" : "on");
        formStatus(msg, kColorSub);
    }
}

// ---- Games ------------------------------------------------------------------

void gameExit()
{
    // Called by the game's Back button; return to the Apps page.
    buildApps();
    s_last_refresh_ms = 0;
}

void buildGame()
{
    clearScreen();
    s_page = Page::Game;
    lv_obj_t *scr = lv_screen_active();
    // Keep the shared status bar (clock / caller / volume / CPU) visible in
    // the game too; the board layout starts at y=56, right below it.
    topBar(scr);
    GAME_TETRIS_Build(scr, gameExit);
}

void updateAudioValueLabels()
{
    if (s_slider_speaker != nullptr && s_lbl_speaker_value != nullptr) {
        char text[16] = {};
        snprintf(text, sizeof(text), "%ld%%", static_cast<long>(lv_slider_get_value(s_slider_speaker)));
        lv_label_set_text(s_lbl_speaker_value, text);
    }
    if (s_slider_mic != nullptr && s_lbl_mic_value != nullptr) {
        char text[16] = {};
        snprintf(text, sizeof(text), "%ld", static_cast<long>(lv_slider_get_value(s_slider_mic)));
        lv_label_set_text(s_lbl_mic_value, text);
    }
}

void markAudioChanged()
{
    updateAudioValueLabels();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, tr("Changed. Tap Save to persist."));
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

void setVolumePercentDelta(int pct_delta)
{
    // While a Bluetooth headset is connected, the volume keys drive the headset's
    // own speaker volume (one HFP step per press), not the onboard codec. The
    // top-bar readout (refreshVolume) follows the headset value.
    if (NRL_BtHfp_IsConnected()) {
        NRL_BtHfp_AdjustVolume(pct_delta);
        s_last_refresh_ms = 0;
        return;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int pct = (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255;
    pct += pct_delta;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    const int volume = (pct * 255 + 50) / 100;
    if (volume != static_cast<int>(cfg->line_out_volume)) {
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
        s_volume_dirty = true;
        s_volume_change_ms = millis();
        s_last_refresh_ms = 0;
    }
}

void setSsidDelta(int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int ssid = static_cast<int>(cfg->callsign_ssid) + delta;
    if (ssid < 0) {
        ssid = 0;
    } else if (ssid > 255) {
        ssid = 255;
    }
    if (ssid != static_cast<int>(cfg->callsign_ssid)) {
        EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), true);
        if (s_ta_callsign_ssid != nullptr) {
            char text[8] = {};
            snprintf(text, sizeof(text), "%d", ssid);
            lv_textarea_set_text(s_ta_callsign_ssid, text);
        }
    }
}

void wifiScanTask(void *)
{
    const bool ok = nrlWifiScanStartBlocking(6000u);
    s_wifi_scan_ok = ok;
    s_wifi_scan_complete = true;
    s_wifi_scan_running = false;
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void scanWifiForDropdown()
{
    if (s_wifi_scan_running) {
        if (s_lbl_form_status != nullptr) {
            lv_label_set_text(s_lbl_form_status, tr("Scanning WiFi..."));
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
        }
        return;
    }

    s_wifi_scan_complete = false;
    s_wifi_scan_ok = false;
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, tr("Scanning WiFi..."));
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
    }
    lv_timer_handler();

    s_wifi_scan_running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan_ui", 8192, nullptr, 4,
                                                       &s_wifi_scan_task, 0);
    if (created != pdPASS) {
        s_wifi_scan_task = nullptr;
        s_wifi_scan_running = false;
        s_wifi_scan_complete = false;
        if (s_lbl_form_status != nullptr) {
            lv_label_set_text(s_lbl_form_status, tr("Scan failed: task create failed."));
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
        }
    }
}

void pollWifiScan()
{
    if (!s_wifi_scan_complete) {
        return;
    }
    s_wifi_scan_complete = false;

    setWifiDropdownOptionsFromCache();
    if (s_lbl_form_status != nullptr) {
        if (s_wifi_scan_ok) {
            NrlWifiScanResult results[16] = {};
            const size_t got = nrlWifiScanGetCache(results, sizeof(results) / sizeof(results[0]));
            if (got > 0u && s_ta_wifi_ssid != nullptr && s_wifi_option_ssids[0][0] != '\0') {
                lv_textarea_set_text(s_ta_wifi_ssid, s_wifi_option_ssids[0]);
            }
            char text[48] = {};
            snprintf(text, sizeof(text), tr("Found %u WiFi networks."), static_cast<unsigned>(got));
            lv_label_set_text(s_lbl_form_status, text);
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorGood), 0);
        } else {
            lv_label_set_text(s_lbl_form_status, tr("Scan failed."));
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
        }
    }
    s_last_refresh_ms = 0;
}

void setOtaPageStatus(const char *text, uint32_t color)
{
    if (s_lbl_form_status == nullptr) return;
    setLabel(s_lbl_form_status, s_shown_ota_status, sizeof(s_shown_ota_status), text);
    setLabelColor(s_lbl_form_status, s_ota_status_color, color);
}

bool saveOtaUrl(bool show_success)
{
    const char *url = (s_ta_ota_url != nullptr) ? lv_textarea_get_text(s_ta_ota_url) : "";
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        setOtaPageStatus(tr("Save failed: out of memory."), kColorBad);
        return false;
    }
    if (strcmp(url, ota->server_url) == 0) {
        if (show_success) setOtaPageStatus(tr("OTA server URL saved."), kColorGood);
        return true;
    }
    if (!OtaService_SetServerUrl(url)) {
        setOtaPageStatus(tr("Save failed: URL must start with http:// or https://."), kColorBad);
        return false;
    }
    s_ota_selected_index = 0u;
    s_shown_ota_release[0] = '\0';
    s_ota_check_pending = false;
    s_ota_update_pending = false;
    if (show_success) setOtaPageStatus(tr("OTA server URL saved."), kColorGood);
    return true;
}

void checkOtaFromPage()
{
    if (!saveOtaUrl(false)) return;
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        setOtaPageStatus(tr("Check failed: out of memory."), kColorBad);
        return;
    }
    if (!OtaService_CheckNow()) {
        setOtaPageStatus(tr("Check failed: configure the OTA server first."), kColorBad);
        return;
    }
    s_ota_check_baseline_ms = ota->last_check_ms;
    s_ota_check_pending = true;
    s_ota_update_pending = false;
    setOtaPageStatus(tr("Update check requested..."), kColorWarn);
}

void stepOtaRelease(bool newer)
{
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr || ota->release_count == 0u) return;
    if (newer) {
        if (s_ota_selected_index > 0u) --s_ota_selected_index;
    } else if (s_ota_selected_index + 1u < ota->release_count) {
        ++s_ota_selected_index;
    }
    s_shown_ota_release[0] = '\0';
    refreshOtaPage();
}

void installOtaFromPage()
{
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        setOtaPageStatus(tr("Install failed: out of memory."), kColorBad);
        return;
    }
    const char *url = (s_ta_ota_url != nullptr) ? lv_textarea_get_text(s_ta_ota_url) : "";
    if (strcmp(url, ota->server_url) != 0) {
        if (!saveOtaUrl(false)) return;
        setOtaPageStatus(tr("Install failed: check available versions first."), kColorWarn);
        return;
    }
    if (ota->release_count == 0u || s_ota_selected_index >= ota->release_count) {
        setOtaPageStatus(tr("Install failed: check available versions first."), kColorBad);
        return;
    }
    const char *version = ota->releases[s_ota_selected_index].version;
    if (strcmp(version, NRL_FIRMWARE_VERSION) == 0) {
        setOtaPageStatus(tr("Selected version is already installed."), kColorWarn);
        return;
    }
    if (!OtaService_UpdateVersion(version)) {
        setOtaPageStatus(tr("Install failed: check available versions first."), kColorBad);
        return;
    }
    s_ota_update_pending = true;
    s_ota_check_pending = false;
    setOtaPageStatus(tr("Firmware install requested..."), kColorWarn);
}

void saveWifiForm()
{
    const char *ssid = (s_ta_wifi_ssid != nullptr) ? lv_textarea_get_text(s_ta_wifi_ssid) : "";
    const char *pass = (s_ta_wifi_pass != nullptr) ? lv_textarea_get_text(s_ta_wifi_pass) : "";
    bool ok = EXTERNAL_RADIO_SetWifiSsid(ssid, false) &&
              EXTERNAL_RADIO_SetWifiPassword(pass, false) &&
              EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, ok ? "WiFi config saved." : "Save failed: SSID is required.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
}

void saveStationForm()
{
    const char *call = (s_ta_callsign != nullptr) ? lv_textarea_get_text(s_ta_callsign) : "";
    const char *ssid_text = (s_ta_callsign_ssid != nullptr) ? lv_textarea_get_text(s_ta_callsign_ssid) : "";
    const char *host = (s_ta_server_host != nullptr) ? lv_textarea_get_text(s_ta_server_host) : "";
    const char *port_text = (s_ta_server_port != nullptr) ? lv_textarea_get_text(s_ta_server_port) : "";
    char *end = nullptr;
    const unsigned long ssid = strtoul(ssid_text, &end, 10);
    const bool ssid_ok = ssid_text[0] != '\0' && end != ssid_text && *end == '\0' && ssid <= 255ul;
    end = nullptr;
    const unsigned long port = strtoul(port_text, &end, 10);
    const bool port_ok = port_text[0] != '\0' && end != port_text && *end == '\0' &&
                         port > 0ul && port <= 65535ul;
    bool ok = ssid_ok && port_ok &&
              EXTERNAL_RADIO_SetCallsign(call, false) &&
              EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), false) &&
              EXTERNAL_RADIO_SetServerHost(host, false) &&
              EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(port), false) &&
              EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        const char *msg = ok ? "Station config saved."
                             : (!ssid_ok ? "Save failed: SSID must be 0-255."
                                         : (!port_ok ? "Save failed: port must be 1-65535."
                                                     : "Save failed: check callsign and server."));
        lv_label_set_text(s_lbl_form_status, msg);
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
    s_last_refresh_ms = 0;
}

void saveAudioForm()
{
    const bool ok = EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, ok ? "Audio config saved." : "Save failed.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
}

bool parseHexField(lv_obj_t *textarea, unsigned long max_value, unsigned long *value)
{
    if (textarea == nullptr || value == nullptr) return false;
    const char *text = lv_textarea_get_text(textarea);
    if (text == nullptr || text[0] == '\0') return false;
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 16);
    if (end == text || *end != '\0' || parsed > max_value) return false;
    *value = parsed;
    return true;
}

void saveMdcForm()
{
    unsigned long unit_id = 0u;
    unsigned long opcode = 0u;
    unsigned long argument = 0u;
    const bool fields_ok = parseHexField(s_ta_mdc_unit_id, 0xFFFFul, &unit_id) &&
                           parseHexField(s_ta_mdc_opcode, 0xFFul, &opcode) &&
                           parseHexField(s_ta_mdc_argument, 0xFFul, &argument);
    const bool ok = fields_ok &&
        SIGNALING_SetMdcPacket(static_cast<uint8_t>(opcode),
                               static_cast<uint8_t>(argument),
                               static_cast<uint16_t>(unit_id));
    formStatus(ok ? "MDC packet saved; audio cache rebuilt."
                  : (fields_ok ? "MDC save failed: PSRAM cache allocation failed."
                               : "MDC save failed: enter valid hexadecimal values."),
               ok ? kColorGood : kColorBad);
}

void saveDtmfForm()
{
    const char *digits = s_ta_dtmf_digits != nullptr
        ? lv_textarea_get_text(s_ta_dtmf_digits) : "";
    const bool ok = SIGNALING_SetDtmfDigits(digits);
    formStatus(ok ? "DTMF ID saved; audio cache rebuilt."
                  : "DTMF save failed: enter 1-16 valid DTMF digits.",
               ok ? kColorGood : kColorBad);
}

void resetAudioForm()
{
    const bool ok = EXTERNAL_RADIO_ResetAudioConfig(true);
    if (ok) {
        buildAudio();
    } else if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, tr("Reset failed."));
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
    }
}

void saveSmbForm()
{
    const char *server = (s_ta_smb_server != nullptr) ? lv_textarea_get_text(s_ta_smb_server) : "";
    const char *share = (s_ta_smb_share != nullptr) ? lv_textarea_get_text(s_ta_smb_share) : "";
    const char *user = (s_ta_smb_user != nullptr) ? lv_textarea_get_text(s_ta_smb_user) : "";
    const char *pass = (s_ta_smb_pass != nullptr) ? lv_textarea_get_text(s_ta_smb_pass) : "";
    if (server[0] == '\0' && share[0] == '\0') {
        STORAGE_SmbClear();
        formStatus("SMB config cleared.", kColorGood);
        return;
    }
    if (STORAGE_SmbConfigure(server, share, user, pass)) {
        formStatus("SMB saved. Mounting in background...", kColorGood);
    } else {
        formStatus("Save failed: server and share are required.", kColorBad);
    }
}

void clearSmbForm()
{
    STORAGE_SmbClear();
    if (s_ta_smb_server != nullptr) lv_textarea_set_text(s_ta_smb_server, "");
    if (s_ta_smb_share != nullptr) lv_textarea_set_text(s_ta_smb_share, "");
    if (s_ta_smb_user != nullptr) lv_textarea_set_text(s_ta_smb_user, "");
    if (s_ta_smb_pass != nullptr) lv_textarea_set_text(s_ta_smb_pass, "");
    formStatus("SMB config cleared.", kColorGood);
}

void saveNannyForm()
{
    const char *path = (s_ta_beacon_path != nullptr) ? lv_textarea_get_text(s_ta_beacon_path) : "";
    const char *interval_text =
        (s_ta_beacon_interval != nullptr) ? lv_textarea_get_text(s_ta_beacon_interval) : "";
    char *end = nullptr;
    const unsigned long minutes = strtoul(interval_text, &end, 10);
    const bool interval_ok = interval_text[0] != '\0' && end != interval_text && *end == '\0' &&
                             minutes >= 1ul && minutes <= 1440ul;
    if (!interval_ok) {
        formStatus("Save failed: interval must be 1-1440 minutes.", kColorBad);
        return;
    }
    if (NANNY_SetBeacon(path, static_cast<uint32_t>(minutes))) {
        formStatus("Beacon armed.", kColorGood);
    } else {
        formStatus("Save failed: set the beacon file path.", kColorBad);
    }
}

void beaconOffForm()
{
    NANNY_DisableBeacon();
    if (s_ta_beacon_interval != nullptr) {
        lv_textarea_set_text(s_ta_beacon_interval, "");
    }
    formStatus("Beacon disabled.", kColorGood);
}

// Save the station URL; with `play` also tune in right away.
void saveRadioForm(const bool play)
{
    const char *url = (s_ta_radio_url != nullptr) ? lv_textarea_get_text(s_ta_radio_url) : "";
    if (!MUSIC_SetRadioUrl(url)) {
        formStatus("Save failed: URL must start with http:// or https://.", kColorBad);
        return;
    }
    if (!play) {
        formStatus("Station URL saved.", kColorGood);
        return;
    }
    if (url[0] == '\0') {
        formStatus("Set a station URL first.", kColorBad);
        return;
    }
    // Tune through the favorites service when the URL is a favorite so
    // next/prev keep stepping from the right entry.
    const int fav = RADIO_FAV_IndexOfUrl(url);
    const bool started = (fav >= 0) ? RADIO_FAV_PlayIndex(static_cast<size_t>(fav))
                                    : MUSIC_PlayFile(url);
    if (started) {
        formStatus("Tuning in...", kColorGood);
    } else {
        formStatus("Play failed.", kColorBad);
    }
}

// "+" button: store the name/URL fields as a favorite (updates the name in
// place when the URL is already listed), then rebuild so the dropdown shows it.
void radioFavAddForm()
{
    char name[RADIO_FAV_NAME_SIZE] = {};
    char url[RADIO_FAV_URL_SIZE] = {};
    snprintf(name, sizeof(name), "%s",
             (s_ta_radio_name != nullptr) ? lv_textarea_get_text(s_ta_radio_name) : "");
    snprintf(url, sizeof(url), "%s",
             (s_ta_radio_url != nullptr) ? lv_textarea_get_text(s_ta_radio_url) : "");
    const int existing = RADIO_FAV_IndexOfUrl(url);
    int slot = -1;
    if (!RADIO_FAV_Set(existing, name, url, &slot)) {
        if (existing < 0 && RADIO_FAV_Count() >= RADIO_FAV_MAX) {
            formStatus("Favorites list is full.", kColorBad);
        } else {
            formStatus("Add failed: URL must start with http:// or https://.", kColorBad);
        }
        return;
    }
    buildRadio();
    if (s_dd_radio_fav != nullptr && slot >= 0) {
        lv_dropdown_set_selected(s_dd_radio_fav, static_cast<uint32_t>(slot));
        loadRadioFavForm(static_cast<size_t>(slot));
    }
    formStatus("Favorite saved.", kColorGood);
}

// Trash button: delete the favorite selected in the dropdown.
void radioFavDelForm()
{
    if (RADIO_FAV_Count() == 0u) {
        formStatus("No favorites to delete.", kColorBad);
        return;
    }
    const uint32_t selected =
        (s_dd_radio_fav != nullptr) ? lv_dropdown_get_selected(s_dd_radio_fav) : 0u;
    if (!RADIO_FAV_Remove(selected)) {
        formStatus("Delete failed.", kColorBad);
        return;
    }
    buildRadio();
    formStatus("Favorite deleted.", kColorGood);
}

// < / > buttons: retune to the previous/next favorite immediately.
void radioFavStep(const bool next)
{
    if (!(next ? RADIO_FAV_Next() : RADIO_FAV_Prev())) {
        formStatus("No favorites to switch. Add one with +.", kColorBad);
        return;
    }
    buildRadio(); // refresh selection/fields; shows the Playing status line
}

// ---- Bluetooth headset page ------------------------------------------------

void btEnableEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    // Request the change (non-blocking -- the BT task does the slow stack work)
    // and persist the preference. Give immediate on-screen feedback so the user
    // knows the tap registered while the stack comes up/down in the background.
    EXTERNAL_RADIO_SetBtEnabled(checked, true);
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, checked ? "Turning Bluetooth on..."
                                                   : "Turning Bluetooth off...");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

void wifiEnableEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    // Master Wi-Fi switch. Off frees the shared radio for Bluetooth A2DP (music
    // to the headset) but stops the network radio-voice link -- a mode change.
    // The switch lives on the WiFi page (form status label); keep the BT-page
    // label fallback for safety.
    EXTERNAL_RADIO_SetWifiEnabled(checked, true);
    if (s_lbl_form_status != nullptr && s_page == Page::Wifi) {
        formStatus(checked ? "Wi-Fi on: network radio available."
                           : "Wi-Fi off: radio freed for Bluetooth A2DP music.",
                   kColorWarn);
    } else if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status,
                          checked ? "Wi-Fi on: network radio available."
                                  : "Wi-Fi off: radio freed for Bluetooth A2DP music.");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

// Row user-data encodes which list a row belongs to: saved rows are offset by
// kBtSavedTag, scanned rows carry their raw index.
constexpr intptr_t kBtSavedTag = 0x10000;

void btRowConnect(lv_event_t *event)
{
    const intptr_t v = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (v >= kBtSavedTag) {
        NRL_BtHfp_ConnectSaved(static_cast<size_t>(v - kBtSavedTag));
    } else {
        NRL_BtHfp_ConnectIndex(static_cast<size_t>(v));
    }
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, tr("Connecting..."));
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

void btRowForget(lv_event_t *event)
{
    const intptr_t v = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (v < kBtSavedTag) {
        return;  // only saved devices can be forgotten
    }
    NRL_BtHfp_RemoveSaved(static_cast<size_t>(v - kBtSavedTag));
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, tr("Removed saved headset."));
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorSub), 0);
    }
    // Don't rebuild the list from inside this row's own event; let refreshBtPage
    // pick up the count change on the next tick.
    s_last_refresh_ms = 0;
}

lv_obj_t *addBtRow(const char *icon, const char *text, intptr_t tag, bool saved)
{
    lv_obj_t *row = lv_list_add_button(s_list_bt, icon, text);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x274060), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(row, lv_color_hex(kColorText), 0);
    // Short click connects; long-press forgets (saved rows only).
    lv_obj_add_event_cb(row, btRowConnect, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(tag));
    if (saved) {
        lv_obj_add_event_cb(row, btRowForget, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void *>(tag));
    }
    return row;
}

// Rebuild the BT list: saved headsets first (tap = connect, long-press =
// forget), then freshly scanned devices not already saved. Tapping connects.
// (Dropdowns proved unreliable on this touch panel.)
void setBtDeviceList()
{
    if (s_list_bt == nullptr) {
        return;
    }
    lv_obj_clean(s_list_bt);
    const size_t saved = NRL_BtHfp_GetSavedCount();
    for (size_t i = 0; i < saved; ++i) {
        char name[32] = {};
        NRL_BtHfp_GetSavedName(i, name, sizeof(name));
        char row_txt[56];
        snprintf(row_txt, sizeof(row_txt), "%s  (saved)", name);
        addBtRow(LV_SYMBOL_BLUETOOTH, row_txt, kBtSavedTag + static_cast<intptr_t>(i), true);
    }
    const size_t n = NRL_BtHfp_GetDeviceCount();
    size_t scanned_shown = 0;
    for (size_t i = 0; i < n; ++i) {
        char name[32] = {};
        NRL_BtHfp_GetDeviceName(i, name, sizeof(name));
        // Skip ones already in the saved section (best-effort, by name).
        bool dup = false;
        for (size_t s = 0; s < saved; ++s) {
            char sname[32] = {};
            NRL_BtHfp_GetSavedName(s, sname, sizeof(sname));
            if (strcmp(sname, name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        addBtRow(LV_SYMBOL_BLUETOOTH, name, static_cast<intptr_t>(i), false);
        ++scanned_shown;
    }
    if (saved == 0u && scanned_shown == 0u) {
        lv_obj_t *row = lv_list_add_button(s_list_bt, nullptr,
                                           NRL_BtHfp_IsScanning() ? "Scanning..."
                                                                  : "No headsets -- tap Scan");
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_text_color(row, lv_color_hex(kColorSub), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

void buildBt()
{
    clearScreen();
    s_page = Page::Bt;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 270);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 6, "Bluetooth headset");
    s_sw_bt = lv_switch_create(box);
    lv_obj_set_pos(s_sw_bt, 280, 0);
    lv_obj_set_size(s_sw_bt, 64, 34);
    if (cfg != nullptr && cfg->bt_enabled) {
        lv_obj_add_state(s_sw_bt, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sw_bt, btEnableEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    // The master Wi-Fi radio switch lives on the WiFi settings page.
    fieldLabel(box, 0, 58, "Headsets: tap to connect, long-press a saved one to delete");
    s_list_bt = lv_list_create(box);
    lv_obj_set_pos(s_list_bt, 0, 82);
    lv_obj_set_size(s_list_bt, 710, 122);
    lv_obj_set_style_radius(s_list_bt, 6, 0);
    lv_obj_set_style_bg_color(s_list_bt, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(s_list_bt, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_list_bt, 1, 0);
    setBtDeviceList();

    s_lbl_bt_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_bt_status, 0, 216);
    lv_obj_set_size(s_lbl_bt_status, 710, 44);
    lv_obj_set_style_text_line_space(s_lbl_bt_status, 2, 0);
    lv_label_set_long_mode(s_lbl_bt_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_bt_status, tr("Turn on, Scan, then tap a headset. Saved ones reconnect automatically."));

    button(scr, 24, 372, 360, 76, "Back", Action::Config);
    button(scr, 414, 372, 360, 76, "Scan", Action::ScanBt);
}

void scanBtForDropdown()
{
    if (!NRL_BtHfp_IsEnabled()) {
        if (s_lbl_bt_status != nullptr) {
            lv_label_set_text(s_lbl_bt_status, tr("Turn Bluetooth on first."));
            lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
        }
        return;
    }
    NRL_BtHfp_StartScan();
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, tr("Scanning for headsets..."));
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
}

// Top-bar Bluetooth indicator: hidden when Bluetooth is off, grey when enabled
// but no headset is linked, and green with a headset glyph once a headset is
// connected (no checkmark).
void refreshBtTop()
{
    if (s_lbl_bt_top == nullptr) {
        return;
    }
    uint32_t color;
    const char *txt;
    if (!NRL_BtHfp_IsEnabled()) {
        color = kColorDim;
        txt = "";  // Bluetooth off: no icon at all.
    } else if (NRL_BtHfp_IsConnected()) {
        // Linked to a headset: green BT glyph plus a headset glyph.
        color = kColorGood;
        txt = LV_SYMBOL_BLUETOOTH " " LV_SYMBOL_AUDIO;
    } else {
        // Enabled but no headset linked yet: grey BT glyph only.
        color = kColorDim;
        txt = LV_SYMBOL_BLUETOOTH;
    }
    setLabel(s_lbl_bt_top, s_shown_bt_top, sizeof(s_shown_bt_top), txt);
    setLabelColor(s_lbl_bt_top, s_clr_bt_top, color);
}

void refreshBtPage()
{
    // Rebuild the row list when the saved/scanned counts or the scanning state
    // change (not every tick, so taps aren't disrupted mid-press).
    static size_t s_bt_last_key = SIZE_MAX;
    static bool s_bt_was_scanning = false;
    const size_t key = NRL_BtHfp_GetSavedCount() * 1000u + NRL_BtHfp_GetDeviceCount();
    const bool scanning = NRL_BtHfp_IsScanning();
    if (key != s_bt_last_key || scanning != s_bt_was_scanning) {
        s_bt_last_key = key;
        s_bt_was_scanning = scanning;
        setBtDeviceList();
    }
    if (s_lbl_bt_status == nullptr) {
        return;
    }
    char bt[96];
    uint32_t color = kColorSub;
    char peer[32] = {};
    if (NRL_BtHfp_TogglePending()) {
        // Mid-transition: the BT task is bringing the stack up/down.
        snprintf(bt, sizeof(bt), "Switching Bluetooth...");
        color = kColorWarn;
    } else if (!NRL_BtHfp_IsEnabled()) {
        snprintf(bt, sizeof(bt), "Bluetooth off.");
    } else if (NRL_BtHfp_IsAudioActive()) {
        NRL_BtHfp_GetPeerName(peer, sizeof(peer));
        snprintf(bt, sizeof(bt), "Voice on headset%s%s.", peer[0] ? ": " : "", peer);
        color = kColorGood;
    } else if (NRL_BtHfp_IsConnected()) {
        NRL_BtHfp_GetPeerName(peer, sizeof(peer));
        snprintf(bt, sizeof(bt), "Connected%s%s (opening audio)...", peer[0] ? ": " : "", peer);
        color = kColorGood;
    } else if (NRL_BtHfp_IsScanning()) {
        snprintf(bt, sizeof(bt), "Scanning for headsets...");
        color = kColorWarn;
    } else {
        snprintf(bt, sizeof(bt), "Scan, pick a headset, then Connect.");
    }
    lv_label_set_text(s_lbl_bt_status, bt);
    lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(color), 0);
}

void action(lv_event_t *event)
{
    const Action id = static_cast<Action>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    switch (id) {
        case Action::Provisioning: buildProvisioning(); break;
        case Action::Config: buildConfig(); break;
        case Action::Home: buildHome(); break;
        case Action::Wifi: buildWifi(); break;
        case Action::Station: buildStation(); break;
        case Action::Audio: buildAudio(); break;
        case Action::Bt: buildBt(); break;
        case Action::ScanBt: scanBtForDropdown(); break;
        case Action::VolumeDown: setVolumePercentDelta(-1); break;
        case Action::VolumeUp: setVolumePercentDelta(1); break;
        case Action::CallsignSsidDown: setSsidDelta(-1); break;
        case Action::CallsignSsidUp: setSsidDelta(1); break;
        case Action::ResetWifi: EXTERNAL_RADIO_ResetNetworkConfig(); break;
        case Action::ScanWifi: scanWifiForDropdown(); break;
        case Action::SaveWifi: saveWifiForm(); break;
        case Action::SaveStation: saveStationForm(); break;
        case Action::SaveAudio: saveAudioForm(); break;
        case Action::ResetAudio: resetAudioForm(); break;
        case Action::Music: buildMusic(); break;
        case Action::MusicToggle: musicToggle(); break;
        case Action::MusicNext: (void)PLAYLIST_Next(); break;
        case Action::MusicPrev: (void)PLAYLIST_Prev(); break;
        case Action::MusicMode: musicModeToggle(); break;
        case Action::MusicRescan:
            startMusicRescan();
            break;
        case Action::Radio: buildRadio(); break;
        case Action::Nanny: buildNanny(); break;
        case Action::Smb: buildSmb(); break;
        case Action::Apps: buildApps(); break;
        case Action::EspNow: buildEspNow(); break;
        case Action::Ctcss: buildCtcss(); break;
        case Action::Mdc: buildMdc(); break;
        case Action::Dtmf: buildDtmf(); break;
        case Action::SaveMdc: saveMdcForm(); break;
        case Action::SaveDtmf: saveDtmfForm(); break;
        case Action::Video: buildVideo(); break;
        case Action::VideoTx: videoTxToggle(); break;
        case Action::Game: buildGame(); break;
        case Action::Ai: buildAi(); break;
        case Action::Aprs:
            if (s_page == Page::AprsMsg) {
                // Leaving the message editor: keep the draft fields.
                if (s_ta_aprs_dest != nullptr) {
                    snprintf(s_aprs_msg_dest, sizeof(s_aprs_msg_dest), "%s",
                             lv_textarea_get_text(s_ta_aprs_dest));
                }
                if (s_ta_aprs_text != nullptr) {
                    snprintf(s_aprs_msg_text, sizeof(s_aprs_msg_text), "%s",
                             lv_textarea_get_text(s_ta_aprs_text));
                }
            }
            buildAprs();
            break;
        case Action::AprsBeacon: {
            const bool ok = APRS_SERVICE_SendBeaconNow();
            formStatus(ok ? "Beacon queued." : "Enable APRS first.",
                       ok ? kColorGood : kColorBad);
            break;
        }
        case Action::AprsNrlTx: {
            AprsConfig aprs_cfg;
            APRS_SERVICE_GetConfig(&aprs_cfg);
            if (APRS_SERVICE_SetNrlTxEnabled(!aprs_cfg.nrl_tx_enabled)) {
                buildAprs();
                formStatus(aprs_cfg.nrl_tx_enabled ? "NRL network TX off."
                                                   : "NRL network TX on: beacons/messages stream over the NRL voice uplink.",
                           kColorSub);
            }
            break;
        }
        case Action::AprsNrlRx: {
            AprsConfig aprs_cfg;
            APRS_SERVICE_GetConfig(&aprs_cfg);
            if (APRS_SERVICE_SetNrlRxEnabled(!aprs_cfg.nrl_rx_enabled)) {
                buildAprs();
                formStatus(aprs_cfg.nrl_rx_enabled ? "NRL network RX off."
                                                   : "NRL network RX on: decoding APRS from the NRL voice downlink.",
                           kColorSub);
            }
            break;
        }
        case Action::AprsMsgOpen: buildAprsMsg(); break;
        case Action::AprsMsgSend: {
            const char *dest = (s_ta_aprs_dest != nullptr)
                                   ? lv_textarea_get_text(s_ta_aprs_dest) : "";
            const char *text = (s_ta_aprs_text != nullptr)
                                   ? lv_textarea_get_text(s_ta_aprs_text) : "";
            snprintf(s_aprs_msg_dest, sizeof(s_aprs_msg_dest), "%s", dest);
            snprintf(s_aprs_msg_text, sizeof(s_aprs_msg_text), "%s", text);
            const bool ok = APRS_SERVICE_SendMessage(dest, text);
            buildAprs();
            formStatus(ok ? "Message queued."
                          : "Send failed: APRS off or invalid callsign/text.",
                       ok ? kColorGood : kColorBad);
            break;
        }
        case Action::AprsGatewayOpen: buildAprsGateway(); break;
        case Action::AprsFwdRfIs: aprsFwdToggle(APRS_FWD_RF_TO_IS, "RF -> APRS-IS"); break;
        case Action::AprsFwdIsRf: aprsFwdToggle(APRS_FWD_IS_TO_RF, "APRS-IS -> RF"); break;
        case Action::AprsFwdNrlIs: aprsFwdToggle(APRS_FWD_NRL_TO_IS, "NRL -> APRS-IS"); break;
        case Action::AprsFwdIsNrl: aprsFwdToggle(APRS_FWD_IS_TO_NRL, "APRS-IS -> NRL"); break;
        case Action::AprsFwdRfNrl: aprsFwdToggle(APRS_FWD_RF_TO_NRL, "RF -> NRL"); break;
        case Action::AprsFwdNrlRf: aprsFwdToggle(APRS_FWD_NRL_TO_RF, "NRL -> RF"); break;
        case Action::Map: buildMap(); break;
        case Action::MapZoomIn: mapZoomStep(1); break;
        case Action::MapZoomOut: mapZoomStep(-1); break;
        case Action::Sstv: s_sstv_view = 0u; buildSstv(); break;
        case Action::SstvTxMode:
            s_sstv_tx_mode = s_sstv_tx_mode == SSTV_MODE_ROBOT36 ? SSTV_MODE_MARTIN_M1 :
                             s_sstv_tx_mode == SSTV_MODE_MARTIN_M1 ? SSTV_MODE_PD120
                                                                   : SSTV_MODE_ROBOT36;
            refreshSstvPage();
            break;
        case Action::SstvFilePrev: stepSstvFile(-1); break;
        case Action::SstvFileNext: stepSstvFile(1); break;
        case Action::SstvSend: sendSstvFromPage(); break;
        case Action::SstvRxSource: {
            s_sstv_rx_source = s_sstv_rx_source == SSTV_SOURCE_MIC ? SSTV_SOURCE_NRL
                                                                   : SSTV_SOURCE_MIC;
            SstvSnapshot snap{};
            SSTV_SERVICE_GetSnapshot(&snap);
            if (snap.rx_active) { // retune the tap to the new source
                (void)SSTV_SERVICE_StartRx(s_sstv_rx_source);
            }
            refreshSstvPage();
            break;
        }
        case Action::SstvRxToggle: {
            SstvSnapshot snap{};
            SSTV_SERVICE_GetSnapshot(&snap);
            if (snap.rx_active) {
                (void)SSTV_SERVICE_StopRx();
            } else {
                if (!SSTV_SERVICE_StartRx(s_sstv_rx_source)) {
                    snprintf(s_sstv_msg, sizeof(s_sstv_msg), "%s", tr("RX start failed / TX busy"));
                    s_sstv_msg_ms = millis();
                }
            }
            refreshSstvPage();
            break;
        }
        case Action::SstvSave: saveSstvFromPage(); break;
        case Action::SstvCam: camSstvFromPage(); break;
        case Action::SstvLog:
            s_sstv_view = 1u;
            s_sstv_log_index = 0u;
            buildSstv();
            break;
        case Action::SstvLogPrev:
            if (s_sstv_log_count > 0u) {
                s_sstv_log_index =
                    (s_sstv_log_index + s_sstv_log_count - 1u) % s_sstv_log_count;
            }
            buildSstv();
            break;
        case Action::SstvLogNext:
            if (s_sstv_log_count > 0u) {
                s_sstv_log_index = (s_sstv_log_index + 1u) % s_sstv_log_count;
            }
            buildSstv();
            break;
        case Action::SstvLogView:
            if (s_sstv_log_count > 0u) {
                s_sstv_view = 2u;
            }
            buildSstv();
            break;
        case Action::SstvLogBack:
            s_sstv_view = s_sstv_view == 2u ? 1u : 0u; // viewer -> list -> main
            buildSstv();
            break;
        case Action::Cw: buildCw(); break;
        case Action::CwDelete: CW_SERVICE_Delete(); break;
        case Action::CwSend: (void)CW_SERVICE_Send(); break;
        case Action::CwMode: s_cw_paddle_mode = !s_cw_paddle_mode; buildCw(); break;
        case Action::CwPractice: {
            CwSnapshot cw{};
            CW_SERVICE_GetSnapshot(&cw);
            // Cycle OFF -> TX (key the shown letter) -> RX (copy the sounded one).
            const CwPracticeMode next =
                cw.practice_mode == CW_PRACTICE_OFF ? CW_PRACTICE_TX :
                cw.practice_mode == CW_PRACTICE_TX ? CW_PRACTICE_RX : CW_PRACTICE_OFF;
            CW_SERVICE_SetPracticeMode(next);
            buildCw();
            break;
        }
        case Action::CwReplay: CW_SERVICE_ReplayTarget(); break;
        case Action::CwScore: buildCwScore(); break;
        case Action::CwScoreSet: {
            CwSnapshot cw{};
            CW_SERVICE_GetSnapshot(&cw);
            const CwCharset next =
                cw.charset == CW_CHARSET_KOCH ? CW_CHARSET_LETTERS :
                cw.charset == CW_CHARSET_LETTERS ? CW_CHARSET_DIGITS :
                cw.charset == CW_CHARSET_DIGITS ? CW_CHARSET_CUSTOM : CW_CHARSET_KOCH;
            CW_SERVICE_SetCharset(next, nullptr);
            buildCwScore();
            break;
        }
        case Action::CwScoreAdapt: {
            CwSnapshot cw{};
            CW_SERVICE_GetSnapshot(&cw);
            CW_SERVICE_SetAdaptiveWpm(!cw.adaptive_wpm);
            buildCwScore();
            break;
        }
        case Action::About: buildAbout(); break;
        case Action::SaveOtaUrl: (void)saveOtaUrl(true); break;
        case Action::CheckOta: checkOtaFromPage(); break;
        case Action::OtaOlder: stepOtaRelease(false); break;
        case Action::OtaNewer: stepOtaRelease(true); break;
        case Action::InstallOta: installOtaFromPage(); break;
        case Action::SaveSmb: saveSmbForm(); break;
        case Action::ClearSmb: clearSmbForm(); break;
        case Action::SaveNanny: saveNannyForm(); break;
        case Action::BeaconOff: beaconOffForm(); break;
        case Action::SaveRadio: saveRadioForm(false); break;
        case Action::RadioPlay: saveRadioForm(true); break;
        case Action::RadioStop:
            MUSIC_Stop();
            formStatus("Stopped.", kColorSub);
            break;
        case Action::RadioFavAdd: radioFavAddForm(); break;
        case Action::RadioFavDel: radioFavDelForm(); break;
        case Action::RadioFavPrev: radioFavStep(false); break;
        case Action::RadioFavNext: radioFavStep(true); break;
    }
    // Changes made from this UI are already on screen (with their status
    // message); consume the bump so refresh() doesn't rebuild over them.
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
    s_last_refresh_ms = 0;
}

void volumeEvent(lv_event_t *event)
{
    const Action id = static_cast<Action>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int delta = (id == Action::VolumeUp) ? 1 : -1;
    setVolumePercentDelta(delta);
}

void textAreaEvent(lv_event_t *event)
{
    if (s_keyboard == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *ta = lv_event_get_target_obj(event);
    const bool number_keyboard = reinterpret_cast<intptr_t>(lv_event_get_user_data(event)) != 0;
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(s_keyboard, number_keyboard ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_keyboard, ta);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(s_keyboard, nullptr);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void wifiOptionEvent(lv_event_t *event)
{
    if (s_ta_wifi_ssid == nullptr) {
        return;
    }
    lv_obj_t *dd = lv_event_get_target_obj(event);
    const uint16_t index = lv_dropdown_get_selected(dd);
    if (index < kWifiOptionCount && s_wifi_option_ssids[index][0] != '\0') {
        lv_textarea_set_text(s_ta_wifi_ssid, s_wifi_option_ssids[index]);
    }
}

void audioSliderEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const AudioControl id =
        static_cast<AudioControl>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int value = static_cast<int>(lv_slider_get_value(obj));
    if (id == AudioControl::Speaker) {
        if (NRL_BtHfp_IsConnected()) {
            // Headset online: the Speaker slider drives its volume, saved live by
            // the BT module (no separate Save needed), so don't flag the form.
            NRL_BtHfp_SetVolumePercent(value);
            updateAudioValueLabels();
            return;
        }
        const int volume = (value * 255 + 50) / 100;
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
    } else if (id == AudioControl::Mic) {
        EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), false);
    }
    markAudioChanged();
}

void audioSwitchEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    const AudioControl id =
        static_cast<AudioControl>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    switch (id) {
        case AudioControl::Aec:
            EXTERNAL_RADIO_SetAecEnabled(checked, false);
            break;
        case AudioControl::Noise:
            EXTERNAL_RADIO_SetAiNoiseEnabled(checked, false);
            break;
        case AudioControl::MicHpf:
            EXTERNAL_RADIO_SetMicHpfEnabled(checked, false);
            break;
        case AudioControl::EspNow:
            // Persists immediately (own NVS entry), no Save step. Enabling
            // fails while WiFi is down -- revert the switch so it shows truth.
            if (ESPNOW_LINK_SetEnabled(checked)) {
                formStatus(checked ? "ESP-NOW intercom on." : "ESP-NOW intercom off.", kColorGood);
            } else {
                if (checked) {
                    lv_obj_remove_state(obj, LV_STATE_CHECKED);
                }
                formStatus("ESP-NOW enable failed (WiFi not started).", kColorBad);
            }
            s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
            return;
        case AudioControl::EspNowRx:
            // Persists immediately; RX is independent of the TX enable.
            ESPNOW_LINK_SetRxEnabled(checked);
            formStatus(checked ? "ESP-NOW receive on." : "ESP-NOW receive muted.", kColorGood);
            s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
            return;
        case AudioControl::EspNowOpus:
            // ESP-NOW TX codec, independent of the NRL codec. Opus pre-allocates
            // its codecs; on RAM shortfall it rolls back to G.711 -- revert the
            // switch so it shows truth.
            if (ESPNOW_LINK_SetTxCodec(checked ? 1u : 0u)) {
                formStatus(checked ? "ESP-NOW TX: Opus 16k wideband."
                                   : "ESP-NOW TX: G.711 8k.", kColorGood);
            } else {
                if (obj != nullptr) {
                    lv_obj_remove_state(obj, LV_STATE_CHECKED);
                }
                formStatus("Opus enable failed (out of memory); staying on G.711.", kColorBad);
            }
            s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
            return;
        case AudioControl::OpusCodec:
            // Applies + persists immediately (bridge NVS); RX auto-detects
            // both codecs so nothing else needs to change. Switching to Opus
            // pre-allocates its codecs; on RAM shortfall it rolls back to
            // G.711 -- revert the switch so it shows truth.
            if (NRLAudioBridge_SetVoiceCodec(checked ? 1u : 0u)) {
                formStatus(checked ? "TX voice: Opus 16k wideband (type 8)."
                                   : "TX voice: G.711 8k (type 1).", kColorGood);
            } else {
                if (obj != nullptr) {
                    lv_obj_remove_state(obj, LV_STATE_CHECKED);
                }
                formStatus("Opus enable failed (out of memory); staying on G.711.", kColorBad);
            }
            return;
        case AudioControl::CtcssRxMic:
        case AudioControl::CtcssRxNrl: {
            const SignalingRoute route = id == AudioControl::CtcssRxMic
                                             ? SIGNAL_ROUTE_RX_MIC : SIGNAL_ROUTE_RX_NRL;
            const bool ok = SIGNALING_SetCtcssRoute(route, checked);
            if (!ok) {
                if (checked) lv_obj_remove_state(obj, LV_STATE_CHECKED);
                else lv_obj_add_state(obj, LV_STATE_CHECKED);
            }
            formStatus(ok ? (checked ? "CTCSS detection enabled."
                                       : "CTCSS detection disabled.")
                          : "CTCSS setting save failed.",
                       ok ? kColorGood : kColorBad);
            return;
        }
        case AudioControl::MdcRxMic:
        case AudioControl::MdcRxNrl:
        case AudioControl::MdcTxNrl:
        case AudioControl::MdcTxSpeaker:
        case AudioControl::DtmfRxMic:
        case AudioControl::DtmfRxNrl:
        case AudioControl::DtmfTxNrl:
        case AudioControl::DtmfTxSpeaker: {
            const bool mdc = id >= AudioControl::MdcRxMic && id <= AudioControl::MdcTxSpeaker;
            const int base = mdc ? static_cast<int>(AudioControl::MdcRxMic)
                                 : static_cast<int>(AudioControl::DtmfRxMic);
            const SignalingRoute route = static_cast<SignalingRoute>(static_cast<int>(id) - base);
            const bool ok = mdc ? SIGNALING_SetMdcRoute(route, checked)
                                : SIGNALING_SetDtmfRoute(route, checked);
            if (!ok) {
                if (checked) lv_obj_remove_state(obj, LV_STATE_CHECKED);
                else lv_obj_add_state(obj, LV_STATE_CHECKED);
            }
            formStatus(ok ? (checked ? "Signaling route enabled." : "Signaling route disabled.")
                          : "Signaling setting save failed.", ok ? kColorGood : kColorBad);
            return;
        }
        default:
            break;
    }
    markAudioChanged();
}

// Music output-device dropdown (Apps page): applies + persists immediately.
void musicOutputEvent(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target_obj(event);
    MUSIC_SetOutput(static_cast<int>(lv_dropdown_get_selected(dd)));
    formStatus("Music output saved (applies from the next track).", kColorGood);
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
}

// Playback-target dropdown (Apps page): applies + persists immediately.
void musicTargetEvent(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target_obj(event);
    MUSIC_SetTarget(static_cast<int>(lv_dropdown_get_selected(dd)));
    formStatus("Playback target saved (applies from the next track).", kColorGood);
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
}

// Language dropdown (Config page): persist the choice and rebuild the page so
// every string re-renders in the new language immediately.
void languageEvent(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target_obj(event);
    setUiLang(static_cast<int>(lv_dropdown_get_selected(dd)));
    buildConfig();
    s_last_refresh_ms = 0;
}

// The Home "network" panel doubles as a hold-to-talk soft PTT: pressing anywhere
// in it keys up; releasing (or sliding a finger off) stops transmit.
void softPttEvent(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            STATUS_IO_SetSoftPtt(true);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            STATUS_IO_SetSoftPtt(false);
            break;
        default:
            return;
    }
    s_last_refresh_ms = 0;  // reflect the TX state on screen immediately
}

// Dedicated ESP-NOW hold-to-talk (bottom half of the split Home PTT).
void espnowPttEvent(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            ESPNOW_LINK_SetPtt(true);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            ESPNOW_LINK_SetPtt(false);
            break;
        default:
            return;
    }
    s_last_refresh_ms = 0;
}

void refreshClock()
{
    if (!s_time_sync_started && nrlWifiStaConnected()) {
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_time_sync_started = true;
    }

    char text[16];
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(text, sizeof(text), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(text, sizeof(text), "--:--:--");
    }
    setLabel(s_lbl_time, s_shown_time, sizeof(s_shown_time), text);
}

void refreshWifiBadge()
{
    char text[32];
    uint32_t color = kColorSub;
    if (nrlWifiStaConnected()) {
        wifi_ap_record_t ap = {};
        const bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        const int rssi = have_ap ? ap.rssi : 0;
        // Bare channel number after the RSSI (matches gezipai); shown because
        // ESP-NOW peers must share the WiFi channel.
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI " %ddB %u",
                 rssi, have_ap ? static_cast<unsigned>(ap.primary) : 0u);
        color = (rssi >= -65) ? kColorGood : ((rssi >= -78) ? kColorWarn : kColorBad);
    } else {
        uint8_t channel = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        (void)esp_wifi_get_channel(&channel, &second);
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI " AP %u",
                 static_cast<unsigned>(channel));
        color = kColorWarn;
    }
    if (setLabel(s_lbl_wifi, s_shown_wifi, sizeof(s_shown_wifi), text)) {
        lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(color), 0);
    }
}

// UTF-8 helpers for the now-playing ticker (titles are valid UTF-8 after the
// metadata GBK conversion).
size_t utf8GlyphCount(const char *s)
{
    size_t n = 0;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if ((static_cast<uint8_t>(s[i]) & 0xC0u) != 0x80u) {
            ++n;
        }
    }
    return n;
}

size_t utf8NextOffset(const char *s, size_t off)
{
    if (s[off] == '\0') {
        return 0;
    }
    ++off;
    while (s[off] != '\0' && (static_cast<uint8_t>(s[off]) & 0xC0u) == 0x80u) {
        ++off;
    }
    return (s[off] == '\0') ? 0 : off;
}

// Copy `glyphs` UTF-8 characters starting at byte `start`, wrapping around.
void tickerWindow(const char *src, size_t start, const size_t glyphs, char *out, const size_t cap)
{
    size_t pos = 0;
    size_t off = start;
    for (size_t g = 0; g < glyphs; ++g) {
        if (src[off] == '\0') {
            off = 0;
            if (src[0] == '\0') {
                break;
            }
        }
        const uint8_t b = static_cast<uint8_t>(src[off]);
        size_t clen = 1;
        if ((b & 0xE0u) == 0xC0u) clen = 2;
        else if ((b & 0xF0u) == 0xE0u) clen = 3;
        else if ((b & 0xF8u) == 0xF0u) clen = 4;
        if (pos + clen + 1u > cap) {
            break;
        }
        for (size_t k = 0; k < clen && src[off] != '\0'; ++k) {
            out[pos++] = src[off++];
        }
    }
    out[pos] = '\0';
}

void refreshStationBadges()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    char local[24] = {};
    formatStationBadge(local, sizeof(local), cfg ? cfg->callsign : nullptr,
                       cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0u);
    setLabel(s_lbl_local_station, s_shown_local_station, sizeof(s_shown_local_station), local);

    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    char remote[160] = {};
    bool now_playing = false;
    if (rx && voice_call[0] != '\0') {
        // A caller always wins the badge.
        formatStationBadge(remote, sizeof(remote), voice_call, voice_ssid);
    } else if (MUSIC_IsPlaying()) {
        // Idle badge doubles as a now-playing ticker (song / net radio /
        // nanny beacon). Manual one-glyph-per-tick scroll: an LVGL circular
        // scroll label would re-render the whole screen at animation rate in
        // FULL mode.
        constexpr size_t kTickerGlyphs = 7;
        const MediaTrackInfo *track = MUSIC_GetTrackInfo();
        const char *title = (track != nullptr && track->title[0] != '\0')
                                ? track->title
                                : musicBasename(MUSIC_CurrentPath());
        static char s_ticker_src[136] = {};
        static size_t s_ticker_off = 0;
        char padded[136];
        snprintf(padded, sizeof(padded), "%.100s | ", title);
        if (strcmp(padded, s_ticker_src) != 0) {
            snprintf(s_ticker_src, sizeof(s_ticker_src), "%s", padded);
            s_ticker_off = 0;
        }
        if (utf8GlyphCount(title) <= kTickerGlyphs) {
            snprintf(remote, sizeof(remote), LV_SYMBOL_AUDIO " %.100s", title);
        } else {
            char window[48];
            tickerWindow(s_ticker_src, s_ticker_off, kTickerGlyphs, window, sizeof(window));
            s_ticker_off = utf8NextOffset(s_ticker_src, s_ticker_off);
            snprintf(remote, sizeof(remote), "%s", window);
        }
        now_playing = true;
    } else {
        formatStationBadge(remote, sizeof(remote), nullptr, 0);
    }
    setLabel(s_lbl_remote_station, s_shown_remote_station, sizeof(s_shown_remote_station), remote);
    setLabelColor(s_lbl_remote_station, s_clr_remote_station,
                  rx ? kColorAccent : (now_playing ? kColorSub : kColorDim));
}

void refreshHome()
{
    // ESP-NOW got toggled while sitting on Home (config page, web, AT, or the
    // deferred boot restore): rebuild so the PTT area matches (single/split).
    if (ESPNOW_LINK_IsEnabled() != s_home_espnow_split) {
        buildHome();
        return;
    }

    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();
    const bool espnow_tx = ESPNOW_LINK_PttActive();
    const bool espnow_rx = ESPNOW_LINK_IsReceiving();

    // Mirror the physical PTT / ESP-NOW PTT state onto the on-screen buttons so
    // the user sees the same pressed highlight regardless of input source.
    if (s_btn_ptt != nullptr && tx != s_ptt_shown_pressed) {
        s_ptt_shown_pressed = tx;
        if (tx) lv_obj_add_state(s_btn_ptt, LV_STATE_PRESSED);
        else    lv_obj_remove_state(s_btn_ptt, LV_STATE_PRESSED);
    }
    if (s_btn_eptt != nullptr && espnow_tx != s_eptt_shown_pressed) {
        s_eptt_shown_pressed = espnow_tx;
        if (espnow_tx) lv_obj_add_state(s_btn_eptt, LV_STATE_PRESSED);
        else           lv_obj_remove_state(s_btn_eptt, LV_STATE_PRESSED);
    }
    char espnow_peer[16] = {};
    if (espnow_rx) {
        ESPNOW_LINK_GetLastPeer(espnow_peer, sizeof(espnow_peer));
    }

    // TX-codec tags on the PTT buttons: each link has its own codec switch.
    setLabel(s_lbl_ptt_codec, s_shown_ptt_codec, sizeof(s_shown_ptt_codec),
             (NRLAudioBridge_GetVoiceCodec() == 1u) ? "OPUS" : "G711");
    setLabel(s_lbl_eptt_codec, s_shown_eptt_codec, sizeof(s_shown_eptt_codec),
             (ESPNOW_LINK_GetTxCodec() == 1u) ? "OPUS" : "G711");

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // Main panel shows only the remote caller (NRL stream first, else the
    // ESP-NOW peer). Local station lives in the top bar.
    const bool has_caller = (rx && voice_call[0] != '\0');
    const bool has_espnow_caller = (!has_caller && espnow_rx && espnow_peer[0] != '\0');
    char call[24] = {};
    if (has_espnow_caller) {
        snprintf(call, sizeof(call), "%s", espnow_peer);
    } else {
        formatStationBadge(call, sizeof(call), has_caller ? voice_call : nullptr, voice_ssid);
    }
    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call);

    char dmrid[sizeof(s_shown_dmrid)] = {};
    const uint32_t remote_dmr_id = NRLAudioBridge_GetRemoteDmrId();
    if (rx && remote_dmr_id != 0u) {
        snprintf(dmrid, sizeof(dmrid), "DMRID %lu",
                 static_cast<unsigned long>(remote_dmr_id));
    }
    setLabel(s_lbl_dmrid, s_shown_dmrid, sizeof(s_shown_dmrid), dmrid);
    if (s_lbl_callsign != nullptr) {
        // Spread only the placeholder dashes so they read bigger; keep a real
        // callsign at normal spacing. Only re-apply on change (avoids a full
        // re-render every tick).
        const int letter_space = (has_caller || has_espnow_caller) ? 0 : 12;
        if (letter_space != s_ls_callsign) {
            s_ls_callsign = letter_space;
            lv_obj_set_style_text_letter_space(s_lbl_callsign, letter_space, 0);
        }
    }

    const char *caption = "STANDBY";
    uint32_t color = kColorDim;
    uint32_t call_color = kColorText;
    if (tx && rx) {
        caption = "FULL DUPLEX";
        color = kColorDuplex;
        call_color = kColorAccent;
    } else if (tx) {
        caption = "TRANSMITTING";
        color = kColorTx;
    } else if (espnow_tx) {
        // Dedicated ESP-NOW PTT held: purple, to match its button.
        caption = "ESP-NOW TX";
        color = kColorDuplex;
    } else if (rx) {
        caption = "RECEIVING";
        color = kColorAccent;
        call_color = kColorAccent;
    } else if (espnow_rx) {
        // Voice arriving over the off-grid ESP-NOW link (not the NRL server).
        caption = "ESP-NOW RX";
        color = kColorDuplex;
        call_color = kColorDuplex;
    } else {
        call_color = kColorDim;
    }
    if (setLabel(s_lbl_caption, s_shown_caption, sizeof(s_shown_caption), tr(caption))) {
        lv_obj_set_style_text_color(s_lbl_caption, lv_color_hex(color), 0);
    }
    setLabelColor(s_lbl_callsign, s_clr_callsign, call_color);

    // Last decoded signaling ticker (MDC/CTCSS/DTMF) below the callsign.
    {
        char sig[sizeof(s_shown_signaling)] = {};
        SIGNALING_GetLastResult(sig, sizeof(sig));
        setLabel(s_lbl_signaling, s_shown_signaling, sizeof(s_shown_signaling), sig);
    }

    // Source audio codec of the incoming stream (blank unless receiving): the
    // NRL network caller takes precedence, else the off-grid ESP-NOW peer.
    const char *rx_codec = "";
    if (has_caller) {
        rx_codec = (NRLAudioBridge_GetRxCodec() == 1u) ? "OPUS" : "G.711";
    } else if (has_espnow_caller) {
        rx_codec = (ESPNOW_LINK_GetRxCodec() == 1u) ? "OPUS" : "G.711";
    }
    setLabel(s_lbl_rx_codec, s_shown_rx_codec, sizeof(s_shown_rx_codec), rx_codec);

    char ip[96];
    uint32_t ip_color = kColorAccent;
    if (nrlWifiStaConnected()) {
        char buf[16] = {};
        nrlIpToString(nrlWifiStaIp(), buf, sizeof(buf));
        snprintf(ip, sizeof(ip), "%s", buf);
    } else {
        char buf[16] = {};
        nrlIpToString(nrlWifiApIp(), buf, sizeof(buf));
        snprintf(ip, sizeof(ip), "%s", buf[0] ? buf : "---");
        ip_color = kColorWarn;
    }
    if (setLabel(s_lbl_ip, s_shown_ip, sizeof(s_shown_ip), ip)) {
        lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(ip_color), 0);
    }

    char server[96];
    const char *host = (cfg && cfg->server_host[0]) ? cfg->server_host : "---";
    snprintf(server, sizeof(server), "%s", host);
    const uint32_t server_color = tx ? kColorTx : (rx ? kColorAccent : kColorSub);
    setLabel(s_lbl_server, s_shown_server, sizeof(s_shown_server), server);
    setLabelColor(s_lbl_server, s_clr_server, server_color);
}

// Per-core CPU load from the FreeRTOS idle-task runtime counters (delta over
// the ~500 ms refresh tick). Repaints only on a >=3% move: every label change
// forces a full-screen re-render in FULL mode.
void refreshCpu()
{
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (s_lbl_cpu == nullptr) {
        return;
    }
    static uint32_t s_last_idle[2] = {};
    static uint32_t s_last_us = 0;
    static int s_pct_shown[2] = {-100, -100};
    const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t idle[2] = {
        static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(0)),
        static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(1)),
    };
    const uint32_t dt = now_us - s_last_us; // wrap-safe unsigned math
    if (s_last_us != 0u && dt > 0u) {
        int pct[2];
        for (int core = 0; core < 2; ++core) {
            const uint32_t di = idle[core] - s_last_idle[core];
            const uint64_t idle_pct = static_cast<uint64_t>(di) * 100u / dt;
            long load = 100 - static_cast<long>(idle_pct);
            if (load < 0) load = 0;
            if (load > 100) load = 100;
            pct[core] = static_cast<int>(load);
        }
        const int d0 = pct[0] - s_pct_shown[0];
        const int d1 = pct[1] - s_pct_shown[1];
        if (d0 >= 3 || d0 <= -3 || d1 >= 3 || d1 <= -3) {
            s_pct_shown[0] = pct[0];
            s_pct_shown[1] = pct[1];
            char text[16];
            snprintf(text, sizeof(text), "%d/%d", pct[0], pct[1]);
            setLabel(s_lbl_cpu, s_shown_cpu, sizeof(s_shown_cpu), text);
            setLabelColor(s_lbl_cpu, s_clr_cpu,
                          (pct[0] > 85 || pct[1] > 85) ? kColorWarn : kColorSub);
        }
    }
    s_last_us = now_us;
    s_last_idle[0] = idle[0];
    s_last_idle[1] = idle[1];
#endif
}

// Volume now lives in the shared top bar, so refresh it on every page.
void refreshVolume()
{
    if (s_lbl_vol == nullptr) {
        return;
    }
    char vol[24];
    uint32_t color = kColorSub;
    const int bt_pct = NRL_BtHfp_IsConnected() ? NRL_BtHfp_GetVolumePercent() : -1;
    if (bt_pct >= 0) {
        // Headset connected: show its speaker volume (green, with the BT glyph).
        snprintf(vol, sizeof(vol), LV_SYMBOL_BLUETOOTH " %d%%", bt_pct);
        color = kColorGood;
    } else {
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        const int pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;
        snprintf(vol, sizeof(vol), LV_SYMBOL_VOLUME_MID " %d%%", pct);
    }
    if (setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol)) {
        lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(color), 0);
    }
}

void refreshDetailPage()
{
    if (s_lbl_detail == nullptr) {
        return;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    char text[512];
    if (s_page == Page::Wifi) {
        char sta[16] = {};
        char ap[16] = {};
        nrlIpToString(nrlWifiStaIp(), sta, sizeof(sta));
        nrlIpToString(nrlWifiApIp(), ap, sizeof(ap));
        snprintf(text, sizeof(text),
                 tr("SSID: %s\nMode: %s\nSTA IP: %s\nConfig AP: %s\n\nReset WiFi clears saved network credentials."),
                 (cfg && cfg->wifi_ssid[0]) ? cfg->wifi_ssid : "(not set)",
                 nrlWifiStaConnected() ? "STA connected" : "AP/config",
                 sta[0] ? sta : "---",
                 ap[0] ? ap : "192.168.4.1");
    } else if (s_page == Page::Station) {
        snprintf(text, sizeof(text),
                 tr("Callsign: %s\nSSID: %u\nServer: %s:%u\n\nEdit station and server settings on the Station page."),
                 (cfg && cfg->callsign[0]) ? cfg->callsign : "----",
                 cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0,
                 (cfg && cfg->server_host[0]) ? cfg->server_host : "---",
                 cfg ? static_cast<unsigned>(cfg->server_port) : 0);
    } else if (s_page == Page::Audio) {
        const int pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;
        snprintf(text, sizeof(text),
                 tr("Speaker volume: %d%%\nMic volume: %u\nAEC: %s\nNoise reduction: %s"),
                 pct,
                 cfg ? static_cast<unsigned>(cfg->mic_volume) : 0,
                 (cfg && cfg->aec_enabled) ? "on" : "off",
                 (cfg && cfg->ai_noise_enabled) ? "on" : "off");
    } else {
        return;
    }
    setLabel(s_lbl_detail, s_shown_detail, sizeof(s_shown_detail), text);
}

void refreshOtaPage()
{
    if (s_page != Page::About || s_lbl_ota_release == nullptr) return;

    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        setOtaPageStatus(tr("OTA status unavailable: out of memory."), kColorBad);
        return;
    }
    if (ota->checking) {
        s_ota_check_pending = false;
    } else if (s_ota_check_pending && ota->last_check_ms != s_ota_check_baseline_ms) {
        s_ota_check_pending = false;
    }
    if (ota->updating) {
        s_ota_update_pending = false;
    } else if (s_ota_update_pending && ota->last_error[0] != '\0') {
        s_ota_update_pending = false;
    }

    char selected[sizeof(s_shown_ota_release)] = {};
    if (ota->release_count == 0u) {
        s_ota_selected_index = 0u;
        snprintf(selected, sizeof(selected), "%s", tr("No versions available"));
    } else {
        if (s_ota_selected_index >= ota->release_count) s_ota_selected_index = 0u;
        snprintf(selected, sizeof(selected), tr("Selected: %s"),
                 ota->releases[s_ota_selected_index].version);
    }
    setLabel(s_lbl_ota_release, s_shown_ota_release, sizeof(s_shown_ota_release), selected);

    char status_text[sizeof(s_shown_ota_status)] = {};
    uint32_t status_color = kColorSub;
    if (ota->updating) {
        if (ota->update_size > 0u) {
            snprintf(status_text, sizeof(status_text), tr("Installing firmware... %u%%"),
                     static_cast<unsigned>(ota->update_percent));
        } else {
            snprintf(status_text, sizeof(status_text), "%s", tr("Installing firmware..."));
        }
        status_color = kColorWarn;
    } else if (ota->checking) {
        snprintf(status_text, sizeof(status_text), "%s", tr("Checking for updates..."));
        status_color = kColorWarn;
    } else if (s_ota_update_pending) {
        snprintf(status_text, sizeof(status_text), "%s", tr("Firmware install requested..."));
        status_color = kColorWarn;
    } else if (s_ota_check_pending) {
        snprintf(status_text, sizeof(status_text), "%s", tr("Update check requested..."));
        status_color = kColorWarn;
    } else if (ota->last_error[0] != '\0') {
        snprintf(status_text, sizeof(status_text), tr("OTA error: %s"), ota->last_error);
        status_color = kColorBad;
    } else if (!ota->configured) {
        snprintf(status_text, sizeof(status_text), "%s", tr("Set and save the OTA server URL."));
    } else if (ota->release_count == 0u) {
        snprintf(status_text, sizeof(status_text), "%s",
                 tr("Tap Check Update to query available versions."));
    } else {
        snprintf(status_text, sizeof(status_text), tr("Latest: %s  |  %u version(s) available"),
                 ota->latest_version[0] ? ota->latest_version : ota->releases[0].version,
                 static_cast<unsigned>(ota->release_count));
        status_color = (ota->latest_version[0] != '\0' &&
                        strcmp(ota->latest_version, NRL_FIRMWARE_VERSION) != 0)
                           ? kColorGood
                           : kColorSub;
    }
    setOtaPageStatus(status_text, status_color);

    const bool busy = ota->checking || ota->updating ||
                      s_ota_check_pending || s_ota_update_pending;
    auto set_disabled = [](lv_obj_t *obj, bool disabled) {
        if (obj == nullptr || lv_obj_has_state(obj, LV_STATE_DISABLED) == disabled) return;
        if (disabled) lv_obj_add_state(obj, LV_STATE_DISABLED);
        else lv_obj_remove_state(obj, LV_STATE_DISABLED);
    };
    set_disabled(s_btn_ota_check, busy);
    set_disabled(s_btn_ota_install, busy || ota->release_count == 0u);
}

void refreshDisplayNotice()
{
    DisplayNoticeSnapshot notice = {};
    DISPLAY_NOTICE_Get(&notice);
    const uint32_t now = millis();
    const bool active = notice.text[0] != '\0' &&
        (notice.duration_ms == 0u || now - notice.posted_ms < notice.duration_ms);
    if (!active) {
        if (s_notice_panel != nullptr) lv_obj_add_flag(s_notice_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    uint32_t color = kColorAccent;
    if (notice.level == DISPLAY_NOTICE_SUCCESS) color = kColorGood;
    else if (notice.level == DISPLAY_NOTICE_WARNING) color = kColorWarn;
    else if (notice.level == DISPLAY_NOTICE_ERROR) color = kColorBad;

    if (s_notice_panel == nullptr) {
        lv_obj_t *scr = lv_screen_active();
        s_notice_panel = panel(scr, 120, 62, 560, 52);
        lv_obj_set_style_bg_color(s_notice_panel, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_border_width(s_notice_panel, 2, 0);
        s_lbl_notice = label(s_notice_panel, &lv_font_montserrat_20, color);
        lv_obj_set_width(s_lbl_notice, 526);
        lv_obj_set_style_text_align(s_lbl_notice, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_lbl_notice);
        s_bar_notice_progress = lv_bar_create(s_notice_panel);
        lv_obj_set_size(s_bar_notice_progress, 526, 10);
        lv_bar_set_range(s_bar_notice_progress, 0, 100);
        lv_bar_set_value(s_bar_notice_progress, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(s_bar_notice_progress, 5, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar_notice_progress, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_bar_notice_progress, lv_color_hex(kColorBorder), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar_notice_progress, lv_color_hex(color), LV_PART_INDICATOR);
        lv_obj_add_flag(s_bar_notice_progress, LV_OBJ_FLAG_HIDDEN);
        s_notice_sequence = 0u;
        s_notice_color = kColorUnset;
    }
    lv_obj_remove_flag(s_notice_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_notice_panel);
    if (notice.progress_percent >= 0 && s_bar_notice_progress != nullptr) {
        lv_obj_set_size(s_notice_panel, 560, 78);
        lv_obj_set_pos(s_lbl_notice, 0, -2);
        lv_obj_set_pos(s_bar_notice_progress, 0, 30);
        lv_bar_set_value(s_bar_notice_progress, notice.progress_percent, LV_ANIM_OFF);
        lv_obj_remove_flag(s_bar_notice_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_size(s_notice_panel, 560, 52);
        lv_obj_center(s_lbl_notice);
        if (s_bar_notice_progress != nullptr) {
            lv_obj_add_flag(s_bar_notice_progress, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_notice_sequence != notice.sequence) {
        s_notice_sequence = notice.sequence;
        if (notice.progress_percent >= 0) {
            char progress_text[40] = {};
            snprintf(progress_text, sizeof(progress_text), tr("OTA Updating %u%%"),
                     static_cast<unsigned>(notice.progress_percent));
            setLabel(s_lbl_notice, s_shown_notice, sizeof(s_shown_notice), progress_text);
        } else {
            setLabel(s_lbl_notice, s_shown_notice, sizeof(s_shown_notice), tr(notice.text));
        }
    }
    if (s_notice_color != color) {
        setLabelColor(s_lbl_notice, s_notice_color, color);
        lv_obj_set_style_border_color(s_notice_panel, lv_color_hex(color), 0);
        if (s_bar_notice_progress != nullptr) {
            lv_obj_set_style_bg_color(s_bar_notice_progress, lv_color_hex(color), LV_PART_INDICATOR);
        }
    }
}

void refresh()
{
    // Config changed underneath us (web portal / AT console): rebuild the
    // visible form page so its widgets show the new values. Pages without
    // config-backed widgets just consume the bump; while the on-screen
    // keyboard is open the rebuild is deferred so the user's edit survives.
    const uint32_t cfg_gen = CONFIG_NOTIFY_Generation();
    if (cfg_gen != s_cfg_gen_seen) {
        const bool form_page = s_page == Page::Wifi || s_page == Page::Station ||
                               s_page == Page::Audio || s_page == Page::Apps ||
                               s_page == Page::Nanny || s_page == Page::Smb ||
                               s_page == Page::Radio || s_page == Page::EspNow ||
                               s_page == Page::Ctcss || s_page == Page::Mdc || s_page == Page::Dtmf ||
                               s_page == Page::Ai || s_page == Page::About;
        const bool keyboard_open =
            s_keyboard != nullptr && !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (!form_page) {
            s_cfg_gen_seen = cfg_gen;
        } else if (!keyboard_open) {
            rebuildCurrentPage(); // syncs s_cfg_gen_seen via clearScreen
            formStatus("Settings updated from web/serial.", kColorWarn);
        }
    }

    refreshClock();
    refreshWifiBadge();
    refreshVolume();
    refreshCpu();
    refreshStationBadges();
    if (s_page == Page::Home) {
        refreshHome();
    } else {
        refreshDetailPage();
    }
    refreshBtTop();
    if (s_page == Page::Bt) {
        refreshBtPage();
    }
    if (s_page == Page::Music) {
        refreshMusic();
    }
    if (s_page == Page::EspNow) {
        refreshEspNowPage();
    }
    if (s_page == Page::Video) {
        refreshVideo();
    }
    if (s_page == Page::Ai) {
        refreshAiPage();
    }
    if (s_page == Page::About) {
        refreshOtaPage();
    }
    if (s_page == Page::Aprs) {
        refreshAprsPage();
    }
    if (s_page == Page::Cw) {
        refreshCwPage();
    }
    if (s_page == Page::Map) {
        refreshMapPage();
    }
    if (s_page == Page::Sstv) {
        refreshSstvPage();
    }
}

// Rebuild the active page so a font-engine switch takes effect on every
// label immediately (LVGL caches glyph layout per label).
void rebuildCurrentPage()
{
    switch (s_page) {
        case Page::Provisioning: buildProvisioning(); break;
        case Page::Home: buildHome(); break;
        case Page::Config: buildConfig(); break;
        case Page::Wifi: buildWifi(); break;
        case Page::Station: buildStation(); break;
        case Page::Audio: buildAudio(); break;
        case Page::Bt: buildBt(); break;
        case Page::Music: buildMusic(); break;
        case Page::Radio: buildRadio(); break;
        case Page::Nanny: buildNanny(); break;
        case Page::Smb: buildSmb(); break;
        case Page::Apps: buildApps(); break;
        case Page::EspNow: buildEspNow(); break;
        case Page::Ctcss: buildCtcss(); break;
        case Page::Mdc: buildMdc(); break;
        case Page::Dtmf: buildDtmf(); break;
        case Page::Video: buildVideo(); break;
        case Page::Game: buildGame(); break;
        case Page::Ai: buildAi(); break;
        case Page::About: buildAbout(); break;
        case Page::Aprs: buildAprs(); break;
        case Page::AprsMsg: buildAprsMsg(); break;
        case Page::AprsGateway: buildAprsGateway(); break;
        case Page::Cw: buildCw(); break;
        case Page::CwScore: buildCwScore(); break;
        case Page::Map: buildMap(); break;
        case Page::Sstv: buildSstv(); break;
    }
    s_last_refresh_ms = 0;
}

#if LV_USE_FREETYPE
// Lazily bring up FreeType and create the two vector font sizes from the
// TTF on the TF card. Kept out of Display_Init so boots without the file
// (or without a card) pay nothing.
bool ensureFreetypeFonts()
{
    if (s_ft_font_16 != nullptr && s_ft_font_20 != nullptr) {
        return true;
    }
    FILE *probe = fopen(kCjkTtfPath, "rb");
    if (probe == nullptr) {
        ESP_LOGW(TAG, "no TTF at %s", kCjkTtfPath);
        return false;
    }
    fclose(probe);

    if (!s_freetype_inited) {
        if (lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT) != LV_RESULT_OK) {
            ESP_LOGE(TAG, "FreeType init failed");
            return false;
        }
        s_freetype_inited = true;
    }
    if (s_ft_font_16 == nullptr) {
        s_ft_font_16 = lv_freetype_font_create(kCjkTtfPath, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               16, LV_FREETYPE_FONT_STYLE_NORMAL);
    }
    if (s_ft_font_20 == nullptr) {
        s_ft_font_20 = lv_freetype_font_create(kCjkTtfPath, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               20, LV_FREETYPE_FONT_STYLE_NORMAL);
    }
    if (s_ft_font_16 == nullptr || s_ft_font_20 == nullptr) {
        ESP_LOGE(TAG, "FreeType font create failed (%s)", kCjkTtfPath);
        return false;
    }
    ESP_LOGI(TAG, "FreeType fonts ready from %s", kCjkTtfPath);
    return true;
}
#endif

} // namespace

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    if (!initPanel() || !initLvgl()) {
        return;
    }
    // UI fonts: Montserrat primary with the generated CJK fallback so any
    // translated/Chinese text renders (lv_font_montserrat_* are const).
    s_font_ui_16 = lv_font_montserrat_16;
    s_font_ui_16.fallback = &lv_font_cjk_16;
    s_font_ui_20 = lv_font_montserrat_20;
    s_font_ui_20.fallback = &lv_font_cjk_20;
    loadUiLang(); // restore saved language before the first page is built
    initTouch();
    if (s_provisioning_mode) {
        buildProvisioning();
        refreshProvisioning();
    } else {
        buildHome();
        refresh();
    }
    lv_refr_now(nullptr);
    s_ready = true;
    ESP_LOGI(TAG, "display ready");
}

extern "C" void Display_SetProvisioningMode(bool enabled)
{
    if (s_provisioning_mode == enabled) {
        return;
    }
    s_provisioning_mode = enabled;
    s_last_refresh_ms = 0u;
    if (!s_ready) {
        return;
    }
    if (enabled) {
        buildProvisioning();
        refreshProvisioning();
    } else {
        buildHome();
        refresh();
    }
    lv_refr_now(nullptr);
}

extern "C" void Display_Poll(void)
{
    if (!s_ready) {
        return;
    }
    const uint32_t now = millis();
    if (s_provisioning_mode) {
        pollWifiScan();
        if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
            s_last_refresh_ms = now;
            refreshProvisioning();
        }
        lv_timer_handler();
        return;
    }
    if (s_force_invalidate) {
        s_force_invalidate = false;
        lv_obj_invalidate(lv_screen_active()); // repaint after the FB benchmark
    }
    pollWifiScan();
    pollMusicScan();
    // Update the top-bar volume every poll (~20 ms) so a physical volume key held
    // down shows each 1% step live, matching the soft buttons. setLabel no-ops
    // when the value is unchanged, so this is cheap. The full refresh below stays
    // on the slower cadence.
    refreshVolume();
    // Video frames arrive at ~5 fps; the 500 ms page cadence would halve
    // that. Acquire is cheap when no new frame is pending.
    if (s_page == Page::Video) {
        refreshVideo();
    }
    if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
        s_last_refresh_ms = now;
        refresh();
    }
    if (s_volume_dirty && (now - s_volume_change_ms) >= kVolumeSaveDelayMs) {
        s_volume_dirty = !EXTERNAL_RADIO_SaveConfig();
        // Deferred save of our own volume change: consume the bump so the
        // page isn't pointlessly rebuilt half a second later.
        s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
    }
    refreshDisplayNotice();
    // UI-latency telemetry: everything LVGL does (touch read, layout, full
    // render) runs inside this call, so its duration IS the felt lag. Warn
    // on single slow passes and print a 10 s max/count digest while hunting
    // responsiveness regressions.
    const int64_t lv_t0 = esp_timer_get_time();
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
    TaskStatus_t lv_ts0;
    vTaskGetInfo(nullptr, &lv_ts0, pdFALSE, eRunning);
#endif
    lv_timer_handler();
    const uint32_t lv_us = static_cast<uint32_t>(esp_timer_get_time() - lv_t0);
    const uint32_t lv_ms = lv_us / 1000u;
    flushLvglTelemetry();
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
    // CPU time this task actually consumed inside the handler: splits
    // "render is computing" from "render is blocked/preempted".
    TaskStatus_t lv_ts1;
    vTaskGetInfo(nullptr, &lv_ts1, pdFALSE, eRunning);
    const uint32_t lv_cpu_ms =
        (lv_ts1.ulRunTimeCounter - lv_ts0.ulRunTimeCounter) / 1000u;
#else
    const uint32_t lv_cpu_ms = 0;
#endif
    static uint32_t s_lv_max_ms = 0;
    static uint32_t s_lv_slow_count = 0;
    static uint32_t s_lv_report_ms = 0;
    static uint64_t s_lv_sum_us = 0;
    static uint32_t s_lv_cpu_sum_ms = 0;
    static uint32_t s_lv_pass_count = 0; // handler passes that did real work
    s_lv_sum_us += lv_us;
    s_lv_cpu_sum_ms += lv_cpu_ms;
    if (lv_ms > 5u) {
        ++s_lv_pass_count;
    }
    if (lv_ms > s_lv_max_ms) {
        s_lv_max_ms = lv_ms;
    }
    if (lv_ms > 150u) {
        ESP_LOGW(TAG, "lv_timer_handler took %lums (cpu %lums)",
                 static_cast<unsigned long>(lv_ms),
                 static_cast<unsigned long>(lv_cpu_ms));
    }
    if (lv_ms > 60u) {
        ++s_lv_slow_count;
    }
    if (now - s_lv_report_ms >= 10000u) {
        s_lv_report_ms = now;
        // Quiet unless something was actually slow. The full field set stays:
        // during the RTCRAM-stack hunt the per-window cpu/inv/invkpx split was
        // what separated "renders too often" from "each render too expensive".
        if (s_lv_max_ms > 60u) {
        ESP_LOGI(TAG, "lv 10s digest: wall=%lums cpu=%lums passes>5ms=%lu max=%lums slow(>60ms)=%lu biginv=%lu inv=%lu invkpx=%lu",
                 static_cast<unsigned long>(s_lv_sum_us / 1000u),
                 static_cast<unsigned long>(s_lv_cpu_sum_ms),
                 static_cast<unsigned long>(s_lv_pass_count),
                 static_cast<unsigned long>(s_lv_max_ms),
                 static_cast<unsigned long>(s_lv_slow_count),
                 static_cast<unsigned long>(s_big_invalidate_count),
                 static_cast<unsigned long>(s_invalidate_count),
                 static_cast<unsigned long>(s_invalidate_px / 1000u));
        }
        s_lv_sum_us = 0;
        s_lv_cpu_sum_ms = 0;
        s_lv_pass_count = 0;
        s_lv_max_ms = 0;
        s_lv_slow_count = 0;
        s_big_invalidate_count = 0;
        s_invalidate_count = 0;
        s_invalidate_px = 0;
    }
}

extern "C" int Display_GetBatteryRawMv(void)
{
    return 0;
}

extern "C" long Display_FramebufferBenchMBps(void)
{
    if (s_fb_bench == nullptr) {
        return -1;
    }
    const size_t bytes = static_cast<size_t>(kWidth) * kHeight * 2u;
    const int64_t t0 = esp_timer_get_time();
    memset(s_fb_bench, 0, bytes);
    const int64_t us = esp_timer_get_time() - t0;
    s_force_invalidate = true; // repaint the smear from the display task
    if (us <= 0) {
        return -1;
    }
    return static_cast<long>(static_cast<int64_t>(bytes) * 1000000 / us / 1048576);
}

extern "C" int Display_GetBatteryCalibratedMv(void)
{
    return 0;
}

extern "C" bool Display_SetCjkFontEngine(const int engine)
{
    if (!s_ready) {
        return false;
    }
    if (engine == DISPLAY_CJK_FONT_BITMAP) {
        s_font_ui_16.fallback = &lv_font_cjk_16;
        s_font_ui_20.fallback = &lv_font_cjk_20;
        s_cjk_font_engine = DISPLAY_CJK_FONT_BITMAP;
        rebuildCurrentPage();
        return true;
    }
    if (engine == DISPLAY_CJK_FONT_FREETYPE) {
#if LV_USE_FREETYPE
        if (!ensureFreetypeFonts()) {
            return false;
        }
        s_font_ui_16.fallback = s_ft_font_16;
        s_font_ui_20.fallback = s_ft_font_20;
        s_cjk_font_engine = DISPLAY_CJK_FONT_FREETYPE;
        rebuildCurrentPage();
        return true;
#else
        return false;
#endif
    }
    return false;
}

extern "C" int Display_GetCjkFontEngine(void)
{
    return s_cjk_font_engine;
}

#endif
