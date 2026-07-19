package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

var validBoardID = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)
var validFeatureKey = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)

type boardCatalogEntry struct {
	ID                 string               `json:"id"`
	NameZH             string               `json:"name_zh"`
	NameEN             string               `json:"name_en"`
	TaglineZH          string               `json:"tagline_zh"`
	TaglineEN          string               `json:"tagline_en"`
	DescriptionZH      string               `json:"description_zh"`
	DescriptionEN      string               `json:"description_en"`
	ChipLabel          string               `json:"chip_label"`
	WebFlashChipFamily string               `json:"web_flash_chip_family"`
	ImageURL           string               `json:"image_url"`
	DisplayOrder       int                  `json:"display_order"`
	Status             string               `json:"status"`
	HighlightsZH       []string             `json:"highlights_zh"`
	HighlightsEN       []string             `json:"highlights_en"`
	Features           map[string]string    `json:"features"`
	FeatureNotes       map[string]langValue `json:"feature_notes,omitempty"`
	CreatedAt          int64                `json:"created_at"`
	UpdatedAt          int64                `json:"updated_at"`
}

type featureCatalogEntry struct {
	Key           string `json:"key"`
	LabelZH       string `json:"label_zh"`
	LabelEN       string `json:"label_en"`
	DescriptionZH string `json:"description_zh"`
	DescriptionEN string `json:"description_en"`
	GroupName     string `json:"group"`
	DisplayOrder  int    `json:"display_order"`
	Active        bool   `json:"active"`
}

type langValue struct {
	ZH string `json:"zh"`
	EN string `json:"en"`
}

type catalogResponse struct {
	Boards   []boardCatalogEntry   `json:"boards"`
	Features []featureCatalogEntry `json:"features"`
}

func initDB(db *sql.DB) error {
	if _, err := db.Exec(`PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS releases (id INTEGER PRIMARY KEY, board_type TEXT NOT NULL, version TEXT NOT NULL, channel TEXT NOT NULL, filename TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL, notes TEXT NOT NULL, created_at INTEGER NOT NULL, UNIQUE(board_type,version,channel));
CREATE TABLE IF NOT EXISTS devices (device_id TEXT PRIMARY KEY, board_type TEXT NOT NULL, firmware_version TEXT NOT NULL, ip_address TEXT NOT NULL, metadata_json TEXT NOT NULL, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS boards (
 id TEXT PRIMARY KEY,
 name_zh TEXT NOT NULL, name_en TEXT NOT NULL,
 tagline_zh TEXT NOT NULL DEFAULT '', tagline_en TEXT NOT NULL DEFAULT '',
 description_zh TEXT NOT NULL DEFAULT '', description_en TEXT NOT NULL DEFAULT '',
 chip_label TEXT NOT NULL DEFAULT '', web_flash_chip_family TEXT NOT NULL DEFAULT '',
 image_url TEXT NOT NULL DEFAULT '', image_filename TEXT NOT NULL DEFAULT '',
 display_order INTEGER NOT NULL DEFAULT 0,
 status TEXT NOT NULL DEFAULT 'draft' CHECK(status IN ('draft','published','archived')),
 highlights_zh_json TEXT NOT NULL DEFAULT '[]', highlights_en_json TEXT NOT NULL DEFAULT '[]',
 created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS features (
 key TEXT PRIMARY KEY,
 label_zh TEXT NOT NULL, label_en TEXT NOT NULL,
 description_zh TEXT NOT NULL DEFAULT '', description_en TEXT NOT NULL DEFAULT '',
 group_name TEXT NOT NULL DEFAULT 'general', display_order INTEGER NOT NULL DEFAULT 0,
 active INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS board_features (
 board_id TEXT NOT NULL REFERENCES boards(id) ON DELETE CASCADE,
 feature_key TEXT NOT NULL REFERENCES features(key) ON DELETE CASCADE,
 state TEXT NOT NULL CHECK(state IN ('yes','partial','no')),
 note_zh TEXT NOT NULL DEFAULT '', note_en TEXT NOT NULL DEFAULT '',
 PRIMARY KEY(board_id,feature_key)
);
CREATE TABLE IF NOT EXISTS admin_audit (
 id INTEGER PRIMARY KEY,
 source TEXT NOT NULL, action TEXT NOT NULL, target TEXT NOT NULL,
 detail_json TEXT NOT NULL DEFAULT '{}', created_at INTEGER NOT NULL
);`); err != nil {
		return err
	}
	// Legacy releases derive their URL from filename. This remains idempotent.
	_, _ = db.Exec(`ALTER TABLE releases ADD COLUMN url TEXT NOT NULL DEFAULT ''`)
	return seedCatalog(db)
}

