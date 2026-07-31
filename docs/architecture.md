# NRL-ESP32 S31 功能扩展架构规划

> 状态: 规划基准 (2026-07)。适用板卡: ESP32-S31-Korvo (esp32s31)。
> 格子派 / BH4TDV 复用其中的音频核心层, 应用层功能按硬件能力裁剪。

## 目标功能

1. 网络收音机 (HTTP 流)
2. 硬件 NRL 保姆: 播放 TF 卡音乐/信标音频, 目标可切换 本地 / NRL 网络 / 同时
3. 视频通话
4. 音乐播放器: TF 卡 / U 盘 (USB Host MSC) / SMB 网络共享上的
   WAV/MP3/FLAC/M4A/AAC, 支持元数据/封面显示, 蓝牙耳机听音乐
   (APE 已放弃: 唯一成熟的嵌入式解码器 libdemac 为 GPLv2,
   与本项目 MIT 许可冲突; 无损需求由 FLAC 覆盖)
5. 蓝牙耳机通话 (HFP, 已有基础)
6. AI 助手
7. ESP-NOW (脱网近距离语音)
8. APRS 收发 (四块板): 独立 UART2 GPS (NMEA) 位置信标 → APRS-IS 网络
   (可配服务器, 默认 asia.aprs2.net:14580, passcode 自动计算; 无 GPS 时用配置的
   默认经纬度) 和/或 Bell 202 AFSK 经扬声器→电台发射 (VOX 起振);
   麦克风侧解调电台收到的 APRS 音频, 解码后转发 APRS-IS (qAR iGate 方向)
   并进入 PSRAM 站点表 (呼号/SSID/经纬度/高度/航向/速度/注释 + 本机距离
   与连续报文推算速度)。展示: web 后台 /aprs 页 (全部字段, 5s 轮询)、
   AT+APRS 命令族、S31 韦韵屏 Apps→APRS 列表页、格子派主屏滚动监视行
   + 菜单 APRS LIST 页。实现: `src/lib/aprs/` 移植 aprsc 解析器 (BSD) 与
   VP-Digi 调制解调/AX.25 (GPL3, 经 ESP32APRS_Audio); 解调固定 9600Hz
   (16k 麦克风流 5:3 线性插值重采样), 发射 8kHz Q16 分数步进 DDS 经
   AudioRouter (AUDIO_SRC_APRS / AUDIO_SINK_APRS)。

## 一、现状与核心问题

现有音频管线是点对点的:

- `src/app/driver/audio_passthrough.*`: 拥有 I2S、mic 采集任务、**单一** frame hook、
  **单一** 8kHz 输出队列, AFE (AEC/NS) 挂在 mic 路径上。
- `src/lib/nrl_audio_bridge.*`: 独占 frame hook, G.711 编码 → UDP; 下行 UDP →
  `AUDIO_QueueOutputSamples`。
- `src/lib/nrl_bt_hfp.*`: 通过 `NRLAudioBridge_FeedExternalMic` 旁路塞入。

单 hook + 单队列无法承载多个并发音频功能, 扩展的前提是升级为多源多汇路由。

## 二、总体分层

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层 services/  (每个功能一个 App, 统一 start/stop/suspend) │
│  nrl_voice │ nanny │ net_radio │ music │ ai │ video │ espnow │
├──────────────────────────┬──────────────────────────────────┤
│ App 管理器 (资源仲裁)      │ UI 屏幕注册表 (LVGL, 拆分 display_s31)│
├──────────────────────────┴──────────────────────────────────┤
│ 音频核心 audio/  ★ 最重要的公用层                             │
│   AudioRouter: N源 × M汇 路由矩阵 + 混音 + SRC 重采样          │
├──────────────────────────────────────────────────────────────┤
│ 媒体引擎 media/ (解码/元数据)  │ 传输层 (UDP/ESP-NOW/HTTP/BT)  │
├──────────────────────────────────────────────────────────────┤
│ 驱动层 driver/ + vendor BSP (ES8389/I2S/LCD/SDMMC/摄像头)     │
└──────────────────────────────────────────────────────────────┘
```

## 三、公用组件

### 3.1 AudioRouter (音频路由器) — 所有音频功能的总线

- `AudioSource` / `AudioSink` 接口: 实现 `read_frame()` 即为源,
  `write_frame()` 即为汇, 向 Router 注册。
- 路由矩阵: 每条 (源→汇) 路由独立开关 + 独立增益; 多源进同一汇时混音。
- 语音域统一格式: **16 kHz 单声道 PCM16** (AFE/AEC/G.711/BT-SCO 的原生域)。

各功能即路由配置:

| 功能 | 路由 |
|---|---|
| NRL 对讲 | mic→NRL上行; NRL下行→喇叭 |
| 保姆 | 解码器→喇叭 / 解码器→NRL上行 / 双开 (三档切换); 信标源定时插入 |
| 网络收音机 | HTTP流解码器→喇叭 |
| 音乐 | 解码器→喇叭 或 →BT-A2DP汇 |
| BT 通话 | BT-HFP源→NRL上行; NRL下行→BT-HFP汇 |
| AI 助手 | mic→ASR汇; TTS源→喇叭 |
| ESP-NOW | mic→ESPNOW汇; ESPNOW源→喇叭 |

### 3.2 双时钟域 (无损播放的关键设计)

```
                      ┌── 高保真域 (无损直通) ──────────────────────┐
