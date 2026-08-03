"""Fetch the LVGL graphics library into components/lvgl/ if it is missing.

Behavior:
  * If components/lvgl/lvgl.h already exists and its lv_version.h matches the
    pinned LVGL_VERSION, nothing happens.
  * If components/lvgl exists but has a different version, it is replaced with
    the pinned LVGL_VERSION.
  * Otherwise, downloads the pinned LVGL_VERSION tarball from GitHub with a
    visible byte-by-byte progress line and bounded socket timeouts so the
    build never silently hangs on a stalled connection.
  * On any failure (network unreachable, timeout, extraction error) the
    script aborts the build with a clear manual-fallback message that tells
    the user exactly which URL to download and where to place the files.
  * Lives under components/ (not managed_components/): the IDF component
    manager owns managed_components/ and prunes anything not in its lock
    file, so vendored components must stay under components/.

Environment overrides:
  LVGL_VERSION         Pin a different version (default below).
  LVGL_FETCH_URL       Mirror URL (default: github release tarball).
  LVGL_FETCH_TIMEOUT_S Per-read socket timeout in seconds (default 60).
  LVGL_SKIP_FETCH      Set to 1 to skip download attempt entirely; the build
                       then fails fast if components/lvgl is still absent.

Usage:
  * Auto-invoked by PlatformIO as an extra_script for the gezipai env.
  * Or run standalone to pre-fetch:  python scripts/fetch_lvgl.py
"""

from __future__ import annotations

import os
import shutil
import socket
import sys
import tarfile
import tempfile
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import urlopen

DEFAULT_VERSION = "9.5.0"
LVGL_VERSION = os.environ.get("LVGL_VERSION", DEFAULT_VERSION)
DEFAULT_URL = f"https://github.com/lvgl/lvgl/archive/refs/tags/v{LVGL_VERSION}.tar.gz"
LVGL_URL = os.environ.get("LVGL_FETCH_URL", DEFAULT_URL)
TIMEOUT_S = float(os.environ.get("LVGL_FETCH_TIMEOUT_S", "60"))
SKIP_FETCH = os.environ.get("LVGL_SKIP_FETCH", "0") == "1"


def _resolve_project_dir() -> Path:
    # Standalone invocation: __file__ is set, parent.parent of scripts/ is the
    # project root.
    try:
        return Path(__file__).resolve().parent.parent
    except NameError:
        pass
    # PlatformIO extra_script invocation: SCons runs us via exec() so __file__
    # is missing, but the env exposes $PROJECT_DIR. Import("env") injects it
    # into our module globals dict, so we read it back from there to keep the
    # static checker happy (it can't see the runtime injection).
    try:
        from SCons.Script import Import  # type: ignore[import-not-found]

        Import("env")
        env_obj = globals().get("env")
        if env_obj is not None:
            return Path(env_obj.subst("$PROJECT_DIR"))
    except Exception:
        pass
    # Last-resort fallback: cwd, which is the project dir under both pio and
    # interactive shell invocations.
    return Path.cwd()


PROJECT_DIR = _resolve_project_dir()
DEST_DIR = PROJECT_DIR / "components" / "lvgl"
MARKER_FILE = DEST_DIR / "lvgl.h"
VERSION_FILE = DEST_DIR / "lv_version.h"


def _log(msg: str) -> None:
    print(f"[lvgl] {msg}", flush=True)


def _is_installed() -> bool:
    return MARKER_FILE.is_file()


def _installed_version() -> str | None:
    if not VERSION_FILE.is_file():
        return None
    try:
        text = VERSION_FILE.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None

    values: dict[str, str] = {}
    for line in text.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[0] == "#define":
            values[parts[1]] = parts[2]
    major = values.get("LVGL_VERSION_MAJOR")
    minor = values.get("LVGL_VERSION_MINOR")
    patch = values.get("LVGL_VERSION_PATCH")
    if major is None or minor is None or patch is None:
        return None
    return f"{major}.{minor}.{patch}"