type seedBoard struct {
	id, zh, en, taglineZH, taglineEN, chip, chipFamily, image string
	order                                                     int
	highlightsZH, highlightsEN                                []string
}

type seedFeature struct {
	key, zh, en string
	states      map[string]string
}

func seedCatalog(db *sql.DB) error {
	boards := []seedBoard{
		{"gezipai", "格子派 gezipai", "Gezipai", "ESP32-S3 彩色显示终端", "ESP32-S3 color display terminal", "ESP32-S3", "ESP32-S3", "/boards/gezipai.jpg", 10,
			[]string{"ES7210 麦克风 ADC + ES8311 DAC 音频链路", "240×240 ST7789 彩色屏与 LVGL 图形界面", "屏幕菜单管理信令与 APRS", "BLE 配网、Wi-Fi 配置、远程 AT 与 OTA"},
			[]string{"ES7210 microphone ADC and ES8311 DAC audio path", "240×240 ST7789 color display with LVGL UI", "On-screen signaling and APRS controls", "BLE provisioning, Wi-Fi portal, remote AT, and OTA"}},
		{"bh4tdv", "BH4TDV ESP32 3188", "BH4TDV ESP32 3188", "ESP32-S3 无屏电台接口", "ESP32-S3 headless radio interface", "ESP32-S3", "ESP32-S3", "/boards/bh4tdv-esp32-3188.jpg", 20,
			[]string{"ES8311 全双工音频，连接 Moto3188 / NRL 电台", "PTT、SQL 与三位频道选择", "SCI 串口透明传输", "BLE 配网、Wi-Fi 配置、远程 AT 与 OTA"},
			[]string{"ES8311 full-duplex audio for Moto3188 / NRL radios", "PTT, squelch, and three-bit channel selection", "SCI serial passthrough", "BLE provisioning, Wi-Fi portal, remote AT, and OTA"}},
		{"s31_korvo", "S31 Korvo", "S31 Korvo", "ESP32-S31 全功能开发板", "ESP32-S31 full-featured dev board", "ESP32-S31 · RISC-V", "", "/boards/s31-korvo.png", 30,
			[]string{"ES8389 音频、800×480 RGB 电容触摸屏", "TF 卡、USB 主机、本地音乐与网络收音机", "蓝牙 HFP / A2DP、ESP-NOW 与 AI 语音", "可配置 UART1/SCI 与 UART2/GPS"},
			[]string{"ES8389 audio and 800×480 RGB capacitive display", "TF card, USB host, local music, and Internet radio", "Bluetooth HFP / A2DP, ESP-NOW, and AI voice", "Configurable UART1/SCI and UART2/GPS"}},
		{"s31_function_coreboard", "S31 功能核心板", "S31 Function Coreboard", "ESP32-S31 精简核心板", "ESP32-S31 compact core board", "ESP32-S31 · RISC-V", "", "/boards/s31-function-coreboard.png", 40,
			[]string{"ES8311 音频编解码", "YT8531 千兆以太网与 Wi-Fi 回退", "USB-A 主机、RGB 状态灯与 SCI 串口", "无屏核心板方案"},
			[]string{"ES8311 audio codec", "YT8531 Gigabit Ethernet with Wi-Fi fallback", "USB-A host, RGB status LED, and SCI serial", "Compact screenless core-board design"}},
	}
	features := []seedFeature{
		{"nrl_voice", "NRL UDP 网络语音桥接（G.711 / Opus）", "NRL UDP voice bridge (G.711 / Opus)", allBoards("yes")},
		{"wifi_portal", "Wi-Fi 配置门户 / SoftAP 配网", "Wi-Fi configuration portal / SoftAP provisioning", allBoards("yes")},
		{"remote_at_ota", "远程 AT 配置与设备 OTA 升级", "Remote AT configuration and device OTA", allBoards("yes")},
		{"aprs", "APRS-IS 网络与无线电 AFSK 收发", "APRS-IS networking and radio AFSK TX/RX", allBoards("yes")},
		{"mdc1200", "MDC1200 信令编码与解码", "MDC1200 signaling encode/decode", allBoards("yes")},
		{"dtmf", "DTMF 信令编码与解码", "DTMF signaling encode/decode", allBoards("yes")},
		{"ctcss", "CTCSS/PL 亚音频率识别", "CTCSS/PL tone-frequency detection", allBoards("yes")},
		{"screen_signaling", "屏幕信令与 APRS 设置菜单", "On-screen signaling and APRS settings", states("yes", "no", "partial", "no")},
		{"web_flash", "网页 USB 首次全量刷机", "Browser USB full flashing", states("yes", "yes", "no", "no")},
		{"ble", "BLE 蓝牙配网", "BLE provisioning", states("yes", "yes", "no", "no")},
		{"es7210", "ES7210 麦克风 ADC", "ES7210 microphone ADC", states("yes", "no", "no", "no")},
		{"es8311", "ES8311 音频编解码", "ES8311 audio codec", states("yes", "yes", "no", "yes")},
		{"es8389", "ES8389 音频编解码", "ES8389 audio codec", states("no", "no", "yes", "no")},
		{"audio_processing", "AEC、降噪、高通、尾音抑制与增益", "AEC, noise reduction, high-pass, tail suppression, and gain", allBoards("yes")},
		{"radio_ptt_sql", "电台 PTT / SQL 控制", "Radio PTT / squelch control", states("yes", "yes", "partial", "partial")},
		{"channel_select", "三位频道选择（0–7）", "Three-bit channel selection (0–7)", states("no", "yes", "no", "no")},
		{"sci", "SCI 串口透明传输", "SCI serial passthrough", states("yes", "yes", "partial", "yes")},
		{"status_indicator", "状态指示灯", "Status indicator", allBoards("yes")},
		{"color_display", "彩色显示屏", "Color display", states("yes", "no", "yes", "no")},
		{"touch", "触摸界面", "Touch interface", states("no", "no", "yes", "no")},
		{"buttons", "本地实体按键 / PTT", "Physical buttons / PTT", states("yes", "no", "yes", "no")},
		{"battery", "电池电压检测", "Battery-voltage sensing", states("yes", "no", "no", "no")},
		{"tf_media", "TF 卡本地媒体", "TF-card local media", states("no", "no", "yes", "no")},
		{"usb_host", "USB 主机 / U 盘存储", "USB host / flash storage", states("no", "no", "yes", "yes")},
		{"smb_media", "SMB 网络共享媒体", "SMB network-share media", states("no", "no", "yes", "yes")},
		{"ethernet", "千兆以太网", "Gigabit Ethernet", states("no", "no", "no", "yes")},
		{"bluetooth_audio", "蓝牙耳机 / 音箱麦克风（HFP / A2DP）", "Bluetooth headset / speaker-mic (HFP / A2DP)", states("no", "no", "yes", "yes")},
		{"bluetooth_call", "蓝牙 HFP 双向语音通话", "Bluetooth HFP two-way voice calls", states("no", "no", "yes", "yes")},
		{"bluetooth_ptt", "蓝牙耳机按键 PTT", "Bluetooth headset-button PTT", states("no", "no", "yes", "yes")},
		{"espnow", "ESP-NOW 脱网对讲", "ESP-NOW offline intercom", allBoards("yes")},
		{"music_radio", "本地音乐、网络收音机与定时播报", "Local music, Internet radio, and timed playback", states("no", "no", "yes", "yes")},
		{"ai_voice", "小智 AI 语音助手", "Xiaozhi AI voice assistant", states("no", "no", "yes", "yes")},
		{"video_call", "NRL 视频通话（DVP 摄像头）", "NRL video calls (DVP camera)", states("no", "no", "yes", "no")},
	}
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().Unix()
	for _, b := range boards {
		zh, _ := json.Marshal(b.highlightsZH)
		en, _ := json.Marshal(b.highlightsEN)
		if _, err = tx.Exec(`INSERT OR IGNORE INTO boards(id,name_zh,name_en,tagline_zh,tagline_en,chip_label,web_flash_chip_family,image_url,display_order,status,highlights_zh_json,highlights_en_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,'published',?,?,?,?)`,
			b.id, b.zh, b.en, b.taglineZH, b.taglineEN, b.chip, b.chipFamily, b.image, b.order, string(zh), string(en), now, now); err != nil {
			return err
		}
	}
	// Preserve legacy/custom board types already referenced by releases or
	// devices. They become private drafts until an administrator completes and
	// publishes their catalog metadata.
	if _, err = tx.Exec(`INSERT OR IGNORE INTO boards(id,name_zh,name_en,status,display_order,created_at,updated_at)
		SELECT board_type,board_type,board_type,'draft',1000,?,? FROM releases WHERE board_type GLOB '[a-z0-9]*'
		UNION SELECT board_type,board_type,board_type,'draft',1000,?,? FROM devices WHERE board_type GLOB '[a-z0-9]*'`, now, now, now, now); err != nil {
		return err
	}
	for i, f := range features {
		if _, err = tx.Exec(`INSERT OR IGNORE INTO features(key,label_zh,label_en,display_order,active) VALUES(?,?,?,?,1)`, f.key, f.zh, f.en, (i+1)*10); err != nil {
			return err
		}
		for board, state := range f.states {
			if _, err = tx.Exec(`INSERT OR IGNORE INTO board_features(board_id,feature_key,state) VALUES(?,?,?)`, board, f.key, state); err != nil {
				return err
			}
		}
	}
	_, err = tx.Exec(`INSERT OR IGNORE INTO schema_migrations(version,applied_at) VALUES(1,?)`, now)
	if err != nil {
		return err
	}
	return tx.Commit()
}

