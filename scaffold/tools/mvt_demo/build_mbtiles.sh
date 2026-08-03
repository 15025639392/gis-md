#!/bin/bash
# 重庆 OSM → chongqing.mbtiles(demo P4 底图数据管线)。
# 前置:
#   1. china-latest.osm.pbf(geofabrik)
#   2. pip3 install --user osmium
#   3. tippecanoe 二进制(源码 make,见 tmp/tippecanoe)
# 用法:build_mbtiles.sh <china-latest.osm.pbf> <工作目录> [tippecanoe路径]
set -e

SRC="$1"
WORK="$2"
TIPPECANOE="${3:-tippecanoe}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$WORK"
if [ ! -f "$WORK/roads.geojsonseq" ]; then
    echo "== 提取 OSM 三图层(bbox 106.2,29.3,106.9,29.9)=="
    python3 "$SCRIPT_DIR/extract_chongqing_geojson.py" "$SRC" "$WORK"
fi

echo "== tippecanoe 切瓦片(z0-14,分层)=="
"$TIPPECANOE" -o "$WORK/chongqing.mbtiles" --force \
    -Z 0 -z 14 \
    --drop-densest-as-needed \
    --extend-zooms-if-still-dropping \
    -L roads:"$WORK/roads.geojsonseq" \
    -L water:"$WORK/water.geojsonseq" \
    -L building:"$WORK/building.geojsonseq"

echo "== 完成:$WORK/chongqing.mbtiles =="
