# NRL OTA 服务器（Go + SQLite API + 独立 Vue 前端）

Go 可执行文件提供 OTA API、动态板卡目录和 MCP 服务；请独立发布 `frontend/dist`，并通过同域名反向代理连接前端与 API。数据库首次迁移会保留并预置 `gezipai`、`bh4tdv`、`s31_korvo` 和 `s31_function_coreboard`，管理员随后可在页面中创建更多板卡类型。

独立构建并发布 Vue 管理站点，再构建 Go 服务：

Vite+（`vp`）使用 `PATH` 中的 `node` 运行，要求 **Node 24 或更高版本**（见 `frontend/.node-version`）。较低版本的 Node 运行 `vp` 启动器时会报 `ERR_UNKNOWN_FILE_EXTENSION`；请先切换至 Node 24，例如执行 `nvm use 24`。

```powershell
cd ota-server/frontend
vp install --frozen-lockfile
vp check
vp build
cd ..
go build -o nrl-ota.exe .
$env:OTA_ADMIN_TOKEN = 'long-random-admin-secret'      # 自动发布流水线使用的机器令牌
$env:OTA_ADMIN_USER = 'admin'                          # Web 登录用户名（默认：admin）
$env:OTA_ADMIN_PASSWORD = 'long-random-admin-password' # Web 登录密码（启用管理界面登录所必需）
$env:OTA_DEVICE_TOKEN = 'optional-device-access-token'
.\nrl-ota.exe -listen 127.0.0.1:8080 -data-dir D:\ota-data
```

修改 Vue 前端后，执行 `vp build` 并发布生成的 `frontend/dist` 即可，不需要重新构建 Go 可执行文件。

在 Windows PowerShell 中发布 Linux（`amd64`）API 后端：

```powershell
cd ota-server
.\deploy.ps1 -DeployUser your-ssh-user
```

脚本默认上传到 `/nrlota/nrl-ota`，保留 `/nrlota/nrl-ota.previous`，然后重启
`nrl-ota.service`。如服务器路径或服务名不同，可用 `-RemoteBinary`、`-Service`
覆盖。Linux/macOS 可使用 `OTA_DEPLOY_USER=... bash deploy.sh`。

也可将前端单独发布为容器：

```powershell
cd ota-server/frontend
docker build -t nrl-ota-frontend .
docker run -d --name nrl-ota-frontend -p 8081:80 nrl-ota-frontend
```

前端容器默认把 `/nrlota/api/` 代理到 `http://nrl-ota:8080`。如 Go API
使用其他可访问地址，可覆盖 `API_UPSTREAM`，例如
`-e API_UPSTREAM=http://host.docker.internal:8080`。

如果前端和 API 位于不同源，请通过反向代理将 API 暴露在前端的同一域名下（参考 `Caddyfile.example`）。前端所有动态请求统一使用同源的 `/nrlota/api/` 前缀，`/nrlota/www` 仅作为静态文件目录。
Nginx 可使用 `nginx.conf.example`，Caddy 可使用 `Caddyfile.example`；两者都会在转发到 Go 前去掉 `/nrlota/api/` 前缀。

生产环境可用以下命令将前端发布到 `ota.nrlptt.com` 的 `/nrlota/www/`：

```bash
cd ota-server/frontend
OTA_DEPLOY_USER=your-ssh-user bash deploy.sh
```

`deploy.sh` 会先运行 `vp install --frozen-lockfile` 和 `vp build`，再使用
`rsync --delete` 发布。本机需要 `vp`、`ssh`、`rsync`，且远端目录必须预先创建。

Windows PowerShell 请使用原生 OpenSSH 发布脚本：

```powershell
cd ota-server\frontend
.\deploy.ps1 -DeployUser your-ssh-user
```

该脚本使用新版 Windows 自带的 `scp`。与 `rsync` 不同，它不会清理远端旧的哈希资源文件。

设备端同时接受 `http://` 和 `https://` OTA 地址。HTTP 仅适用于测试或可信的私有局域网，因为固件和设备上报数据不会加密，也无法验证服务器身份。其他部署请使用 Caddy 或 nginx 配置 HTTPS 反向代理。内网私有部署时 `OTA_DEVICE_TOKEN` 可选；面向互联网部署时强烈建议设置。

网站是菜单式单页应用（SPA），提供以下页面：

- **首页**：可搜索的动态板卡目录、板卡介绍和功能对照。
- **固件**：按板卡展示版本历史和更新日志。
- **USB 烧录**：浏览器内 USB 全量烧录。
- **设备**：管理员登录后可见的设备管理面板，显示每台设备的在线状态、板卡型号、固件版本（含可升级标记）、NRL 呼号、SSID、IP 地址和最后在线时间。
- **板卡管理**：创建板卡草稿、上传 JPEG/PNG/WebP 图片、编辑中英文名称与说明、选择现有功能或添加功能，并在资料完整后发布。
- **发布固件**：从动态板卡目录选择板卡，上传应用固件；草稿板卡可以预先上传固件，但设备只会获取已发布板卡的更新。