func allBoards(state string) map[string]string { return states(state, state, state, state) }
func states(gezipai, bh4tdv, korvo, core string) map[string]string {
	return map[string]string{"gezipai": gezipai, "bh4tdv": bh4tdv, "s31_korvo": korvo, "s31_function_coreboard": core}
}

func (s *server) boardExists(id string) bool {
	var one int
	return s.db.QueryRow(`SELECT 1 FROM boards WHERE id=?`, id).Scan(&one) == nil
}

func (s *server) boardPublished(id string) bool {
	var one int
	return s.db.QueryRow(`SELECT 1 FROM boards WHERE id=? AND status='published'`, id).Scan(&one) == nil
}

func (s *server) ensureBoardPublishable(id, nameZH, nameEN string) error {
	if strings.TrimSpace(nameZH) == "" || strings.TrimSpace(nameEN) == "" {
		return errors.New("bilingual board names are required before publishing")
	}
	var image string
	if err := s.db.QueryRow(`SELECT image_url FROM boards WHERE id=?`, id).Scan(&image); err != nil {
		return errors.New("board not found")
	}
	if image == "" {
		return errors.New("board image is required before publishing")
	}
	var featureCount int
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM board_features WHERE board_id=?`, id).Scan(&featureCount); err != nil {
		return err
	}
	if featureCount == 0 {
		return errors.New("configure at least one feature before publishing")
	}
	return nil
}

func (s *server) boardChipFamily(id string) (string, bool) {
	var family string
	if err := s.db.QueryRow(`SELECT web_flash_chip_family FROM boards WHERE id=?`, id).Scan(&family); err != nil {
		return "", false
	}
	return family, true
}

func (s *server) publicCatalog(w http.ResponseWriter, _ *http.Request) {
	catalog, err := s.loadCatalog(false)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, catalog)
}

func (s *server) adminCatalog(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	catalog, err := s.loadCatalog(true)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, catalog)
}

type auditEvent struct {
	Source    string          `json:"source"`
	Action    string          `json:"action"`
	Target    string          `json:"target"`
	Detail    json.RawMessage `json:"detail"`
	CreatedAt int64           `json:"created_at"`
}

func (s *server) recordAudit(source, action, target string, detail any) {
	data, err := json.Marshal(detail)
	if err != nil {
		data = []byte(`{}`)
	}
	_, _ = s.db.Exec(`INSERT INTO admin_audit(source,action,target,detail_json,created_at) VALUES(?,?,?,?,?)`, source, action, target, string(data), time.Now().Unix())
}

func (s *server) auditEvents(limit int) ([]auditEvent, error) {
	if limit <= 0 || limit > 500 {
		limit = 100
	}
	rows, err := s.db.Query(`SELECT source,action,target,detail_json,created_at FROM admin_audit ORDER BY id DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []auditEvent
	for rows.Next() {
		var event auditEvent
		var detail string
		if err := rows.Scan(&event.Source, &event.Action, &event.Target, &detail, &event.CreatedAt); err != nil {
			return nil, err
		}
		event.Detail = json.RawMessage(detail)
		out = append(out, event)
	}
	return out, rows.Err()
}