TF/HTTP → 解码器 ──┬──│ 原生率 44.1k/48k/96k/192k, 16/24bit, 立体声 │→ 喇叭汇(ES8389)
                   │  └────────────────────────────────────────┘
                   │  ┌── 语音域 (固定 16k mono 16bit) ────────┐
                   └─→│ SRC降采样+下混 → Router 混音矩阵        │→ NRL上行(8k G711)
  mic/NRL下行/TTS/──→│                                         │→ BT-SCO/ESPNOW/ASR
  信标/ESPNOW         └────────────────────────────────────────┘
```

- **高保真路径直通**, 不进混音矩阵, 中途零重采样零位深转换。
- **喇叭汇动态重配**: `SpeakerSink_SetFormat(rate, bits, ch)`, 换曲目时
  `esp_codec_dev_close()` → 按新参数重开。ES8389 芯片规格支持至 192kHz 采样。
  待 bring-up 验证:
  - 改用 MCLK (板级已接 GPIO2, 现配置 `use_mclk=false`), 高采样率必需;
  - 96k/192k、24bit 在 esp_codec_dev es8389 驱动中的实测支持
    (若驱动时钟表未覆盖 192k, 需补充其 coeff 配置)。
- **SRC 重采样器**: 已统一为 esp_audio_effects (`esp_ae_rate_cvt/ch_cvt`) ——
  媒体→8k 上行 (`audio/voice_resampler`)、媒体→44.1k BT A2DP
  (`services/music_player`)、AEC 16k→8k (`app/aec/aec_processor`) 三处共用;
  AudioRouter 边缘的 2:1 定点转换因 bit-exact 要求保留自研。
- **域冲突策略** (App 管理器仲裁, 可配置): 打断(默认, 先做) / 闪避混音(后期) / 忽略。
- **保姆"同时"模式**: 解码 PCM 出口 fan-out 两路 (原生率→喇叭 + SRC→NRL 上行),
  一次解码两处消费。

### 3.3 MediaPlayer (媒体引擎) — 收音机/保姆/音乐共用

- 实现 (2026-07 起): 解码链基于乐鑫 **ESP-GMF** pipeline —— 输入为 GMF IO
  (`io_file` 本地/U盘, 自写 `io_smb` 包装 SmbStream 专用连接, 自写 `io_radio`
  包装 TLS 先验证后降级重试的 HTTP 电台流, 官方 `io_hls` 处理 m3u8) →
  `aud_dec` (按内容嗅探格式, 底层 esp_audio_codec, 只注册 wav/mp3/flac/m4a/aac
  所需解码器以防分区溢出) → 回调 out-port 推 PCM 入 2MB PSRAM 环形缓冲;
  公开 API 仍是 `media_decoder.h` 的 Open/Decode/GetInfo/Close。
- 输入抽象: TF 卡文件 (VFS) 与 HTTP 流同一接口。
- 解码器插件 (`open(stream)/decode_frame()/seek()`, 按文件头探测格式):

| 格式 | 方案 | 成本 |
|---|---|---|
| WAV | 自写解析 | 极低 |
| MP3/FLAC/AAC/OGG | 乐鑫 `esp_audio_codec` 组件 | 低 |

- 元数据解析器 (`media/metadata_parser`, 公用): ID3v1/v2 (含 APIC 封面)、
  Vorbis Comment + PICTURE、APE Tag → 统一
  `TrackInfo{title, artist, album, duration, cover}`。
- 封面渲染: 复用已有 `esp_new_jpeg` / `libpng` / `esp_lv_decoder`, 贴 LVGL image。
  (视频通话的 JPEG 帧解码同源复用。)

### 3.4 VoiceLink (语音链路抽象) — NRL 与 ESP-NOW 互换

从 nrl_audio_bridge 拆出 `send_voice_frame()/on_voice_frame()` 接口;
NRL-UDP 与 ESP-NOW 为两个实现。PTT 逻辑、尾音抑制、呼号显示复用。

### 3.5 存储服务 (TF 卡 + U 盘)

统一挂载到 VFS/FatFS, MediaPlayer/播放列表按路径访问, 不感知介质差异:

- **TF 卡**: `bsp_sdcard_mount()` (vendor BSP 已有, SDMMC 4-bit GPIO20-25,
  电源开关 GPIO39 低有效), 挂载 `/sdcard`;
- **U 盘**: USB-OTG Host + `usb_host_msc` 组件 (esp-idf 官方), 挂载 `/usb`。
  仓库已有 `espressif__usb` host 栈 (原为 UVC 引入)。热插拔: MSC 事件 →
  挂载/卸载 → 通知播放列表与 UI 刷新。注意 USB-OTG host 与将来视频通话的
  UVC 外接摄像头共用同一个 USB 口, 由 App 管理器仲裁 (板载 DVP 摄像头不受影响)。

目录规范: `<mount>/music`、`<mount>/beacon`、`<mount>/rec`。曲库索引按挂载点
分别缓存, 避免每次全盘扫描; 播放列表合并扫描所有在线挂载点。

### 3.6 App 管理器 + UI 屏幕注册表

- App 生命周期 `start()/stop()/suspend()`; 仲裁冲突 (视频通话时暂停音乐;
  BT A2DP 与 WiFi 大流量并发限制)。
- display_s31.cpp (~1800 行) 拆分: 各 App 注册自己的 LVGL 屏幕。
- 音乐播放器屏幕: 封面 + 标题/艺术家/专辑 + 进度条; 触摸 上一曲/播放暂停/下一曲
  (GT1151 触摸); 板上 ADC 按键 VOL± 保持音量; 输出目标切换 喇叭/BT耳机 (切 Router 汇)。
- 播放队列组件 (`services/music/playlist`): `next()/prev()/shuffle()/repeat()`,
  保姆功能复用。

## 四、硬件依据 (官方 BSP, 已 vendor 到 `vendor/esp32_s31_korvo`)

与官方 `esp-dev-kits/examples/esp32-s31-korvo/examples/common_components` 逐字节一致。
参考实现: 官方 `factory_demo` (含 camera/audio/usb_hid)。

| 外设 | 引脚 | 状态 |
|---|---|---|
| TF 卡 (SDMMC 4-bit) | D0-D3=20-23, CLK=24, CMD=25, 电源=39(低有效) | BSP API 现成, 无引脚冲突 |
| DVP 摄像头 | D0-D7=46-53, PCLK=54, XCLK=55; 默认 1280×720 | BSP API 现成, 无引脚冲突 |
| I2S MCLK | GPIO2 (板级已接) | 高保真需切 `use_mclk=true` |

## 五、各功能落点与难度

| 功能 | 复用 | 新增 | 难度 |
|---|---|---|---|
| 音乐播放器(本地) | Router+MediaPlayer+TF | 元数据/封面 | 中 |
| 保姆 | 全部+现有NRL上行 | 信标调度器、三档切换UI | 低(架构就绪后) |
| 网络收音机 | MediaPlayer | HTTP 流输入 | 低 |
| BT 耳机通话 | 现有 nrl_bt_hfp | 收编为标准源/汇 | 低 |
| BT 耳机音乐 | Router | A2DP Source (Bluedroid) | 中 |
| ESP-NOW | VoiceLink 抽象 | ESP-NOW 传输实现 | 中 |
| AI 助手 | mic源+esp-sr AFE+TTS源 | 采用开源小智 (xiaozhi-esp32) 方案: WebSocket 连小智服务器, Opus 16k 音频, 服务器做 ASR/LLM/TTS; esp_audio_codec 已含 Opus | 中高 |
| 视频通话 | 音频走语音链路, JPEG 编解码复用 esp_new_jpeg | 扩展 NRL 协议: 报文类型 13 承载视频分片 (子头: 帧序号/分片号/分片数), DVP 摄像头 JPEG 帧经 NRL 服务器转发 | 高, 最后做 |

## 六、实施顺序

1. **Phase 0 (地基)**: 抽出 AudioRouter, 迁移现有 NRL 对讲与 BT HFP。
   外部行为零变化, 三块板同时受益。
2. **Phase 1**: 存储服务 (TF 卡挂载, U 盘 MSC 热插拔) + MediaPlayer
   (WAV→MP3→FLAC 递进) + 高保真喇叭汇 (动态采样率/MCLK) +
   元数据/封面 + 播放器 UI → 音乐播放器可用。
3. **Phase 2**: 保姆 (双路 fan-out + 信标调度 + 三档切换) + 网络收音机 (HTTP 流)。
4. **Phase 3**: BT A2DP 音乐、ESP-NOW。
5. **Phase 4**: AI 助手 → 视频通话。

## 七、目录规划

```
src/
  audio/        AudioRouter、source/sink 接口、resampler、tone/beacon 生成
  media/        解码器插件、metadata_parser、流抽象 (file/http)
  services/     nrl_voice(由 nrl_audio_bridge 重构)、nanny、net_radio、
                music、ai_assistant、video_call、espnow_link
  app/driver/   硬件驱动 (现状) + sdcard、camera 接入 (调 vendor BSP)
  lib/          门户/AT/BLE 等 (现状)
```
