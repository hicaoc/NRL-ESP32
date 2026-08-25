#!/usr/bin/env python3
"""Publish complete NRL firmware packages through the OTA MCP endpoint.

The server stages each package behind a one-time upload token. This client
uploads the binary parts over multipart HTTP, verifies the staged result, and
then explicitly calls firmware.publish. Existing releases are treated as
idempotent only when their application size and SHA-256 match the local build.

Required environment:
  OTA_SERVER_URL   Public OTA API root, for example
                   https://ota.nrlptt.com/nrlota/api
  OTA_ADMIN_TOKEN  Administrator Bearer token (OTA_UPLOAD_TOKEN is accepted as
                   a compatibility fallback).

Optional environment: OTA_VERSION, OTA_CHANNEL, OTA_RELEASE_NOTES.
When this script is run from NRL-OTA, set NRL_FIRMWARE_REPO or pass
--firmware-repo unless an adjacent NRL-ESP32 checkout can be auto-detected.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

MCP_PROTOCOL_VERSION = "2025-11-25"
BOARDS = ("gezipai", "gezipai_4g", "bi4umd", "bh4tdv", "bh4tdv_rf", "s31_korvo", "s31_function_coreboard")
DEFAULT_PUBLISH_BOARDS = (
    "gezipai",
    "gezipai_4g",
    "bi4umd",
    "bh4tdv",
    "bh4tdv_rf",
    "s31_korvo",
    "s31_function_coreboard",
)
REQUIRED_TOOLS = {
    "firmware.create_upload",
    "firmware.get_status",
    "firmware.list",
    "firmware.publish",
}


class MCPError(RuntimeError):
    """An HTTP, protocol, or OTA tool error."""


class MCPClient:
    def __init__(self, server: str, token: str) -> None:
        server = server.rstrip("/")
        self.url = server if server.endswith("/mcp") else server + "/mcp"
        parsed = urllib.parse.urlsplit(self.url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc:
            raise ValueError("OTA_SERVER_URL must be an HTTP or HTTPS URL")
        self.origin = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))
        self.token = token
        self.request_id = 0

    def _request(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        self.request_id += 1
        payload = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": self.request_id,
                "method": method,
                "params": params,
            },
            separators=(",", ":"),
        ).encode("utf-8")
        request = urllib.request.Request(
            self.url,
            data=payload,
            headers={
                "Authorization": "Bearer " + self.token,
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
                "MCP-Protocol-Version": MCP_PROTOCOL_VERSION,
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                body = response.read()
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise MCPError(f"MCP HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise MCPError(f"MCP connection failed: {exc.reason}") from exc
        try:
            result = json.loads(body)
        except json.JSONDecodeError as exc:
            raise MCPError("MCP returned invalid JSON") from exc
        if "error" in result:
            raise MCPError(f"MCP error: {json.dumps(result['error'], ensure_ascii=False)}")
        if "result" not in result:
            raise MCPError("MCP returned no result")
        return result["result"]

    def list_tools(self) -> set[str]:
        result = self._request("tools/list", {})
        return {str(tool.get("name", "")) for tool in result.get("tools", [])}

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        result = self._request(
            "tools/call", {"name": name, "arguments": arguments}
        )
        if result.get("isError"):
            messages = [
                str(item.get("text", ""))
                for item in result.get("content", [])
                if item.get("type") == "text"
            ]
            raise MCPError(f"{name} failed: {'; '.join(messages) or 'unknown error'}")
        structured = result.get("structuredContent")
        if isinstance(structured, dict):
            return structured
        for item in result.get("content", []):
            if item.get("type") == "text":
                try:
                    decoded = json.loads(item.get("text", ""))
                except json.JSONDecodeError:
                    continue
                if isinstance(decoded, dict):
                    return decoded
        raise MCPError(f"{name} returned no structured result")

    def upload_url(self, path: str) -> str:
        parsed = urllib.parse.urlsplit(path)
        if parsed.scheme in ("http", "https") and parsed.netloc:
            return path
        if path.startswith("/"):
            return self.origin + path
        return urllib.parse.urljoin(self.url + "/", path)


def resolve_firmware_repo(configured: str) -> Path:
    script_repo = Path(__file__).resolve().parent.parent
    candidates: list[Path] = []
    if configured:
        candidates.append(Path(configured).expanduser())
    else:
        candidates.extend(
            [
                script_repo,
                script_repo.parent / "NRL-ESP32",
                script_repo.parent / "nrl-esp32",
            ]
        )
    for candidate in candidates:
        candidate = candidate.resolve()
        if (
            (candidate / "scripts" / "publish_ota.py").is_file()
            and (candidate / "src" / "lib" / "nrl_version.h").is_file()
        ):
            return candidate
    if configured:
        raise ValueError(f"not an NRL-ESP32 checkout: {configured}")
    raise ValueError(
        "NRL-ESP32 checkout not found; pass --firmware-repo or set "
        "NRL_FIRMWARE_REPO"
    )


def load_package_helpers(repo: Path) -> Any:
    source = repo / "scripts" / "publish_ota.py"
    spec = importlib.util.spec_from_file_location("_nrl_publish_ota", source)
    if spec is None or spec.loader is None:
        raise ValueError(f"could not load package helpers from {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def application_identity(
    meta: dict[str, Any], files: list[tuple[str, Path]]
) -> tuple[int, str]:
    app_name = next(
        (
            str(part["name"])
            for part in meta["parts"]
            if int(part["offset"]) == int(meta["app_offset"])
        ),
        "",
    )
    app_path = next((path for name, path in files if name == app_name), None)
    if app_path is None:
        raise ValueError("application image is missing from the flash package")
    digest = hashlib.sha256()
    with app_path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return app_path.stat().st_size, digest.hexdigest()


def existing_release(
    client: MCPClient,
    board: str,
    version: str,
    channel: str,
) -> dict[str, Any] | None:
    result = client.call_tool(
        "firmware.list",
        {"board": board, "channel": channel, "include_archived": False},
    )
    return next(
        (
            release
            for release in (result.get("releases") or [])
            if release.get("version") == version
            and release.get("channel") == channel
        ),
        None,
    )


def upload_package(
    client: MCPClient,
    upload: dict[str, Any],
    meta: dict[str, Any],
    files: list[tuple[str, Path]],
    package_helpers: Any,
) -> None:
    body, boundary = package_helpers.build_multipart(meta, files)
    request = urllib.request.Request(
        client.upload_url(str(upload["upload_path"])),
        data=body,
        headers={
            "Authorization": "Bearer " + str(upload["upload_token"]),
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise MCPError(f"firmware upload HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise MCPError(f"firmware upload failed: {exc.reason}") from exc


def publish_board(
    client: MCPClient,
    repo: Path,
    board: str,
    version: str,
    channel: str,
    notes: str,
    ttl_minutes: int,
    verify_only: bool,
    package_helpers: Any,
) -> bool:
    meta, files = package_helpers.package_from_build(
        repo, board, version, channel, notes
    )
    app_size, app_sha256 = application_identity(meta, files)
    current = existing_release(client, board, version, channel)
    if current is not None:
        matches = (
            int(current.get("size", -1)) == app_size
            and str(current.get("sha256", "")).lower() == app_sha256
            and "/packages/" in str(current.get("url", ""))
        )
        if not matches:
            raise MCPError(
                f"{board} {version} already exists with a different or incomplete package"
            )
        print(
            f"{board} {version}: already published and verified "
            f"({app_size} bytes, sha256={app_sha256})"
        )
        return True
    if verify_only:
        raise MCPError(f"{board} {version} is not published")

    print(f"{board} {version}: creating reviewed MCP upload ({len(files)} parts)...")
    upload = client.call_tool(
        "firmware.create_upload",
        {
            "board": board,
            "version": version,
            "channel": channel,
            "notes": notes,
            "ttl_minutes": ttl_minutes,
        },
    )
    upload_package(client, upload, meta, files, package_helpers)
    status = client.call_tool(
        "firmware.get_status", {"upload_id": str(upload["upload_id"])}
    )
    if status.get("upload", {}).get("status") != "uploaded":
        raise MCPError(f"{board} staged upload did not reach uploaded status")
    published = client.call_tool(
        "firmware.publish",
        {"upload_id": str(upload["upload_id"]), "confirm": True},
    )
    if (
        published.get("status") != "published"
        or int(published.get("size", -1)) != app_size
        or str(published.get("sha256", "")).lower() != app_sha256
    ):
        raise MCPError(f"{board} published result failed local hash verification")
    print(
        f"{board} {version}: published {published.get('part_count')} parts "
        f"({app_size} bytes, sha256={app_sha256})"
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Stage, verify, and publish complete firmware packages via OTA MCP."
    )
    parser.add_argument(
        "boards",
        nargs="*",
        choices=BOARDS,
        metavar="BOARD",
        help="board(s) to publish; omitted means released boards only",
    )
    parser.add_argument("--server", default=os.environ.get("OTA_SERVER_URL", ""))
    parser.add_argument(
        "--firmware-repo",
        default=os.environ.get("NRL_FIRMWARE_REPO", ""),
        help="NRL-ESP32 checkout containing build/<board> (auto-detected when adjacent)",
    )
    parser.add_argument("--version")
    parser.add_argument("--channel", default=os.environ.get("OTA_CHANNEL", "stable"))
    parser.add_argument("--notes", default=os.environ.get("OTA_RELEASE_NOTES", ""))
    parser.add_argument("--ttl-minutes", type=int, default=30)
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="verify matching published packages without creating uploads",
    )
    args = parser.parse_args()

    token = os.environ.get("OTA_ADMIN_TOKEN") or os.environ.get("OTA_UPLOAD_TOKEN", "")
    if not args.server or not token:
        raise SystemExit(
            "OTA_SERVER_URL and OTA_ADMIN_TOKEN (or OTA_UPLOAD_TOKEN) must be set"
        )
    if args.channel not in ("stable", "beta"):
        raise SystemExit("channel must be stable or beta")
    if not 5 <= args.ttl_minutes <= 120:
        raise SystemExit("ttl-minutes must be between 5 and 120")

    try:
        repo = resolve_firmware_repo(args.firmware_repo)
        package_helpers = load_package_helpers(repo)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    version = (args.version or package_helpers.firmware_version(repo)).removeprefix("v")
    boards = args.boards or list(DEFAULT_PUBLISH_BOARDS)
    try:
        client = MCPClient(args.server, token)
        missing = REQUIRED_TOOLS - client.list_tools()
        if missing:
            raise MCPError("OTA MCP is missing tools: " + ", ".join(sorted(missing)))
    except (MCPError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    succeeded = 0
    for board in boards:
        try:
            succeeded += int(
                publish_board(
                    client,
                    repo,
                    board,
                    version,
                    args.channel,
                    args.notes,
                    args.ttl_minutes,
                    args.verify_only,
                    package_helpers,
                )
            )
        except (KeyError, TypeError, ValueError, json.JSONDecodeError, MCPError) as exc:
            print(f"{board} {version}: FAILED: {exc}", file=sys.stderr)
    action = "Verified" if args.verify_only else "Published"
    print(f"{action} {succeeded}/{len(boards)} complete package(s) via MCP.")
    return 0 if succeeded == len(boards) else 1


if __name__ == "__main__":
    raise SystemExit(main())