func (s *server) listAudit(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	events, err := s.auditEvents(limit)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"events": events})
}

func (s *server) loadCatalog(includeInactive bool) (catalogResponse, error) {
	var out catalogResponse
	featureQuery := `SELECT key,label_zh,label_en,description_zh,description_en,group_name,display_order,active FROM features`
	if !includeInactive {
		featureQuery += ` WHERE active=1`
	}
	featureQuery += ` ORDER BY display_order,key`
	rows, err := s.db.Query(featureQuery)
	if err != nil {
		return out, err
	}
	for rows.Next() {
		var f featureCatalogEntry
		var active int
		if err = rows.Scan(&f.Key, &f.LabelZH, &f.LabelEN, &f.DescriptionZH, &f.DescriptionEN, &f.GroupName, &f.DisplayOrder, &active); err != nil {
			rows.Close()
			return out, err
		}
		f.Active = active != 0
		out.Features = append(out.Features, f)
	}
	if err = rows.Close(); err != nil {
		return out, err
	}
	boardQuery := `SELECT id,name_zh,name_en,tagline_zh,tagline_en,description_zh,description_en,chip_label,web_flash_chip_family,image_url,display_order,status,highlights_zh_json,highlights_en_json,created_at,updated_at FROM boards`
	if !includeInactive {
		boardQuery += ` WHERE status='published'`
	}
	boardQuery += ` ORDER BY display_order,id`
	rows, err = s.db.Query(boardQuery)
	if err != nil {
		return out, err
	}
	for rows.Next() {
		var b boardCatalogEntry
		var zh, en string
		if err = rows.Scan(&b.ID, &b.NameZH, &b.NameEN, &b.TaglineZH, &b.TaglineEN, &b.DescriptionZH, &b.DescriptionEN, &b.ChipLabel, &b.WebFlashChipFamily, &b.ImageURL, &b.DisplayOrder, &b.Status, &zh, &en, &b.CreatedAt, &b.UpdatedAt); err != nil {
			rows.Close()
			return out, err
		}
		_ = json.Unmarshal([]byte(zh), &b.HighlightsZH)
		_ = json.Unmarshal([]byte(en), &b.HighlightsEN)
		b.Features = map[string]string{}
		b.FeatureNotes = map[string]langValue{}
		out.Boards = append(out.Boards, b)
	}
	if err = rows.Close(); err != nil {
		return out, err
	}
	byID := map[string]*boardCatalogEntry{}
	for i := range out.Boards {
		byID[out.Boards[i].ID] = &out.Boards[i]
	}
	rows, err = s.db.Query(`SELECT board_id,feature_key,state,note_zh,note_en FROM board_features`)
	if err != nil {
		return out, err
	}
	defer rows.Close()
	for rows.Next() {
		var board, key, state, zh, en string
		if err = rows.Scan(&board, &key, &state, &zh, &en); err != nil {
			return out, err
		}
		if b := byID[board]; b != nil {
			b.Features[key] = state
			if zh != "" || en != "" {
				b.FeatureNotes[key] = langValue{ZH: zh, EN: en}
			}
		}
	}
	return out, rows.Err()
}

