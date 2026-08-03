#!/usr/bin/env python3
"""从 china-latest.osm.pbf 提取重庆城区三图层 GeoJSONSeq(tippecanoe 输入)。

图层与 demo 样式(GLESView mvt-basemap match 表达式)对齐:
  roads    — highway=* 的 way(线)
  water    — natural=water / waterway=riverbank / landuse=reservoir(面),
             waterway=river/canal(线)
  building — building=*(面)

用法:
  python3 extract_chongqing_geojson.py <china-latest.osm.pbf> <输出目录>

依赖:pip3 install --user osmium
"""

import json
import sys

import osmium
from osmium.geom import GeoJSONFactory

# 重庆城区 bbox(demo 相机 106.508,29.617 周边 ~70km)
BBOX_W, BBOX_S, BBOX_E, BBOX_N = 106.2, 29.3, 106.9, 29.9

_geojson = GeoJSONFactory()


def _in_bbox(lon, lat):
    return BBOX_W <= lon <= BBOX_E and BBOX_S <= lat <= BBOX_N


class Extractor(osmium.SimpleHandler):
    def __init__(self, out_dir):
        super().__init__()
        self.files = {
            name: open(f"{out_dir}/{name}.geojsonseq", "w", encoding="utf-8")
            for name in ("roads", "water", "building")
        }
        self.counts = {name: 0 for name in self.files}

    def close(self):
        for f in self.files.values():
            f.close()

    def _emit(self, layer, geometry_json, props):
        feature = {
            "type": "Feature",
            "geometry": json.loads(geometry_json),
            "properties": props,
        }
        self.files[layer].write(json.dumps(feature, ensure_ascii=False) + "\n")
        self.counts[layer] += 1

    def way(self, w):
        if len(w.nodes) < 2:
            return
        try:
            first = w.nodes[0]
            if not first.location.valid() or not _in_bbox(
                    first.location.lon, first.location.lat):
                return
        except osmium.InvalidLocationError:
            return

        tags = w.tags
        if "highway" in tags:
            try:
                geom = _geojson.create_linestring(w)
            except RuntimeError:
                return
            props = {"highway": tags["highway"]}
            if "name" in tags:
                props["name"] = tags["name"]
            self._emit("roads", geom, props)
        elif tags.get("waterway") in ("river", "canal"):
            try:
                geom = _geojson.create_linestring(w)
            except RuntimeError:
                return
            props = {"waterway": tags["waterway"]}
            if "name" in tags:
                props["name"] = tags["name"]
            self._emit("water", geom, props)

    def area(self, a):
        tags = a.tags
        layer = None
        props = {}
        if "building" in tags:
            layer = "building"
            props["building"] = tags["building"]
        elif (tags.get("natural") == "water"
              or tags.get("waterway") == "riverbank"
              or tags.get("landuse") == "reservoir"):
            layer = "water"
        if layer is None:
            return
        # bbox 粗筛:用第一个外环第一个点
        try:
            outer = next(a.outer_rings())
            first = next(iter(outer))
            if not _in_bbox(first.lon, first.lat):
                return
        except StopIteration:
            return
        try:
            geom = _geojson.create_multipolygon(a)
        except RuntimeError:
            return
        if "name" in tags:
            props["name"] = tags["name"]
        self._emit(layer, geom, props)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    src, out_dir = sys.argv[1], sys.argv[2]
    handler = Extractor(out_dir)
    # locations=True 供 way 取节点坐标;flex_mem 平衡内存与速度
    handler.apply_file(src, locations=True, idx="flex_mem")
    handler.close()
    print("done:", handler.counts)


if __name__ == "__main__":
    main()
