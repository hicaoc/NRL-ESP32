// NRL OTA server: Go API + SQLite registry. The Vue frontend is deployed separately.
package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

var validName = regexp.MustCompile(`^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$`)

type server struct {
	db                                                                              *sql.DB
	firmwareDir, packagesDir, boardImagesDir, adminToken, deviceToken, publicPrefix string
	adminUser, adminPassword                                                        string
	sessionSecret                                                                   []byte
	sessionTTL                                                                      time.Duration
}

// A flash package is one build's complete flash image set. The server stores it
// versioned, serves the esp-web-tools USB manifest from it, and registers the
// app slice (the part at appOffset) as the board's OTA release — so the OTA
// online-update firmware is literally a file inside the USB flashing package.
type packagePart struct {
	Offset int64  `json:"offset"`
	Name   string `json:"name"`
}
type packageMeta struct {
	Board      string        `json:"board"`
	Version    string        `json:"version"`
	Channel    string        `json:"channel"`
	Notes      string        `json:"notes"`
	ChipFamily string        `json:"chip_family"` // e.g. "ESP32-S3"; empty = not web-flashable
	AppOffset  int64         `json:"app_offset"`
	Parts      []packagePart `json:"parts"`
}
type release struct {
	Version   string `json:"version"`
	Channel   string `json:"channel"`
	URL       string `json:"url"`
	SHA256    string `json:"sha256"`
	Notes     string `json:"notes"`
	Size      int64  `json:"size"`
	CreatedAt int64  `json:"created_at"`
}
type deviceCheck struct {
	DeviceID        string          `json:"device_id"`
	BoardType       string          `json:"board_type"`
	FirmwareVersion string          `json:"firmware_version"`
	Channel         string          `json:"channel"`
	Metadata        json.RawMessage `json:"metadata"`
}

func main() {
	var dataDir, listen, publicPrefix string
	flag.StringVar(&dataDir, "data-dir", "./ota-data", "SQLite database and firmware directory")
	flag.StringVar(&listen, "listen", "127.0.0.1:8080", "HTTP listen address (put HTTPS proxy in front in production)")
	flag.StringVar(&publicPrefix, "public-prefix", "/nrlota/api", "public reverse-proxy prefix for API and firmware URLs")
	flag.Parse()
	publicPrefix = strings.Trim(publicPrefix, "/")
	if publicPrefix != "" {
		publicPrefix = "/" + publicPrefix
	}
	admin := os.Getenv("OTA_ADMIN_TOKEN")
	if admin == "" {
		log.Fatal("OTA_ADMIN_TOKEN is required")
	}
	if err := os.MkdirAll(filepath.Join(dataDir, "firmware"), 0750); err != nil {
		log.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dataDir, "packages"), 0750); err != nil {
		log.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dataDir, "board-images"), 0750); err != nil {
		log.Fatal(err)
	}
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "ota.sqlite3"))
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()
	if err = initDB(db); err != nil {
		log.Fatal(err)
	}
	// Migration: the download URL used to be derived as "/firmware/"+filename.
	// Package releases live under /packages/, so store the URL explicitly. The
	// ALTER fails harmlessly once the column exists.
	_, _ = db.Exec(`ALTER TABLE releases ADD COLUMN url TEXT NOT NULL DEFAULT ''`)
	adminUser := os.Getenv("OTA_ADMIN_USER")
	if adminUser == "" {
		adminUser = "admin"
	}
	secret := make([]byte, 32)
	if _, err := rand.Read(secret); err != nil {
		log.Fatal(err)
	}
	s := &server{
		db:             db,
		firmwareDir:    filepath.Join(dataDir, "firmware"),
		packagesDir:    filepath.Join(dataDir, "packages"),
		boardImagesDir: filepath.Join(dataDir, "board-images"),
		publicPrefix:   publicPrefix,
		adminToken:     admin,
		deviceToken:    os.Getenv("OTA_DEVICE_TOKEN"),
		adminUser:      adminUser,
		adminPassword:  os.Getenv("OTA_ADMIN_PASSWORD"),
		sessionSecret:  secret,
		sessionTTL:     12 * time.Hour,
	}
	mux := http.NewServeMux()
	s.routes(mux)
	log.Printf("NRL OTA server listening at %s", listen)
	log.Fatal(http.ListenAndServe(listen, securityHeaders(mux)))
}

