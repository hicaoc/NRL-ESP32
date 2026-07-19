// The full esm-bundler build (not the runtime-only default) is required because
// the app below uses an inline `template` string, which needs Vue's runtime
// template compiler. Without it the app never renders and the page is stuck on
// the "Loading NRL OTA…" fallback.
import { createApp, computed, ref, watch } from "vue/dist/vue.esm-bundler.js";
import "./style.css";

// The deployed OTA API already owns its /api/v1 routes; static files are
// served separately by the web server.
const apiURL = (path) => path;

// Board catalog for the public "功能介绍" section. Kept here (not in the DB) so
// the marketing copy ships with the frontend and stays bilingual. Order defines
// the order boards appear on the page and in the firmware history.
const boards = [
  {
    id: "gezipai",
    chip: "ESP32-S3",
    flashable: true,
    image: "/boards/gezipai.jpg",
    zh: {
      name: "格子派 gezipai",
      tagline: "ESP32-S3 彩色显示终端",
      features: [
        "ES7210 麦克风 ADC + ES8311 DAC 音频链路",
        "240×240 ST7789 彩色屏与 LVGL 图形界面",
        "屏幕菜单管理 MDC1200、DTMF、CTCSS/PL 与 APRS",
        "音量+/音量-/PTT 三按键、电池电压检测",
        "三色状态灯、SCI 串口透明传输",
        "BLE 配网、Wi-Fi 配置门户、远程 AT 与 OTA 升级",
      ],
    },
    en: {
      name: "Gezipai",
      tagline: "ESP32-S3 color display terminal",
      features: [
        "ES7210 microphone ADC and ES8311 DAC audio path",
        "240×240 ST7789 color display with LVGL UI",
        "On-screen MDC1200, DTMF, CTCSS/PL, and APRS controls",
        "Volume up/down/PTT buttons and battery-voltage sensing",
        "Three-color status LED and SCI serial passthrough",
        "BLE provisioning, Wi-Fi portal, remote AT, and OTA updates",
      ],
    },
  },
  {
    id: "bh4tdv",
    chip: "ESP32-S3",
    flashable: true,
    image: "/boards/bh4tdv-esp32-3188.jpg",
    zh: {
      name: "BH4TDV ESP32 3188",
      tagline: "ESP32-S3 无屏电台接口",
      features: [
        "ES8311 全双工音频，连接 Moto3188 / NRL 电台",
        "PTT 输出、SQL 检测与三色状态灯",
        "三位频道选择输出，支持 0–7 共 8 个频道",
        "SCI 串口透明传输；无板载屏幕",
        "BLE 配网、Wi-Fi 配置门户、远程 AT 与 OTA 升级",
      ],
    },
    en: {
      name: "BH4TDV ESP32 3188",
      tagline: "ESP32-S3 headless radio interface",
      features: [
        "ES8311 full-duplex audio for Moto3188 / NRL radios",
        "PTT output, squelch detection, and three-color status LEDs",
        "Three-bit channel selection for channels 0–7",
        "SCI serial passthrough; no onboard display",
        "BLE provisioning, Wi-Fi portal, remote AT, and OTA updates",
      ],
    },
  },
  {
    id: "s31_korvo",
    chip: "ESP32-S31 · RISC-V",
    flashable: false,
    image: "/boards/s31-korvo.png",
    zh: {
      name: "S31 Korvo",
      tagline: "ESP32-S31 全功能开发板",
      features: [
        "ES8389 音频、800×480 RGB 电容触摸屏与 LVGL 界面",
        "ADC 按键：音量、模式和 PTT；板载 RGB 状态灯",
        "TF 卡、USB-OTG 主机、本地音乐与网络收音机",
        "蓝牙 HFP 耳机 / A2DP、ESP-NOW 脱网对讲、AI 语音",
        "UART1/SCI 与 UART2/GPS 可由 Web/AT 配置，默认关闭并与 DVP 摄像头互斥",
      ],
    },
    en: {
      name: "S31 Korvo",
      tagline: "ESP32-S31 full-featured dev board",
      features: [
        "ES8389 audio, 800×480 RGB capacitive touch display, and LVGL UI",
        "ADC volume/mode/PTT buttons and onboard RGB status LED",
        "TF card, USB-OTG host, local music, and Internet radio",
        "Bluetooth HFP headset / A2DP, ESP-NOW intercom, and AI voice",
        "Web/AT configurable UART1/SCI and UART2/GPS; off by default and mutually exclusive with DVP cameras",
      ],
    },
  },
  {
    id: "s31_function_coreboard",
    chip: "ESP32-S31 · RISC-V",
    flashable: false,
    image: "/boards/s31-function-coreboard.png",
    zh: {
      name: "S31 功能核心板",
      tagline: "ESP32-S31 精简核心板",
      features: [
        "ES8311 音频编解码，适合网络语音与电台桥接",
        "YT8531 千兆以太网，Wi-Fi 可作为回退连接",
        "USB-A 主机、WS2812 RGB 状态灯与 SCI 串口",
        "紧凑无屏、无实体音量/PTT 按键的核心板方案",
        "Wi-Fi 配置门户、远程 AT 与 OTA 升级",
      ],
    },
    en: {
      name: "S31 Function Coreboard",
      tagline: "ESP32-S31 compact core board",
      features: [
        "ES8311 audio codec for network voice and radio bridging",
        "YT8531 Gigabit Ethernet with Wi-Fi fallback",
        "USB-A host, WS2812 RGB status LED, and SCI serial port",
        "Compact screenless core board without volume/PTT buttons",
        "Wi-Fi configuration portal, remote AT, and OTA updates",
      ],
    },
  },
];

