#!/usr/bin/env python3
"""Native ESP-IDF build helper for all NRL boards (replaces PlatformIO).

Run it from an ESP-IDF 6.2 environment (i.e. after sourcing export.ps1 /
export.sh so `idf.py` is on PATH):

    python scripts/build.py <board> [idf.py args...]

Examples:
    python scripts/build.py gezipai build
    python scripts/build.py bi4umd build
    python scripts/build.py gezipai_4g build
    python scripts/build.py s31_korvo flash monitor -p COM5
    python scripts/build.py s31_function_coreboard flash monitor -p COM5
    
    python scripts/build.py bh4tdv fullclean
    python scripts/build.py gezipai menuconfig

Boards: gezipai, bi4umd, gezipai_4g, bh4tdv, s31_korvo,
s31_function_coreboard. If you omit the idf.py args it defaults to `build`.

Each board gets its own build directory (build/<board>) and sdkconfig
(build/<board>/sdkconfig), so you can switch boards without a reconfigure or
clobbering another board's config. This mirrors exactly what the GitHub CI does
(.github/workflows/firmware.yml) and what CMakeLists.txt's NRL_NATIVE_BUILD
block expects.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# board -> (IDF target, NRL_BOARD macro, board sdkconfig defaults, needs LVGL,
#           extra -D args). Keep in sync with the CI matrix in
#           .github/workflows/firmware.yml.
BOARDS = {
    "gezipai": dict(
        target="esp32s3",
        macro="NRL_BOARD_GEZIPAI",
        sdkconfig=["sdkconfig.gezipai.defaults"],
        lvgl=True,
        extra=[],
    ),
    "bi4umd": dict(
        target="esp32s3",
        macro="NRL_BOARD_BI4UMD",
        sdkconfig=["sdkconfig.bi4umd.defaults"],
        lvgl=True,
        extra=[],
    ),
    "gezipai_4g": dict(
        target="esp32s3",
        macro="NRL_BOARD_GEZIPAI_4G",
        sdkconfig=["sdkconfig.gezipai.defaults"],
        lvgl=True,
        extra=[],
    ),
    "bh4tdv": dict(
        target="esp32s3",
        macro="NRL_BOARD_BH4TDV",
        sdkconfig=["sdkconfig.bh4tdv.defaults"],
        lvgl=False,
        extra=[],
    ),
    "s31_korvo": dict(
        target="esp32s31",
        macro="NRL_BOARD_S31_KORVO",
        sdkconfig=["sdkconfig.s31_korvo.defaults"],
        lvgl=True,
        extra=["-DNRL_EXTRA_COMPONENT_DIRS=vendor/esp32_s31_korvo"],
    ),
    "s31_function_coreboard": dict(
        target="esp32s31",
        macro="NRL_BOARD_S31_FUNCTION_COREBOARD",
        sdkconfig=["sdkconfig.s31_function_coreboard.defaults"],
        lvgl=False,
        extra=[],
    ),
}


def idf_python() -> Path:
    """Return a Python interpreter containing ESP-IDF's dependencies."""
    configured = os.environ.get("IDF_PYTHON_ENV_PATH")
    if configured:
        candidate = Path(configured) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if candidate.is_file():
            return candidate

    probe = subprocess.run(
        [sys.executable, "-c", "import click"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode == 0:
        return Path(sys.executable)

    env_root = Path.home() / ".espressif" / "python_env"
    executable = "Scripts/python.exe" if os.name == "nt" else "bin/python"
    candidates = sorted(env_root.glob(f"idf*_env/{executable}"), reverse=True)
    if candidates:
        return candidates[0]
    return Path(sys.executable)


def idf_environment(idf_path: Path, python: Path) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("IDF_PYTHON_ENV_PATH", str(python.parent.parent))
    if not env.get("ESP_IDF_VERSION"):
        version_file = idf_path / "tools" / "cmake" / "version.cmake"
        if version_file.is_file():
            text = version_file.read_text(encoding="utf-8")
            values = dict(re.findall(r"set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\)", text))
            if all(part in values for part in ("MAJOR", "MINOR", "PATCH")):
                env["ESP_IDF_VERSION"] = ".".join(
                    values[part] for part in ("MAJOR", "MINOR", "PATCH")
                )
    return env


def firmware_version() -> str:
    text = (REPO / "src" / "lib" / "nrl_version.h").read_text(encoding="utf-8")
    match = re.search(r'#define\s+NRL_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise RuntimeError("NRL_FIRMWARE_VERSION is missing from src/lib/nrl_version.h")
    return match.group(1)


def patch_managed_components():
    """Apply tiny local Kconfig cleanups to downloaded managed components.

    These files live under managed_components/ and are not committed here. The
    component manager may recreate them, so keep the patch idempotent and cheap.
    """
    esp_sr_kconfig = REPO / "managed_components" / "espressif__esp-sr" / "Kconfig.projbuild"
    if esp_sr_kconfig.exists():
        text = esp_sr_kconfig.read_text(encoding="utf-8")
        patched = text.replace("default False", "default n")
        if patched != text:
            esp_sr_kconfig.write_text(patched, encoding="utf-8", newline="")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in BOARDS:
        print(__doc__)
        print(f"error: first arg must be one of: {', '.join(BOARDS)}")
        sys.exit(2)

    board = sys.argv[1]
    passthrough = sys.argv[2:] or ["build"]
    cfg = BOARDS[board]

    if cfg["lvgl"]:
        subprocess.run(
            [sys.executable, str(REPO / "scripts" / "fetch_lvgl.py")], check=True
        )
    patch_managed_components()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        print("error: IDF_PATH is not set. Source the ESP-IDF 6.2 environment "
              "first (export.ps1 / export.sh), then re-run.")
        sys.exit(2)
    idf_root = Path(idf_path)
    idf_py = idf_root / "tools" / "idf.py"
    python = idf_python()
    build_env = idf_environment(idf_root, python)

    # Optional isolated build directory for clean verification while a serial
    # monitor still holds build/<board>/log or ELF files open on Windows.
    build_dir = os.environ.get("NRL_BUILD_DIR", f"build/{board}")
    # Invoke idf.py through the current Python with shell=False: this resolves
    # idf.py cross-platform without relying on a PATH wrapper, and keeps the
    # semicolon-separated SDKCONFIG_DEFAULTS list intact (a Windows shell would
    # otherwise split it on ';').
    cmd = [
        str(python), str(idf_py),
        "-B", build_dir,
        "-DIDF_TARGET=" + cfg["target"],
        "-DNRL_NATIVE_BUILD=1",
        "-DNRL_BOARD_ID=" + cfg["macro"],
        "-DNRL_FW_VERSION=" + firmware_version(),
        "-DSDKCONFIG=" + build_dir + "/sdkconfig",
        "-DSDKCONFIG_DEFAULTS=" + ";".join(cfg["sdkconfig"]),
        *cfg["extra"],
        *passthrough,
    ]
    print("+ " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, cwd=REPO, env=build_env)
    # Publishing is opt-in: CI/developer environments that define both server URL
    # and upload token publish every successful firmware build. This keeps normal
    # local builds offline while making the release pipeline one command. The
    # whole flash package (bootloader + partition table + app + …) is uploaded;
    # the external OTA server registers the app slice as the OTA release and uses
    # the same package for its browser-based USB flashing service.
    if (result.returncode == 0 and "build" in passthrough and
            os.environ.get("OTA_SERVER_URL") and os.environ.get("OTA_UPLOAD_TOKEN")):
        publish = [sys.executable, str(REPO / "scripts" / "publish_ota.py"), board]
        print("+ " + " ".join(publish), flush=True)
        result = subprocess.run(publish, cwd=REPO)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