func (s *server) routes(m *http.ServeMux) {
	m.HandleFunc("POST /api/v1/admin/login", s.login)
	m.HandleFunc("POST /api/v1/admin/releases", s.uploadRelease)
	m.HandleFunc("POST /api/v1/admin/flash-package", s.uploadPackage)
	m.HandleFunc("GET /api/v1/admin/devices", s.listDevices)
	m.HandleFunc("GET /api/v1/admin/catalog", s.adminCatalog)
	m.HandleFunc("GET /api/v1/admin/audit", s.listAudit)
	m.HandleFunc("POST /api/v1/admin/boards", s.createBoard)
	m.HandleFunc("POST /api/v1/admin/catalog/import", s.importCatalog)
	m.HandleFunc("PUT /api/v1/admin/boards/{id}", s.updateBoard)
	m.HandleFunc("PUT /api/v1/admin/boards/{id}/features", s.updateBoardFeatures)
	m.HandleFunc("POST /api/v1/admin/boards/{id}/image", s.uploadBoardImage)
	m.HandleFunc("POST /api/v1/admin/features", s.createFeature)
	m.HandleFunc("PUT /api/v1/admin/features/{key}", s.updateFeature)
	m.HandleFunc("POST /api/v1/devices/check", s.checkDevice)
	m.HandleFunc("GET /api/v1/catalog", s.publicCatalog)
	m.HandleFunc("GET /api/v1/releases", s.listReleases)
	m.HandleFunc("GET /firmware/", s.serveFirmware)
	m.HandleFunc("GET /packages/", s.servePackage)
	m.HandleFunc("GET /flasher/", s.serveFlasher)
	m.HandleFunc("GET /board-images/", s.serveBoardImage)
	m.Handle("/mcp", s.mcpHandler())
}

// admin authorizes admin API calls. It accepts either the long-lived admin
// token (used by the machine publish pipeline via X-OTA-Token) or a signed
// session token minted by /api/v1/admin/login (sent by the web UI, either as a
// Bearer token or X-OTA-Token).
func (s *server) admin(w http.ResponseWriter, r *http.Request) bool {
	if s.isAdminRequest(r) {
		return true
	}
	writeError(w, http.StatusUnauthorized, "admin authorization required")
	return false
}

func (s *server) isAdminRequest(r *http.Request) bool {
	presented := r.Header.Get("X-OTA-Token")
	if presented == "" {
		presented = strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	}
	if presented != "" && subtle.ConstantTimeCompare([]byte(presented), []byte(s.adminToken)) == 1 {
		return true
	}
	if s.validSession(presented) {
		return true
	}
	return false
}

// login verifies username/password and returns a signed session token for the
// web UI. Password login is available only when OTA_ADMIN_PASSWORD is set; the
// machine pipeline keeps using the raw admin token instead.
func (s *server) login(w http.ResponseWriter, r *http.Request) {
	if s.adminPassword == "" {
		writeError(w, http.StatusServiceUnavailable, "password login is not configured (set OTA_ADMIN_PASSWORD)")
		return
	}
	var body struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4<<10)).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "invalid request")
		return
	}
	userOK := subtle.ConstantTimeCompare([]byte(body.Username), []byte(s.adminUser)) == 1
	passOK := subtle.ConstantTimeCompare([]byte(body.Password), []byte(s.adminPassword)) == 1
	if !userOK || !passOK {
		writeError(w, http.StatusUnauthorized, "invalid username or password")
		return
	}
	expires := time.Now().Add(s.sessionTTL)
	writeJSON(w, http.StatusOK, map[string]any{
		"token":    s.mintSession(expires),
		"username": s.adminUser,
		"expires":  expires.Unix(),
	})
}

