package main

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	_ "modernc.org/sqlite"
)

func TestUploadPackageUpgradesMatchingAppOnlyRelease(t *testing.T) {
	dataDir := t.TempDir()
	firmwareDir := filepath.Join(dataDir, "firmware")
	packagesDir := filepath.Join(dataDir, "packages")
	if err := os.MkdirAll(firmwareDir, 0750); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(packagesDir, 0750); err != nil {
		t.Fatal(err)
	}
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "test.sqlite3"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if err = initDB(db); err != nil {
		t.Fatal(err)
	}

	app := []byte("matching application image")
	digest := sha256.Sum256(app)
	appSHA := hex.EncodeToString(digest[:])
	oldName := "gezipai-0.6.0-old.bin"
	if err = os.WriteFile(filepath.Join(firmwareDir, oldName), app, 0640); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(`INSERT INTO releases(board_type,version,channel,filename,sha256,size,notes,created_at,url) VALUES(?,?,?,?,?,?,?,?,?)`,
		"gezipai", "0.6.0", "stable", oldName, appSHA, len(app), "app only", 1, "/firmware/"+oldName); err != nil {
		t.Fatal(err)
	}

	s := &server{db: db, firmwareDir: firmwareDir, packagesDir: packagesDir, adminToken: "admin"}
	meta := packageMeta{
		Board: "gezipai", Version: "0.6.0", Channel: "stable", Notes: "complete",
		ChipFamily: "ESP32-S3", AppOffset: 0x10000,
		Parts: []packagePart{
			{Offset: 0, Name: "bootloader.bin"},
			{Offset: 0x10000, Name: "nrl-esp32.bin"},
		},
	}
	request := packageRequest(t, meta, map[string][]byte{
		"bootloader.bin": []byte("bootloader"),
		"nrl-esp32.bin":  app,
	})
	recorder := httptest.NewRecorder()
	s.uploadPackage(recorder, request)
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}

	var gotURL, gotSHA string
	if err = db.QueryRow(`SELECT url,sha256 FROM releases WHERE board_type=? AND version=? AND channel=?`,
		"gezipai", "0.6.0", "stable").Scan(&gotURL, &gotSHA); err != nil {
		t.Fatal(err)
	}
	if want := "/packages/gezipai/0.6.0/nrl-esp32.bin"; gotURL != want {
		t.Fatalf("url = %q, want %q", gotURL, want)
	}
	if gotSHA != appSHA {
		t.Fatalf("sha256 = %q, want %q", gotSHA, appSHA)
	}
	if _, err = os.Stat(filepath.Join(packagesDir, "gezipai", "0.6.0", "package.json")); err != nil {
		t.Fatalf("package metadata was not stored: %v", err)
	}
	if _, err = os.Stat(filepath.Join(firmwareDir, oldName)); !os.IsNotExist(err) {
		t.Fatalf("old app-only image still exists, stat error: %v", err)
	}
}

func TestCatalogSeedsExistingBoardsAndFeatures(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "catalog.sqlite3"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if err = initDB(db); err != nil {
		t.Fatal(err)
	}
	s := &server{db: db}
	catalog, err := s.loadCatalog(false)
	if err != nil {
		t.Fatal(err)
	}
	if len(catalog.Boards) != 4 {
		t.Fatalf("published boards = %d, want 4", len(catalog.Boards))
	}
	if len(catalog.Features) < 30 {
		t.Fatalf("features = %d, want at least 30", len(catalog.Features))
	}
	if got := catalog.Boards[0].Features["aprs"]; got != "yes" {
		t.Fatalf("gezipai APRS state = %q, want yes", got)
	}
}

func TestMCPRequiresAdminAndListsTools(t *testing.T) {
	dataDir := t.TempDir()
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "mcp.sqlite3"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if err = initDB(db); err != nil {
		t.Fatal(err)
	}
	s := &server{db: db, adminToken: "admin", boardImagesDir: filepath.Join(dataDir, "images")}
	if err = os.MkdirAll(s.boardImagesDir, 0750); err != nil {
		t.Fatal(err)
	}
	handler := s.mcpHandler()
	body := `{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}`

	unauthorized := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/mcp", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json, text/event-stream")
	handler.ServeHTTP(unauthorized, request)
	if unauthorized.Code != http.StatusUnauthorized {
		t.Fatalf("unauthorized status = %d, want 401", unauthorized.Code)
	}

	authorized := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodPost, "/mcp", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json, text/event-stream")
	request.Header.Set("Authorization", "Bearer admin")
	request.Header.Set("MCP-Protocol-Version", "2025-11-25")
	handler.ServeHTTP(authorized, request)
	if authorized.Code != http.StatusOK {
		t.Fatalf("authorized status = %d, body = %s", authorized.Code, authorized.Body.String())
	}
	if !strings.Contains(authorized.Body.String(), `catalog.list`) || !strings.Contains(authorized.Body.String(), `board.publish`) {
		t.Fatalf("MCP tools missing from response: %s", authorized.Body.String())
	}
}

