# FMO 设备激活接口（绑定 MAC 自动获取证书）API 文档

> 服务端实现：`fmo-certificate-tools` 仓库 `server/platform/device.go`、`server/cert/payload.go`
> 设备端实现（本项目）：`src/services/fmo_activate.cpp`（HTTP/NVS/密钥管理）、`src/services/fmo_activate_core.cpp`（CBOR/JSON 编解码，含 golden 向量主机测试 `tests/fmo_activate_test.cpp`）

## 1. 概述

设备通过本接口上报自己的 MAC 地址和 Ed25519 设备公钥，平台对该 MAC 已绑定的用户自动签发 FMO-V4 身份证书（userCert + intermediateCert），设备无需人工上传证书文件即可完成身份配置。同一接口重复调用即为**续期**（uid 沿用，旧证书自动进入 CRL，reason=2 superseded）。

- **端点**：`POST /api/device/activate`
- **Content-Type**：`application/json`
- **认证**：无 HTTP 层认证；身份与防篡改完全依赖请求体自带的 Ed25519 签名
- **默认服务器**：`https://www.hamptt.com`（设备端可在 Web 管理页 `/fmo` 修改）
- **响应**：**恒为 HTTP 200**，业务结果在响应体 `result`/`code` 字段中

### 前置条件

MAC 与用户的绑定**不是**本接口完成的，必须先满足其一，否则返回 `code=2`（未登记）或 `code=1`（未绑定）：

- 用户登录平台后调用 `POST /api/account/devices/bind`（Bearer token，body `{"mac":"AABBCCDDEEFF","model":"..."}`）认领设备；
- 或管理员预登记 `POST /api/admin/device/create`。

## 2. 请求

### 2.1 请求体字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `version` | string | 固定 `"4"` |
| `action` | string | 固定 `"activate"` |
| `mac` | string | 设备 MAC，12 位十六进制，无分隔符，大小写不敏感（平台统一转大写） |
| `timestamp` | number | Unix 秒，与服务器时间偏差须在 **±120 秒**内 |
| `firmwareVersion` | string | 固件版本号，如 `"0.8.80"` |
| `firmwareHash` | string | 固件镜像哈希，**32 字节**，hex（64 字符）或 base64/base64url |
| `countryCode` | string | 两位国家码，如 `"CN"` |
| `devicePublicKey` | string | 设备 Ed25519 公钥，**32 字节**，hex 或 base64url |
| `nonce` | string | 防重放随机数，**6 字节**，hex（12 字符）或 base64url；全库查重，不可重复 |
| `signature` | string | Ed25519 签名，**64 字节**，hex 或 base64url；签名内容见 §3 |

字节类字段的解码规则（服务端 `decodeFlexibleBytes`）：**长度恰为目标字节数 2 倍时优先按 hex 解码**，否则按 base64/base64url。设备端统一用 hex 最不容易产生歧义。

### 2.2 请求示例

```json
{
  "version": "4",
  "action": "activate",
  "mac": "D0CF13510C4C",
  "timestamp": 1783291391,
  "firmwareVersion": "0.8.80",
  "firmwareHash": "0707070707070707070707070707070707070707070707070707070707070707",
  "countryCode": "CN",
  "devicePublicKey": "ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1",
  "nonce": "010203040506",
  "signature": "<64 字节签名的 hex，128 字符>"
}
```

## 3. 签名算法（关键）

`signature` = 设备 Ed25519 私钥对**确定性 CBOR 编码的 10 元素数组**的 detached 签名：

```
["FMO", 4, "activateReq", macBytes(6), timestamp, nonceBytes(6),
 firmwareVersion(text), firmwareHashBytes(32), countryCode(text), devicePublicKeyBytes(32)]
```

编码规则（必须与平台字节级一致，否则验签失败 `code=9`）：

- CBOR 确定性编码：整数/长度一律**最短形式**（<24 单字节，24/25/26/27 分别带 1/2/4/8 字节大端长度）；
- `mac` 在签名数组里是 **6 字节原始字节串**（major type 2），**不是** 12 字符文本；
- `timestamp` 为正整数（major type 0）；`firmwareVersion`、`countryCode` 为 UTF-8 文本串（major type 3）；
- 数组头部为 `0x8A`（10 元素定长数组）。

### Golden 测试向量（设备端实现自校验用）

输入：

- mac = `D0CF13510C4C`，timestamp = `1783291391`，nonce = `010203040506`
- firmwareVersion = `"4.0.0"`，firmwareHash = `0x07` × 32，countryCode = `"CN"`
- devicePublicKey = `ED4928C628D1C2C6EAE90338905995612959273A5C63F93636C14614AC8737D1`

