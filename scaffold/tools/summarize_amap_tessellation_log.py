#!/usr/bin/env python3
"""Summarize segmented VectorTessSlow records from MinimalGlobe logcat."""

import argparse
import json
import re
from pathlib import Path


FIELDS = (
    "tess", "admit", "polygon", "line", "polySetup", "polyDensify",
    "polyIntersect", "polyCdt", "polyEcef", "extrude", "symbol", "slowest"
    , "cdtSuper", "cdtPoint", "cdtConstraint", "cdtExtract"
)
LINE = re.compile(
    r"VectorTessSlow(?:\(\s*\d+\))?: (?P<layer>\S+) "
    r".*?features=(?P<features>\d+) .*?"
    r"tess=(?P<tess>[0-9.]+)ms admit=(?P<admit>[0-9.]+)ms "
    r"polygon=(?P<polygon>[0-9.]+)ms line=(?P<line>[0-9.]+)ms "
    r"(?:polySetup=(?P<polySetup>[0-9.]+)ms "
    r"polyDensify=(?P<polyDensify>[0-9.]+)ms "
    r"polyIntersect=(?P<polyIntersect>[0-9.]+)ms "
    r"polyCdt=(?P<polyCdt>[0-9.]+)ms polyEcef=(?P<polyEcef>[0-9.]+)ms )?"
    r"(?:cdtSuper=(?P<cdtSuper>[0-9.]+)ms "
    r"cdtPoint=(?P<cdtPoint>[0-9.]+)ms "
    r"cdtConstraint=(?P<cdtConstraint>[0-9.]+)ms "
    r"cdtExtract=(?P<cdtExtract>[0-9.]+)ms )?"
    r"extrude=(?P<extrude>[0-9.]+)ms symbol=(?P<symbol>[0-9.]+)ms .*?"
    r"accepted=(?P<accepted>\d+) rejected=(?P<rejected>\d+) .*?"
    r"slowest=(?P<slowest>[0-9.]+)ms class=(?P<class>-?\d+) "
    r"sub=(?P<sub>-?\d+) slowRings=(?P<rings>\d+) "
    r"slowPoints=(?P<points>\d+)"
    r"(?: .*?cdtPointTests=(?P<cdtPointTests>\d+) "
    r"cdtBad=(?P<cdtBad>\d+) cdtEdge(?:Tests|Lookups)=(?P<cdtEdgeTests>\d+) "
    r"cdtCrossTests=(?P<cdtCrossTests>\d+) "
    r"cdtConstraints=(?P<cdtAlready>\d+)/(?P<cdtInserted>\d+) "
    r"cdtPeakTris=(?P<cdtPeakTris>\d+)"
    r"(?: cdtCapacityGrowths=(?P<cdtPointGrowths>\d+)/(?P<cdtTriGrowths>\d+))?"
    r"(?: rejectReasons=(?P<rejectGeometry>\d+)/(?P<rejectDrawOrder>\d+)/"
    r"(?P<rejectZoomWindow>\d+)/(?P<rejectFillIdentity>\d+)/"
    r"(?P<rejectLineIdentity>\d+)/(?P<rejectPointIdentity>\d+)/"
    r"(?P<rejectRank>\d+)/(?P<rejectDegenerate>\d+)/"
    r"(?P<rejectLabelLayout>\d+) rejectTop=(?P<rejectTopGeometry>\d+):"
    r"(?P<rejectTopClass>-?\d+):(?P<rejectTopSub>-?\d+)/"
    r"(?P<rejectTopCount>\d+))?)?"
)


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def summarize(text: str) -> dict:
    rows = []
    for match in LINE.finditer(text):
        row = match.groupdict()
        for field in FIELDS:
            row[field] = float(row[field] or 0.0)
        for field in ("features", "accepted", "rejected", "class", "sub",
                      "rings", "points", "cdtPointTests", "cdtBad",
                      "cdtEdgeTests", "cdtCrossTests", "cdtAlready",
                      "cdtInserted", "cdtPeakTris", "cdtPointGrowths",
                      "cdtTriGrowths"):
            row[field] = int(row[field] or 0)
        for field in (
            "rejectGeometry", "rejectDrawOrder", "rejectZoomWindow",
            "rejectFillIdentity", "rejectLineIdentity",
            "rejectPointIdentity", "rejectRank", "rejectDegenerate",
            "rejectLabelLayout", "rejectTopGeometry", "rejectTopClass",
            "rejectTopSub", "rejectTopCount"):
            row[field] = int(row[field] or 0)
        rows.append(row)
    by_layer = {}
    for layer in sorted({row["layer"] for row in rows}):
        selected = [row for row in rows if row["layer"] == layer]
        tess = [row["tess"] for row in selected]
        stage_totals = {field: sum(row[field] for row in selected)
                        for field in ("admit", "polygon", "line", "extrude",
                                      "symbol")}
        total_stages = sum(stage_totals.values())
        polygon_totals = {
            field: sum(row[field] for row in selected)
            for field in ("polySetup", "polyDensify", "polyIntersect",
                          "polyCdt", "polyEcef")
        }
        polygon_measured = sum(polygon_totals.values())
        cdt_totals = {
            field: sum(row[field] for row in selected)
            for field in ("cdtSuper", "cdtPoint", "cdtConstraint", "cdtExtract")
        }
        cdt_measured = sum(cdt_totals.values())
        cdt_counts = {
            field: sum(row[field] for row in selected)
            for field in ("cdtPointTests", "cdtBad", "cdtEdgeTests",
                          "cdtCrossTests", "cdtAlready", "cdtInserted")
        }
        cdt_counts["cdtPeakTris"] = max(
            (row["cdtPeakTris"] for row in selected), default=0)
        cdt_counts["cdtPointGrowths"] = sum(
            row["cdtPointGrowths"] for row in selected)
        cdt_counts["cdtTriGrowths"] = sum(
            row["cdtTriGrowths"] for row in selected)
        rejection_counts = {
            field: sum(row[field] for row in selected)
            for field in (
                "rejectGeometry", "rejectDrawOrder", "rejectZoomWindow",
                "rejectFillIdentity", "rejectLineIdentity",
                "rejectPointIdentity", "rejectRank", "rejectDegenerate",
                "rejectLabelLayout")
        }
        slowest = max(selected, key=lambda row: row["slowest"])
        by_layer[layer] = {
            "samples": len(selected),
            "tessMs": {"min": min(tess), "median": percentile(tess, 0.5),
                       "p95": percentile(tess, 0.95), "max": max(tess)},
            "stageShare": {field: (value / total_stages if total_stages else 0.0)
                           for field, value in stage_totals.items()},
            "polygonBreakdownMs": polygon_totals,
            "polygonBreakdownShare": {
                field: (value / polygon_measured if polygon_measured else 0.0)
                for field, value in polygon_totals.items()
            },
            "cdtBreakdownMs": cdt_totals,
            "cdtBreakdownShare": {
                field: (value / cdt_measured if cdt_measured else 0.0)
                for field, value in cdt_totals.items()
            },
            "cdtCounts": cdt_counts,
            "rejectionCounts": rejection_counts,
            "slowestFeature": {key: slowest[key] for key in
                               ("slowest", "class", "sub", "rings", "points")},
        }
    return {"records": len(rows), "layers": by_layer}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    result = summarize(args.log.read_text(errors="ignore"))
    if result["records"] == 0:
        raise SystemExit("no segmented VectorTessSlow records found")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
