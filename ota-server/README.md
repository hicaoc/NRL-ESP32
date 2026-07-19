# NRL OTA Server (Go + SQLite API + separate Vue frontend)

The Go executable serves the OTA API, dynamic board catalog, and MCP endpoint. Deploy `frontend/dist`
separately on the same origin (or use the frontend container); the supplied
Caddy example routes API requests to Go and serves the SPA. The initial migration
preserves and seeds `gezipai`, `bh4tdv`, `s31_korvo`, and
`s31_function_coreboard`; administrators can create additional board types in
the site without changing frontend source code.

Build and publish the Vue admin site separately, then build Go:

Vite+ (`vp`) runs under whatever `node` is on `PATH`; it needs **Node 24+**
(see `frontend/.node-version`). An older Node fails with
`ERR_UNKNOWN_FILE_EXTENSION` on the `vp` launcher. Select Node 24 first (e.g.
`nvm use 24`).

```powershell
cd ota-server/frontend
vp install --frozen-lockfile
vp check
vp build
cd ..
go build -o nrl-ota.exe .
$env:OTA_ADMIN_TOKEN = 'long-random-admin-secret'      # machine token (publish pipeline)
$env:OTA_ADMIN_USER = 'admin'                          # web login username (default: admin)
$env:OTA_ADMIN_PASSWORD = 'long-random-admin-password' # web login password (required for UI login)
$env:OTA_DEVICE_TOKEN = 'optional-device-access-token'
.\nrl-ota.exe -listen 127.0.0.1:8080 -data-dir D:\ota-data
```

After changing the Vue frontend, run `vp build` and publish the resulting
`frontend/dist` directory; rebuilding the Go executable is not required.

To publish the Linux (`amd64`) API server from Windows PowerShell:

```powershell
cd ota-server
.\deploy.ps1 -DeployUser your-ssh-user
```

The script uploads to `/nrlota/nrl-ota`, retains `/nrlota/nrl-ota.previous`,
then restarts `nrl-ota.service`. Override `-RemoteBinary` or `-Service` if the
server uses different names. The Linux/macOS equivalent is `OTA_DEPLOY_USER=... bash deploy.sh`.

To publish the frontend as its own container:

```powershell
cd ota-server/frontend
docker build -t nrl-ota-frontend .
docker run -d --name nrl-ota-frontend -p 8081:80 nrl-ota-frontend
```

The frontend container proxies `/nrlota/api/` to `http://nrl-ota:8080` by
default. Override `API_UPSTREAM` when the Go API uses another reachable address,
for example `-e API_UPSTREAM=http://host.docker.internal:8080`.

When the frontend and API are served from different origins, configure a reverse
proxy to expose the API under the frontend origin (as in `Caddyfile.example`).
The frontend uses the single same-origin dynamic prefix `/nrlota/api/`; static
files are served separately from `/nrlota/www`.
Use `nginx.conf.example` for Nginx, or `Caddyfile.example` for Caddy. Both
strip `/nrlota/api/` before proxying to Go.

For the production host, publish the built frontend to `/nrlota/www/` with:

```bash
cd ota-server/frontend
OTA_DEPLOY_USER=your-ssh-user bash deploy.sh
```

`deploy.sh` runs `vp install --frozen-lockfile` then `vp build`, and uses
`rsync --delete` to publish. It requires `vp`, `ssh`, and `rsync` locally, and
requires the target directory to already exist on `ota.nrlptt.com`.

On Windows PowerShell, use the native OpenSSH deployment script instead:

```powershell
cd ota-server\frontend
.\deploy.ps1 -DeployUser your-ssh-user
```

It uses `scp`, which is included with current Windows OpenSSH. Unlike `rsync`,
it does not prune old hashed assets.

Devices accept both `http://` and `https://` OTA URLs. Plain HTTP is intended
only for testing or a trusted private LAN because firmware and device reports
are not encrypted or server-authenticated. Put Caddy/nginx in front of the
process and use HTTPS for any other deployment. `OTA_DEVICE_TOKEN` is optional
for a private LAN but strongly recommended for an Internet-facing instance.