// mintSession returns "<expiryUnix>.<hexHMAC>" signed with the per-process
// session secret. Restarting the server invalidates outstanding sessions.
func (s *server) mintSession(expires time.Time) string {
	payload := strconv.FormatInt(expires.Unix(), 10)
	mac := hmac.New(sha256.New, s.sessionSecret)
	mac.Write([]byte(payload))
	return payload + "." + hex.EncodeToString(mac.Sum(nil))
}

func (s *server) validSession(token string) bool {
	payload, sig, ok := strings.Cut(token, ".")
	if !ok {
		return false
	}
	expiry, err := strconv.ParseInt(payload, 10, 64)
	if err != nil || time.Now().Unix() > expiry {
		return false
	}
	mac := hmac.New(sha256.New, s.sessionSecret)
	mac.Write([]byte(payload))
	expected := hex.EncodeToString(mac.Sum(nil))
	return subtle.ConstantTimeCompare([]byte(sig), []byte(expected)) == 1
}
func (s *server) device(w http.ResponseWriter, r *http.Request) bool {
	if s.deviceToken == "" || r.Header.Get("X-Device-Token") == s.deviceToken {
		return true
	}
	writeError(w, http.StatusUnauthorized, "device authorization required")
	return false
}

func (s *server) uploadRelease(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	board, version, channel := r.Header.Get("X-Firmware-Board"), r.Header.Get("X-Firmware-Version"), r.Header.Get("X-Firmware-Channel")
	if channel == "" {
		channel = "stable"
	}
	if !validName.MatchString(board) || !validName.MatchString(version) || (channel != "stable" && channel != "beta") {
		writeError(w, 400, "invalid board, version or channel")
		return
	}
	if !s.boardExists(board) {
		writeError(w, 400, "board is not registered")
		return
	}
	file, err := os.CreateTemp(s.firmwareDir, ".upload-*")
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	defer os.Remove(file.Name())
	hash := sha256.New()
	size, err := io.Copy(io.MultiWriter(file, hash), io.LimitReader(r.Body, 16<<20+1))
	file.Close()
	if err != nil || size == 0 || size > 16<<20 {
		writeError(w, 400, "invalid firmware size")
		return
	}
	sha := hex.EncodeToString(hash.Sum(nil))
	name := fmt.Sprintf("%s-%s-%s.bin", board, version, sha[:12])
	if err = os.Rename(file.Name(), filepath.Join(s.firmwareDir, name)); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	now := time.Now().Unix()
	notes, _ := url.QueryUnescape(r.Header.Get("X-Release-Notes"))
	_, err = s.db.Exec(`INSERT INTO releases(board_type,version,channel,filename,sha256,size,notes,created_at,url) VALUES(?,?,?,?,?,?,?,?,?)`, board, version, channel, name, sha, size, notes, now, "/firmware/"+name)
	if err != nil {
		_ = os.Remove(filepath.Join(s.firmwareDir, name))
		writeError(w, 409, "release already exists")
		return
	}
	writeJSON(w, http.StatusCreated, release{Version: version, Channel: channel, URL: s.publicPath("/firmware/" + name), SHA256: sha, Size: size, Notes: notes, CreatedAt: now})
}

func (s *server) checkDevice(w http.ResponseWriter, r *http.Request) {
	if !s.device(w, r) {
		return
	}
	var d deviceCheck
	if err := json.NewDecoder(io.LimitReader(r.Body, 32<<10)).Decode(&d); err != nil || d.DeviceID == "" || !validName.MatchString(d.BoardType) || !validName.MatchString(d.FirmwareVersion) {
		writeError(w, 400, "device_id, board_type and firmware_version are required")
		return
	}
	if d.Channel == "" {
		d.Channel = "stable"
	}
	if !json.Valid(d.Metadata) {
		d.Metadata = []byte(`{}`)
	}
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	now := time.Now().Unix()
	_, err := s.db.Exec(`INSERT INTO devices(device_id,board_type,firmware_version,ip_address,metadata_json,first_seen,last_seen) VALUES(?,?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET board_type=excluded.board_type,firmware_version=excluded.firmware_version,ip_address=excluded.ip_address,metadata_json=excluded.metadata_json,last_seen=excluded.last_seen`, d.DeviceID, d.BoardType, d.FirmwareVersion, host, string(d.Metadata), now, now)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	var releases []release
	if s.boardPublished(d.BoardType) {
		releases, err = s.releases(d.BoardType, d.Channel)
	}
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	latest := ""
	if len(releases) > 0 {
		latest = releases[0].Version
	}
	writeJSON(w, 200, map[string]any{"device_id": d.DeviceID, "current_version": d.FirmwareVersion, "latest_version": latest, "update_available": latest != "" && latest != d.FirmwareVersion, "releases": releases})
}