管理员通过页面右上角使用 `OTA_ADMIN_USER` / `OTA_ADMIN_PASSWORD` 登录。登录成功后会返回一个有效期为 12 小时、用于授权管理 API 的签名会话令牌。长期有效的 `OTA_ADMIN_TOKEN` 仍可直接用于 API，自动发布脚本正是使用该令牌。未设置 `OTA_ADMIN_PASSWORD` 时，密码登录功能会被禁用并返回 HTTP 503。会话令牌使用每次进程启动时生成的密钥签名，因此重启服务器会使所有管理员退出登录。

## AI / MCP 板卡管理

服务在 `/mcp` 提供 MCP Streamable HTTP 接口；通过示例反向代理访问时地址为 `https://<host>/nrlota/api/mcp`。每个请求都必须携带 `Authorization: Bearer <OTA_ADMIN_TOKEN或管理员会话令牌>`。当前提供以下工具：

- `catalog.list`：读取板卡和可复用功能目录；
- `board.save_draft`：创建或更新板卡草稿；
- `feature.save`：创建或更新功能定义；
- `board.set_features`：设置板卡功能矩阵；
- `board.upload_image`：提交不超过 5 MB 的 base64 JPEG/PNG/WebP 图片；
- `board.publish`：显式确认后发布资料完整的板卡。
- `audit.list`：查看最近的页面/API/MCP 板卡目录变更记录。

AI 提交默认保存为草稿。公开发布被设计成独立操作，并要求板卡已具备双语名称、图片和至少一个功能配置。管理页面还提供“AI / JSON 导入”，可一次导入板卡资料、功能定义和功能状态；图片继续走独立校验接口。远程公网部署应在 HTTPS 反向代理后使用，并为 MCP 配置专用的短期管理员会话；长期机器令牌更适合受控自动化环境。

要在原生固件构建成功后自动发布，请在构建环境中设置以下变量（只有同时设置前两个变量才会执行上传）：

```powershell
$env:OTA_SERVER_URL = 'https://ota.example.com'
$env:OTA_UPLOAD_TOKEN = $env:OTA_ADMIN_TOKEN
$env:OTA_VERSION = '0.5.2'       # 可选；默认从 nrl_version.h 读取
$env:OTA_RELEASE_NOTES = '修复说明'
python scripts/build.py s31_korvo build
```

要用一条命令发布四种板卡的完整刷机包及 OTA 版本，请运行 `python scripts/publish_ota.py`。如只需发布部分板卡，可在命令后指定一个或多个板卡标识。脚本会从各构建目录的 `flasher_args.json` 读取全部镜像及其烧录地址，不再需要单独上传应用固件。

API 容器请在 `ota-server/` 目录中构建，持久化挂载 `/data`，并设置两个令牌环境变量。前端容器或构建产物需独立发布；项目提供的 `Caddyfile.example` 会终止 TLS、提供 `/nrlota/www` 下的前端静态文件并将 API 请求反向代理到 API 端口。

## USB 网页烧录器

网站内置浏览器 USB 烧录器（esp-web-tools / Web Serial），可用于首次进行全量烧录。它只支持通过 HTTPS（或 localhost）访问的 Chrome/Edge，并且 **仅支持两种 ESP32-S3 板卡**：`gezipai` 与 `bh4tdv`。由于 esptool-js 不支持 ESP32-S31，`s31_korvo` 和 `s31_function_coreboard` 只能使用串口烧录，页面会给出相应提示。

esp-web-tools 资源会随独立发布的前端提供。完整刷机包按版本存储在 `<data-dir>/packages/<board>/<version>/`，API 会动态生成各板卡的 `manifest-<board>.json`，因此发布新的可烧录固件时无需重新构建任何服务。

先构建所有板卡，再用一条命令发布全部完整包及其中的 OTA 应用固件：

```powershell
python scripts/build.py gezipai build
python scripts/build.py bh4tdv build
python scripts/build.py s31_korvo build
python scripts/build.py s31_function_coreboard build
python scripts/publish_ota.py
```

如果原生构建时已经设置 `OTA_SERVER_URL` 和 `OTA_UPLOAD_TOKEN`，`scripts/build.py` 会在构建成功后自动发布该板卡的完整包。

## 设备端升级控制

所有板卡（包括没有屏幕的板卡）都在各自的配置服务器中提供 `/update` 页面。完成 HTTP 或 HTTPS OTA 服务器配置后，串口 AT 控制台支持以下命令：

```text
AT+OTAURL=https://ota.example.com,device-token  # 配置服务器地址和令牌
AT+OTAURL=?                                     # 显示地址和最新版本
AT+OTACHECK=1                                   # 获取当前固件列表
AT+OTALIST=?                                    # 列出兼容版本
AT+OTA=LATEST                                   # 检查并安装最新版本
AT+OTA=0.5.1                                    # 安装列表中的历史版本
AT+OTA=?                                        # 显示 OTA 状态
```

OTA 配置与安装命令只接受本地串口 AT 控制台请求。无触摸屏的 Gezipai 会在 LCD 上提示有新固件；同时长按 `VOL+` 和 `VOL-` 会检查并安装最新的兼容版本。
