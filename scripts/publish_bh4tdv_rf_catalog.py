#!/usr/bin/env python3
"""Create and publish the BH4TDV-RF entry in the NRL OTA catalog."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path

from publish_ota_mcp import MCPClient, MCPError


BOARD_ID = "bh4tdv_rf"

FEATURES = [
    {
        "key": "sr110u_radio",
        "label_zh": "SR-110U UHF 射频收发",
        "label_en": "SR-110U UHF radio transceiver",
        "description_zh": "SR-110U 模块电源、PTT、静噪、功率选择和串口控制。",
        "description_en": "SR-110U power, PTT, squelch, power-level, and UART control.",
        "group": "hardware",
        "display_order": 42,
        "active": True,
    },
    {
        "key": "gps",
        "label_zh": "GPS 定位",
        "label_en": "GPS positioning",
        "description_zh": "板载 GPS 定位、状态显示和 APRS 位置数据。",
        "description_en": "On-board GPS positioning, status display, and APRS location data.",
        "group": "hardware",
        "display_order": 43,
        "active": True,
    },
    {
        "key": "environment_sensors",
        "label_zh": "环境与光照传感器",
        "label_en": "Environmental and light sensors",
        "description_zh": "BMP280 温度/气压、AHT20 湿度和 BH1750 照度采集，并在屏幕及 Web 显示。",
        "description_en": "BMP280 temperature/pressure, AHT20 humidity, and BH1750 illuminance with screen and Web display.",
        "group": "hardware",
        "display_order": 44,
        "active": True,
    },
    {
        "key": "electronic_compass",
        "label_zh": "电子罗盘",
        "label_en": "Electronic compass",
        "description_zh": "QMC5883L 三轴磁场和水平航向显示。",
        "description_en": "QMC5883L three-axis magnetic field and level heading display.",
        "group": "hardware",
        "display_order": 45,
        "active": True,
    },
]

BOARD = {
    "id": BOARD_ID,
    "name_zh": "BH4TDV-RF 射频伴侣终端",
    "name_en": "BH4TDV-RF Radio Companion",
    "tagline_zh": "集成 UHF 射频、实体键和环境传感器的 ESP32-S3 终端",
    "tagline_en": "ESP32-S3 terminal with UHF radio, physical keys, and sensors",
    "description_zh": (
        "基于 BI4UMD 2.8 英寸触摸屏主板与 NRL 伴侣扩展板，集成 SR-110U UHF 射频、"
        "PCA9555 实体键、GPS、BMP280、BH1750 和 QMC5883L；传感器数据可在屏幕和 Web 查看。"
    ),
    "description_en": (
        "BI4UMD 2.8-inch touch-screen main board with the NRL companion expansion board, "
        "integrating an SR-110U UHF radio, PCA9555 physical keys, GPS, BMP280, BH1750, "
        "and QMC5883L with on-screen and Web sensor telemetry."
    ),
    "chip_label": "ESP32-S3",
    "web_flash_chip_family": "ESP32-S3",
    "image_url": "",
    "display_order": 25,
    "status": "draft",
    "highlights_zh": [
        "SR-110U UHF 射频收发、PTT、SQL 和功率控制",
        "2.8 英寸 ILI9341 触摸屏与 PCA9555 实体键",
        "GPS、APRS 地图、TF/SMB 媒体和网络电台",
        "BMP280、BH1750、QMC5883L 屏幕/Web 实时数据",
    ],
    "highlights_en": [
        "SR-110U UHF transceiver with PTT, squelch, and power control",
        "2.8-inch ILI9341 touch display and PCA9555 physical keys",
        "GPS, APRS map, TF/SMB media, and Internet radio",
        "BMP280, BH1750, and QMC5883L telemetry on screen and Web",
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
    "aprs_map": "yes",
    "sstv": "yes",
    "mdc1200": "yes",
    "dtmf": "yes",
    "ctcss": "yes",
    "cwdecode": "yes",
    "screen_signaling": "yes",
    "web_flash": "yes",
    "ble": "yes",
    "es8311": "yes",
    "audio_processing": "yes",
    "radio_ptt_sql": "yes",
    "sci": "yes",
    "status_indicator": "yes",
    "color_display": "yes",
    "touch": "yes",
    "buttons": "yes",
    "battery": "yes",
    "tf_media": "yes",
    "smb_media": "yes",
    "espnow": "yes",
    "music_radio": "yes",
    "sr110u_radio": "yes",
    "gps": "yes",
    # BMP280/BH1750 work, but AHT20 is disabled until its 0x38 conflict is fixed.
    "environment_sensors": "partial",
    # Raw QMC5883L heading works; installation orientation and calibration remain.
    "electronic_compass": "partial",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--image",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "docs" / "BH4TDV-RF-2.jpg",
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

    result = client.call_tool("board.save_draft", {"board": BOARD})
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
