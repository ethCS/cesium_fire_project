#!/usr/bin/env python3
"""
fetch_mtbs_api.py — Pull MTBS fire data from the USFS ArcGIS MapServer and
produce the same NDJSON artifacts as fire_data_etl.py, with no local ZIPs needed.

Source: https://apps.fs.usda.gov/arcx/rest/services/EDW/EDW_MTBS_01/MapServer
  Layer 63: Burned Area Boundaries (All Years) — polygon geometry + attributes
  Layer 62: Fire Occurrence Locations (All Years) — point centroids (fallback)

Usage:
  python fetch_mtbs_api.py --years 2024 --output-dir FireData/Generated
  python fetch_mtbs_api.py --years 2023,2024,2025 --output-dir FireData/Generated
  python fetch_mtbs_api.py --all --output-dir FireData/Generated

Outputs (merged with existing files by default):
  catalog/events.ndjson              one record per fire event
  index/year_state_day_index.json    Year -> State -> DayOfYear -> [event_ids]
  geometry/<year>.ndjson             polygon rings per fire per year
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

BASE_URL = "https://apps.fs.usda.gov/arcx/rest/services/EDW/EDW_MTBS_01/MapServer"
LAYER_BOUNDARIES = 63   # Burned Area Boundaries (All Years)
LAYER_OCCURRENCES = 62  # Fire Occurrence Locations (All Years)
PAGE_SIZE = 200  # server rejects geometry responses larger than ~200 features
RETRY_LIMIT = 3
RETRY_DELAY = 2.0

# Explicit field list — avoids computed fields (st_area, st_perimeter) that
# break geometry queries when outFields=* is used.
BOUNDARY_FIELDS = (
    "fire_id,fire_name,fire_type,year,startmonth,startday,ig_date,"
    "acres,latitude,longitude,map_prog,asmnt_type,"
    "low_threshold,moderate_threshold,high_threshold,dnbr_offst,"
    "dnbr_stddv,nodata_threshold,greenness_threshold,"
    "irwinid,pre_id,post_id"
)


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

_HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    "Accept": "application/json, text/plain, */*",
    "Referer": "https://lcms-viewer.fs2c.usda.gov/",
}


def _get_json(url: str, params: Dict[str, str]) -> Dict[str, Any]:
    full_url = url + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(full_url, headers=_HEADERS)
    for attempt in range(1, RETRY_LIMIT + 1):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except (urllib.error.URLError, OSError) as exc:
            if attempt == RETRY_LIMIT:
                raise RuntimeError(f"HTTP error after {RETRY_LIMIT} attempts: {exc}\nURL: {full_url}") from exc
            print(f"  Retry {attempt}/{RETRY_LIMIT} after error: {exc}", file=sys.stderr)
            time.sleep(RETRY_DELAY * attempt)
    raise RuntimeError("unreachable")


def _fetch_object_ids(layer: int, where: str) -> List[int]:
    """Return all ObjectIDs matching `where` — avoids resultOffset instability."""
    url = f"{BASE_URL}/{layer}/query"
    data = _get_json(url, {"where": where, "returnIdsOnly": "true", "f": "json"})
    if "error" in data:
        raise RuntimeError(f"Server error fetching IDs on layer {layer}: {data['error']}")
    return data.get("objectIds") or []


def _fetch_chunk(url: str, oids: List[int], fields: str, geometry: bool) -> Tuple[List[Dict[str, Any]], List[int]]:
    """Fetch a chunk of features by objectid list. On server error, bisect to isolate
    bad records and skip them, returning (features, skipped_oids)."""
    if not oids:
        return [], []

    clause = f"objectid IN ({','.join(str(x) for x in oids)})"
    params: Dict[str, str] = {
        "where": clause,
        "outFields": fields,
        "returnGeometry": "true" if geometry else "false",
        "f": "json",
    }
    data = _get_json(url, params)

    if "error" not in data:
        return data.get("features", []), []

    # Single bad record — skip it
    if len(oids) == 1:
        print(f"    WARNING: skipping corrupt objectid {oids[0]} (server error)", file=sys.stderr)
        return [], oids

    # Bisect to isolate the bad record
    mid = len(oids) // 2
    left_feats, left_bad = _fetch_chunk(url, oids[:mid], fields, geometry)
    right_feats, right_bad = _fetch_chunk(url, oids[mid:], fields, geometry)
    return left_feats + right_feats, left_bad + right_bad


def query_layer(layer: int, where: str, fields: str = "*", geometry: bool = True) -> List[Dict[str, Any]]:
    """Fetch all features matching `where` using ObjectID-chunked pagination.

    ESRI resultOffset breaks on layer 63 past ~600 features when returnGeometry=true.
    Fetches all ObjectIDs first, then requests data in PAGE_SIZE chunks. Corrupt
    server records are automatically isolated via bisection and skipped with a warning.
    """
    url = f"{BASE_URL}/{layer}/query"

    object_ids = _fetch_object_ids(layer, where)
    if not object_ids:
        return []

    results: List[Dict[str, Any]] = []
    total_skipped: List[int] = []
    for i in range(0, len(object_ids), PAGE_SIZE):
        chunk = object_ids[i: i + PAGE_SIZE]
        feats, skipped = _fetch_chunk(url, chunk, fields, geometry)
        results.extend(feats)
        total_skipped.extend(skipped)

    if total_skipped:
        print(f"  Skipped {len(total_skipped)} corrupt server records: {total_skipped}", file=sys.stderr)
    return results


# ---------------------------------------------------------------------------
# Geometry helpers (same Douglas-Peucker used by fire_data_etl.py)
# ---------------------------------------------------------------------------

def _point_line_dist(p: Tuple[float, float], a: Tuple[float, float], b: Tuple[float, float]) -> float:
    (x, y), (x1, y1), (x2, y2) = p, a, b
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(x - x1, y - y1)
    t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - (x1 + t * dx), y - (y1 + t * dy))


def _douglas_peucker(pts: List[Tuple[float, float]], tol: float) -> List[Tuple[float, float]]:
    if len(pts) <= 2:
        return pts
    max_d, idx = -1.0, -1
    for i in range(1, len(pts) - 1):
        d = _point_line_dist(pts[i], pts[0], pts[-1])
        if d > max_d:
            max_d, idx = d, i
    if max_d > tol and idx != -1:
        return _douglas_peucker(pts[: idx + 1], tol)[:-1] + _douglas_peucker(pts[idx:], tol)
    return [pts[0], pts[-1]]


def simplify_ring(ring: List[List[float]], tol: float) -> List[List[float]]:
    if len(ring) <= 8 or tol <= 0.0:
        return ring
    pts = [(p[0], p[1]) for p in ring]
    closed = pts[0] == pts[-1]
    core = pts[:-1] if closed else pts
    simplified = _douglas_peucker(core, tol)
    if len(simplified) < 3:
        simplified = core
    if simplified[0] != simplified[-1]:
        simplified.append(simplified[0])
    if len(simplified) < 4:
        return ring
    return [[p[0], p[1]] for p in simplified]


def esri_rings_to_ndjson(esri_rings: List[List[List[float]]], tol: float) -> List[List[List[float]]]:
    """Convert ESRI polygon rings (already [lon, lat]) to simplified NDJSON rings."""
    out: List[List[List[float]]] = []
    for ring in esri_rings:
        if len(ring) < 3:
            continue
        simplified = simplify_ring(ring, tol)
        if len(simplified) >= 4:
            out.append(simplified)
    return out


# ---------------------------------------------------------------------------
# Attribute parsing
# ---------------------------------------------------------------------------

def _as_float(v: Any, default: float = 0.0) -> float:
    if v is None:
        return default
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _as_str(v: Any) -> str:
    return "" if v is None else str(v).strip()


def parse_ig_date(attrs: Dict[str, Any]) -> Optional[dt.date]:
    """ig_date comes as YYYYMMDD integer or a fallback from year/startmonth/startday."""
    raw = attrs.get("ig_date")
    if raw is not None:
        try:
            return dt.datetime.strptime(str(int(raw)), "%Y%m%d").date()
        except (ValueError, TypeError):
            pass
    # Fallback: construct from year + startmonth + startday
    try:
        y = int(attrs["year"])
        m = int(attrs.get("startmonth") or 1)
        d = int(attrs.get("startday") or 1)
        return dt.date(y, m, d)
    except (KeyError, ValueError, TypeError):
        return None


def attrs_to_event(attrs: Dict[str, Any], ig_date: dt.date) -> Dict[str, Any]:
    event_id = _as_str(attrs.get("fire_id"))
    state = event_id[:2].upper() if len(event_id) >= 2 and event_id[:2].isalpha() else "NA"
    return {
        "event_id": event_id,
        "name": _as_str(attrs.get("fire_name")),
        "incident_type": _as_str(attrs.get("fire_type")),
        "map_prog": _as_str(attrs.get("map_prog")),
        "asmnt_type": _as_str(attrs.get("asmnt_type")),
        "state": state,
        "ig_date": ig_date.isoformat(),
        "year": ig_date.year,
        "day_of_year": int(ig_date.strftime("%j")),
        "burnbndac": _as_float(attrs.get("acres")),
        "burnbndlat": _as_float(attrs.get("latitude")),
        "burnbndlon": _as_float(attrs.get("longitude")),
        "low_t": _as_float(attrs.get("low_threshold")),
        "mod_t": _as_float(attrs.get("moderate_threshold")),
        "high_t": _as_float(attrs.get("high_threshold")),
        "nodata_t": _as_float(attrs.get("nodata_threshold")),
        "greenness_t": _as_float(attrs.get("greenness_threshold")),
        "dnbr_val": _as_float(attrs.get("dnbr_offst")),
        "dnbr_stddev": _as_float(attrs.get("dnbr_stddv")),
        "prenbr_val": 0.0,
        "posnbr_val": 0.0,
        "cbi": 0.0,
        "irwinid": _as_str(attrs.get("irwinid")),
        "pre_id": _as_str(attrs.get("pre_id")),
        "post_id": _as_str(attrs.get("post_id")),
    }


# ---------------------------------------------------------------------------
# Main fetch logic
# ---------------------------------------------------------------------------

def fetch_years(years: List[int], output_dir: Path, simplify_tol: float, merge: bool) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "catalog").mkdir(exist_ok=True)
    (output_dir / "index").mkdir(exist_ok=True)
    (output_dir / "geometry").mkdir(exist_ok=True)

    year_clause = ",".join(str(y) for y in years)
    if len(years) == 1:
        where = f"year={years[0]}"
    else:
        where = " OR ".join(f"year={y}" for y in years)
    print(f"Fetching layers for years: {year_clause}")

    # -- Fetch boundary polygons + attributes --
    print(f"  Querying layer {LAYER_BOUNDARIES} (Burned Area Boundaries)...")
    features = query_layer(LAYER_BOUNDARIES, where, fields=BOUNDARY_FIELDS, geometry=True)
    print(f"  Retrieved {len(features)} boundary features")

    new_events: Dict[str, Dict[str, Any]] = {}
    new_geometry: Dict[int, Dict[str, List[List[List[float]]]]] = defaultdict(dict)

    for feat in features:
        attrs = {k.lower(): v for k, v in feat.get("attributes", {}).items()}
        ig_date = parse_ig_date(attrs)
        if not ig_date:
            continue
        event_id = _as_str(attrs.get("fire_id"))
        if not event_id:
            continue

        event = attrs_to_event(attrs, ig_date)
        new_events[event_id] = event

        geom = feat.get("geometry")
        if geom and geom.get("rings"):
            rings = esri_rings_to_ndjson(geom["rings"], simplify_tol)
            if rings:
                new_geometry[ig_date.year][event_id] = rings

    # -- Merge with existing data --
    catalog_path = output_dir / "catalog" / "events.ndjson"
    index_path = output_dir / "index" / "year_state_day_index.json"

    existing_events: Dict[str, Dict[str, Any]] = {}
    if merge and catalog_path.exists():
        with catalog_path.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    ev = json.loads(line)
                    existing_events[ev["event_id"]] = ev
        print(f"  Loaded {len(existing_events)} existing events from catalog")

    # Merge: API fields win, but preserve existing non-zero burn severity values
    # that the API doesn't supply (prenbr_val, posnbr_val, cbi).
    _PRESERVE_IF_NONZERO = ("prenbr_val", "posnbr_val", "cbi")
    merged_events: Dict[str, Dict[str, Any]] = {}
    for eid, new_ev in new_events.items():
        if eid in existing_events:
            existing = existing_events[eid]
            for field in _PRESERVE_IF_NONZERO:
                if new_ev.get(field, 0.0) == 0.0 and existing.get(field, 0.0) != 0.0:
                    new_ev[field] = existing[field]
        merged_events[eid] = new_ev
    # Keep events not touched by this fetch run
    for eid, ev in existing_events.items():
        if eid not in merged_events:
            merged_events[eid] = ev
    print(f"  Merged catalog: {len(merged_events)} total events ({len(new_events)} new/updated)")

    # Write geometry per year (replace fetched years, keep others untouched)
    for year, year_geom in new_geometry.items():
        geo_path = output_dir / "geometry" / f"{year}.ndjson"
        # If merging, load existing geometry for this year and overlay new entries
        existing_geo: Dict[str, List[List[List[float]]]] = {}
        if merge and geo_path.exists():
            with geo_path.open("r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        row = json.loads(line)
                        existing_geo[row["event_id"]] = row["rings"]
        merged_geo = {**existing_geo, **year_geom}
        rows = sorted(merged_geo.items())
        with geo_path.open("w", encoding="utf-8") as f:
            for eid, rings in rows:
                f.write(json.dumps({"event_id": eid, "rings": rings}, separators=(",", ":")) + "\n")
        print(f"  Wrote geometry/{year}.ndjson ({len(rows)} fires)")

    # Write merged catalog
    sorted_events = sorted(merged_events.values(), key=lambda e: e["event_id"])
    with catalog_path.open("w", encoding="utf-8") as f:
        for ev in sorted_events:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")
    print(f"  Wrote catalog/events.ndjson ({len(sorted_events)} events)")

    # Rebuild full index from merged events
    index: Dict[str, Dict[str, Dict[str, List[str]]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )
    for ev in sorted_events:
        y = str(ev["year"])
        s = ev["state"]
        d = str(ev["day_of_year"])
        index[y][s][d].append(ev["event_id"])

    with index_path.open("w", encoding="utf-8") as f:
        json.dump(index, f, separators=(",", ":"))
    print(f"  Wrote index/year_state_day_index.json ({len(index)} years indexed)")


def available_years() -> List[int]:
    """Query the server for the distinct years available in layer 63."""
    url = f"{BASE_URL}/{LAYER_BOUNDARIES}/query"
    params = {
        "where": "1=1",
        "outFields": "year",
        "returnGeometry": "false",
        "returnDistinctValues": "true",
        "orderByFields": "year ASC",
        "resultRecordCount": "200",
        "f": "json",
    }
    data = _get_json(url, params)
    years = []
    for feat in data.get("features", []):
        y = feat.get("attributes", {}).get("year")
        if y is not None:
            years.append(int(y))
    return years


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--years", help="Comma-separated years to fetch, e.g. 2024 or 2023,2024,2025")
    group.add_argument("--all", action="store_true", help="Fetch all available years from the server")
    parser.add_argument("--output-dir", required=True, type=Path, help="Root of Generated data directory")
    parser.add_argument("--simplify-tolerance-deg", type=float, default=0.002, help="Douglas-Peucker tolerance in degrees (default 0.002)")
    parser.add_argument("--no-merge", action="store_true", help="Replace catalog and index instead of merging with existing files")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if args.all:
        print("Querying server for available years...")
        years = available_years()
        if not years:
            print("ERROR: No years returned from server.", file=sys.stderr)
            sys.exit(1)
        print(f"Available years: {years[0]}–{years[-1]} ({len(years)} years)")
    else:
        years = [int(y.strip()) for y in args.years.split(",")]

    fetch_years(
        years=years,
        output_dir=args.output_dir,
        simplify_tol=args.simplify_tolerance_deg,
        merge=not args.no_merge,
    )
    print("Done.")


if __name__ == "__main__":
    main()