期望 CBOR（hex）：

```
8A63464D4F046B616374697661746552657146D0CF13510C4C1A6A4ADDFF4601020304050665342E302E305820070707070707070707070707070707070707070707070707070707070707070762434E5820ED4928C628D1C2C6EAE90338905995612959273A5C63F93636C14614AC8737D1
```

本项目主机测试 `tests/fmo_activate_test.cpp` 对该向量做逐字节比对；服务端见 `fmo-certificate-tools/server/cert/golden_test.go:11`。

## 4. 响应

### 4.1 成功（`code=0`）

```json
{
  "result": "ok",
  "code": 0,
  "serverTime": 1783291391,
  "authTag": "<32 字符随机串>",
  "certPackage": {
    "userCert": {
      "type": "userCert",
      "issuerSn": 1,
      "subject": { "callsign": "BG5ESN", "uid": 1001, "publicKey": "<base64url>" },
      "iat": 1783291391,
      "exp": 1914827391,
      "signatureAlgorithm": "Ed25519",
      "signature": "<base64url>"
    },
    "intermediateCert": { "..." : "平台 Intermediate CA 证书，结构同上，含 extensions" }
  }
}
```

- 证书为 FMO-V4 自定义 **JSON 证书**（Ed25519 + 确定性 CBOR 签名数组），**不是 PEM/DER**；公钥/签名字段均为 base64url。
- userCert 有效期 **365 天**；到期前用同一接口续期即可。
- `authTag`：设备在平台的认证串，供后续平台 API 使用（当前固件仅记录日志）。
- 设备端处理：把 `certPackage.userCert`、`certPackage.intermediateCert` 两个子对象分别序列化为 JSON 后写入证书存储（本项目 `FMO_CERT_Put(FMO_CERT_USER / FMO_CERT_INTERMEDIATE, ...)`），随后用新身份重连 FMO。

### 4.2 失败 / 待审核

```json
{ "result": "error",   "code": 2,   "reason": "Device not registered", "serverTime": 1783291391 }
{ "result": "pending", "code": 100, "reason": "manual review required", "serverTime": 1783291391 }
```

| code | 含义 | 设备端建议处理 |
|---|---|---|
| 1 | User not bound（设备已登记但未绑定用户） | 提示用户先在平台绑定该 MAC |
| 2 | Device not registered（MAC 未登记） | 提示用户先在平台登记/绑定该 MAC |
| 3 | Timestamp error（超出 ±120 s 窗口） | 等待 SNTP 同步后重试 |
| 4 | Request replay（nonce 重复） | 重新生成 nonce 重试 |
| 5 | Malformed request（字段/编码错误） | 检查请求构造 |
| 6 | Country restriction | 检查 countryCode |
| 7 | Device or user blacklisted | 联系平台 |
| 8 | Certificate request limit exceeded（同 MAC **1 小时 5 次**上限） | 稍后重试 |
| 9 | Device signature error（验签失败） | 检查 CBOR 字节级一致性（对照 §3 golden 向量）与设备密钥 |
| 10 | Server CA not configured | 平台侧问题 |
| 100 | manual review required（待人工审核） | 等待审核后重试 |

注：验签通过之前的失败（3/4/5 及解码错误）不在平台落库、不消耗 nonce；限频只统计 ok/pending 请求。

## 5. 设备端实现要点（本项目固件）

调用入口：Web 管理页 `/fmo` → “自动获取证书”面板（服务器地址默认 `www.hamptt.com`，可修改保存）。固件流程（`FMO_ACTIVATE_Run()`）：

1. 时间检查：`time(nullptr) < 1700000000` 视为 SNTP 未同步，拒绝发起（否则必得 `code=3`）；
2. 设备密钥：首次运行时本地生成 Ed25519 密钥对（32 字节随机 seed），以 `{"type":"deviceKey","seed":"<b64url>","pubKey":"<b64url>"}` 写入板载存储；**私钥不出设备**，后续复用；
3. 采集字段：MAC 取 STA MAC（`esp_read_mac(..., ESP_MAC_WIFI_STA)`）；firmwareVersion 取编译版本号；firmwareHash 取固件 ELF/分区 SHA-256（需 `CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64`）；nonce 取 6 字节随机数；
4. 按 §3 构造 CBOR → 设备私钥签名 → 组 JSON → POST；
5. 成功则写入两份证书并触发 FMO（MQTT）用新身份重连；失败按上表给出中文提示。

安全注意：deviceKey JSON 含私钥种子，不进日志、不从网页读回；平台侧私钥/seed 同样不入日志与审计。