func validateBoard(b *boardCatalogEntry, creating bool) error {
	if creating && !validBoardID.MatchString(b.ID) {
		return errors.New("board id must use lowercase letters, numbers, underscore or hyphen")
	}
	if strings.TrimSpace(b.NameZH) == "" || strings.TrimSpace(b.NameEN) == "" {
		return errors.New("Chinese and English board names are required")
	}
	if b.Status == "" {
		b.Status = "draft"
	}
	if b.Status != "draft" && b.Status != "published" && b.Status != "archived" {
		return errors.New("status must be draft, published or archived")
	}
	if len(b.HighlightsZH) > 12 || len(b.HighlightsEN) > 12 {
		return errors.New("too many board highlights")
	}
	return nil
}

func (s *server) createBoard(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	var b boardCatalogEntry
	if err := json.NewDecoder(io.LimitReader(r.Body, 64<<10)).Decode(&b); err != nil {
		writeError(w, 400, "invalid board JSON")
		return
	}
	if err := validateBoard(&b, true); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	if b.Status == "published" {
		writeError(w, 400, "create the board as a draft, then upload an image and configure features before publishing")
		return
	}
	b.ImageURL = ""
	now := time.Now().Unix()
	zh, _ := json.Marshal(b.HighlightsZH)
	en, _ := json.Marshal(b.HighlightsEN)
	_, err := s.db.Exec(`INSERT INTO boards(id,name_zh,name_en,tagline_zh,tagline_en,description_zh,description_en,chip_label,web_flash_chip_family,image_url,display_order,status,highlights_zh_json,highlights_en_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		b.ID, b.NameZH, b.NameEN, b.TaglineZH, b.TaglineEN, b.DescriptionZH, b.DescriptionEN, b.ChipLabel, b.WebFlashChipFamily, b.ImageURL, b.DisplayOrder, b.Status, string(zh), string(en), now, now)
	if err != nil {
		writeError(w, 409, "board id already exists")
		return
	}
	s.recordAudit("admin-api", "board.create", b.ID, map[string]any{"status": b.Status})
	writeJSON(w, 201, map[string]any{"id": b.ID})
}

// importCatalog is the structured entry point for automation and AI-assisted
// authoring. It applies a board, any new/updated feature definitions and all
// feature assignments in one transaction. Uploaded images intentionally use
// the separate binary endpoint so MIME and size checks cannot be bypassed.
func (s *server) importCatalog(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	var body struct {
		Board       boardCatalogEntry     `json:"board"`
		Features    []featureCatalogEntry `json:"features"`
		Assignments map[string]struct {
			State  string `json:"state"`
			NoteZH string `json:"note_zh"`
			NoteEN string `json:"note_en"`
		} `json:"assignments"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 256<<10)).Decode(&body); err != nil {
		writeError(w, 400, "invalid catalog import JSON")
		return
	}
	if err := validateBoard(&body.Board, true); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	// AI/import submissions always land as drafts. Publication remains a
	// separate reviewed action in both the page and MCP interfaces.
	body.Board.Status = "draft"
	body.Board.ImageURL = ""
	var existingStatus string
	if err := s.db.QueryRow(`SELECT status FROM boards WHERE id=?`, body.Board.ID).Scan(&existingStatus); err == nil && existingStatus == "published" {
		writeError(w, 409, "published boards must be moved to draft explicitly before AI/JSON import")
		return
	} else if err != nil && err != sql.ErrNoRows {
		writeError(w, 500, err.Error())
		return
	}
	for i := range body.Features {
		if err := validateFeature(&body.Features[i], true); err != nil {
			writeError(w, 400, err.Error())
			return
		}
	}
	tx, err := s.db.Begin()
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	defer tx.Rollback()
	now := time.Now().Unix()
	zh, _ := json.Marshal(body.Board.HighlightsZH)
	en, _ := json.Marshal(body.Board.HighlightsEN)
	_, err = tx.Exec(`INSERT INTO boards(id,name_zh,name_en,tagline_zh,tagline_en,description_zh,description_en,chip_label,web_flash_chip_family,image_url,display_order,status,highlights_zh_json,highlights_en_json,created_at,updated_at)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(id) DO UPDATE SET name_zh=excluded.name_zh,name_en=excluded.name_en,tagline_zh=excluded.tagline_zh,tagline_en=excluded.tagline_en,description_zh=excluded.description_zh,description_en=excluded.description_en,chip_label=excluded.chip_label,web_flash_chip_family=excluded.web_flash_chip_family,display_order=excluded.display_order,status=excluded.status,highlights_zh_json=excluded.highlights_zh_json,highlights_en_json=excluded.highlights_en_json,updated_at=excluded.updated_at`,
		body.Board.ID, body.Board.NameZH, body.Board.NameEN, body.Board.TaglineZH, body.Board.TaglineEN, body.Board.DescriptionZH, body.Board.DescriptionEN, body.Board.ChipLabel, body.Board.WebFlashChipFamily, body.Board.ImageURL, body.Board.DisplayOrder, body.Board.Status, string(zh), string(en), now, now)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	for _, f := range body.Features {
		_, err = tx.Exec(`INSERT INTO features(key,label_zh,label_en,description_zh,description_en,group_name,display_order,active) VALUES(?,?,?,?,?,?,?,?)
			ON CONFLICT(key) DO UPDATE SET label_zh=excluded.label_zh,label_en=excluded.label_en,description_zh=excluded.description_zh,description_en=excluded.description_en,group_name=excluded.group_name,display_order=excluded.display_order,active=excluded.active`,
			f.Key, f.LabelZH, f.LabelEN, f.DescriptionZH, f.DescriptionEN, f.GroupName, f.DisplayOrder, boolInt(f.Active))
		if err != nil {
			writeError(w, 500, err.Error())
			return
		}
	}
	if _, err = tx.Exec(`DELETE FROM board_features WHERE board_id=?`, body.Board.ID); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	for key, a := range body.Assignments {
		if a.State != "yes" && a.State != "partial" && a.State != "no" {
			writeError(w, 400, "invalid feature state for "+key)
			return
		}
		var one int
		if err = tx.QueryRow(`SELECT 1 FROM features WHERE key=?`, key).Scan(&one); err != nil {
			writeError(w, 400, "unknown feature: "+key)
			return
		}
		if _, err = tx.Exec(`INSERT INTO board_features(board_id,feature_key,state,note_zh,note_en) VALUES(?,?,?,?,?)`, body.Board.ID, key, a.State, a.NoteZH, a.NoteEN); err != nil {
			writeError(w, 500, err.Error())
			return
		}
	}
	if err = tx.Commit(); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	s.recordAudit("admin-api", "catalog.import", body.Board.ID, map[string]any{"features": len(body.Assignments), "status": "draft"})
	writeJSON(w, 200, map[string]any{"id": body.Board.ID, "features": len(body.Assignments)})
}

