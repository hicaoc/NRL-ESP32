#!/usr/bin/env python3
"""Publish complete flash packages and OTA releases for NRL boards.

Each upload contains every image listed by build/<board>/flasher_args.json.
The server stores the complete package for flashing and registers its app image
as the device OTA release. With no board arguments, all six boards are
published in one run.

Required environment: OTA_SERVER_URL, OTA_UPLOAD_TOKEN.
Optional environment: OTA_VERSION, OTA_CHANNEL (stable/beta), OTA_RELEASE_NOTES.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path


CHIP_FAMILY = {
    "gezipai": "ESP32-S3",
    "gezipai_4g": "ESP32-S3",
    "bi4umd": "ESP32-S3",
    "bh4tdv_rf": "ESP32-S3",
    "bh4tdv": "ESP32-S3",
    "s31_korvo": "ESP32-S31",
    "s31_function_coreboard": "ESP32-S31",
}
BOARDS = tuple(CHIP_FAMILY)


def firmware_version(repo: Path) -> str:
    value = os.environ.get("OTA_VERSION", "")
    if value:
        return value.removeprefix("v")
    text = (repo / "src" / "lib" / "nrl_version.h").read_text(encoding="utf-8")
    match = re.search(r'NRL_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise ValueError("could not determine firmware version; set OTA_VERSION")
    return match.group(1)


def package_from_build(
    repo: Path,
    board: str,
    version: str,
    channel: str,
    notes: str,
) -> tuple[dict, list[tuple[str, Path]]]:
    build_dir = repo / "build" / board
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise ValueError(
            f"{flasher_args_path} not found; build it first: "
            f"python scripts/build.py {board} build"
        )
    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    app_offset = int(flasher_args["app"]["offset"], 16)

    parts: list[dict] = []
    files: list[tuple[str, Path]] = []
    seen: set[str] = set()
    flash_files = sorted(
        flasher_args["flash_files"].items(), key=lambda item: int(item[0], 16)
    )
    for offset_hex, relative_path in flash_files:
        name = Path(relative_path).name
        if name in seen:
            raise ValueError(f"duplicate part filename in flasher_args: {name}")
        seen.add(name)
        source = build_dir / relative_path
        if not source.is_file():
            raise ValueError(f"missing flash image: {source}")
        parts.append({"offset": int(offset_hex, 16), "name": name})
        files.append((name, source))

    return {
        "board": board,
        "version": version,
        "channel": channel,
        "notes": notes,
        "chip_family": CHIP_FAMILY[board],
        "app_offset": app_offset,
        "parts": parts,
    }, files


def build_multipart(meta: dict, files: list[tuple[str, Path]]) -> tuple[bytes, str]:
    boundary = uuid.uuid4().hex
    separator = b"--" + boundary.encode()
    body = bytearray()
    body += separator + b'\r\nContent-Disposition: form-data; name="meta"\r\n\r\n'
    body += json.dumps(meta).encode("utf-8") + b"\r\n"
    for name, path in files:
        body += separator + b"\r\n"
        body += (
            f'Content-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
        ).encode("utf-8")
        body += b"Content-Type: application/octet-stream\r\n\r\n"
        body += path.read_bytes() + b"\r\n"
    body += separator + b"--\r\n"
    return bytes(body), boundary


def publish_package(base: str, token: str, meta: dict, files: list[tuple[str, Path]]) -> bool:
    body, boundary = build_multipart(meta, files)
    request = urllib.request.Request(
        base + "/api/v1/admin/flash-package",
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
            "X-OTA-Token": token,
        },
        method="POST",
    )
    board, version = meta["board"], meta["version"]
    print(f"Publishing {board} {version} complete package ({len(files)} parts)...")
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            result = response.read().decode("utf-8", errors="replace")
            print(f"  OK: {result}")
            return True
    except urllib.error.HTTPError as exc:
        result = exc.read().decode("utf-8", errors="replace")
        print(f"  FAILED: HTTP {exc.code}: {result}", file=sys.stderr)
    except urllib.error.URLError as exc:
        print(f"  FAILED: {exc.reason}", file=sys.stderr)
    except OSError as exc:
        print(f"  FAILED: {exc}", file=sys.stderr)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Publish complete flash packages and OTA releases for all six boards."
    )
    parser.add_argument(
        "boards",
        nargs="*",
        metavar="BOARD",
        help="board(s) to publish; omitted means all six boards",
    )
    parser.add_argument("--version")
    parser.add_argument("--channel", default=os.environ.get("OTA_CHANNEL", "stable"))
    parser.add_argument("--notes", default=os.environ.get("OTA_RELEASE_NOTES", ""))
    args = parser.parse_args()

    unknown_boards = sorted(set(args.boards) - set(BOARDS))
    if unknown_boards:
        parser.error(
            "unknown board(s): "
            + ", ".join(unknown_boards)
            + "; choose from: "
            + ", ".join(BOARDS)
        )

    base = os.environ.get("OTA_SERVER_URL", "").rstrip("/")
    token = os.environ.get("OTA_UPLOAD_TOKEN", "")
    if not base or not token:
        raise SystemExit("OTA_SERVER_URL and OTA_UPLOAD_TOKEN must be set")
    if args.channel not in ("stable", "beta"):
        raise SystemExit("channel must be stable or beta")

    repo = Path(__file__).resolve().parent.parent
    boards = args.boards or list(BOARDS)
    version = (args.version or firmware_version(repo)).removeprefix("v")

    packages: list[tuple[dict, list[tuple[str, Path]]]] = []
    try:
        for board in boards:
            packages.append(
                package_from_build(repo, board, version, args.channel, args.notes)
            )
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid build package: {exc}") from exc

    succeeded = sum(
        publish_package(base, token, meta, files) for meta, files in packages
    )
    print(f"Published {succeeded}/{len(packages)} complete package(s).")
    return 0 if succeeded == len(packages) else 1


if __name__ == "__main__":
    raise SystemExit(main())
