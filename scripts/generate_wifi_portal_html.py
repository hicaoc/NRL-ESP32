from pathlib import Path
import json
import re

include_pattern = re.compile(r"<!--\s*@include\s+([A-Za-z0-9_./-]+)\s*-->")
template_pattern = re.compile(
    r"<!--\s*@template\s+([A-Za-z0-9_]+)\s*-->(.*?)<!--\s*@endtemplate\s*-->",
    re.S,
)

# (source html, generated header, include guard macro, C++ template variable)
PAGES = [
    (
        "wifi_config_portal.html",
        "wifi_config_portal_page.generated.h",
        "SRC_LIB_WIFI_CONFIG_PORTAL_PAGE_GENERATED_H",
        "kWifiConfigPortalHtmlTemplate",
    ),
    (
        "wifi_update_portal.html",
        "wifi_update_portal_page.generated.h",
        "SRC_LIB_WIFI_UPDATE_PORTAL_PAGE_GENERATED_H",
        "kWifiUpdatePortalHtmlTemplate",
    ),
]

# Each management page keeps its section templates in its own small file;
# all of them are combined into one generated header with one C++ variable per
# @template block (e.g. NETWORK_SECTION -> kWifiConfigPortalNetworkSectionTemplate).
# Keep the shared form controls (sliders, number inputs, WiFi option) in
# wifi_config_portal_sections.html.
SECTION_HEADER = (
    "wifi_config_portal_sections.generated.h",
    "SRC_LIB_WIFI_CONFIG_PORTAL_SECTIONS_GENERATED_H",
)
SECTION_SOURCES = [
    "wifi_config_portal_sections.html",
    "wifi_config_portal_wifi.html",
    "wifi_config_portal_nrl.html",
    "wifi_config_portal_serial.html",
    "wifi_config_portal_audio.html",
    "wifi_config_portal_media.html",
    "wifi_config_portal_aprs.html",
    "wifi_config_portal_signaling.html",
]

# Raw asset files served as standalone HTTP responses (text/css, text/javascript).
# Keeping them out of the HTML template keeps each page response small enough to
# go through the WebServer in a single chunked send without truncation, and lets
# the browser cache them across page navigations.
ASSET_FILES = [
    (
        "wifi_portal_parts/wifi_config_portal.css",
        "kWifiConfigPortalCss",
    ),
    (
        "wifi_portal_parts/wifi_config_portal.js",
        "kWifiConfigPortalJs",
    ),
    (
        "wifi_portal_parts/wifi_update_portal.css",
        "kWifiUpdatePortalCss",
    ),
    (
        "wifi_portal_parts/wifi_update_portal.js",
        "kWifiUpdatePortalJs",
    ),
]
ASSET_OUTPUT = (
    "wifi_portal_assets.generated.h",
    "SRC_LIB_WIFI_PORTAL_ASSETS_GENERATED_H",
)


def resolve_project_dir():
    # Under PlatformIO the build env exposes $PROJECT_DIR; when run standalone
    # fall back to the repository root (this script lives in <root>/scripts/).
    try:
        from SCons.Script import Import

        Import("env")
        return Path(env.subst("$PROJECT_DIR"))  # noqa: F821
    except Exception:
        return Path(__file__).resolve().parent.parent


def read_html_with_includes(path, seen=None):
    if seen is None:
        seen = set()
    path = path.resolve()
    if path in seen:
        raise RuntimeError(f"recursive include in {path}")
    seen.add(path)
    html = path.read_text(encoding="utf-8")

    def replace_include(match):
        include_path = (path.parent / match.group(1)).resolve()
        if not include_path.is_file():
            raise FileNotFoundError(f"missing html include: {include_path}")
        return read_html_with_includes(include_path, seen)

    html = include_pattern.sub(replace_include, html)
    seen.remove(path)
    return html


def generate_headers(*args, **kwargs):
    lib_dir = resolve_project_dir() / "src" / "lib"
    for src_name, dst_name, guard, var_name in PAGES:
        html = read_html_with_includes(lib_dir / src_name)
        content = (
            f"#ifndef {guard}\n"
            f"#define {guard}\n\n"
            f"static const char {var_name}[] = "
            + json.dumps(html)
            + ";\n\n"
            "#endif\n"
        )
        (lib_dir / dst_name).write_text(content, encoding="utf-8")
        print(f"[wifi_portal] generated {dst_name} from {src_name} and includes")

    dst_name, guard = SECTION_HEADER
    lines = [f"#ifndef {guard}", f"#define {guard}", ""]
    for src_name in SECTION_SOURCES:
        html = read_html_with_includes(lib_dir / src_name)
        templates = template_pattern.findall(html)
        if not templates:
            raise RuntimeError(f"no @template blocks found in {src_name}")
        for name, body in templates:
            value = body.strip()
            lines.append(f"static const char kWifiConfigPortal{name.title().replace('_', '')}Template[] = " + json.dumps(value) + ";")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    (lib_dir / dst_name).write_text("\n".join(lines), encoding="utf-8")
    print(f"[wifi_portal] generated {dst_name} from {len(SECTION_SOURCES)} section files")

    asset_dst, asset_guard = ASSET_OUTPUT
    asset_lines = [f"#ifndef {asset_guard}", f"#define {asset_guard}", ""]
    for src_rel, var_name in ASSET_FILES:
        body = (lib_dir / src_rel).read_text(encoding="utf-8")
        asset_lines.append(f"static const char {var_name}[] = " + json.dumps(body) + ";")
    asset_lines.append("")
    asset_lines.append("#endif")
    asset_lines.append("")
    (lib_dir / asset_dst).write_text("\n".join(asset_lines), encoding="utf-8")
    print(f"[wifi_portal] generated {asset_dst} from asset parts")


generate_headers()
