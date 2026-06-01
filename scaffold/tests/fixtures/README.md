# 测试 Fixtures 目录

本目录存放地球引擎测试所需的数据文件。

## 文件列表

| 文件 | 用途 | 对应 test-fixtures.md 章节 |
|------|------|---------------------------|
| `beijing.geojson` | 北京坐标点 (WGS84) | 坐标转换 Fixtures → 北京近似点 |
| `polygon_hole.geojson` | 带洞 polygon | GeoJSON Fixtures → Polygon With Hole |
| `antimeridian.geojson` | 跨反经线矩形 | 反经线 Fixtures |
| `high_latitude.geojson` | 高纬度点 (北纬 85°) | 高纬 Fixtures |

## 使用方式

```cpp
// 在 GoogleTest 中加载 fixture 文件
#include <fstream>
#include <nlohmann/json.hpp>

std::ifstream f("tests/fixtures/beijing.geojson");
nlohmann::json fixture = nlohmann::json::parse(f);
```