// ✓ = supported, △ = supported with a board-specific limitation, — = unavailable.
// Keep this matrix aligned with README.md and board_pins.h so the home page is
// a practical selection guide instead of four isolated marketing cards.
const featureMatrix = [
  {
    zh: "NRL UDP 网络语音桥接（G.711 / Opus）",
    en: "NRL UDP voice bridge (G.711 / Opus)",
    all: "yes",
  },
  {
    zh: "Wi-Fi 配置门户 / SoftAP 配网",
    en: "Wi-Fi configuration portal / SoftAP provisioning",
    all: "yes",
  },
  { zh: "远程 AT 配置与设备 OTA 升级", en: "Remote AT configuration and device OTA", all: "yes" },
  {
    zh: "APRS-IS 网络与无线电 AFSK 收发",
    en: "APRS-IS networking and radio AFSK TX/RX",
    all: "yes",
  },
  { zh: "MDC1200 信令编码与解码", en: "MDC1200 signaling encode/decode", all: "yes" },
  { zh: "DTMF 信令编码与解码", en: "DTMF signaling encode/decode", all: "yes" },
  { zh: "CTCSS/PL 亚音频率识别", en: "CTCSS/PL tone-frequency detection", all: "yes" },
  {
    zh: "屏幕信令与 APRS 设置菜单",
    en: "On-screen signaling and APRS settings",
    gezipai: "yes",
    bh4tdv: "no",
    s31_korvo: "partial",
    s31_function_coreboard: "no",
  },
  {
    zh: "网页 USB 首次全量刷机（Chrome / Edge）",
    en: "Browser USB full flashing (Chrome / Edge)",
    gezipai: "yes",
    bh4tdv: "yes",
    s31_korvo: "no",
    s31_function_coreboard: "no",
  },
  {
    zh: "BLE 蓝牙配网",
    en: "BLE provisioning",
    gezipai: "yes",
    bh4tdv: "yes",
    s31_korvo: "no",
    s31_function_coreboard: "no",
  },
  {
    zh: "ES7210 专用麦克风 ADC",
    en: "Dedicated ES7210 microphone ADC",
    gezipai: "yes",
    bh4tdv: "no",
    s31_korvo: "no",
    s31_function_coreboard: "no",
  },
  {
    zh: "ES8311 音频编解码",
    en: "ES8311 audio codec",
    gezipai: "yes",
    bh4tdv: "yes",
    s31_korvo: "no",
    s31_function_coreboard: "yes",
  },
  {
    zh: "ES8389 音频编解码",
    en: "ES8389 audio codec",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "AEC、降噪、高通、尾音抑制与编解码器增益",
    en: "AEC, noise reduction, high-pass, tail suppression, and codec gain",
    all: "yes",
  },
  {
    zh: "电台 PTT / SQL 控制",
    en: "Radio PTT / squelch control",
    gezipai: "yes",
    bh4tdv: "yes",
    s31_korvo: "partial",
    s31_function_coreboard: "partial",
  },
  {
    zh: "三位频道选择（0–7）",
    en: "Three-bit channel selection (0–7)",
    gezipai: "no",
    bh4tdv: "yes",
    s31_korvo: "no",
    s31_function_coreboard: "no",
  },
  {
    zh: "SCI 串口透明传输",
    en: "SCI serial passthrough",
    gezipai: "yes",
    bh4tdv: "yes",
    s31_korvo: "partial",
    s31_function_coreboard: "yes",
  },
  { zh: "状态灯", en: "Status indicator", all: "yes" },
  {
    zh: "彩色显示屏",
    en: "Color display",
    gezipai: "yes",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "触摸界面",
    en: "Touch interface",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "触控俄罗斯方块小游戏",
    en: "Touch Tetris mini-game",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "本地实体按键 / PTT",
    en: "Physical buttons / PTT",
    gezipai: "yes",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "电池电压检测",
    en: "Battery-voltage sensing",
    gezipai: "yes",
    bh4tdv: "no",
    s31_korvo: "no",
    s31_function_coreboard: "no",
  },
  {
    zh: "TF 卡本地媒体",
    en: "TF-card local media",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
  {
    zh: "USB 主机 / U 盘存储",
    en: "USB host / flash storage",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "SMB 网络共享媒体",
    en: "SMB network-share media",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "千兆以太网",
    en: "Gigabit Ethernet",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "no",
    s31_function_coreboard: "yes",
  },
  {
    zh: "蓝牙耳机 / 外接蓝牙音箱麦克风（HFP / A2DP）",
    en: "Bluetooth headset / external speaker-mic (HFP / A2DP)",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "蓝牙 HFP 双向语音通话",
    en: "Bluetooth HFP two-way voice calls",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "蓝牙耳机按键 PTT",
    en: "Bluetooth headset-button PTT",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  { zh: "ESP-NOW 脱网对讲", en: "ESP-NOW offline intercom", all: "yes" },
  {
    zh: "本地音乐、网络收音机与定时播报",
    en: "Local music, Internet radio, and timed playback",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "小智 AI 语音助手",
    en: "Xiaozhi AI voice assistant",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "yes",
  },
  {
    zh: "NRL 视频通话（DVP 摄像头）",
    en: "NRL video calls (DVP camera)",
    gezipai: "no",
    bh4tdv: "no",
    s31_korvo: "yes",
    s31_function_coreboard: "no",
  },
];

const messages = {
  zh: {
    title: "NRL OTA 固件中心",
    subtitle: "查看各板卡的固件版本与更新日志。管理员登录后可发布固件并查看联网设备状态。",
    language: "语言",
    chinese: "中文",
    english: "English",
    brandName: "NRL OTA",
    navHome: "首页",
    navFirmware: "固件下载",
    navFlash: "网页刷机",
    navDevices: "设备管理",
    navPublish: "发布固件",
    navBoards: "板卡管理",
    adminLogin: "管理登录",
    loginTitle: "管理员登录",
    username: "用户名",
    password: "密码",
    welcome: "{user}",
    boardsHeading: "支持的板卡",
    boardSearch: "搜索板卡名称、型号或主控…",
    boardCount: "共 {count} 个板卡类型",
    chip: "主控",
    features: "特性",
    featureMatrixHeading: "功能支持对照",
    featureMatrixHint: "✓ 支持　△ 部分支持或受板卡外设限制　— 不支持",
    function: "功能",
    historyHeading: "固件历史",
    latest: "最新",
    version: "版本",
    channel: "渠道",
    stable: "稳定版",
    beta: "测试版",
    size: "大小",
    notes: "更新日志",
    noNotes: "（无更新说明）",
    releasedAt: "发布时间",
    download: "下载",
    noReleases: "该板卡暂无已发布固件。",
    flashHeading: "USB 网页刷机",
    flashIntro:
      "用 USB 数据线连接设备，在 Chrome 或 Edge 浏览器里刷写完整固件（含引导程序与分区表）。需要 HTTPS 或本机访问。仅支持 ESP32-S3 板卡。",
    flashReady: "支持 USB 网页刷机",
    flashButton: "USB 刷机",
    flashUnsupported: "当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge。",
    flashNeedsHttps:
      "当前页面使用非安全的 HTTP 地址。Web Serial 要求使用 HTTPS（localhost 除外），请通过配置了 HTTPS 的服务器地址访问。",
    flashNotAllowed: "Web Serial 权限被阻止，请检查浏览器的网站权限或管理员策略。",
    flashSerialOnly:
      "RISC-V 芯片暂不支持网页刷机，请使用串口烧录（scripts/build.py <board> flash）。",
    flashUnavailable: "该板卡暂未上传可刷写的固件包。",
    flashTip: "若无法识别设备，请按住 BOOT 再插入 USB 后重试。",
    navSerial: "串口调试",
    serialHeading: "串口调试终端",
    serialIntro:
      "连接已运行的板卡后，可直接查看调试输出并输入 AT 指令。日志显示区与命令输入行独立，输出不会打断正在编辑的命令。",
    serialBoard: "板卡型号",
    serialConnect: "连接串口",
    serialDisconnect: "断开连接",
    serialReady: "已连接，输入 AT 查看命令列表。",
    serialUnsupported: "当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge。",
    serialConnectFailed: "串口连接失败：{error}",
    serialWriteFailed: "命令发送失败：{error}",
    loadFailed: "加载失败：{error}",
    adminArea: "管理员",
    adminHint: "输入管理员用户名和密码，登录后可查看设备状态并发布固件。",
    login: "登录",
    logout: "退出登录",
    loginFailed: "登录失败：{error}",
    loggedIn: "已登录",
    dashboardTitle: "设备管理",
    statTotal: "设备总数",
    statOnline: "在线设备",
    statBoards: "板卡型号",
    statOutdated: "待升级",
    onlineHint: "5 分钟内有上报即视为在线",
    status: "状态",
    statusOnline: "在线",
    statusOffline: "离线",
    upToDate: "最新",
    updateAvailable: "可升级",
    deviceStatusHeading: "设备状态",
    searchPlaceholder: "搜索 设备ID / 板卡 / 呼号 / IP…",
    filterAll: "全部",
    noMatch: "没有匹配的设备。",
    pageInfo: "第 {from}–{to} 条，共 {total} 条",
    prevPage: "上一页",
    nextPage: "下一页",
    pageOf: "{page} / {count}",
    refresh: "刷新",
    noDevices: "尚无设备上报。",
    deviceId: "设备 ID",
    board: "板卡",
    firmware: "固件",
    callsign: "NRL 呼号",
    ssid: "SSID",
    ipAddress: "IP 地址",
    lastSeen: "最后在线",
    publishHeading: "发布固件",
    boardType: "板卡类型",
    firmwareVersion: "版本",
    firmwareFile: "固件文件",
    releaseNotes: "更新日志",
    publish: "发布固件",
    publishing: "正在上传固件…",
    published: "已发布 {version}（{size} 字节）",
    uploadFailed: "发布失败：{error}",
    boardManagerTitle: "板卡类型与功能管理",
    boardManagerHint:
      "创建板卡、上传图片、编辑中英文介绍，并配置功能对照。草稿不会出现在公开页面。",
    newBoard: "新建板卡",
    boardId: "板卡 ID（保存后不可修改）",
    nameZH: "中文名称",
    nameEN: "英文名称",
    taglineZH: "中文短介绍",
    taglineEN: "英文短介绍",
    descriptionZH: "中文详细说明",
    descriptionEN: "英文详细说明",
    chipLabel: "主控显示名称",
    webFlashFamily: "网页刷机芯片类型（不支持则留空）",
    displayOrder: "显示顺序",
    boardStatus: "发布状态",
    draft: "草稿",
    publishedStatus: "已发布",
    archived: "已归档",
    highlightsZH: "中文板卡要点（每行一项）",
    highlightsEN: "英文板卡要点（每行一项）",
    saveBoard: "保存板卡资料",
    boardSaved: "板卡资料已保存",
    boardImage: "板卡图片（JPEG / PNG / WebP，最大 5 MB）",
    uploadImage: "上传图片",
    imageUploaded: "图片已上传",
    featureAssignments: "板卡功能对照",
    saveFeatures: "保存功能配置",
    featuresSaved: "功能配置已保存",
    featureYes: "支持",
    featurePartial: "部分支持",
    featureNo: "不支持",
    addFeature: "添加功能项",
    featureKey: "功能键（小写英文）",
    featureLabelZH: "功能中文名称",
    featureLabelEN: "功能英文名称",
    featureDescriptionZH: "功能中文说明",
    featureDescriptionEN: "功能英文说明",
    editFeature: "编辑功能项",
    newFeature: "新功能项",
    partialNoteZH: "中文限制说明",
    partialNoteEN: "英文限制说明",
    featureAdded: "功能项已添加",
    aiImport: "AI / JSON 导入",
    aiImportHint:
      "粘贴符合管理 API 格式的 JSON，一次提交板卡、功能定义和功能状态。图片需保存后单独上传。",
    importCatalog: "导入并保存",
    catalogImported: "目录资料已导入",
    manageFailed: "保存失败：{error}",
    unknownError: "请求失败",
  },
  en: {
    title: "NRL OTA Firmware Center",
    subtitle:
      "Browse firmware versions and changelogs for every board. Administrators can publish firmware and view connected-device status after logging in.",
    language: "Language",
    chinese: "中文",
    english: "English",
    brandName: "NRL OTA",
    navHome: "Home",
    navFirmware: "Firmware",
    navFlash: "USB Flash",
    navDevices: "Devices",
    navPublish: "Publish",
    navBoards: "Boards",
    adminLogin: "Admin login",
    loginTitle: "Administrator login",
    username: "Username",
    password: "Password",
    welcome: "{user}",
    boardsHeading: "Supported boards",
    boardSearch: "Search board name, ID, or SoC…",
    boardCount: "{count} board types",
    chip: "SoC",
    features: "Features",
    featureMatrixHeading: "Feature comparison",
    featureMatrixHint: "✓ Supported　△ Limited by board hardware　— Unavailable",
    function: "Function",
    historyHeading: "Firmware history",
    latest: "Latest",
    version: "Version",
    channel: "Channel",
    stable: "Stable",
    beta: "Beta",
    size: "Size",
    notes: "Changelog",
    noNotes: "(no release notes)",
    releasedAt: "Released",
    download: "Download",
    noReleases: "No firmware has been published for this board yet.",
    flashHeading: "USB web flashing",
    flashIntro:
      "Connect the device over USB and flash the full firmware (bootloader and partition table included) from Chrome or Edge. Requires HTTPS or localhost. ESP32-S3 boards only.",
    flashReady: "Web-flashable over USB",
    flashButton: "Flash via USB",
    flashUnsupported: "This browser does not support Web Serial. Use Chrome or Edge.",
    flashNeedsHttps:
      "This page is using an insecure HTTP address. Web Serial requires HTTPS (except on localhost). Open the site through its HTTPS address.",
    flashNotAllowed:
      "Web Serial permission was blocked. Check the site's browser permissions or administrator policy.",
    flashSerialOnly:
      "RISC-V chip — not web-flashable. Use serial flashing (scripts/build.py <board> flash).",
    flashUnavailable: "No flashable firmware package has been staged for this board yet.",
    flashTip: "If the device is not detected, hold BOOT while plugging in USB, then retry.",
    navSerial: "Serial debug",
    serialHeading: "Serial debug terminal",
    serialIntro:
      "Connect a running board to view debug output and send AT commands. Output and command entry are separate, so incoming logs never interrupt typing.",
    serialBoard: "Board model",
    serialConnect: "Connect serial",
    serialDisconnect: "Disconnect",
    serialReady: "Connected. Type AT to list commands.",
    serialUnsupported: "This browser does not support Web Serial. Use Chrome or Edge.",
    serialConnectFailed: "Serial connection failed: {error}",
    serialWriteFailed: "Command send failed: {error}",
    loadFailed: "Load failed: {error}",
    adminArea: "Administrator",
    adminHint:
      "Sign in with the administrator username and password to view device status and publish firmware.",
    login: "Log in",
    logout: "Log out",
    loginFailed: "Login failed: {error}",
    loggedIn: "Logged in",
    dashboardTitle: "Device management",
    statTotal: "Total devices",
    statOnline: "Online",
    statBoards: "Board models",
    statOutdated: "Need update",
    onlineHint: "Reported within the last 5 minutes counts as online",
    status: "Status",
    statusOnline: "Online",
    statusOffline: "Offline",
    upToDate: "Up to date",
    updateAvailable: "Update available",
    deviceStatusHeading: "Device status",
    searchPlaceholder: "Search device ID / board / callsign / IP…",
    filterAll: "All",
    noMatch: "No devices match the filter.",
    pageInfo: "{from}–{to} of {total}",
    prevPage: "Prev",
    nextPage: "Next",
    pageOf: "{page} / {count}",
    refresh: "Refresh",
    noDevices: "No devices have reported yet.",
    deviceId: "Device ID",
    board: "Board",
    firmware: "Firmware",
    callsign: "NRL callsign",
    ssid: "SSID",
    ipAddress: "IP address",
    lastSeen: "Last seen",
    publishHeading: "Publish firmware",
    boardType: "Board type",
    firmwareVersion: "Version",
    firmwareFile: "Firmware file",
    releaseNotes: "Release notes",
    publish: "Publish firmware",
    publishing: "Uploading firmware…",
    published: "Published {version} ({size} bytes)",
    uploadFailed: "Publish failed: {error}",
    boardManagerTitle: "Board types and features",
    boardManagerHint:
      "Create boards, upload images, edit bilingual descriptions, and configure the comparison matrix. Drafts stay private.",
    newBoard: "New board",
    boardId: "Board ID (immutable after save)",
    nameZH: "Chinese name",
    nameEN: "English name",
    taglineZH: "Chinese tagline",
    taglineEN: "English tagline",
    descriptionZH: "Chinese description",
    descriptionEN: "English description",
    chipLabel: "SoC display label",
    webFlashFamily: "Web-flash chip family (blank if unsupported)",
    displayOrder: "Display order",
    boardStatus: "Status",
    draft: "Draft",
    publishedStatus: "Published",
    archived: "Archived",
    highlightsZH: "Chinese highlights (one per line)",
    highlightsEN: "English highlights (one per line)",
    saveBoard: "Save board",
    boardSaved: "Board saved",
    boardImage: "Board image (JPEG / PNG / WebP, max 5 MB)",
    uploadImage: "Upload image",
    imageUploaded: "Image uploaded",
    featureAssignments: "Board feature comparison",
    saveFeatures: "Save features",
    featuresSaved: "Feature assignments saved",
    featureYes: "Supported",
    featurePartial: "Partial",
    featureNo: "Unavailable",
    addFeature: "Add feature",
    featureKey: "Feature key (lowercase)",
    featureLabelZH: "Chinese feature label",
    featureLabelEN: "English feature label",
    featureDescriptionZH: "Chinese feature description",
    featureDescriptionEN: "English feature description",
    editFeature: "Edit feature",
    newFeature: "New feature",
    partialNoteZH: "Chinese limitation note",
    partialNoteEN: "English limitation note",
    featureAdded: "Feature added",
    aiImport: "AI / JSON import",
    aiImportHint:
      "Paste management API JSON to submit a board, feature definitions, and assignments together. Upload the image separately after saving.",
    importCatalog: "Import and save",
    catalogImported: "Catalog imported",
    manageFailed: "Save failed: {error}",
    unknownError: "Request failed",
  },
};

function savedLanguage() {
  const saved = localStorage.otaLanguage;
  if (saved === "zh" || saved === "en") return saved;
  return navigator.language?.toLowerCase().startsWith("zh") ? "zh" : "en";
}

const app = createApp({
  setup() {
    const language = ref(savedLanguage());
    const view = ref("home");
    const session = ref(localStorage.otaSession || "");
    const sessionUser = ref(localStorage.otaUser || "");
    const authed = ref(false);
    const username = ref("");
    const password = ref("");
    const history = ref({}); // board id -> release[]
    const catalogBoards = ref(boards);
    const catalogFeatures = ref([]);
    const devices = ref([]);
    const loadError = ref("");
    const boardSearch = ref("");
    const loginError = ref("");
    const secureContext = window.isSecureContext;
    const webSerialAvailable = "serial" in navigator;

    // Serial debug terminal. Keep the log view and command entry as separate
    // DOM elements: ESP_LOG output can arrive at any time without moving or
    // corrupting what the user is currently typing.
    const serialBoard = ref("s31_korvo");
    const serialPort = ref(null);
    const serialConnected = ref(false);
    const serialOutput = ref("");
    const serialInput = ref("");
    const serialMessage = ref("");
    const serialOutputEl = ref(null);
    const serialPrompt = computed(() => `${boardName(serialBoard.value)}# `);
    let activeSerialReader = null;
    let serialReadTask = null;

    function appendSerialOutput(text) {
      serialOutput.value = (serialOutput.value + text).slice(-200000);
      setTimeout(() => {
        const output = serialOutputEl.value;
        if (output) output.scrollTop = output.scrollHeight;
      });
    }

    async function readSerial(port) {
      const reader = port.readable.getReader();
      const decoder = new TextDecoder();
      activeSerialReader = reader;
      try {
        while (serialConnected.value && serialPort.value === port) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) appendSerialOutput(decoder.decode(value, { stream: true }));
        }
        const tail = decoder.decode();
        if (tail) appendSerialOutput(tail);
      } catch (error) {
        if (serialConnected.value) {
          appendSerialOutput(`\r\n[serial read error: ${error.message}]\r\n`);
        }
      } finally {
        try {
          reader.releaseLock();
        } catch {
          // The port may already have been closed by the browser.
        }
        if (activeSerialReader === reader) activeSerialReader = null;
        if (serialPort.value === port) serialConnected.value = false;
      }
    }

    async function connectSerial() {
      if (!webSerialAvailable) {
        serialMessage.value = t("serialUnsupported");
        return;
      }
      serialMessage.value = "";
      try {
        const port = await navigator.serial.requestPort();
        await port.open({ baudRate: 115200, bufferSize: 8192 });
        serialPort.value = port;
        serialConnected.value = true;
        appendSerialOutput(`[${serialPrompt.value}${t("serialReady")}]\r\n`);
        serialReadTask = readSerial(port);
      } catch (error) {
        serialMessage.value = t("serialConnectFailed", { error: error.message });
      }
    }

    async function disconnectSerial() {
      const port = serialPort.value;
      serialConnected.value = false;
      try {
        await activeSerialReader?.cancel();
      } catch {
        // A disconnected device may reject cancellation.
      }
      try {
        await serialReadTask;
      } catch {
        // readSerial reports its own error in the terminal.
      }
      try {
        await port?.close();
      } catch {
        // The browser may already have closed the port.
      }
      if (serialPort.value === port) serialPort.value = null;
      serialReadTask = null;
    }

    async function sendSerialCommand() {
      const command = serialInput.value;
      if (!serialConnected.value || !serialPort.value || command.length === 0) return;
      serialInput.value = "";
      appendSerialOutput(`${serialPrompt.value}${command}\r\n`);
      const writer = serialPort.value.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(`${command}\r\n`));
      } catch (error) {
        serialMessage.value = t("serialWriteFailed", { error: error.message });
      } finally {
        writer.releaseLock();
      }
    }

    function openSerialDebug(boardId) {
      serialBoard.value = boardId;
      setView("serial");
    }

    // publish form
    const board = ref("s31_korvo");
    const version = ref("");
    const channel = ref("stable");
    const notes = ref("");
    const firmware = ref();
    const publishMessage = ref("");

    // Board catalog manager. The same API is intentionally usable by an AI
    // client with an administrator token; the page provides a JSON import box
    // for reviewing and submitting that structured result.
    const emptyBoard = () => ({
      id: "",
      name_zh: "",
      name_en: "",
      tagline_zh: "",
      tagline_en: "",
      description_zh: "",
      description_en: "",
      chip_label: "",
      web_flash_chip_family: "",
      display_order: 100,
      status: "draft",
      highlights_zh_text: "",
      highlights_en_text: "",
      features: {},
      feature_notes: {},
    });
    const boardEditor = ref(emptyBoard());
    const boardIsNew = ref(true);
    const boardImage = ref();
    const boardMessage = ref("");
    const featureDraft = ref({
      key: "",
      label_zh: "",
      label_en: "",
      description_zh: "",
      description_en: "",
      group: "general",
      display_order: 100,
      active: true,
    });
    const featureIsNew = ref(true);
    const aiImportJSON = ref("");

    const t = (key, values = {}) => {
      let text = messages[language.value][key] || messages.en[key] || key;
      for (const [name, value] of Object.entries(values))
        text = text.replace(`{${name}}`, String(value));
      return text;
    };
    const setLanguage = (value) => {
      language.value = value;
    };
    const boardName = (id) => {
      const entry = catalogBoards.value.find((b) => b.id === id);
      if (!entry) return id;
      return entry.name_zh
        ? language.value === "zh"
          ? entry.name_zh
          : entry.name_en
        : entry[language.value].name;
    };
    const requestError = async (response) => {
      const body = await response.json().catch(() => ({}));
      return body.error || t("unknownError");
    };

    async function loadCatalog(admin = false) {
      const path = admin ? "/api/v1/admin/catalog" : "/api/v1/catalog";
      const response = await fetch(
        apiURL(path),
        admin ? { headers: { "X-OTA-Token": session.value } } : undefined,
      );
      if (!response.ok) throw new Error(await requestError(response));
      const body = await response.json();
      catalogBoards.value = body.boards || [];
      catalogFeatures.value = body.features || [];
      if (!catalogBoards.value.some((b) => b.id === board.value))
        board.value = catalogBoards.value[0]?.id || "";
      if (!catalogBoards.value.some((b) => b.id === serialBoard.value))
        serialBoard.value = catalogBoards.value[0]?.id || "";
    }

    async function loadHistory() {
      loadError.value = "";
      const next = {};
      try {
        await Promise.all(
          catalogBoards.value
            .filter((b) => (b.status || "published") === "published")
            .map(async (b) => {
              const response = await fetch(
                apiURL(`/api/v1/releases?board=${encodeURIComponent(b.id)}`),
              );
              if (!response.ok) throw new Error(await requestError(response));
              next[b.id] = (await response.json()).releases || [];
            }),
        );
        history.value = next;
      } catch (error) {
        loadError.value = t("loadFailed", { error: error.message });
      }
    }

    async function loadDevices() {
      const response = await fetch(apiURL("/api/v1/admin/devices"), {
        headers: { "X-OTA-Token": session.value },
      });
      if (!response.ok) throw new Error(await requestError(response));
      devices.value = (await response.json()).devices || [];
    }

    async function login() {
      loginError.value = "";
      try {
        const response = await fetch(apiURL("/api/v1/admin/login"), {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ username: username.value, password: password.value }),
        });
        if (!response.ok) throw new Error(await requestError(response));
        const body = await response.json();
        session.value = body.token;
        sessionUser.value = body.username;
        localStorage.otaSession = body.token;
        localStorage.otaUser = body.username;
        authed.value = true;
        password.value = "";
        await Promise.all([loadDevices(), loadCatalog(true)]);
        setView("devices");
      } catch (error) {
        authed.value = false;
        loginError.value = t("loginFailed", { error: error.message });
      }
    }
    function logout() {
      authed.value = false;
      devices.value = [];
      session.value = "";
      sessionUser.value = "";
      localStorage.removeItem("otaSession");
      localStorage.removeItem("otaUser");
      loadCatalog(false)
        .then(loadHistory)
        .catch(() => {});
      setView("home");
    }
    async function refreshDevices() {
      try {
        await Promise.all([loadDevices(), loadCatalog(true)]);
      } catch (error) {
        loginError.value = t("loginFailed", { error: error.message });
        authed.value = false;
        setView("login");
      }
    }

    // Restore an existing session on load by validating it against the API.
    async function restoreSession() {
      if (!session.value) return;
      try {
        await loadDevices();
        authed.value = true;
      } catch {
        logout();
      }
    }

    async function upload() {
      const file = firmware.value?.files?.[0];
      if (!file) return;
      publishMessage.value = t("publishing");
      const response = await fetch(apiURL("/api/v1/admin/releases"), {
        method: "POST",
        headers: {
          "X-OTA-Token": session.value,
          "X-Firmware-Board": board.value,
          "X-Firmware-Version": version.value,
          "X-Firmware-Channel": channel.value,
          "X-Release-Notes": encodeURIComponent(notes.value),
        },
        body: file,
      });
      if (!response.ok) {
        publishMessage.value = t("uploadFailed", { error: await requestError(response) });
        return;
      }
      const released = await response.json();
      publishMessage.value = t("published", { version: released.version, size: released.size });
      await Promise.all([loadHistory(), refreshDevices()]);
    }

    function editBoard(entry) {
      boardIsNew.value = false;
      boardEditor.value = {
        ...entry,
        highlights_zh_text: (entry.highlights_zh || []).join("\n"),
        highlights_en_text: (entry.highlights_en || []).join("\n"),
        features: { ...entry.features },
        feature_notes: structuredClone(entry.feature_notes || {}),
      };
      for (const f of catalogFeatures.value) {
        boardEditor.value.feature_notes[f.key] ||= { zh: "", en: "" };
      }
      boardMessage.value = "";
    }

    function newBoard() {
      boardIsNew.value = true;
      boardEditor.value = emptyBoard();
      for (const f of catalogFeatures.value) {
        boardEditor.value.features[f.key] = "no";
        boardEditor.value.feature_notes[f.key] = { zh: "", en: "" };
      }
      boardMessage.value = "";
    }

    function boardPayload() {
      const b = boardEditor.value;
      return {
        id: b.id.trim(),
        name_zh: b.name_zh.trim(),
        name_en: b.name_en.trim(),
        tagline_zh: b.tagline_zh.trim(),
        tagline_en: b.tagline_en.trim(),
        description_zh: b.description_zh.trim(),
        description_en: b.description_en.trim(),
        chip_label: b.chip_label.trim(),
        web_flash_chip_family: b.web_flash_chip_family.trim(),
        display_order: Number(b.display_order) || 0,
        status: b.status,
        highlights_zh: b.highlights_zh_text
          .split("\n")
          .map((x) => x.trim())
          .filter(Boolean),
        highlights_en: b.highlights_en_text
          .split("\n")
          .map((x) => x.trim())
          .filter(Boolean),
      };
    }

    async function saveBoard() {
      boardMessage.value = "";
      try {
        const payload = boardPayload();
        const response = await fetch(
          apiURL(
            boardIsNew.value
              ? "/api/v1/admin/boards"
              : `/api/v1/admin/boards/${encodeURIComponent(payload.id)}`,
          ),
          {
            method: boardIsNew.value ? "POST" : "PUT",
            headers: { "Content-Type": "application/json", "X-OTA-Token": session.value },
            body: JSON.stringify(payload),
          },
        );
        if (!response.ok) throw new Error(await requestError(response));
        await loadCatalog(true);
        const saved = catalogBoards.value.find((b) => b.id === payload.id);
        if (saved) editBoard(saved);
        boardMessage.value = t("boardSaved");
      } catch (error) {
        boardMessage.value = t("manageFailed", { error: error.message });
      }
    }

    async function uploadBoardImage() {
      const file = boardImage.value?.files?.[0];
      if (!file || boardIsNew.value) return;
      try {
        const response = await fetch(
          apiURL(`/api/v1/admin/boards/${encodeURIComponent(boardEditor.value.id)}/image`),
          {
            method: "POST",
            headers: {
              "Content-Type": file.type || "application/octet-stream",
              "X-OTA-Token": session.value,
            },
            body: file,
          },
        );
        if (!response.ok) throw new Error(await requestError(response));
        await loadCatalog(true);
        const saved = catalogBoards.value.find((b) => b.id === boardEditor.value.id);
        if (saved) editBoard(saved);
        boardMessage.value = t("imageUploaded");
      } catch (error) {
        boardMessage.value = t("manageFailed", { error: error.message });
      }
    }

    async function saveBoardFeatures() {
      if (boardIsNew.value) return;
      try {
        const assignments = {};
        for (const f of catalogFeatures.value) {
          const note = boardEditor.value.feature_notes[f.key] || {};
          assignments[f.key] = {
            state: boardEditor.value.features[f.key] || "no",
            note_zh: note.zh || "",
            note_en: note.en || "",
          };
        }
        const response = await fetch(
          apiURL(`/api/v1/admin/boards/${encodeURIComponent(boardEditor.value.id)}/features`),
          {
            method: "PUT",
            headers: { "Content-Type": "application/json", "X-OTA-Token": session.value },
            body: JSON.stringify({ features: assignments }),
          },
        );
        if (!response.ok) throw new Error(await requestError(response));
        await loadCatalog(true);
        const saved = catalogBoards.value.find((b) => b.id === boardEditor.value.id);
        if (saved) editBoard(saved);
        boardMessage.value = t("featuresSaved");
      } catch (error) {
        boardMessage.value = t("manageFailed", { error: error.message });
      }
    }

    async function addFeature() {
      try {
        const key = featureDraft.value.key;
        const response = await fetch(
          apiURL(
            featureIsNew.value
              ? "/api/v1/admin/features"
              : `/api/v1/admin/features/${encodeURIComponent(key)}`,
          ),
          {
            method: featureIsNew.value ? "POST" : "PUT",
            headers: { "Content-Type": "application/json", "X-OTA-Token": session.value },
            body: JSON.stringify(featureDraft.value),
          },
        );
        if (!response.ok) throw new Error(await requestError(response));
        await loadCatalog(true);
        boardEditor.value.features[key] ||= "no";
        boardEditor.value.feature_notes[key] ||= { zh: "", en: "" };
        newFeatureDraft();
        boardMessage.value = t("featureAdded");
      } catch (error) {
        boardMessage.value = t("manageFailed", { error: error.message });
      }
    }

    function newFeatureDraft() {
      featureIsNew.value = true;
      featureDraft.value = {
        key: "",
        label_zh: "",
        label_en: "",
        description_zh: "",
        description_en: "",
        group: "general",
        display_order: 100,
        active: true,
      };
    }

    function editFeature(feature) {
      featureIsNew.value = false;
      featureDraft.value = { ...feature };
    }

    async function importCatalog() {
      try {
        const payload = JSON.parse(aiImportJSON.value);
        const response = await fetch(apiURL("/api/v1/admin/catalog/import"), {
          method: "POST",
          headers: { "Content-Type": "application/json", "X-OTA-Token": session.value },
          body: JSON.stringify(payload),
        });
        if (!response.ok) throw new Error(await requestError(response));
        const result = await response.json();
        await loadCatalog(true);
        const saved = catalogBoards.value.find((b) => b.id === result.id);
        if (saved) editBoard(saved);
        boardMessage.value = t("catalogImported");
      } catch (error) {
        boardMessage.value = t("manageFailed", { error: error.message });
      }
    }

    const formatTime = (timestamp) =>
      new Date(timestamp * 1000).toLocaleString(language.value === "zh" ? "zh-CN" : "en-US");
    const formatSize = (bytes) => {
      if (bytes < 1024) return `${bytes} B`;
      if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
      return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    };
    const localizedBoards = computed(() =>
      catalogBoards.value.map((b) => {
        if (!b.name_zh)
          return {
            id: b.id,
            chip: b.chip,
            flashable: b.flashable,
            image: b.image,
            ...b[language.value],
          };
        return {
          id: b.id,
          chip: b.chip_label,
          flashable: Boolean(b.web_flash_chip_family),
          image: b.image_url,
          name: language.value === "zh" ? b.name_zh : b.name_en,
          tagline: language.value === "zh" ? b.tagline_zh : b.tagline_en,
          description: language.value === "zh" ? b.description_zh : b.description_en,
          features: language.value === "zh" ? b.highlights_zh : b.highlights_en,
          status: b.status,
        };
      }),
    );
    const localizedFeatureMatrix = computed(() => {
      if (!catalogFeatures.value.length)
        return featureMatrix.map((row) => ({ ...row, label: row[language.value] }));
      return catalogFeatures.value
        .filter((f) => f.active)
        .map((f) => {
          const row = { key: f.key, label: language.value === "zh" ? f.label_zh : f.label_en };
          for (const b of catalogBoards.value) {
            row[b.id] = b.features?.[f.key] || "no";
            row[`${b.id}_note`] = b.feature_notes?.[f.key]?.[language.value] || "";
          }
          return row;
        });
    });
    const publicBoards = computed(() =>
      localizedBoards.value.filter((b) => !b.status || b.status === "published"),
    );
    const uploadBoards = computed(() =>
      localizedBoards.value.filter((b) => b.status !== "archived"),
    );
    const visibleBoards = computed(() => {
      const q = boardSearch.value.trim().toLowerCase();
      if (!q) return publicBoards.value;
      return publicBoards.value.filter((b) =>
        [b.id, b.name, b.tagline, b.chip].filter(Boolean).join(" ").toLowerCase().includes(q),
      );
    });
    const featureMark = (state) => ({ yes: "✓", partial: "△", no: "—" })[state] || "—";

    // A board is only web-flashable if its full-flash manifest has been staged
    // into the server's data-dir. Probe so boards without a staged package show
    // a clear message instead of a button that fails when clicked.
    const flasherReady = ref({});
    async function loadFlasher() {
      const next = {};
      await Promise.all(
        catalogBoards.value
          .filter((b) => b.web_flash_chip_family || b.flashable)
          .map(async (b) => {
            try {
              const response = await fetch(apiURL(`/flasher/manifest-${b.id}.json`), {
                cache: "no-store",
              });
              next[b.id] = response.ok;
            } catch {
              next[b.id] = false;
            }
          }),
      );
      flasherReady.value = next;
    }

    // Views: home | firmware | flash | serial | login | devices | publish. The devices
    // and publish views are gated behind a valid session; navigating to them
    // while logged out sends the user to login. Keep the URL hash in sync for
    // refresh/back support.
    const views = ["home", "firmware", "flash", "serial", "login", "devices", "publish", "boards"];
    const adminViews = ["devices", "publish", "boards"];
    function setView(next) {
      if (adminViews.includes(next) && !authed.value) next = "login";
      if (next === "login" && authed.value) next = "devices";
      if (!views.includes(next)) next = "home";
      view.value = next;
      if (location.hash.slice(1) !== next) location.hash = next;
    }
    function syncFromHash() {
      setView(location.hash.slice(1) || "home");
    }
    window.addEventListener("hashchange", syncFromHash);

    // Device-management dashboard summary.
    const nowSeconds = () => Math.floor(Date.now() / 1000);
    const isOnline = (device) => nowSeconds() - device.last_seen < 300; // 5 min
    const latestByBoard = computed(() => {
      const map = {};
      for (const b of catalogBoards.value) {
        const list = history.value[b.id] || [];
        if (list.length) map[b.id] = list[0].version;
      }
      return map;
    });
    const deviceRows = computed(() =>
      devices.value.map((d) => {
        const latest = latestByBoard.value[d.board_type];
        return {
          ...d,
          online: isOnline(d),
          latest,
          outdated: latest ? latest !== d.firmware_version : false,
        };
      }),
    );
    const stats = computed(() => {
      const rows = deviceRows.value;
      return {
        total: rows.length,
        online: rows.filter((d) => d.online).length,
        boards: new Set(rows.map((d) => d.board_type)).size,
        outdated: rows.filter((d) => d.outdated).length,
      };
    });

    // Search / filter / pagination for large fleets. Filtering is client-side;
    // the stat cards double as quick filters. (For very large deployments the
    // /admin/devices endpoint would move to server-side paging.)
    const search = ref("");
    const statusFilter = ref("all"); // all | online | offline | outdated
    const page = ref(1);
    const pageSize = 20;
    const setFilter = (value) => {
      statusFilter.value = statusFilter.value === value ? "all" : value;
    };
    const filteredRows = computed(() => {
      const q = search.value.trim().toLowerCase();
      return deviceRows.value.filter((d) => {
        if (statusFilter.value === "online" && !d.online) return false;
        if (statusFilter.value === "offline" && d.online) return false;
        if (statusFilter.value === "outdated" && !d.outdated) return false;
        if (!q) return true;
        const hay = [
          d.device_id,
          d.board_type,
          boardName(d.board_type),
          d.firmware_version,
          d.ip_address,
          d.metadata?.nrl_callsign,
          d.metadata?.nrl_ssid,
        ]
          .filter(Boolean)
          .join(" ")
          .toLowerCase();
        return hay.includes(q);
      });
    });
    const pageCount = computed(() => Math.max(1, Math.ceil(filteredRows.value.length / pageSize)));
    const pagedRows = computed(() => {
      const start = (page.value - 1) * pageSize;
      return filteredRows.value.slice(start, start + pageSize);
    });
    const pageFrom = computed(() =>
      filteredRows.value.length ? (page.value - 1) * pageSize + 1 : 0,
    );
    const pageTo = computed(() => Math.min(page.value * pageSize, filteredRows.value.length));
    const goPage = (delta) => {
      page.value = Math.min(pageCount.value, Math.max(1, page.value + delta));
    };
    watch([search, statusFilter], () => {
      page.value = 1;
    });

    watch(
      language,
      (value) => {
        localStorage.otaLanguage = value;
        document.documentElement.lang = value === "zh" ? "zh-CN" : "en";
        document.title = t("title");
      },
      { immediate: true },
    );

    async function bootstrap() {
      try {
        await loadCatalog(false);
      } catch (error) {
        loadError.value = t("loadFailed", { error: error.message });
      }
      await Promise.all([loadHistory(), loadFlasher()]);
      await restoreSession();
      if (authed.value) await Promise.all([loadHistory(), loadFlasher()]);
      syncFromHash();
    }
    void bootstrap();

    return {
      language,
      view,
      setView,
      authed,
      sessionUser,
      username,
      password,
      history,
      devices,
      deviceRows,
      stats,
      search,
      statusFilter,
      setFilter,
      pagedRows,
      filteredRows,
      page,
      pageCount,
      pageFrom,
      pageTo,
      goPage,
      loadError,
      boardSearch,
      visibleBoards,
      publicBoards,
      uploadBoards,
      loginError,
      flasherReady,
      secureContext,
      webSerialAvailable,
      serialBoard,
      serialConnected,
      serialOutput,
      serialInput,
      serialMessage,
      serialOutputEl,
      serialPrompt,
      connectSerial,
      disconnectSerial,
      sendSerialCommand,
      openSerialDebug,
      board,
      version,
      channel,
      notes,
      firmware,
      publishMessage,
      catalogBoards,
      catalogFeatures,
      boardEditor,
      boardIsNew,
      boardImage,
      boardMessage,
      featureDraft,
      featureIsNew,
      aiImportJSON,
      apiURL,
      t,
      setLanguage,
      boardName,
      localizedBoards,
      localizedFeatureMatrix,
      featureMark,
      loadHistory,
      loadCatalog,
      login,
      logout,
      refreshDevices,
      upload,
      editBoard,
      newBoard,
      saveBoard,
      uploadBoardImage,
      saveBoardFeatures,
      addFeature,
      newFeatureDraft,
      editFeature,
      importCatalog,
      formatTime,
      formatSize,
    };
  },
  template: `
    <div class="app">
      <header class="navbar">
        <div class="nav-brand" @click="setView('home')">
          <span class="nav-logo">◉</span><span class="nav-title">{{ t('brandName') }}</span>
        </div>
        <nav class="nav-menu">
          <button :class="{ active: view === 'home' }" @click="setView('home')">{{ t('navHome') }}</button>
          <button :class="{ active: view === 'firmware' }" @click="setView('firmware')">{{ t('navFirmware') }}</button>
          <button :class="{ active: view === 'flash' }" @click="setView('flash')">{{ t('navFlash') }}</button>
          <button :class="{ active: view === 'serial' }" @click="setView('serial')">{{ t('navSerial') }}</button>
          <button v-if="authed" :class="{ active: view === 'devices' }" @click="setView('devices')">{{ t('navDevices') }}</button>
					<button v-if="authed" :class="{ active: view === 'boards' }" @click="setView('boards')">{{ t('navBoards') }}</button>
          <button v-if="authed" :class="{ active: view === 'publish' }" @click="setView('publish')">{{ t('navPublish') }}</button>
        </nav>
        <div class="nav-right">
          <div class="language" :aria-label="t('language')">
            <button :class="{ active: language === 'zh' }" @click="setLanguage('zh')">中</button>
            <button :class="{ active: language === 'en' }" @click="setLanguage('en')">EN</button>
          </div>
          <template v-if="authed">
            <span class="user-chip"><span class="user-avatar">{{ sessionUser.slice(0, 1).toUpperCase() }}</span>{{ sessionUser }}</span>
            <button class="ghost" @click="logout">{{ t('logout') }}</button>
          </template>
          <button v-else class="primary" @click="setView('login')">{{ t('adminLogin') }}</button>
        </div>
      </header>

      <main class="content">
        <!-- Home: board introductions -->
        <section v-if="view === 'home'" class="view">
          <div class="view-head">
            <h1>{{ t('title') }}</h1>
            <p class="subtitle">{{ t('subtitle') }}</p>
          </div>
						<div class="catalog-home-toolbar">
							<div><h2 class="section-h">{{ t('boardsHeading') }}</h2><span class="muted-sm">{{ t('boardCount', { count: visibleBoards.length }) }}</span></div>
							<input v-model="boardSearch" class="search" type="search" :placeholder="t('boardSearch')">
						</div>
          <div class="board-grid board-intro-grid">
							<article v-for="b in visibleBoards" :key="b.id" class="board-card">
              <img class="board-image" :src="b.image" :alt="b.name" />
              <div class="board-card-head">
                <h3>{{ b.name }}</h3>
                <span class="chip">{{ b.chip }}</span>
              </div>
              <p class="tagline">{{ b.tagline }}</p>
							<p v-if="b.description" class="board-description">{{ b.description }}</p>
              <ul class="features">
                <li v-for="feature in b.features" :key="feature">{{ feature }}</li>
              </ul>
              <code class="board-id">{{ b.id }}</code>
            </article>
          </div>
          <section class="panel feature-matrix-panel">
            <div class="feature-matrix-head">
              <div>
                <h2 class="section-h">{{ t('featureMatrixHeading') }}</h2>
                <p class="hint">{{ t('featureMatrixHint') }}</p>
              </div>
            </div>
            <div class="table-scroll">
              <table class="feature-matrix">
                <thead>
									<tr><th>{{ t('function') }}</th><th v-for="b in visibleBoards" :key="b.id">{{ b.name }}</th></tr>
                </thead>
                <tbody>
                  <tr v-for="row in localizedFeatureMatrix" :key="row.label">
                    <th>{{ row.label }}</th>
									<td v-for="b in visibleBoards" :key="b.id">
										<span :title="row[b.id + '_note'] || ''" :class="['feature-mark', 'feature-' + (row[b.id] || row.all)]">{{ featureMark(row[b.id] || row.all) }}</span>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </section>

        <!-- Firmware download / history -->
        <section v-else-if="view === 'firmware'" class="view">
          <div class="view-head row">
            <h1>{{ t('navFirmware') }}</h1>
            <button class="ghost" @click="loadHistory">{{ t('refresh') }}</button>
          </div>
          <p v-if="loadError" class="error">{{ loadError }}</p>
						<div v-for="b in publicBoards" :key="b.id" class="panel board-history">
            <div class="board-history-head"><h3>{{ b.name }}</h3><code class="board-id">{{ b.id }}</code></div>
            <div class="table-scroll" v-if="(history[b.id] || []).length">
              <table>
                <thead>
                  <tr><th>{{ t('version') }}</th><th>{{ t('channel') }}</th><th>{{ t('size') }}</th><th class="notes-col">{{ t('notes') }}</th><th>{{ t('releasedAt') }}</th><th></th></tr>
                </thead>
                <tbody>
                  <tr v-for="(release, index) in history[b.id]" :key="release.version + release.channel">
                    <td><strong>{{ release.version }}</strong> <span v-if="index === 0" class="badge latest">{{ t('latest') }}</span></td>
                    <td><span class="badge" :class="release.channel">{{ release.channel === 'stable' ? t('stable') : t('beta') }}</span></td>
                    <td>{{ formatSize(release.size) }}</td>
                    <td class="notes-col">{{ release.notes || t('noNotes') }}</td>
                    <td>{{ formatTime(release.created_at) }}</td>
                    <td><a class="download" :href="release.url">{{ t('download') }}</a></td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-else class="empty">{{ t('noReleases') }}</p>
          </div>
        </section>

        <!-- USB web flashing -->
        <section v-else-if="view === 'flash'" class="view">
          <div class="view-head">
            <h1>{{ t('flashHeading') }}</h1>
            <p class="subtitle">{{ t('flashIntro') }}</p>
          </div>
          <div class="board-grid">
							<article v-for="b in publicBoards" :key="b.id" class="board-card flash-card">
              <div class="board-card-head">
                <h3>{{ b.name }}</h3>
                <span class="chip">{{ b.chip }}</span>
              </div>
              <template v-if="b.flashable">
                <p class="tagline">{{ t('flashReady') }}</p>
                <template v-if="flasherReady[b.id]">
                  <p v-if="!secureContext" class="unsupported">{{ t('flashNeedsHttps') }}</p>
                  <p v-else-if="!webSerialAvailable" class="unsupported">{{ t('flashUnsupported') }}</p>
                  <template v-else>
                    <esp-web-install-button :manifest="apiURL('/flasher/manifest-' + b.id + '.json')">
                      <button slot="activate" class="flash-btn primary">{{ t('flashButton') }}</button>
                      <span slot="unsupported" class="unsupported">{{ t('flashUnsupported') }}</span>
                      <span slot="not-allowed" class="unsupported">{{ t('flashNotAllowed') }}</span>
                    </esp-web-install-button>
                    <p class="flash-tip">{{ t('flashTip') }}</p>
                  </template>
                </template>
                <p v-else class="empty">{{ t('flashUnavailable') }}</p>
              </template>
              <p v-else class="tagline serial-only">{{ t('flashSerialOnly') }}</p>
              <button class="ghost serial-open" @click="openSerialDebug(b.id)">{{ t('navSerial') }}</button>
              <code class="board-id">{{ b.id }}</code>
            </article>
          </div>
        </section>

        <!-- Interactive Web Serial terminal. The editable line is deliberately
             outside the scrolling debug output, so logs never disrupt typing. -->
        <section v-else-if="view === 'serial'" class="view">
          <div class="view-head">
            <h1>{{ t('serialHeading') }}</h1>
            <p class="subtitle">{{ t('serialIntro') }}</p>
          </div>
          <div class="panel serial-panel">
            <div class="serial-toolbar">
              <label>{{ t('serialBoard') }}
                <select v-model="serialBoard" :disabled="serialConnected">
									<option v-for="b in publicBoards" :key="b.id" :value="b.id">{{ b.name }}</option>
                </select>
              </label>
              <button v-if="!serialConnected" class="primary" :disabled="!webSerialAvailable" @click="connectSerial">{{ t('serialConnect') }}</button>
              <button v-else class="ghost" @click="disconnectSerial">{{ t('serialDisconnect') }}</button>
            </div>
            <p v-if="!webSerialAvailable" class="unsupported">{{ t('serialUnsupported') }}</p>
            <p v-if="serialMessage" class="error">{{ serialMessage }}</p>
            <div class="serial-terminal" @click="$refs.serialCommand?.focus()">
              <pre ref="serialOutputEl" class="serial-output">{{ serialOutput }}</pre>
              <form class="serial-command" @submit.prevent="sendSerialCommand">
                <span class="serial-prompt">{{ serialPrompt }}</span>
                <input ref="serialCommand" v-model="serialInput" :disabled="!serialConnected" autocomplete="off" autocapitalize="off" spellcheck="false" aria-label="AT command">
              </form>
            </div>
          </div>
        </section>

        <!-- Admin login -->
        <section v-else-if="view === 'login'" class="view login-view">
          <div class="login-card">
            <div class="login-logo">◉</div>
            <h2>{{ t('loginTitle') }}</h2>
            <p class="hint">{{ t('adminHint') }}</p>
            <label>{{ t('username') }}<input v-model="username" autocomplete="username" @keyup.enter="login"></label>
            <label>{{ t('password') }}<input v-model="password" type="password" autocomplete="current-password" @keyup.enter="login"></label>
            <button class="primary block" @click="login">{{ t('login') }}</button>
            <p v-if="loginError" class="error">{{ loginError }}</p>
          </div>
        </section>

        <!-- Device management dashboard -->
        <section v-else-if="view === 'devices'" class="view">
          <div class="view-head row">
            <h1>{{ t('dashboardTitle') }}</h1>
            <button class="ghost" @click="refreshDevices">{{ t('refresh') }}</button>
          </div>
          <div class="stat-grid">
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'all' }" @click="statusFilter = 'all'">
              <span class="stat-num">{{ stats.total }}</span><span class="stat-label">{{ t('statTotal') }}</span>
            </button>
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'online' }" @click="setFilter('online')">
              <span class="stat-num accent-green">{{ stats.online }}</span><span class="stat-label">{{ t('statOnline') }}</span>
            </button>
            <div class="stat-card static">
              <span class="stat-num">{{ stats.boards }}</span><span class="stat-label">{{ t('statBoards') }}</span>
            </div>
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'outdated' }" @click="setFilter('outdated')">
              <span class="stat-num accent-amber">{{ stats.outdated }}</span><span class="stat-label">{{ t('statOutdated') }}</span>
            </button>
          </div>
          <p v-if="loginError" class="error">{{ loginError }}</p>
          <div class="panel table-panel">
            <div class="table-toolbar">
              <input class="search" v-model="search" type="search" :placeholder="t('searchPlaceholder')">
              <span class="muted-sm">{{ t('onlineHint') }}</span>
            </div>
            <div class="table-scroll sticky" v-if="filteredRows.length">
              <table>
                <thead>
                  <tr><th>{{ t('status') }}</th><th>{{ t('deviceId') }}</th><th>{{ t('board') }}</th><th>{{ t('firmware') }}</th><th>{{ t('callsign') }}</th><th>{{ t('ssid') }}</th><th>{{ t('ipAddress') }}</th><th>{{ t('lastSeen') }}</th></tr>
                </thead>
                <tbody>
                  <tr v-for="d in pagedRows" :key="d.device_id">
                    <td><span class="status"><span class="dot" :class="d.online ? 'on' : 'off'"></span>{{ d.online ? t('statusOnline') : t('statusOffline') }}</span></td>
                    <td class="mono">{{ d.device_id }}</td>
                    <td>{{ boardName(d.board_type) }}</td>
                    <td>{{ d.firmware_version }} <span v-if="d.outdated" class="badge beta">{{ t('updateAvailable') }}</span><span v-else-if="d.latest" class="badge stable">{{ t('upToDate') }}</span></td>
                    <td>{{ d.metadata?.nrl_callsign || '-' }}</td>
                    <td>{{ d.metadata?.nrl_ssid ?? '-' }}</td>
                    <td class="mono">{{ d.ip_address }}</td>
                    <td>{{ formatTime(d.last_seen) }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-else-if="deviceRows.length" class="empty">{{ t('noMatch') }}</p>
            <p v-else class="empty">{{ t('noDevices') }}</p>
            <div class="pager" v-if="pageCount > 1">
              <span class="muted-sm">{{ t('pageInfo', { from: pageFrom, to: pageTo, total: filteredRows.length }) }}</span>
              <div class="pager-controls">
                <button class="ghost" :disabled="page <= 1" @click="goPage(-1)">{{ t('prevPage') }}</button>
                <span class="muted-sm">{{ t('pageOf', { page: page, count: pageCount }) }}</span>
                <button class="ghost" :disabled="page >= pageCount" @click="goPage(1)">{{ t('nextPage') }}</button>
              </div>
            </div>
          </div>
        </section>

				<!-- Board type, image and feature catalog management -->
				<section v-else-if="view === 'boards'" class="view">
					<div class="view-head row">
						<div><h1>{{ t('boardManagerTitle') }}</h1><p class="subtitle">{{ t('boardManagerHint') }}</p></div>
						<button class="primary" @click="newBoard">{{ t('newBoard') }}</button>
					</div>
					<div class="catalog-layout">
						<aside class="panel board-list">
							<button v-for="b in catalogBoards" :key="b.id" :class="{ active: !boardIsNew && boardEditor.id === b.id }" @click="editBoard(b)">
								<strong>{{ language === 'zh' ? b.name_zh : b.name_en }}</strong>
								<code>{{ b.id }}</code><span class="badge" :class="b.status === 'published' ? 'stable' : 'beta'">{{ b.status }}</span>
							</button>
						</aside>
						<div>
							<div class="panel publish-grid">
								<label>{{ t('boardId') }}<input v-model.trim="boardEditor.id" :disabled="!boardIsNew" pattern="[a-z0-9][a-z0-9_-]*"></label>
								<label>{{ t('boardStatus') }}<select v-model="boardEditor.status"><option value="draft">{{ t('draft') }}</option><option value="published">{{ t('publishedStatus') }}</option><option value="archived">{{ t('archived') }}</option></select></label>
								<label>{{ t('nameZH') }}<input v-model="boardEditor.name_zh"></label>
								<label>{{ t('nameEN') }}<input v-model="boardEditor.name_en"></label>
								<label>{{ t('taglineZH') }}<input v-model="boardEditor.tagline_zh"></label>
								<label>{{ t('taglineEN') }}<input v-model="boardEditor.tagline_en"></label>
								<label class="wide">{{ t('descriptionZH') }}<textarea v-model="boardEditor.description_zh"></textarea></label>
								<label class="wide">{{ t('descriptionEN') }}<textarea v-model="boardEditor.description_en"></textarea></label>
								<label>{{ t('chipLabel') }}<input v-model="boardEditor.chip_label" placeholder="ESP32-S3"></label>
								<label>{{ t('webFlashFamily') }}<input v-model="boardEditor.web_flash_chip_family" placeholder="ESP32-S3"></label>
								<label>{{ t('displayOrder') }}<input v-model.number="boardEditor.display_order" type="number"></label>
								<label class="wide">{{ t('highlightsZH') }}<textarea v-model="boardEditor.highlights_zh_text"></textarea></label>
								<label class="wide">{{ t('highlightsEN') }}<textarea v-model="boardEditor.highlights_en_text"></textarea></label>
								<div class="actions wide"><button class="primary" @click="saveBoard">{{ t('saveBoard') }}</button></div>
							</div>

							<div v-if="!boardIsNew" class="panel">
								<h2 class="panel-h">{{ t('boardImage') }}</h2>
								<div class="image-upload-row">
									<img v-if="boardEditor.image_url" class="board-image-preview" :src="boardEditor.image_url" :alt="boardEditor.name_en">
									<div><input ref="boardImage" type="file" accept="image/jpeg,image/png,image/webp"><button class="primary image-upload-button" @click="uploadBoardImage">{{ t('uploadImage') }}</button></div>
								</div>
							</div>

							<div v-if="!boardIsNew" class="panel">
								<h2 class="panel-h">{{ t('featureAssignments') }}</h2>
								<div class="feature-editor-grid">
									<div v-for="f in catalogFeatures" :key="f.key" class="feature-assignment">
										<div class="feature-assignment-row">
											<span>{{ language === 'zh' ? f.label_zh : f.label_en }} <code>{{ f.key }}</code></span>
											<select v-model="boardEditor.features[f.key]"><option value="yes">{{ t('featureYes') }}</option><option value="partial">{{ t('featurePartial') }}</option><option value="no">{{ t('featureNo') }}</option></select>
										</div>
										<div v-if="boardEditor.features[f.key] === 'partial'" class="feature-note-row">
											<input v-model="boardEditor.feature_notes[f.key].zh" :placeholder="t('partialNoteZH')">
											<input v-model="boardEditor.feature_notes[f.key].en" :placeholder="t('partialNoteEN')">
										</div>
									</div>
								</div>
								<div class="actions"><button class="primary" @click="saveBoardFeatures">{{ t('saveFeatures') }}</button></div>
							</div>

							<div class="panel">
								<div class="panel-head"><h2>{{ featureIsNew ? t('addFeature') : t('editFeature') }}</h2><button class="ghost" @click="newFeatureDraft">{{ t('newFeature') }}</button></div>
								<div class="feature-picker">
									<button v-for="f in catalogFeatures" :key="f.key" class="ghost" @click="editFeature(f)">{{ language === 'zh' ? f.label_zh : f.label_en }}</button>
								</div>
								<div class="publish-grid compact-form">
									<label>{{ t('featureKey') }}<input v-model.trim="featureDraft.key" :disabled="!featureIsNew"></label>
									<label>{{ t('displayOrder') }}<input v-model.number="featureDraft.display_order" type="number"></label>
									<label>{{ t('featureLabelZH') }}<input v-model="featureDraft.label_zh"></label>
									<label>{{ t('featureLabelEN') }}<input v-model="featureDraft.label_en"></label>
									<label class="wide">{{ t('featureDescriptionZH') }}<textarea v-model="featureDraft.description_zh"></textarea></label>
									<label class="wide">{{ t('featureDescriptionEN') }}<textarea v-model="featureDraft.description_en"></textarea></label>
									<div class="actions wide"><button class="primary" @click="addFeature">{{ featureIsNew ? t('addFeature') : t('editFeature') }}</button></div>
								</div>
							</div>

							<details class="panel ai-import">
								<summary>{{ t('aiImport') }}</summary>
								<p class="hint">{{ t('aiImportHint') }}</p>
								<textarea v-model="aiImportJSON" spellcheck="false" placeholder='{"board":{"id":"...","name_zh":"...","name_en":"...","status":"draft"},"features":[],"assignments":{"aprs":{"state":"yes"}}}'></textarea>
								<button class="primary" @click="importCatalog">{{ t('importCatalog') }}</button>
							</details>
							<p v-if="boardMessage" class="message catalog-message" aria-live="polite">{{ boardMessage }}</p>
						</div>
					</div>
				</section>

        <!-- Publish firmware -->
        <section v-else-if="view === 'publish'" class="view">
          <div class="view-head"><h1>{{ t('publishHeading') }}</h1></div>
          <div class="panel">
            <div class="publish-grid">
								<label>{{ t('boardType') }}<select v-model="board"><option v-for="b in uploadBoards" :key="b.id" :value="b.id">{{ b.name }}</option></select></label>
              <label>{{ t('firmwareVersion') }}<input v-model="version" :placeholder="t('firmwareVersion')"></label>
              <label>{{ t('channel') }}<select v-model="channel"><option value="stable">{{ t('stable') }}</option><option value="beta">{{ t('beta') }}</option></select></label>
              <label>{{ t('firmwareFile') }}<input ref="firmware" type="file" accept=".bin"></label>
              <label class="wide">{{ t('releaseNotes') }}<textarea v-model="notes" :placeholder="t('releaseNotes')"></textarea></label>
              <div class="actions wide"><button class="primary" @click="upload">{{ t('publish') }}</button><span class="message" aria-live="polite">{{ publishMessage }}</span></div>
            </div>
          </div>
        </section>
      </main>

      <footer class="app-footer">{{ t('brandName') }} · {{ t('subtitle') }}</footer>
    </div>`,
});

// <esp-web-install-button> is a custom element defined by esp-web-tools; tell the
// Vue template compiler not to treat it as a Vue component.
app.config.compilerOptions.isCustomElement = (tag) => tag.startsWith("esp-web-");
app.mount("#app");

// Load the esp-web-tools web component at runtime (kept out of the Vite bundle so
// its own relative chunk imports resolve against /esp-web-tools/).
if (!document.querySelector("script[data-esp-web-tools]")) {
  const script = document.createElement("script");
  script.type = "module";
  script.src = "/esp-web-tools/install-button.js";
  script.dataset.espWebTools = "1";
  document.head.appendChild(script);
}