func (s *server) updateBoard(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	id := r.PathValue("id")
	if !validBoardID.MatchString(id) {
		writeError(w, 400, "invalid board id")
		return
	}
	var b boardCatalogEntry
	if err := json.NewDecoder(io.LimitReader(r.Body, 64<<10)).Decode(&b); err != nil {
		writeError(w, 400, "invalid board JSON")
		return
	}
	if b.ID != "" && b.ID != id {
		writeError(w, 400, "board id is immutable")
		return
	}
	if err := validateBoard(&b, false); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	if b.Status == "published" {
		if err := s.ensureBoardPublishable(id, b.NameZH, b.NameEN); err != nil {
			writeError(w, 400, err.Error())
			return
		}
	}
	zh, _ := json.Marshal(b.HighlightsZH)
	en, _ := json.Marshal(b.HighlightsEN)
	result, err := s.db.Exec(`UPDATE boards SET name_zh=?,name_en=?,tagline_zh=?,tagline_en=?,description_zh=?,description_en=?,chip_label=?,web_flash_chip_family=?,display_order=?,status=?,highlights_zh_json=?,highlights_en_json=?,updated_at=? WHERE id=?`,
		b.NameZH, b.NameEN, b.TaglineZH, b.TaglineEN, b.DescriptionZH, b.DescriptionEN, b.ChipLabel, b.WebFlashChipFamily, b.DisplayOrder, b.Status, string(zh), string(en), time.Now().Unix(), id)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	if n, _ := result.RowsAffected(); n == 0 {
		writeError(w, 404, "board not found")
		return
	}
	s.recordAudit("admin-api", "board.update", id, map[string]any{"status": b.Status})
	writeJSON(w, 200, map[string]any{"id": id})
}