The site is a menu-based SPA: **Home** (searchable board introductions and a
dynamic feature comparison), **Firmware**
(per-board version history and changelogs), **USB Flash**, and — after admin
login — a **Devices** management dashboard showing each device's online status,
board, firmware version (with an update-available badge), NRL callsign, SSID,
IP, and last-seen time. The admin-only **Boards** page creates board drafts,
uploads JPEG/PNG/WebP images, edits bilingual descriptions, selects existing
features or creates new ones, and publishes complete board definitions. The
**Publish** page selects from that dynamic catalog. Draft boards may receive
firmware in advance, but devices only see updates for published boards. Admins
sign in from the top-right with
`OTA_ADMIN_USER` / `OTA_ADMIN_PASSWORD`; login returns a signed session token
(12 h) that authorizes the admin API. The long-lived `OTA_ADMIN_TOKEN` is still
accepted directly and is what the machine publish pipeline uses. Password login
is disabled (HTTP 503) until `OTA_ADMIN_PASSWORD` is set. Session tokens are
signed with a per-process secret, so restarting the server logs admins out.

## AI / MCP board management

The server exposes MCP Streamable HTTP at `/mcp`, or
`https://<host>/nrlota/api/mcp` through the example reverse proxy. Every request
must send `Authorization: Bearer <OTA_ADMIN_TOKEN-or-admin-session>`. Available
tools are `catalog.list`, `board.save_draft`, `feature.save`,
`board.set_features`, `board.upload_image`, and `board.publish`.
`audit.list` returns recent board-catalog changes made through the admin API or MCP.

AI submissions land as drafts by default. Public publication is a separate,
explicitly confirmed tool and requires bilingual names, an image, and at least
one feature assignment. The admin page also has an **AI / JSON import** form for
submitting board metadata, new feature definitions, and assignments in one
transaction; images continue through the separately validated upload path.
Expose MCP only behind HTTPS and prefer short-lived administrator sessions for
interactive remote clients; the long-lived machine token is intended for
controlled automation.

To publish automatically after a successful native firmware build, set these in
the build environment (the upload is deliberately disabled unless both values
exist):

```powershell
$env:OTA_SERVER_URL = 'https://ota.example.com'
$env:OTA_UPLOAD_TOKEN = $env:OTA_ADMIN_TOKEN
$env:OTA_VERSION = '0.5.2'       # optional; defaults to nrl_version.h
$env:OTA_RELEASE_NOTES = 'Fix …'
python scripts/build.py s31_korvo build
```

To publish the complete flash packages and OTA releases for all four boards in
one command, run `python scripts/publish_ota.py`. Pass one or more board
identifiers to publish only those boards. The script reads every image and its
flash offset from each build's `flasher_args.json`; a separate app-only upload
is not needed.

For the API container, build from `ota-server/`, mount `/data` persistently,
and set the two token environment variables. Deploy the frontend container (or
the built `frontend/dist`) separately. The included `Caddyfile.example`
terminates TLS, serves `/nrlota/www`, and proxies API requests to the API port.

## USB web flasher

The site includes an in-browser USB flasher (esp-web-tools / Web Serial) for a
first-time full-flash install. It only works in Chrome/Edge over HTTPS (or
localhost), and **only for the two ESP32-S3 boards** (`gezipai`, `bh4tdv`) —
esptool-js has no ESP32-S31 support, so `s31_korvo` and `s31_function_coreboard`
are serial-only and the page says so.

The esp-web-tools assets ship with the separately published frontend. Complete
flash packages are stored versioned under `<data-dir>/packages/<board>/<version>/`,
and the API generates each `manifest-<board>.json` dynamically. Publishing new
flashable firmware does not require rebuilding either service.

Build all boards, then publish every package and its OTA app image in one run:

```powershell
python scripts/build.py gezipai build
python scripts/build.py bh4tdv build
python scripts/build.py s31_korvo build
python scripts/build.py s31_function_coreboard build
python scripts/publish_ota.py
```

When `OTA_SERVER_URL` and `OTA_UPLOAD_TOKEN` are already set during a native
build, `scripts/build.py` publishes that board's complete package automatically.

## Device-side update controls

Every board exposes its own configuration server's `/update` page, including
boards without a display. After configuring its HTTP or HTTPS OTA server, the
serial AT console supports:

```text
AT+OTAURL=https://ota.example.com,device-token  # configure server/token
AT+OTAURL=?                                     # show URL and latest version
AT+OTACHECK=1                                   # fetch current release list
AT+OTALIST=?                                    # list compatible versions
AT+OTA=LATEST                                   # check then install newest
AT+OTA=0.5.1                                    # install a listed historical version
AT+OTA=?                                        # show OTA state
```

OTA configuration and installation commands are accepted only over the local
serial AT console. On the non-touch Gezipai, a new firmware notice is shown on
the LCD; holding `VOL+` and `VOL-` together checks and installs the latest
compatible release.
