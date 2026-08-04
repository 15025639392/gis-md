#!/bin/bash
# 重庆 OSM → chongqing.mbtiles(demo MVT 底图数据管线)。
#
# 数据来源走 **Overpass 按 bbox 直取**,不再下 geofabrik china-latest.osm.pbf:
#   - 磁盘从 ~5GB 降到 ~30MB(本机曾因 95% 满而丢失整棵 tmp/);
#   - 不依赖 pyosmium(无 Homebrew 的机器上装它要现编 libosmium)。
# 代价:Overpass 有速率/超时限制,不适合大范围。要全国级数据仍回 pbf 那条路
# (extract_chongqing_geojson.py 保留着)。
#
# 前置:
#   1. 网络可达 overpass-api.de
#   2. tippecanoe 二进制(源码 make;本机在 tmp/tippecanoe/tippecanoe)
#
# 用法:build_mbtiles.sh <工作目录> [tippecanoe路径]
set -e

WORK="${1:?用法: build_mbtiles.sh <工作目录> [tippecanoe路径]}"
TIPPECANOE="${2:-tippecanoe}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERPASS="${OVERPASS_URL:-https://overpass-api.de/api/interpreter}"

mkdir -p "$WORK"

# bbox 取重庆主城。建筑单独用更小的窗 —— 全城建筑会把 Overpass 拖超时,
# 而底图观感上建筑只在近景(z13+)才可见,窗小不影响验收。
BBOX_MAIN="29.45,106.35,29.75,106.70"
BBOX_BLDG="29.50,106.45,29.62,106.60"

fetch() {   # fetch <输出名> <Overpass QL>
    local name="$1" ql="$2"
    if [ -s "$WORK/$name.json" ]; then
        echo "== $name.json 已存在,跳过拉取 =="
        return
    fi
    echo "== Overpass 拉取 $name =="
    curl -s --max-time 420 -X POST -d "$ql" "$OVERPASS" -o "$WORK/$name.json"
    # Overpass 超限时会返回 HTML 错误页而非 JSON,且 curl 仍是 exit 0 ——
    # 不校验的话会一路走到 tippecanoe 才炸,且错得莫名其妙。
    python3 -c "import json,sys; json.load(open('$WORK/$name.json'))" \
        || { echo "!! $name 返回的不是 JSON(多半 Overpass 限流/超时),重试或改小 bbox"; exit 1; }
}

fetch roads "[out:json][timeout:180];
way[\"highway\"~\"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|motorway_link|trunk_link|primary_link|secondary_link)\$\"]($BBOX_MAIN);
out geom;"

fetch water "[out:json][timeout:120];
(
  way[\"natural\"=\"water\"]($BBOX_MAIN);
  way[\"waterway\"=\"riverbank\"]($BBOX_MAIN);
);
out geom;"

fetch buildings "[out:json][timeout:300];
way[\"building\"]($BBOX_BLDG);
out geom;"

echo "== 转 GeoJSON-seq =="
python3 "$SCRIPT_DIR/overpass_to_geojsonseq.py" "$WORK/roads.json"     "$WORK/roads.geojsonseq"     line
python3 "$SCRIPT_DIR/overpass_to_geojsonseq.py" "$WORK/water.json"     "$WORK/water.geojsonseq"     polygon
python3 "$SCRIPT_DIR/overpass_to_geojsonseq.py" "$WORK/buildings.json" "$WORK/buildings.geojsonseq" polygon

# 按 zoom 分级过滤道路等级(对齐生产惯例):粗档只留干线,细档才全量。
# ⚠️ tippecanoe 的 -j 里 zoom 是 "$zoom" **字面量**,不是 ["zoom"] 表达式。
# 用后者会报 `comparison key is not a string`,而且**照常退出 0 并产出一个
# 几十 KB 的空 mbtiles** —— 极容易被当成「数据源没抓到」而查错方向。
echo "== tippecanoe 切瓦片(z0-14,分层 + 分级过滤)=="
"$TIPPECANOE" -o "$WORK/chongqing.mbtiles" --force \
    -Z 0 -z 14 \
    --drop-densest-as-needed \
    --extend-zooms-if-still-dropping \
    -L roads:"$WORK/roads.geojsonseq" \
    -L water:"$WORK/water.geojsonseq" \
    -L building:"$WORK/buildings.geojsonseq" \
    -j '{
      "roads": ["any",
        ["all", ["<", "$zoom", 9],  ["in", "highway", "motorway", "trunk", "primary"]],
        ["all", [">=", "$zoom", 9],  ["<", "$zoom", 10], ["in", "highway", "motorway", "trunk", "primary", "secondary"]],
        ["all", [">=", "$zoom", 10], ["<", "$zoom", 12], ["in", "highway", "motorway", "trunk", "primary", "secondary", "tertiary"]],
        [">=", "$zoom", 12]],
      "building": [">=", "$zoom", 13]
    }'

echo "== 完成:$WORK/chongqing.mbtiles =="
python3 - "$WORK/chongqing.mbtiles" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
rows = c.execute("select zoom_level, count(*) from tiles group by zoom_level order by zoom_level").fetchall()
print("zoom 分布:", rows)
print("最大瓦片字节:", c.execute("select max(length(tile_data)) from tiles").fetchone()[0])
# 空 mbtiles 的典型特征:只有极粗档、总瓦片数个位数。见上面 -j 语法坑。
if sum(n for _, n in rows) < 20:
    print("!! 瓦片数异常少,检查 -j 过滤器语法与源数据")
PY
echo "起服务:python3 $SCRIPT_DIR/serve_mvt_tiles.py $WORK/chongqing.mbtiles 8092"
echo "设备转发:adb reverse tcp:8092 tcp:8092"
