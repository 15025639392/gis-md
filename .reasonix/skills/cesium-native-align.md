---
name: cesium-native-align
description: 按模块逐个对齐 cesium-native — 读源码、读测试、改实现、验证
---
# cesium-native 对齐协议

执行对齐一个模块时，必须严格按以下步骤，不得跳跃或合并。

## 前置准备

1. 读取 `/Users/ldy/Desktop/work/gis-md/ALIGNMENT_CHECKLIST.md`，定位目标模块的逐项状态
2. 读取 `/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md`，按算法名定位 cesium-native 源文件
3. 读取该模块关键 cesium-native 源文件（`.h` + `.cpp`）和对应的 `test/Test*.cpp`
4. 读取本项目对应文件的当前实现

## 执行规则（优先级由高到低）

### 1. 测试驱动
cesium-native 的测试是行为规格。从 `test/Test*.cpp` 提取：
- 输入参数和设置
- 期望输出（数值、状态、异常）
- 边界条件（空、零、越界、反子午线、极区）
- 数值容差
- 状态变量语义

将这些 case 转写为本项目的测试，再修改实现。

### 2. 不留债
发现问题必须一次性修复。禁止：
- 小修小补留 TODO
- 硬编码数值（如 256 tileSize、特定容差）
- 仅在当前场景验证，不覆盖边界
- 不更新 ALIGNMENT_CHECKLIST.md 状态

### 3. 每一步都要验证
- `./test_native.sh --ctest -R <test_target>` 编译 + 通过
- 涉及性能变化时记录可测指标
- 涉及手势/交互时记录交互契约

### 4. 输出格式
每个模块对齐完成后，在最终回复输出：
- 目标体验/策略
- 关键取舍（与 cesium-native 的差异点及理由）
- 参考依据（cesium-native 文件:行号）
- 验证方式（通过的测试名）
- 性能影响判断
- 更新 `ALIGNMENT_CHECKLIST.md` 对应项状态

## 模块对齐顺序建议

1. **CesiumGeometry** — Rectangle 修复（已有发现），包围体/裁剪验证
2. **CesiumQuantizedMeshTerrain** — 3 个测试失败修复（零三角 mesh/padding/校验）
3. **Cesium3DTilesSelection** — FrameResourceBudget 平滑公式 + 9 个测试泄漏
4. **CesiumGeospatial** — S2CellBoundingVolume / GlobeAnchor
5. **CesiumGltf/CesiumGltfReader** — Draco/MeshOpt 解压
6. **Cesium3DTilesContent** — I3DM/PNTS/CMPT
7. **CesiumAsync** — SqliteCache/SharedAssetDepot
8. **CesiumUtility** — AttributeCompression

## 参考路径

- cesium-native 源码: `/Users/ldy/Desktop/work/cesium-native/`
- cesium-native 索引: `AI_INDEX.md`
- 本项目源码: `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/`
- 本项目测试: `/Users/ldy/Desktop/work/gis-md/scaffold/tests/unit/`
- 对齐清单: `/Users/ldy/Desktop/work/gis-md/ALIGNMENT_CHECKLIST.md`
- 测试运行: `./test_native.sh`（需在 `scaffold/` 目录）

## 对齐终止条件

- ✅ 全部 `✅` 或 `🔶→✅`（关键项已对齐，非关键项可标记技术债但必须更新清单）
- ✅ 编译全通过
- ✅ 该模块涉及的所有测试通过（排除重构前已存在的失败）
