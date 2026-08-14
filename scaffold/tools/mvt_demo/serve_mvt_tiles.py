#!/usr/bin/env python3
"""从 mbtiles 起本地 MVT 瓦片服务器(demo P4 底图源)。

GET /{z}/{x}/{y}.pbf → mbtiles tiles 表(TMS y 翻转)。tippecanoe 写入的
tile_data 是 gzip 压缩 pbf,原样返回(引擎 MvtDecoder 透明解压,不设
Content-Encoding 头避免 curl 提前解压——两边都能解,原样最省)。

用法:
  python3 serve_mvt_tiles.py <chongqing.mbtiles> [port=8092]
真机需:adb reverse tcp:8092 tcp:8092
"""

import os
import sqlite3
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class TileHandler(BaseHTTPRequestHandler):
    db_path = None
    _local = threading.local()

    def _conn(self):
        if getattr(self._local, "conn", None) is None:
            self._local.conn = sqlite3.connect(
                f"file:{self.db_path}?mode=ro", uri=True)
        return self._local.conn

    def do_GET(self):
        parts = self.path.lstrip("/").split("/")
        if len(parts) != 3 or not parts[2].endswith(".pbf"):
            self.send_error(404)
            return
        try:
            z = int(parts[0])
            x = int(parts[1])
            y = int(parts[2][:-4])
        except ValueError:
            self.send_error(404)
            return
        tms_y = (1 << z) - 1 - y  # XYZ → TMS
        row = self._conn().execute(
            "SELECT tile_data FROM tiles WHERE zoom_level=? AND "
            "tile_column=? AND tile_row=?", (z, x, tms_y)).fetchone()
        if row is None:
            self.send_error(404)
            return
        data = row[0]
        self.send_response(200)
        self.send_header("Content-Type", "application/x-protobuf")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        # 默认静默;MVT_LOG=1 开逐请求日志(消费方去重对拍用:数同一
        # z/x/y 被拉几次)。
        if os.environ.get("MVT_LOG"):
            sys.stderr.write(fmt % args + "\n")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    TileHandler.db_path = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8092
    meta = dict(sqlite3.connect(TileHandler.db_path).execute(
        "SELECT name, value FROM metadata").fetchall())
    print(f"serving {sys.argv[1]} on :{port} "
          f"(zoom {meta.get('minzoom')}-{meta.get('maxzoom')}, "
          f"bounds {meta.get('bounds')})")
    ThreadingHTTPServer(("127.0.0.1", port), TileHandler).serve_forever()


if __name__ == "__main__":
    main()
