#!/usr/bin/env python3
"""Download map tiles for offline use on the NRL device (TF card).

Fetches z/x/y tiles from a Slippy-Map source, converts them to JPEG (the
device decodes JPEG via esp_new_jpeg; no PNG decoder on most boards), and
writes the TF-card layout:

    <out>/<z>/<x>/<y>.jpg

Copy the resulting <out> directory to the card as /tiles (i.e. the device
looks for <sd>/tiles/<z>/<x>/<y>.jpg).

Examples:
    # 以某个台站坐标为中心，半径 20 km，缩放 10-15 级
    python scripts/fetch_tiles.py --lat 30.66 --lon 104.06 --radius-km 20 --zoom 10-15 --out E:/tiles

    # 指定矩形范围
    python scripts/fetch_tiles.py --bbox 103.9,30.5,104.2,30.8 --zoom 12-14 --out E:/tiles

Needs: pip install pillow requests
OSM etiquette: keep zoom ranges and areas modest, do not hammer the server.
"""

from __future__ import annotations

import argparse
import io
import math
import sys
import time
from pathlib import Path

import requests
from PIL import Image

USER_AGENT = "NRL-ESP32-tile-fetcher/1.0 (https://github.com/hicaoc/NRL-ESP32)"
DEFAULT_SOURCE = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"


def lonlat_to_tile(lon: float, lat: float, z: int) -> tuple[int, int]:
    lat = max(-85.05112878, min(85.05112878, lat))
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def tiles_for_bbox(min_lon: float, min_lat: float, max_lon: float, max_lat: float, z: int):
    x0, y1 = lonlat_to_tile(min_lon, min_lat, z)  # SW corner
    x1, y0 = lonlat_to_tile(max_lon, max_lat, z)  # NE corner
    for x in range(min(x0, x1), max(x0, x1) + 1):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            yield z, x, y


def bbox_from_center(lat: float, lon: float, radius_km: float) -> tuple[float, float, float, float]:
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * max(math.cos(math.radians(lat)), 0.01))
    return lon - dlon, lat - dlat, lon + dlon, lat + dlat


def parse_zoom(spec: str) -> list[int]:
    if "-" in spec:
        a, b = spec.split("-", 1)
        return list(range(int(a), int(b) + 1))
    return [int(spec)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float)
    ap.add_argument("--lon", type=float)
    ap.add_argument("--radius-km", type=float, default=10.0)
    ap.add_argument("--bbox", help="minLon,minLat,maxLon,maxLat")
    ap.add_argument("--zoom", default="12-14", help="z or z1-z2 (default 12-14)")
    ap.add_argument("--out", required=True, help="output directory (the card's /tiles)")
    ap.add_argument("--source", default=DEFAULT_SOURCE,
                    help="URL template with {z}/{x}/{y} (default OSM; result must be PNG or JPEG)")
    ap.add_argument("--quality", type=int, default=85, help="JPEG quality (default 85)")
    ap.add_argument("--delay", type=float, default=0.15, help="seconds between downloads")
    args = ap.parse_args()

    if args.bbox:
        min_lon, min_lat, max_lon, max_lat = (float(v) for v in args.bbox.split(","))
    elif args.lat is not None and args.lon is not None:
        min_lon, min_lat, max_lon, max_lat = bbox_from_center(args.lat, args.lon, args.radius_km)
    else:
        ap.error("give --bbox or both --lat and --lon")

    zooms = parse_zoom(args.zoom)
    out_root = Path(args.out)
    todo: list[tuple[int, int, int]] = []
    for z in zooms:
        for zxy in tiles_for_bbox(min_lon, min_lat, max_lon, max_lat, z):
            z2, x, y = zxy
            if not (out_root / str(z2) / str(x) / f"{y}.jpg").is_file():
                todo.append(zxy)
    total = len(todo)
    if total == 0:
        print("all tiles already present, nothing to do")
        return 0
    print(f"{total} tiles to fetch -> {out_root}")

    session = requests.Session()
    session.headers["User-Agent"] = USER_AGENT
    done = failed = 0
    for z, x, y in todo:
        path = out_root / str(z) / str(x) / f"{y}.jpg"
        try:
            r = session.get(args.source.format(z=z, x=x, y=y), timeout=30)
            r.raise_for_status()
            img = Image.open(io.BytesIO(r.content)).convert("RGB")
            if img.size != (256, 256):
                img = img.resize((256, 256), Image.LANCZOS)
            path.parent.mkdir(parents=True, exist_ok=True)
            img.save(path, "JPEG", quality=args.quality)
            done += 1
        except Exception as exc:  # noqa: BLE001 - keep going, report at end
            failed += 1
            print(f"  FAIL {z}/{x}/{y}: {exc}", file=sys.stderr)
        if (done + failed) % 20 == 0:
            print(f"  {done + failed}/{total} ({failed} failed)")
        time.sleep(args.delay)
    print(f"done: {done} ok, {failed} failed -> {out_root}")
    print("copy the directory to the TF card as /tiles")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