func (s *server) listReleases(w http.ResponseWriter, r *http.Request) {
	board := r.URL.Query().Get("board")
	if !validName.MatchString(board) {
		writeError(w, 400, "valid board is required")
		return
	}
	if !s.boardPublished(board) {
		writeError(w, 404, "published board not found")
		return
	}
	rows, err := s.releases(board, "")
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"releases": rows})
}

// releases returns a board's releases newest-first. An empty channel returns
// every channel (the public history); a specific channel filters to it (the
// device update check).
func (s *server) releases(board, channel string) ([]release, error) {
	query := `SELECT version,channel,filename,sha256,size,notes,created_at,url FROM releases WHERE board_type=?`
	args := []any{board}
	if channel != "" {
		query += ` AND channel=?`
		args = append(args, channel)
	}
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []release
	for rows.Next() {
		var x release
		var filename, storedURL string
		if err := rows.Scan(&x.Version, &x.Channel, &filename, &x.SHA256, &x.Size, &x.Notes, &x.CreatedAt, &storedURL); err != nil {
			return nil, err
		}
		if storedURL == "" {
			storedURL = "/firmware/" + filename // legacy rows predate the url column
		}
		x.URL = s.publicPath(storedURL)
		out = append(out, x)
	}
	sort.SliceStable(out, func(i, j int) bool { return versionKey(out[i].Version) > versionKey(out[j].Version) })
	return out, rows.Err()
}

func (s *server) publicPath(path string) string {
	return s.publicPrefix + path
}
func versionKey(v string) string {
	parts := strings.FieldsFunc(v, func(r rune) bool { return r == '.' || r == '-' || r == '+' || r == '_' })
	var b strings.Builder
	for _, p := range parts {
		fmt.Fprintf(&b, "%08s", p)
	}
	return b.String()
}

func (s *server) listDevices(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	rows, err := s.db.Query(`SELECT device_id,board_type,firmware_version,ip_address,metadata_json,first_seen,last_seen FROM devices ORDER BY last_seen DESC`)
	if err != nil {
		writeError(w, 500, err.Error())
		return
	}
	defer rows.Close()
	var out []map[string]any
	for rows.Next() {
		var id, b, v, ip, meta string
		var first, last int64
		if err = rows.Scan(&id, &b, &v, &ip, &meta, &first, &last); err != nil {
			writeError(w, 500, err.Error())
			return
		}
		out = append(out, map[string]any{"device_id": id, "board_type": b, "firmware_version": v, "ip_address": ip, "metadata": json.RawMessage(meta), "first_seen": first, "last_seen": last})
	}
	writeJSON(w, 200, map[string]any{"devices": out})
}
func (s *server) serveFirmware(w http.ResponseWriter, r *http.Request) {
	name := filepath.Base(r.URL.Path)
	if !strings.HasSuffix(name, ".bin") || strings.Contains(name, "..") {
		writeError(w, 404, "not found")
		return
	}
	http.ServeFile(w, r, filepath.Join(s.firmwareDir, name))
}

