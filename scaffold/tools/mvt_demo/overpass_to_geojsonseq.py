#!/usr/bin/env python3
"""Overpass `out geom` JSON → GeoJSON-seq(tippecanoe 输入)。

为什么不是原来的 pyosmium + china-latest.osm.pbf:那条路要下 1.5GB 全国
extract 再本地裁剪,而本 demo 只要重庆主城几十公里见方。Overpass 按 bbox
取,总量几十 MB,且不依赖 pyosmium(机器无 Homebrew,装它要现编 libosmium)。
代价是 Overpass 有速率/超时限制,不适合大范围;要全国级数据仍回 pbf 那条路。

用法:
    overpass_to_geojsonseq.py <in.json> <out.geojsonseq> <geom-kind>
geom-kind = line | polygon
"""

import json
import sys


def ring_is_closed(coords):
    return len(coords) >= 4 and coords[0] == coords[-1]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    src, dst, kind = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(src, encoding="utf-8") as f:
        data = json.load(f)

    written = 0
    skipped = 0
    with open(dst, "w", encoding="utf-8") as out:
        for el in data.get("elements", []):
            geom = el.get("geometry")
            if not geom:
                skipped += 1
                continue
            coords = [[p["lon"], p["lat"]] for p in geom]
            if kind == "polygon":
                # Overpass 的 way 未必闭合;tippecanoe 要求外环闭合。
                if len(coords) < 3:
                    skipped += 1
                    continue
                if not ring_is_closed(coords):
                    coords = coords + [coords[0]]
                geometry = {"type": "Polygon", "coordinates": [coords]}
            else:
                if len(coords) < 2:
                    skipped += 1
                    continue
                geometry = {"type": "LineString", "coordinates": coords}

            tags = el.get("tags", {}) or {}
            # 只留渲染/过滤用得上的标签,别把整份 OSM 标签塞进瓦片。
            props = {}
            for key in ("name", "highway", "building", "natural", "waterway"):
                if key in tags:
                    props[key] = tags[key]
            props["osm_id"] = el.get("id", 0)

            out.write(json.dumps(
                {"type": "Feature", "geometry": geometry, "properties": props},
                ensure_ascii=False) + "\n")
            written += 1

    print(f"{dst}: {written} features written, {skipped} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
