#!/usr/bin/env python3
"""从 china-latest.osm.pbf 提取重庆城区三图层 GeoJSONSeq(tippecanoe 输入)。

图层与 demo 样式(GLESView mvt-basemap match 表达式)对齐:
  roads    — highway=* 的 way(线)
  water    — natural=water / waterway=riverbank / landuse=reservoir(面),
             waterway=river/canal(线)
  building — building=*(面)
  poi      — 带 name 的 node(点):place/railway/aeroway/amenity/shop/
             tourism/leisure 白名单,携 kind + rank(数值越小越重要,
             符号系统 rank 预过滤的依据)

用法:
  python3 extract_chongqing_geojson.py <china-latest.osm.pbf> <输出目录> [only_poi]

  only_poi:只提取 poi 层。node 自带坐标,跳过 way/area 与 location 索引,
  1.5G 全国 pbf 下比全量提取快一个量级。

依赖:pip3 install --user osmium
"""

import json
import sys

import osmium
from osmium.geom import GeoJSONFactory

# 重庆城区 bbox(demo 相机 106.508,29.617 周边 ~70km)
BBOX_W, BBOX_S, BBOX_E, BBOX_N = 106.2, 29.3, 106.9, 29.9

_geojson = GeoJSONFactory()

# POI 白名单:key → {value: rank}。rank 语义:1=城市级地名 … 6=一般设施,
# 符号系统按 rank 升序截断(每瓦保留前 N)。'*' 兜底该 key 的其余 value。
POI_RANKS = {
    "place": {"city": 1, "town": 2, "district": 3, "suburb": 3,
              "village": 4, "neighbourhood": 4},
    "railway": {"station": 3},
    "aeroway": {"aerodrome": 2},
    "amenity": {"hospital": 4, "university": 4, "bus_station": 4,
                "school": 5, "police": 5, "cinema": 5, "theatre": 5,
                "marketplace": 5, "bank": 6, "restaurant": 6, "cafe": 6,
                "pharmacy": 6},
    "shop": {"mall": 5, "supermarket": 5, "department_store": 5},
    "tourism": {"attraction": 4, "museum": 4, "zoo": 4, "hotel": 6,
                "viewpoint": 5},
    "leisure": {"park": 4, "stadium": 4, "sports_centre": 5},
}


def _in_bbox(lon, lat):
    return BBOX_W <= lon <= BBOX_E and BBOX_S <= lat <= BBOX_N


class Extractor(osmium.SimpleHandler):
    def __init__(self, out_dir, layers=("roads", "water", "building", "poi")):
        super().__init__()
        # 只打开本次要产出的层 —— "w" 会截断,不能把没在提取的既有文件清空
        self.files = {
            name: open(f"{out_dir}/{name}.geojsonseq", "w", encoding="utf-8")
            for name in layers
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

    def node(self, n):
        tags = n.tags
        if "name" not in tags:
            return
        if not n.location.valid() or not _in_bbox(n.location.lon,
                                                  n.location.lat):
            return
        for key, ranks in POI_RANKS.items():
            v = tags.get(key)
            if v is None:
                continue
            rank = ranks.get(v)
            if rank is None:
                continue
            self._emit("poi", json.dumps({
                "type": "Point",
                "coordinates": [n.location.lon, n.location.lat],
            }), {
                "name": tags["name"],
                "kind": f"{key}:{v}",
                "rank": rank,
            })
            return

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
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    src, out_dir = sys.argv[1], sys.argv[2]
    only_poi = len(sys.argv) == 4 and sys.argv[3] == "only_poi"
    if only_poi:
        handler = Extractor(out_dir, layers=("poi",))
        handler.way = lambda w: None
        handler.area = lambda a: None
        handler.apply_file(src)  # 无 location 索引,node 自带坐标
    else:
        handler = Extractor(out_dir)
        # locations=True 供 way 取节点坐标;flex_mem 平衡内存与速度
        handler.apply_file(src, locations=True, idx="flex_mem")
    handler.close()
    print("done:", handler.counts)


if __name__ == "__main__":
    main()