// serveFlasher generates the esp-web-tools manifest for a board on the fly from
// its latest uploaded flash package: GET /flasher/manifest-<board>.json. The
// manifest points at the package parts under /packages/. Boards whose package
// has no chip_family (not web-flashable, e.g. the ESP32-S31) return 404.
func (s *server) serveFlasher(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/flasher/")
	board, ok := strings.CutPrefix(name, "manifest-")
	board, ok2 := strings.CutSuffix(board, ".json")
	if !ok || !ok2 || !validName.MatchString(board) {
		writeError(w, 404, "not found")
		return
	}
	if !s.boardPublished(board) {
		writeError(w, 404, "published board not found")
		return
	}
	releases, err := s.releases(board, "")
	if err != nil || len(releases) == 0 {
		writeError(w, 404, "no firmware for this board")
		return
	}
	meta, err := s.readPackageMeta(board, releases[0].Version)
	if err != nil || meta.ChipFamily == "" {
		writeError(w, 404, "no web-flashable package for this board")
		return
	}
	parts := make([]map[string]any, 0, len(meta.Parts))
	for _, p := range meta.Parts {
		parts = append(parts, map[string]any{
			"path":   s.publicPath(fmt.Sprintf("/packages/%s/%s/%s", board, meta.Version, p.Name)),
			"offset": p.Offset,
		})
	}
	writeJSON(w, 200, map[string]any{
		"name":                     "NRL " + board,
		"version":                  meta.Version,
		"new_install_prompt_erase": true,
		"builds":                   []map[string]any{{"chipFamily": meta.ChipFamily, "parts": parts}},
	})
}

func (s *server) packageDir(board, version string) string {
	return filepath.Join(s.packagesDir, board, version)
}

func (s *server) readPackageMeta(board, version string) (packageMeta, error) {
	var meta packageMeta
	data, err := os.ReadFile(filepath.Join(s.packageDir(board, version), "package.json"))
	if err != nil {
		return meta, err
	}
	return meta, json.Unmarshal(data, &meta)
}