func TestAIImportCreatesDraftThenAdminPublishesCustomBoard(t *testing.T) {
	dataDir := t.TempDir()
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "custom.sqlite3"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if err = initDB(db); err != nil {
		t.Fatal(err)
	}
	s := &server{
		db:             db,
		adminToken:     "admin",
		boardImagesDir: filepath.Join(dataDir, "images"),
		firmwareDir:    filepath.Join(dataDir, "firmware"),
		packagesDir:    filepath.Join(dataDir, "packages"),
	}
	for _, dir := range []string{s.boardImagesDir, s.firmwareDir, s.packagesDir} {
		if err = os.MkdirAll(dir, 0750); err != nil {
			t.Fatal(err)
		}
	}
	importBody := `{
		"board":{"id":"custom_radio","name_zh":"自定义电台板","name_en":"Custom Radio Board","chip_label":"ESP32-S3","web_flash_chip_family":"ESP32-S3","status":"published","highlights_zh":["测试功能"],"highlights_en":["Test feature"]},
		"features":[{"key":"custom_feature","label_zh":"自定义功能","label_en":"Custom feature","group":"custom","display_order":900,"active":true}],
		"assignments":{"custom_feature":{"state":"partial","note_zh":"需要外接模块","note_en":"External module required"}}
	}`
	request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/catalog/import", strings.NewReader(importBody))
	request.Header.Set("X-OTA-Token", "admin")
	recorder := httptest.NewRecorder()
	s.importCatalog(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("import status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	var status string
	if err = db.QueryRow(`SELECT status FROM boards WHERE id='custom_radio'`).Scan(&status); err != nil {
		t.Fatal(err)
	}
	if status != "draft" {
		t.Fatalf("AI import status = %q, want draft", status)
	}

	pngData, err := base64.StdEncoding.DecodeString("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.saveBoardImage("custom_radio", pngData); err != nil {
		t.Fatal(err)
	}
	request = httptest.NewRequest(http.MethodPut, "/api/v1/admin/boards/custom_radio/features", strings.NewReader(`{"features":{"custom_feature":{"state":"partial","note_zh":"页面限制","note_en":"Page limitation"}}}`))
	request.SetPathValue("id", "custom_radio")
	request.Header.Set("X-OTA-Token", "admin")
	recorder = httptest.NewRecorder()
	s.updateBoardFeatures(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("feature update status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	updateBody := `{"id":"custom_radio","name_zh":"自定义电台板","name_en":"Custom Radio Board","tagline_zh":"可扩展板卡","tagline_en":"Extensible board","chip_label":"ESP32-S3","web_flash_chip_family":"ESP32-S3","display_order":100,"status":"published","highlights_zh":["测试功能"],"highlights_en":["Test feature"]}`
	request = httptest.NewRequest(http.MethodPut, "/api/v1/admin/boards/custom_radio", strings.NewReader(updateBody))
	request.SetPathValue("id", "custom_radio")
	request.Header.Set("X-OTA-Token", "admin")
	recorder = httptest.NewRecorder()
	s.updateBoard(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("publish status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	catalog, err := s.loadCatalog(false)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, board := range catalog.Boards {
		if board.ID == "custom_radio" {
			found = true
			if board.Features["custom_feature"] != "partial" || board.FeatureNotes["custom_feature"].ZH != "页面限制" {
				t.Fatalf("custom feature assignment was not preserved: %#v %#v", board.Features, board.FeatureNotes)
			}
			break
		}
	}
	if !found {
		t.Fatal("published custom board missing from public catalog")
	}
	events, err := s.auditEvents(10)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) < 2 || events[0].Target != "custom_radio" {
		t.Fatalf("custom board audit trail missing: %#v", events)
	}
}

func packageRequest(t *testing.T, meta packageMeta, files map[string][]byte) *http.Request {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	metaField, err := writer.CreateFormField("meta")
	if err != nil {
		t.Fatal(err)
	}
	if err = json.NewEncoder(metaField).Encode(meta); err != nil {
		t.Fatal(err)
	}
	for _, part := range meta.Parts {
		field, err := writer.CreateFormFile(part.Name, part.Name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = field.Write(files[part.Name]); err != nil {
			t.Fatal(err)
		}
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/flash-package", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	request.Header.Set("X-OTA-Token", "admin")
	return request
}