def _download(url: str, dest: Path) -> None:
    _log(f"downloading {url}")
    _log(f"timeout per read: {TIMEOUT_S:.0f}s (override with LVGL_FETCH_TIMEOUT_S)")
    with urlopen(url, timeout=TIMEOUT_S) as resp:
        total = int(resp.getheader("Content-Length") or 0)
        block = 64 * 1024
        received = 0
        last_print = time.monotonic()
        with open(dest, "wb") as f:
            while True:
                chunk = resp.read(block)
                if not chunk:
                    break
                f.write(chunk)
                received += len(chunk)
                now = time.monotonic()
                if now - last_print > 0.5 or received == total:
                    if total:
                        pct = 100.0 * received / total
                        _log(f"  {received/1024:7.0f} KiB / {total/1024:.0f} KiB ({pct:5.1f}%)")
                    else:
                        _log(f"  {received/1024:7.0f} KiB downloaded")
                    last_print = now


def _extract(tarball: Path, dest: Path) -> None:
    # Stage into a sibling dir then rename, so a half-extracted tree never
    # ends up in components/lvgl/ on failure.
    staging = dest.parent / f"{dest.name}.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    try:
        with tarfile.open(tarball, "r:gz") as tar:
            # filter="data" became the recommended default in Python 3.12+
            # (strips device files / setuid bits / etc.). Pass it explicitly
            # to silence the DeprecationWarning that lands in 3.14.
            if sys.version_info >= (3, 12):
                tar.extractall(staging, filter="data")
            else:
                tar.extractall(staging)
        entries = [p for p in staging.iterdir() if not p.name.startswith(".")]
        if len(entries) == 1 and entries[0].is_dir():
            entries[0].rename(dest)
            staging.rmdir()
        else:
            staging.rename(dest)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def _print_manual_fallback(reason: str) -> None:
    sep = "=" * 72
    print(sep)
    print(f"[lvgl] AUTO-DOWNLOAD FAILED: {reason}")
    print(sep)
    print("Manual fallback (do this once, then re-run the build):")
    print()
    print(f"  1. Download the tarball from:")
    print(f"     {LVGL_URL}")
    print()
    print(f"  2. Extract it and rename the inner folder so the result is:")
    print(f"     {DEST_DIR}{os.sep}lvgl.h")
    print(f"     {DEST_DIR}{os.sep}src{os.sep}...")
    print()
    print("  3. (Optional) If the GitHub URL is unreachable, set a mirror:")
    print("       export LVGL_FETCH_URL=<your mirror URL>")
    print("     or skip the auto-fetch entirely:")
    print("       export LVGL_SKIP_FETCH=1")
    print()
    print("  4. Re-run the build.")
    print(sep)


def fetch_lvgl() -> None:
    if _is_installed():
        installed = _installed_version()
        if installed == LVGL_VERSION:
            _log(f"already installed at {DEST_DIR} (v{installed}), skipping download")
            return
        _log(f"installed LVGL is v{installed or 'unknown'}, replacing with v{LVGL_VERSION}")
        shutil.rmtree(DEST_DIR)

    if SKIP_FETCH:
        _print_manual_fallback("LVGL_SKIP_FETCH=1, refused to auto-download")
        sys.exit(1)

    _log(f"{DEST_DIR} missing, fetching LVGL v{LVGL_VERSION}")
    DEST_DIR.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as tmp:
        tarball = Path(tmp.name)
    try:
        try:
            _download(LVGL_URL, tarball)
        except (URLError, TimeoutError, socket.timeout, ConnectionError) as e:
            _print_manual_fallback(f"network error: {e}")
            sys.exit(1)
        except Exception as e:
            _print_manual_fallback(f"download error: {type(e).__name__}: {e}")
            sys.exit(1)

        try:
            _extract(tarball, DEST_DIR)
        except Exception as e:
            _print_manual_fallback(f"extract error: {type(e).__name__}: {e}")
            sys.exit(1)

        if not _is_installed():
            _print_manual_fallback("tarball extracted but lvgl.h not found at expected path")
            sys.exit(1)

        _log(f"installed at {DEST_DIR}")
    finally:
        try:
            tarball.unlink()
        except FileNotFoundError:
            pass


# PlatformIO extra_script entry point: when SCons exec()s us during `pio run`
# there is no __name__ == "__main__" and no __file__, but importlib can find
# the SCons package. Standalone python invocation goes through the __main__
# branch and never sees SCons.
import importlib.util

try:
    _running_under_scons = importlib.util.find_spec("SCons.Script") is not None
except (ImportError, ModuleNotFoundError, ValueError):
    _running_under_scons = False

if _running_under_scons or __name__ == "__main__":
    fetch_lvgl()