// uploadPackage ingests one build's complete flash image set (multipart form: a
// "meta" JSON field plus one file field per part, keyed by part name). It stores
// the parts under packages/<board>/<version>/, writes package.json for manifest
// generation, and registers the app slice (part at app_offset) as the board's
// OTA release.
func (s *server) uploadPackage(w http.ResponseWriter, r *http.Request) {
	if !s.admin(w, r) {
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 64<<20)
	if err := r.ParseMultipartForm(8 << 20); err != nil {
		writeError(w, 400, "invalid multipart upload: "+err.Error())
		return
	}
	var meta packageMeta
	if err := json.Unmarshal([]byte(r.FormValue("meta")), &meta); err != nil {
		writeError(w, 400, "invalid meta field")
		return
	}
	if meta.Channel == "" {
		meta.Channel = "stable"
	}
	if !validName.MatchString(meta.Board) || !validName.MatchString(meta.Version) ||
		(meta.Channel != "stable" && meta.Channel != "beta") || len(meta.Parts) == 0 {
		writeError(w, 400, "invalid board, version, channel or parts")
		return
	}
	if !s.boardExists(meta.Board) {
		writeError(w, 400, "board is not registered")
		return
	}
	if expected, ok := s.boardChipFamily(meta.Board); ok && meta.ChipFamily != expected {
		writeError(w, 400, "chip_family does not match the registered board")
		return
	}
	// An app-only release may already exist when an administrator later uploads
	// the complete flash package. It can be upgraded in place only when the app
	// image in the package is byte-for-byte identical. A different image keeps
	// the immutable board/version/channel guarantee and is rejected below.
	var existingFilename, existingSHA, existingURL string
	var existingSize int64
	existingErr := s.db.QueryRow(`SELECT filename,sha256,size,url FROM releases WHERE board_type=? AND version=? AND channel=?`,
		meta.Board, meta.Version, meta.Channel).Scan(&existingFilename, &existingSHA, &existingSize, &existingURL)
	if existingErr != nil && existingErr != sql.ErrNoRows {
		writeError(w, 500, existingErr.Error())
		return
	}
	if existingErr == nil && strings.HasPrefix(existingURL, "/packages/") {
		writeError(w, 409, "complete package already exists")
		return
	}

	dir := s.packageDir(meta.Board, meta.Version)
	if err := os.MkdirAll(dir, 0750); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	cleanup := func() { _ = os.RemoveAll(dir) }

	var appName, appSHA string
	var appSize int64
	for _, part := range meta.Parts {
		if !validName.MatchString(part.Name) || !strings.HasSuffix(part.Name, ".bin") {
			cleanup()
			writeError(w, 400, "invalid part name: "+part.Name)
			return
		}
		file, _, err := r.FormFile(part.Name)
		if err != nil {
			cleanup()
			writeError(w, 400, "missing part file: "+part.Name)
			return
		}
		out, err := os.Create(filepath.Join(dir, part.Name))
		if err != nil {
			file.Close()
			cleanup()
			writeError(w, 500, err.Error())
			return
		}
		hash := sha256.New()
		n, err := io.Copy(io.MultiWriter(out, hash), file)
		out.Close()
		file.Close()
		if err != nil {
			cleanup()
			writeError(w, 500, err.Error())
			return
		}
		if part.Offset == meta.AppOffset {
			appName, appSHA, appSize = part.Name, hex.EncodeToString(hash.Sum(nil)), n
		}
	}
	if appName == "" {
		cleanup()
		writeError(w, 400, "no part matched app_offset")
		return
	}

	metaJSON, _ := json.MarshalIndent(meta, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, "package.json"), metaJSON, 0640); err != nil {
		cleanup()
		writeError(w, 500, err.Error())
		return
	}

	now := time.Now().Unix()
	appURL := fmt.Sprintf("/packages/%s/%s/%s", meta.Board, meta.Version, appName)
	relPath := fmt.Sprintf("%s/%s/%s", meta.Board, meta.Version, appName)
	var err error
	if existingErr == nil {
		if existingSHA != appSHA || existingSize != appSize {
			cleanup()
			writeError(w, 409, "release already exists with different firmware")
			return
		}
		_, err = s.db.Exec(`UPDATE releases SET filename=?,notes=?,created_at=?,url=? WHERE board_type=? AND version=? AND channel=? AND sha256=? AND size=?`,
			relPath, meta.Notes, now, appURL, meta.Board, meta.Version, meta.Channel, appSHA, appSize)
	} else {
		_, err = s.db.Exec(`INSERT INTO releases(board_type,version,channel,filename,sha256,size,notes,created_at,url) VALUES(?,?,?,?,?,?,?,?,?)`,
			meta.Board, meta.Version, meta.Channel, relPath, appSHA, appSize, meta.Notes, now, appURL)
	}
	if err != nil {
		cleanup()
		writeError(w, 500, err.Error())
		return
	}
	if existingErr == nil && strings.HasPrefix(existingURL, "/firmware/") {
		_ = os.Remove(filepath.Join(s.firmwareDir, filepath.Base(existingFilename)))
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"board": meta.Board, "version": meta.Version, "channel": meta.Channel,
		"app_url": s.publicPath(appURL), "sha256": appSHA, "size": appSize,
		"web_flashable": meta.ChipFamily != "", "parts": len(meta.Parts),
	})
}

// servePackage serves the versioned flash-image files a package is made of:
// GET /packages/<board>/<version>/<file>.bin. Only .bin files under packagesDir
// are servable.
func (s *server) servePackage(w http.ResponseWriter, r *http.Request) {
	rel := strings.TrimPrefix(r.URL.Path, "/packages/")
	if rel == "" || strings.Contains(rel, "..") || !strings.HasSuffix(rel, ".bin") {
		writeError(w, 404, "not found")
		return
	}
	full := filepath.Join(s.packagesDir, filepath.FromSlash(rel))
	if !strings.HasPrefix(full, s.packagesDir+string(os.PathSeparator)) {
		writeError(w, 404, "not found")
		return
	}
	http.ServeFile(w, r, full)
}
func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}
func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "same-origin")
		next.ServeHTTP(w, r)
	})
}
