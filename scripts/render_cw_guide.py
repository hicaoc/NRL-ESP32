#!/usr/bin/env python3
"""Render docs/cw_guide.md to docs/cw_guide.html with the rhythm trainer.

Reuses render_readme_html.py's Markdown renderer, then injects the
interactive trainer fragment (docs/cw_sim.html) at the <!-- CW_SIM --> marker.

Usage: python scripts/render_cw_guide.py
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKER = "<!-- CW_SIM -->"


def main() -> None:
    spec = importlib.util.spec_from_file_location(
        "render_readme_html", ROOT / "scripts" / "render_readme_html.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    output = ROOT / "docs" / "cw_guide.html"
    module.render(
        ROOT / "docs" / "cw_guide.md",
        output,
        "zh-CN",
        "CW 莫尔斯电码功能使用说明 · NRL ESP32",
    )

    page = output.read_text(encoding="utf-8")
    fragment = (ROOT / "docs" / "cw_sim.html").read_text(encoding="utf-8")
    if MARKER not in page:
        raise SystemExit(f"marker {MARKER!r} missing after render")
    output.write_text(page.replace(MARKER, fragment), encoding="utf-8", newline="\n")
    print(f"rendered {output}")


if __name__ == "__main__":
    main()
