#!/usr/bin/env python3
"""Create and publish the BH4TDV-2900W entry in the NRL OTA catalog."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path

from publish_ota_mcp import MCPClient, MCPError


BOARD_ID = "bh4tdv_2900w"

FEATURES = [
    {
        "key": "loopback_test",
        "label_zh": "语音环路测试",
        "label_en": "Voice loopback test",
        "description_zh": "按住 LOOP 按钮后,麦克风声音经 ES8311 ADC 到 ESP32,再由 ES8311 DAC 本地回放,用于快速自检音频链路。",
        "description_en": "Hold the LOOP button to route mic audio through the ES8311 ADC, the ESP32, and back out the ES8311 DAC for a quick audio-path self-test.",
        "group": "hardware",
        "display_order": 46,
        "active": True,
    },
    {
        "key": "dual_ptt_output",
        "label_zh": "双路电台 PTT 输出",
        "label_en": "Dual radio PTT outputs",
        "description_zh": "IO38 与 IO39 同步键控电台,IO38 可通过 Web 配置高或低电平触发,默认高电平。",
        "description_en": "IO38 and IO39 key the radios together; IO38 supports Web-configurable active-high or active-low triggering and defaults to high.",
        "group": "hardware",
        "display_order": 47,
        "active": True,
    },
]

BOARD = {
    "id": BOARD_ID,
    "name_zh": "BH4TDV 2900W 电台接口板",
    "name_en": "BH4TDV 2900W Radio Interface",
    "tagline_zh": "ESP32-S3 无屏电台接口,带电池检测与音频环路自检",
    "tagline_en": "ESP32-S3 headless radio interface with battery sense and audio loopback self-test",
    "description_zh": (
        "基于 ESP32-S3 的无屏电台网络桥接板:ES8311 全双工音频,双路 PTT 输出与双路 SQL 静噪输入,"
        "静噪开启时才向网络发送语音;红/绿/蓝三状态灯分别指示 PTT、SQL 与网络连接;"
        "板载电池电压检测和语音环路测试按钮。"
    ),
    "description_en": (
        "ESP32-S3 headless network bridge for radios: ES8311 full-duplex audio, dual PTT outputs "
        "and dual squelch inputs (voice is sent to the network only while squelch is open); "
        "red/green/blue status LEDs for PTT, SQL, and network link; on-board battery voltage "
        "sense and voice loopback test button."
    ),
    "chip_label": "ESP32-S3",
    "web_flash_chip_family": "ESP32-S3",
    "image_url": "",
    "display_order": 21,
    "status": "draft",
    "highlights_zh": [
        "ES8311 全双工音频,适配两部电台",
        "双路 PTT 输出 + 双路 SQL 静噪输入,静噪开启才发送网络语音",
        "红/绿/蓝三状态灯:PTT、SQL、网络连接",
        "电池电压检测、语音环路测试按钮、IO38 PTT 电平可配置",
        "BLE 配网、Wi-Fi 配置门户、远程 AT 与 OTA",
    ],
    "highlights_en": [
        "ES8311 full-duplex audio for two radios",
        "Dual PTT outputs + dual squelch inputs; uplink only while squelch is open",
        "Red/green/blue status LEDs: PTT, SQL, network link",
        "Battery sense, loopback test, configurable IO38 PTT polarity",
        "BLE provisioning, Wi-Fi portal, remote AT, and OTA",
    ],
    "features": {},
    "feature_notes": {},
    "created_at": 0,
    "updated_at": 0,
}

ASSIGNMENTS = {
    "nrl_voice": "yes",
    "wifi_portal": "yes",
    "remote_at_ota": "yes",
    "aprs": "yes",
    "aprs_map": "no",
    "sstv": "no",
    "mdc1200": "yes",
    "dtmf": "yes",
    "ctcss": "yes",
    "cwdecode": "partial",
    "screen_signaling": "no",
    "web_flash": "yes",
    "ble": "yes",
    "es8311": "yes",
    "audio_processing": "yes",
    "radio_ptt_sql": "yes",
    "channel_select": "no",
    "sci": "no",
    "status_indicator": "yes",
    "color_display": "no",
    "touch": "no",
    "buttons": "no",
    "battery": "yes",
    "tf_media": "no",
    "smb_media": "no",
    "espnow": "yes",
    "music_radio": "yes",
    "loopback_test": "yes",
    "dual_ptt_output": "yes",
}

FEATURE_NOTES = {
    "cwdecode": {
        "zh": "AT+CW 配置与解码,无屏幕界面",
        "en": "AT+CW config and decode, no display UI",
    },
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--image",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "docs" / "BH4TDV-2900W.png",
    )
    args = parser.parse_args()
    server = os.environ.get("OTA_SERVER_URL", "").strip()
    token = os.environ.get("OTA_ADMIN_TOKEN", "").strip()
    if not server or not token:
        raise SystemExit("OTA_SERVER_URL and OTA_ADMIN_TOKEN must be set")
    if not args.image.is_file() or args.image.stat().st_size > 5 * 1024 * 1024:
        raise SystemExit("board image must exist and be no larger than 5 MB")

    client = MCPClient(server, token)
    catalog = client.call_tool("catalog.list", {"include_drafts": True})
    boards = {item["id"]: item for item in catalog.get("boards", [])}
    existing_features = {item["key"] for item in catalog.get("features", [])}
    existing_board = boards.get(BOARD_ID)
    if existing_board and existing_board.get("status") == "published":
        print(f"{BOARD_ID}: already published; catalog mutation skipped")
        return 0

    for feature in FEATURES:
        result = client.call_tool(
            "feature.save",
            {"feature": feature, "confirm_update": feature["key"] in existing_features},
        )
        print(f"{feature['key']}: {result.get('status')}")

    board = dict(BOARD)
    board["feature_notes"] = FEATURE_NOTES
    result = client.call_tool("board.save_draft", {"board": board})
    print(f"{BOARD_ID}: {result.get('status')}")
    result = client.call_tool(
        "board.set_features",
        {"board_id": BOARD_ID, "assignments": ASSIGNMENTS},
    )
    print(f"{BOARD_ID} features: {result.get('status')}")
    image = base64.b64encode(args.image.read_bytes()).decode("ascii")
    result = client.call_tool(
        "board.upload_image", {"board_id": BOARD_ID, "image_base64": image}
    )
    print(f"{BOARD_ID} image: {result.get('message')}")
    result = client.call_tool(
        "board.publish", {"board_id": BOARD_ID, "confirm": True}
    )
    print(f"{BOARD_ID}: {result.get('status')}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MCPError as exc:
        raise SystemExit(str(exc)) from exc