func validateFeature(f *featureCatalogEntry, creating bool) error {
	if creating && !validFeatureKey.MatchString(f.Key) {
		return errors.New("feature key must use lowercase letters, numbers, underscore or hyphen")
	}
	if strings.TrimSpace(f.LabelZH) == "" || strings.TrimSpace(f.LabelEN) == "" {
		return errors.New("Chinese and English feature labels are required")
	}
	if f.GroupName == "" {
		f.GroupName = "general"
	}
	return nil
}

func (s *server) createFeature(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	var f featureCatalogEntry
	if err := json.NewDecoder(io.LimitReader(r.Body, 32<<10)).Decode(&f); err != nil {
		writeError(w, 400, "invalid feature JSON")
		return
	}
	if err := validateFeature(&f, true); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	_, err := s.db.Exec(`INSERT INTO features(key,label_zh,label_en,description_zh,description_en,group_name,display_order,active) VALUES(?,?,?,?,?,?,?,?)`, f.Key, f.LabelZH, f.LabelEN, f.DescriptionZH, f.DescriptionEN, f.GroupName, f.DisplayOrder, boolInt(f.Active))
	if err != nil {
		writeError(w, 409, "feature key already exists")
		return
	}
	s.recordAudit("admin-api", "feature.create", f.Key, map[string]any{"active": f.Active})
	writeJSON(w, 201, map[string]any{"key": f.Key})
}

func (s *server) updateFeature(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	key := r.PathValue("key")
	if !validFeatureKey.MatchString(key) {
		writeError(w, 400, "invalid feature key")
		return
	}
	var f featureCatalogEntry
	if err := json.NewDecoder(io.LimitReader(r.Body, 32<<10)).Decode(&f); err != nil {
		writeError(w, 400, "invalid feature JSON")
		return
	}
	if f.Key != "" && f.Key != key {
		writeError(w, 400, "feature key is immutable")
		return
	}
	if err := validateFeature(&f, false); err != nil {
		writeError(w, 400, err.Error())
		return
	}
	result, err := s.db.Exec(`UPDATE features SET label_zh=?,label_en=?,description_zh=?,description_en=?,group_name=?,display_order=?,active=? WHERE key=?`, f.LabelZH, f.LabelEN, f.DescriptionZH, f.DescriptionEN, f.GroupName, f.DisplayOrder, boolInt(f.Active), key)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	if n, _ := result.RowsAffected(); n == 0 {
		writeError(w, 404, "feature not found")
		return
	}
	s.recordAudit("admin-api", "feature.update", key, map[string]any{"active": f.Active})
	writeJSON(w, 200, map[string]any{"key": key})
}

func boolInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func (s *server) updateBoardFeatures(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	id := r.PathValue("id")
	if !s.boardExists(id) {
		writeError(w, 404, "board not found")
		return
	}
	var body struct {
		Features map[string]struct {
			State  string `json:"state"`
			NoteZH string `json:"note_zh"`
			NoteEN string `json:"note_en"`
		} `json:"features"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 128<<10)).Decode(&body); err != nil {
		writeError(w, 400, "invalid feature assignments")
		return
	}
	keys := make([]string, 0, len(body.Features))
	for key := range body.Features {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	tx, err := s.db.Begin()
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	defer tx.Rollback()
	if _, err = tx.Exec(`DELETE FROM board_features WHERE board_id=?`, id); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	for _, key := range keys {
		a := body.Features[key]
		if a.State != "yes" && a.State != "partial" && a.State != "no" {
			writeError(w, 400, "invalid feature state for "+key)
			return
		}
		var one int
		if err = tx.QueryRow(`SELECT 1 FROM features WHERE key=?`, key).Scan(&one); err != nil {
			writeError(w, 400, "unknown feature: "+key)
			return
		}
		if _, err = tx.Exec(`INSERT INTO board_features(board_id,feature_key,state,note_zh,note_en) VALUES(?,?,?,?,?)`, id, key, a.State, a.NoteZH, a.NoteEN); err != nil {
			writeError(w, 500, err.Error())
			return
		}
	}
	if err = tx.Commit(); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	s.recordAudit("admin-api", "board.features", id, map[string]any{"features": len(keys)})
	writeJSON(w, 200, map[string]any{"id": id, "features": len(keys)})
}

func (s *server) uploadBoardImage(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	id := r.PathValue("id")
	if !s.boardExists(id) {
		writeError(w, 404, "board not found")
		return
	}
	data, err := io.ReadAll(io.LimitReader(r.Body, 5<<20+1))
	if err != nil || len(data) == 0 || len(data) > 5<<20 {
		writeError(w, 400, "image must be between 1 byte and 5 MB")
		return
	}
	imageURL, err := s.saveBoardImage(id, data)
	if err != nil {
		writeError(w, 400, err.Error())
		return
	}
	s.recordAudit("admin-api", "board.image", id, map[string]any{"image_url": imageURL})
	writeJSON(w, 200, map[string]any{"image_url": imageURL})
}

func (s *server) saveBoardImage(id string, data []byte) (string, error) {
	if !s.boardExists(id) {
		return "", errors.New("board not found")
	}
	if len(data) == 0 || len(data) > 5<<20 {
		return "", errors.New("image must be between 1 byte and 5 MB")
	}
	mime := http.DetectContentType(data)
	ext := map[string]string{"image/jpeg": ".jpg", "image/png": ".png", "image/webp": ".webp"}[mime]
	if ext == "" {
		return "", errors.New("only JPEG, PNG and WebP images are allowed")
	}
	digest := sha256.Sum256(data)
	name := fmt.Sprintf("%s-%s%s", id, hex.EncodeToString(digest[:])[:12], ext)
	if err := os.WriteFile(filepath.Join(s.boardImagesDir, name), data, 0640); err != nil {
		return "", err
	}
	var old string
	_ = s.db.QueryRow(`SELECT image_filename FROM boards WHERE id=?`, id).Scan(&old)
	imageURL := s.publicPath("/board-images/" + name)
	if _, err := s.db.Exec(`UPDATE boards SET image_url=?,image_filename=?,updated_at=? WHERE id=?`, imageURL, name, time.Now().Unix(), id); err != nil {
		_ = os.Remove(filepath.Join(s.boardImagesDir, name))
		return "", err
	}
	if old != "" && old != name {
		_ = os.Remove(filepath.Join(s.boardImagesDir, filepath.Base(old)))
	}
	return imageURL, nil
}

func (s *server) serveBoardImage(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/board-images/")
	if name == "" || name != filepath.Base(name) || strings.Contains(name, "..") {
		writeError(w, 404, "not found")
		return
	}
	if ext := strings.ToLower(filepath.Ext(name)); ext != ".jpg" && ext != ".jpeg" && ext != ".png" && ext != ".webp" {
		writeError(w, 404, "not found")
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	http.ServeFile(w, r, filepath.Join(s.boardImagesDir, name))
}
