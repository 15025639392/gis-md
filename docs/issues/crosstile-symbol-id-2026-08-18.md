# crossTile 符号 id 跨换代稳定继承——立项(2026-08-18)

> 判据锚点:**V29**(northstar/vector.md)。血缘:V27 排查的残余
> (placed 集与桶 entry 集连环换代下错开一代)+ V13(换代无闪)的机制面。
> 本文=立项定案:现状量化、maplibre 基准、缺口对照、分刀方案。**冻结文档,
> 收官后状态更新走 northstar,不回写此文。**

## 1. 现状(自家已有什么,真机量化)

`FeatureRenderLayer::crossTileIdFor(name, lonRad, latRad, tileZ)`:
name FNV 哈希分桶 → 桶内逐条比锚点,容差 1.5×MVT 量化格(z 级瓦 2π/(2^z·4096)),
命中继承 id、细 zoom 升级锚点参考;miss 分配新 id。条目只增不淘汰(16384 哨兵)。
fade/避让状态在 `LabelPlacement::fades_`,按该 id 键 —— 等价 maplibre 的
`opacities` 表,**id 稳则状态自动直通**(V24 硬闪修的"bake 烘入现值"也靠它)。

**真机量化(重庆,冷启动+pinch 放大缩回逼两次换代)**:
hit≈130-190,miss≈390-450(其中首见名 ~343 = 冷启动正常新建);
**sameNameMiss=42 ≈ 22% 的同名匹配尝试没吸住**。机制大体在工作,断链集中在
"同名但锚点挪出容差"——首要嫌疑=线标注(路名)锚点取瓦内几何弧长中点,
换代切分一变中点大挪,亚米级容差必然吸不住。

## 2. maplibre 基准(.ref/maplibre-gl-js,源码调研 2026-08-18)

`src/symbol/cross_tile_symbol_index.ts` + `placement.ts`:

| 机制 | 做法 |
|---|---|
| 匹配键 | `key=murmur3(text)` 分桶 + 锚点缩到统一 zoom 的 **~4px 粗网格**(`roundingFactor=512/EXTENT/2`),容差=矩形 ±1 格(≈12px 窗);粗代匹配细代时容差按 `2^Δz` 放大。**feature id 完全不参与** |
| 同名多实例(路名) | 全部实例进列表(>128 上 KDBush);**`zoomCrossTileIDs` 认领集做 1:1 贪心分配**——一个旧 id 每轮只许被一个新实例认领(#5993),吸不住/被抢光的拿新 id |
| 双代并存谁赢 | 瓦片按 **overscaledZ 降序**进 placement(新细代先),`seenCrossTileIDs` 首见即占——同 id 旧代实例被跳过;被逐出的旧瓦 `holdingForFade` 保留一个 fade 窗,其符号强制"未放置"渐隐,**且不占 seen 槽** |
| 生命周期 | `removeStaleBuckets` 每帧按"当前保留瓦集合"清陈旧索引条目;同 tileKey 新桶到达即释放旧 id 供复用认领 |
| 跨代携带的状态 | 只有 `crossTileID` 本身;`opacities` / `variableOffsets` / `placedOrientations` 三张表在 Placement 侧按它键控,`commit()` 时 `prev→next` 直通(fade 中途换代也连续) |

## 3. 缺口对照(我家 vs maplibre)

| # | 缺口 | 后果(已观测) |
|---|---|---|
| G1 | **无 1:1 认领**:同名多实例全部匹配同一 entry → 多个新实例共享一个 id | 路名多段共 id → placement 只显一个、fade 状态互踩 |
| G2 | **无双代去重/胜负规则**:新旧桶并存时同 id(或同名)两份 entry 同时进候选 | V27 残余直接机制:老 entry 靠 id 小 tie-break 压同名新 entry,老桶 drop 后新 entry 停在 collided=0(已被 V27 的 drop→重 placement 缓解,但"谁赢"仍未定义) |
| G3 | **容差窗过窄且各向同性**:1.5 量化格(亚米)vs maplibre 12px 窗 | sameNameMiss≈22%;线标注弧长中点漂移必 miss |
| G4 | **无生命周期**:条目只增不淘汰 | 16384 哨兵后需 LRU;陈旧条目还会抢走本该给新代的匹配 |

## 4. 分刀方案(按依赖序;工时=AI 协作基准,均为小时级)

- **刀1 G3 容差窗改造**:匹配格改 maplibre 式"缩到统一 zoom 的屏幕等效粗网格
  (~4px)±1 格矩形窗",粗细代按 2^Δz 缩放。预期直接吃掉大部分 sameNameMiss。
  判据:同场景 sameNameMiss 率 22%→<5%。
- **刀2 G1 认领集**:每次桶 commit 的匹配 pass 加 per-pass claimed set,
  1:1 贪心;判据:同名多实例各持独立 id(host 单测:两实例两 id,换代后各继承各的)。
- **刀3 G2 双代胜负**:placement 候选按 id 去重,细代(高 tileZ)优先;
  旧桶 drop 前其候选不占位(对齐 holdingForFade 语义的最小版)。
  判据:V27 残余复现法(冷启动静置逼连环换代)placed 集 ⊆ 现桶 entry 集恒成立。
- **刀4 G4 生命周期**:随瓦桶 drop 释放其 entries 的可认领性 + 上限 LRU;
  判据:跨区域漫游 entries 有界(哨兵不再触发)。

不动的:name 哈希分桶(与 murmur3(text) 同构)、fades_ 按 id 键控(等价
opacities,勿重造)、V24 bake 烘入现值(依赖 id 稳,刀 1-3 落地后收益放大)。

## 5. 已判死 / 勿再提

- **feature id 参与匹配键**:maplibre 明确不用(逐瓦 feature id 不跨瓦稳定),
  我家 MVT 同理。
- **全局最优二分图匹配**(替代贪心认领):maplibre 用首见贪心 + 排序即够,
  最优匹配复杂度不换收益。
- **在 placement 侧继续打补丁修断链**:V27 排查已证是错层(三补丁只软化,
  placed/vis 仍错开)。
