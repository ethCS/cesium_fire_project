#!/usr/bin/env python3
"""
firesimulation2 wildfire ETL

Reads MTBS ZIP archives directly and produces UE-friendly generated data:
  - catalog/events.ndjson              (attributes only, no geometry)
  - index/year_state_day_index.json    (Year -> State -> Day -> event ids)
  - geometry/<year>.ndjson             (event geometry by year)
  - rasters/yearly_raster_manifest.json
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import re
import zipfile
from collections import defaultdict
from io import BytesIO
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple
from xml.etree import ElementTree as ET

import shapefile  # pyshp


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--simplify-tolerance-deg", type=float, default=0.002)
    return parser.parse_args()


def as_float(value: Any, default: float = 0.0) -> float:
    if value is None:
        return default
    try:
        if isinstance(value, str):
            value = value.strip()
            if value == "":
                return default
        return float(value)
    except Exception:
        return default


def as_str(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    return str(value)


def parse_date_iso(value: str) -> Optional[dt.date]:
    value = as_str(value)
    if not value:
        return None
    try:
        return dt.datetime.strptime(value[:10], "%Y-%m-%d").date()
    except ValueError:
        return None


def point_line_distance(point: Tuple[float, float], start: Tuple[float, float], end: Tuple[float, float]) -> float:
    (x, y), (x1, y1), (x2, y2) = point, start, end
    dx = x2 - x1
    dy = y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(x - x1, y - y1)
    t = ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    px = x1 + t * dx
    py = y1 + t * dy
    return math.hypot(x - px, y - py)


def douglas_peucker(points: List[Tuple[float, float]], tolerance: float) -> List[Tuple[float, float]]:
    if len(points) <= 2:
        return points

    start = points[0]
    end = points[-1]
    max_dist = -1.0
    idx = -1
    for i in range(1, len(points) - 1):
        dist = point_line_distance(points[i], start, end)
        if dist > max_dist:
            max_dist = dist
            idx = i

    if max_dist > tolerance and idx != -1:
        left = douglas_peucker(points[: idx + 1], tolerance)
        right = douglas_peucker(points[idx:], tolerance)
        return left[:-1] + right
    return [start, end]


def simplify_ring(ring: List[Tuple[float, float]], tolerance: float) -> List[Tuple[float, float]]:
    if len(ring) <= 8 or tolerance <= 0.0:
        return ring
    closed = ring[0] == ring[-1]
    core = ring[:-1] if closed else ring
    simplified = douglas_peucker(core, tolerance)
    if len(simplified) < 3:
        simplified = core
    if simplified[0] != simplified[-1]:
        simplified.append(simplified[0])
    if len(simplified) < 4:
        return ring
    return simplified


def load_shapefile_from_zip(zip_path: Path) -> Iterable[Tuple[Dict[str, Any], shapefile.Shape]]:
    with zipfile.ZipFile(zip_path, "r") as zf:
        names = zf.namelist()
        shp_name = next((n for n in names if n.lower().endswith(".shp")), None)
        shx_name = next((n for n in names if n.lower().endswith(".shx")), None)
        dbf_name = next((n for n in names if n.lower().endswith(".dbf")), None)
        if not shp_name or not shx_name or not dbf_name:
            raise RuntimeError(f"Missing shapefile parts in {zip_path}")

        shp = BytesIO(zf.read(shp_name))
        shx = BytesIO(zf.read(shx_name))
        dbf = BytesIO(zf.read(dbf_name))

        reader = shapefile.Reader(shp=shp, shx=shx, dbf=dbf, encoding="utf-8")
        fields = [f[0] for f in reader.fields[1:]]
        for shape_record in reader.iterShapeRecords():
            values = shape_record.record.as_dict() if hasattr(shape_record.record, "as_dict") else dict(zip(fields, shape_record.record))
            record = {k.lower(): v for k, v in values.items()}
            yield record, shape_record.shape


def shape_to_rings(shape: shapefile.Shape, tolerance: float) -> List[List[List[float]]]:
    if shape.shapeType not in {5, 15, 25}:  # polygon variants
        return []

    points = shape.points
    if not points:
        return []

    part_starts = list(shape.parts) + [len(points)]
    rings: List[List[List[float]]] = []
    for i in range(len(part_starts) - 1):
        start = part_starts[i]
        end = part_starts[i + 1]
        raw_ring = [(float(p[0]), float(p[1])) for p in points[start:end]]
        if len(raw_ring) < 3:
            continue
        if raw_ring[0] != raw_ring[-1]:
            raw_ring.append(raw_ring[0])
        raw_ring = simplify_ring(raw_ring, tolerance)
        if len(raw_ring) < 4:
            continue
        rings.append([[lon, lat] for lon, lat in raw_ring])
    return rings


def parse_xml_from_zip(zip_path: Path) -> Optional[Dict[str, Any]]:
    with zipfile.ZipFile(zip_path, "r") as zf:
        xml_name = next((n for n in zf.namelist() if n.lower().endswith(".xml")), None)
        if not xml_name:
            return None
        xml_text = zf.read(xml_name).decode("utf-8", errors="ignore")
    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError:
        return None

    def get_text(path: str) -> str:
        node = root.find(path)
        return node.text.strip() if node is not None and node.text else ""

    return {
        "title": get_text("./idinfo/citation/citeinfo/title"),
        "doi": next((n.text.strip() for n in root.findall("./idinfo/citation/citeinfo/onlink") if n.text and "doi.org" in n.text), ""),
        "row_count": get_text("./spdoinfo/rastinfo/rowcount"),
        "col_count": get_text("./spdoinfo/rastinfo/colcount"),
        "cell_size_m": get_text("./spref/horizsys/planar/planci/coordrep/absres"),
        "projection": "Albers Equal Area Conic (NAD83)",
        "classes": {
            "0": "Background/No Data",
            "1": "Unburned/Underburned to Low Burn Severity",
            "2": "Low burn severity",
            "3": "Moderate burn severity",
            "4": "High Burn Severity",
            "5": "Increased Greenness/Increased Vegetation Response",
            "6": "Non-Processing Area Mask",
        },
    }


def main() -> None:
    args = parse_args()
    input_dir: Path = args.input_dir
    output_dir: Path = args.output_dir
    simplify_tol: float = args.simplify_tolerance_deg

    output_catalog = output_dir / "catalog"
    output_index = output_dir / "index"
    output_geometry = output_dir / "geometry"
    output_rasters = output_dir / "rasters"
    output_catalog.mkdir(parents=True, exist_ok=True)
    output_index.mkdir(parents=True, exist_ok=True)
    output_geometry.mkdir(parents=True, exist_ok=True)
    output_rasters.mkdir(parents=True, exist_ok=True)

    # Supplemental burn-severity attributes by event id.
    bsp_attrs: Dict[str, Dict[str, Any]] = {}
    bsp_zip = input_dir / "burn_severity_perimeter_data.zip"
    if bsp_zip.exists():
        for rec, _ in load_shapefile_from_zip(bsp_zip):
            event_id = as_str(rec.get("event_id"))
            if not event_id:
                continue
            bsp_attrs[event_id] = {
                "dnbr_val": as_float(rec.get("dnbr_val")),
                "prenbr_val": as_float(rec.get("prenbr_val")),
                "posnbr_val": as_float(rec.get("posnbr_val")),
                "cbi": as_float(rec.get("cbi")),
            }

    # Optional point fallback (if perimeter lat/lon missing).
    fod_points: Dict[str, Tuple[float, float]] = {}
    fod_zip = input_dir / "mtbs_fod_pts_data.zip"
    if fod_zip.exists():
        for rec, shape in load_shapefile_from_zip(fod_zip):
            event_id = as_str(rec.get("event_id"))
            if not event_id:
                continue
            lon = as_float(rec.get("burnbndlon"))
            lat = as_float(rec.get("burnbndlat"))
            if (lon == 0.0 and lat == 0.0) and shape.points:
                lon = float(shape.points[0][0])
                lat = float(shape.points[0][1])
            fod_points[event_id] = (lon, lat)

    # Canonical source: mtbs_perimeter_data.
    mtbs_zip = input_dir / "mtbs_perimeter_data.zip"
    if not mtbs_zip.exists():
        raise RuntimeError(f"Required input not found: {mtbs_zip}")

    events: List[Dict[str, Any]] = []
    geometry_by_year: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    index: Dict[str, Dict[str, Dict[str, List[str]]]] = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))

    for rec, shape in load_shapefile_from_zip(mtbs_zip):
        event_id = as_str(rec.get("event_id"))
        if not event_id:
            continue

        ig_date_raw = as_str(rec.get("ig_date"))
        ig_date = parse_date_iso(ig_date_raw)
        if not ig_date:
            continue

        year = ig_date.year
        day_of_year = int(ig_date.strftime("%j"))

        state = event_id[:2].upper() if len(event_id) >= 2 and event_id[:2].isalpha() else "NA"
        lon = as_float(rec.get("burnbndlon"))
        lat = as_float(rec.get("burnbndlat"))
        if (lon == 0.0 and lat == 0.0) and event_id in fod_points:
            lon, lat = fod_points[event_id]

        event: Dict[str, Any] = {
            "event_id": event_id,
            "name": as_str(rec.get("incid_name")),
            "incident_type": as_str(rec.get("incid_type")),
            "map_prog": as_str(rec.get("map_prog")),
            "asmnt_type": as_str(rec.get("asmnt_type")),
            "state": state,
            "ig_date": ig_date.isoformat(),
            "year": year,
            "day_of_year": day_of_year,
            "burnbndac": as_float(rec.get("burnbndac")),
            "burnbndlat": lat,
            "burnbndlon": lon,
            "low_t": as_float(rec.get("low_t")),
            "mod_t": as_float(rec.get("mod_t")),
            "high_t": as_float(rec.get("high_t")),
            "nodata_t": as_float(rec.get("nodata_t")),
            "greenness_t": as_float(rec.get("greenness_t")),
            "dnbr_val": as_float(rec.get("dnbr_val")),
            "dnbr_stddev": as_float(rec.get("dnbr_stddv")),
            "prenbr_val": 0.0,
            "posnbr_val": 0.0,
            "cbi": 0.0,
            "irwinid": as_str(rec.get("irwinid")),
            "pre_id": as_str(rec.get("pre_id")),
            "post_id": as_str(rec.get("post_id")),
        }
        event.update(bsp_attrs.get(event_id, {}))
        events.append(event)

        rings = shape_to_rings(shape, simplify_tol)
        geometry_by_year[year].append({"event_id": event_id, "rings": rings})
        index[str(year)][state][str(day_of_year)].append(event_id)

    events.sort(key=lambda e: e["event_id"])

    with (output_catalog / "events.ndjson").open("w", encoding="utf-8") as f:
        for event in events:
            f.write(json.dumps(event, separators=(",", ":")) + "\n")

    for year, rows in geometry_by_year.items():
        rows.sort(key=lambda r: r["event_id"])
        with (output_geometry / f"{year}.ndjson").open("w", encoding="utf-8") as f:
            for row in rows:
                f.write(json.dumps(row, separators=(",", ":")) + "\n")

    with (output_index / "year_state_day_index.json").open("w", encoding="utf-8") as f:
        json.dump(index, f, separators=(",", ":"))

    # Yearly raster metadata manifest.
    raster_manifest: Dict[str, Dict[str, Any]] = {}
    for zip_path in sorted(input_dir.glob("mtbs_CONUS_*.zip")):
        year_match = re.search(r"mtbs_CONUS_(\d{4})\.zip$", zip_path.name)
        if not year_match:
            continue
        year = year_match.group(1)
        meta = parse_xml_from_zip(zip_path) or {}
        meta["zip_file"] = zip_path.name
        meta["zip_size_bytes"] = zip_path.stat().st_size
        raster_manifest[year] = meta

    with (output_rasters / "yearly_raster_manifest.json").open("w", encoding="utf-8") as f:
        json.dump(raster_manifest, f, indent=2)

    print(f"Generated events: {len(events)}")
    print(f"Generated geometry years: {len(geometry_by_year)}")
    print(f"Output root: {output_dir}")


if __name__ == "__main__":
    main()

