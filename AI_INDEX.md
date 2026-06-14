# AI_INDEX — 3D Globe Rendering Engine (earth_engine)

> **Project root:** `scaffold/src/earth_engine/`  
> **Language:** C++17, GLSL ES 3.0, MSL  
> **Build system:** CMake, vcpkg  
> **Dependencies:** glm, stb_image, libcurl (optional), nlohmann_json (tests)  

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [core/geodesy — Ellipsoid, Cartographic, Transforms](#2-coregeodesy)
3. [core/math — Vec3, Mat4, BoundingSphere, OBB, Plane, Ray, Rectangle](#3-coremath)
4. [tiling/ — TileQuadTree, TileNode, TileScheme, TilePlan, SurfaceTile](#4-tiling)
5. [camera/ + scene/Camera — CameraController, Camera](#5-camera)
6. [scene/ — Scene, FrameState, Frustum, Diagnostics](#6-scene)
7. [renderer/ — Renderer, RenderDevice, RenderCommand, Shaders](#7-renderer)
8. [platform/android/ — RenderDeviceGLES](#8-platformandroid)
9. [layers/ — BasemapLayer, BasemapLayerStack, TerrainLayer, VectorLayer](#9-layers)
10. [environment/ — Atmosphere, SkyBox, SkyGradient, SunDirection](#10-environment)
11. [terrain/ — QuantizedMeshParser, TerrainTile](#11-terrain)
12. [providers/ — ImageryProvider, TerrainProvider, XYZ, QuantizedMesh](#12-providers)
13. [interaction/ — PickingService, InputManager, SelectionManager](#13-interaction)
14. [globe/ + Engine — GlobeMesh, Engine API](#14-globe--engine)
15. [debug/ + threading/ + style/ + data/ — Supporting Systems](#15-supporting-systems)
16. [Render Pipeline Order & Depth/Blend Settings](#16-render-pipeline-order)
17. [Cross-Subsystem Contracts](#17-cross-subsystem-contracts)
18. [Key Constants Table](#18-key-constants)

---

## 1. Architecture Overview

```
Engine
  └─ Scene
      ├─ Camera + CameraController
      ├─ BasemapLayerStack
      │    └─ BasemapLayer × N
      │         ├─ TileScheme (XYZ/TMS/OpenGlobus/Geographic)
      │         ├─ TileQuadTree → TilePlan
      │         ├─ ImageryProvider → TileTextureCache
      │         └─ SurfaceTileMesh cache
      ├─ TerrainLayer (optional)
      │    ├─ TileQuadTree → TilePlan
      │    └─ TerrainProvider → TerrainTile cache
      ├─ VectorLayer × N
      ├─ TimeController + SunDirection
      ├─ SkyGradient → clear color
      ├─ AtmosphereBackgroundPass
      ├─ SkyBox (procedural starfield)
      ├─ PickingService + SelectionManager
      ├─ InputManager
      └─ DebugOverlay
  └─ RenderDevice (abstract) → RenderDeviceGLES / RenderDeviceMetal
```

**Frame loop:** `Engine::render()` → `device->beginFrame()` (clear reverse-Z) → `Scene::update()` (camera, environment, tile plan, terrain) → `Scene::render()` (build commands: skybox → atmosphere → surface tiles → vectors → debug → sort → validate → submit) → `device->endFrame()`.

---

## 2. core/geodesy

### Cartographic.h / .cpp

| Item | Lines | Description |
|------|-------|-------------|
| `Cartographic` class | header | Longitude/latitude in **radians**, height in **meters** |
| `fromDegrees()` | header | Static factory: lng/lat in degrees |
| `fromRadians()` | header | Static factory: lng/lat in radians |
| `longitudeDegrees()` / `latitudeDegrees()` | .cpp:28-29 | Conversion kRadToDeg=180/π |
| Signed `operator<<` | .cpp:37-40 | Format: `Cartographic(lng:105°, lat:35°, h:1500m)` |

### Ellipsoid.h / .cpp

**Key constants:**
- `WGS84`: `(6378137.0, 6356752.314245)` — `.cpp:318` (static method returning WGS84 Ellipsoid)
- `kEpsilon1` = 1e-1 (center tolerance) — `.cpp:16`
- `kEpsilon12` = 1e-12 (convergence / Vincenty) — `.cpp:17`
- `kEpsilon15` = 1e-15 (ray-intersection discriminant guard) — `.cpp:18`

| Method | Lines | Algorithm |
|--------|-------|-----------|
| `cartographicToCartesian()` | .cpp:62-76 | Prime vertical radius of curvature: N = a/√(1-e²sin²φ). Then x=(N+h)cosφcosλ, y=(N+h)cosφsinλ, z=(N(1-e²)+h)sinφ |
| `cartesianToCartographic()` | .cpp:78-92 | Calls `projectToSurface()`, computes normal via `geodeticSurfaceNormal()`, uses `atan2(n.y,n.x)` for longitude, `asin(clamp(n.z,-1,1))` for latitude, signed height via dot product |
| `geodeticSurfaceNormal(Cartographic)` | .cpp:93-101 | Direct trig: `(cosλ·cosφ, sinλ·cosφ, sinφ)` |
| `geodeticSurfaceNormal(Vec3)` | .cpp:103-114 | ECEF normal via `n_i = ecef_i / radii_i²`, normalized with 1e-24 guard |
| `projectToSurface()` | .cpp:116-146 | **Newton-Raphson** on λ parameter: finds λ s.t. `f(λ)=Σ(x²·m_i²)-1=0` where `m_i=1/(1+λ·r_i¯²)`. First-pass ratio-based estimate, then ≤32 iterations. Converges at `\|f\|<1e-12` |
| `scaleToGeodeticSurface()` | .cpp:148-150 | Delegate to `projectToSurface()` (thin compatibility wrapper) |
| `rayIntersection()` | .cpp:152-191 | Scale to unit sphere via `oneOverRadii`, solve quadratic `\|o+t·d\|²=1` for t. Handles outside/inside/on-surface cases. `kEpsilon15` guards discriminant |
| `inverse()` | .cpp:193-256 | **Vincenty inverse** (iterative). Converges at `\|λ_prev-λ\|<1e-12`, max 1000 iterations. Distance via `b·A·(σ-Δσ)`. Returns `initialAzimuthRadians`, `finalAzimuthRadians`, `distanceMeters` |
| `direct()` | .cpp:258-318 | **Vincenty direct**. Same convergence criteria. Returns `destination` Cartographic, `finalAzimuthRadians` |

**Internal state:** `radii_`, `radiiSquared_`, `oneOverRadii_`, `oneOverRadiiSquared_`, `centerToleranceSquared_≈0.1`.

### Transforms.h / .cpp

| Method | Lines | Description |
|--------|-------|-------------|
| `ecefToEnu()` | .cpp:12-43 | Builds 4×4 rotation+translation matrix: East = `(-sinλ, cosλ, 0)`, North = `(-sinφ·cosλ, -sinφ·sinλ, cosφ)`, Up = `(cosφ·cosλ, cosφ·sinλ, sinφ)` |
| `enuToEcef()` | .cpp:45-47 | `ecefToEnu().inverse()` |
| `toRadians()` / `toDegrees()` | .cpp:7-12 | π/180 conversions |

---

## 3. core/math

### Vec3.h / .cpp (lines: header 67, .cpp 51)

Wraps `glm::dvec3`. Provides `x()`, `y()`, `z()`, `raw()` (glm access), arithmetic operators, `length()`, `lengthSquared()`, `normalized()`, `dot()`, `cross()`, `distanceTo()`.

### Mat4.h / .cpp (lines: header 91, .cpp 76)

Wraps `glm::dmat4`. Column-major. `operator*(Vec3)` does homogeneous multiply (`w=1`). Static factories: `identity()`, `translation()`, `scale()`, `rotationX/Y/Z()`. `inverse()` = `glm::inverse`.

### BoundingSphere.h (40 lines)

| Item | Description |
|------|-------------|
| `intersectPlane(Plane)` | Returns -1 (inside), +1 (outside), 0 (intersecting) — signed distance vs radius |
| `computeDistanceSquaredToPosition()` | 0 if inside sphere, else `(dist - radius)²` |
| `contains(Vec3)` | `\|pos - center\|² ≤ radius²` |

### OrientedBoundingBox.h (90 lines)

| Item | Description |
|------|-------------|
| Constructor | `center + axis0 + axis1 + axis2` define 8 corners |
| `intersectPlane(Plane)` | **Separating Axis Theorem**: projects each half-axis onto plane normal, compares effective radius vs signed distance |
| `isVisible(Plane[6])` | Tests all 6 frustum planes, returns false if any reports Outside |
| `computeDistanceSquaredToPosition()` | Clamp each local coordinate to [-halfExtent, +halfExtent] |
| `toSphere()` | Conservative bounding sphere: radius = |axis0 + axis1 + axis2| |

### Plane.h (50 lines)

Hessian Normal Form: `normal · point + distance = 0`. Static `ORIGIN_XY`, `ORIGIN_YZ`, `ORIGIN_ZX`.

### Ray.h / .cpp (lines: 38 + 15)

`origin + t * direction` (direction always normalized). `pointAt(t)`.

### Rectangle.h / .cpp (lines: 68 + 65)

| Item | Description |
|------|-------------|
| Internal units | Radians (construct `fromDegrees()`) |
| `crossesAntimeridian()` | `west > east` |
| `width()` | Handles antimeridian wrap: `(2π - west) + east` |
| `intersects()` | Antimeridian-crossing rectangles conservatively return true |

---

## 4. tiling

### TileKey.h (36 lines)

Composite key: `schemeId + z + x + y`. Custom `std::hash` with `0x9e3779b9` mixing.

### TileGroupKey.h (36 lines)

Composite grouping key: `schemeId + zoom + viewportWidth + viewportHeight`. Used by `BasemapLayerStack` for deduplication.

### TileScheme.h / .cpp (header 50, .cpp 290)

**Abstract base** with 4 concrete implementations:

| Implementation | `id()` | `yDirection()` | Max Zoom | Notes |
|----------------|--------|----------------|----------|-------|
| `XYZWebMercatorScheme` | `"XYZ-WebMercator"` | `"down"` | 22 | Standard XYZ, y=0 at north. Mercator → lat via `atan(sinh(π-2π·y))` |
| `TMSWebMercatorScheme` | `"TMS-WebMercator"` | `"up"` | 22 | y=0 at south. `y_tms = (2^z-1) - y_xyz` |
| `OpenGlobusEarthScheme` | `"OpenGlobus-Earth"` | `"down-grouped"` | 22 | 3 groups: Mercator + NorthPolar + SouthPolar. `y ∈ [0,3·2^z)`. Polar uses LonLat sampling |
| `GeographicTMSScheme` | `"Geographic-TMS"` | `"up"` | 22 | EPSG:4326. x = `(lng+180)/360·2^z`, y = `(lat+90)/180·2^z` |

**Key constant:** `kMaxWebMercatorLat = 1.4844222297453324` (limit ≈ 85.0511°).  
**Polar group layout:** `yGroup = floor(y / 2^z)`. Group `0` = Mercator, `1` = NorthPolar, `2` = SouthPolar.

### TilePlan.h (105 lines)

**Frame-derived candidate set** — not layer-specific. Key structs:

- `TileTransition` — `key, opacity, fadingNodeCount`
- `TileSelectionState` — `NotVisited → Rendered → Refined → Kicked`
- `TilePlan` — `frameId, zoom, minVisibleZoom, maxVisibleZoom, equalZoomApplied, visibleTiles[], tileTransitions[], selectionRecords[]` + counts for rendering/walkthrough/not-rendering nodes
- `LayerTilePlan` — Extends TilePlan with per-layer: `desiredTiles[], requestTiles[], renderTiles[], fallbackTiles[], kickedTiles[]`
- `TilePlanBuilder` — static `compute()` + `zoomLevelFromHeight()` + `parentKey()`

### TileQuadTree.h / .cpp (header 175, .cpp 1018)

**Core LOD selection engine.** State machine per node:

```
NotRendering ↔ Rendering ↔ Walkthrough
```

**`TileNode` key methods:**

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `ensureChildren()` | .cpp:524-550 | Creates 4 children via `TileKey{x*2, y*2}` and `childYForTile()`. Uses `openGlobusGroupBaseY()` / `openGlobusGroupForY()` for OpenGlobus scheme |
| `subtreeNodeCount()` | .cpp:553-560 | Recursive count of all descendant nodes |
| `isHorizonTangent()` | .cpp:582-584 | Dot product of bounding sphere center normal vs -cameraDir `< kOpenGlobusHorizonTangent (0.81)` |
| `shouldSubdivide()` | .cpp:586-625 | **SSE (Screen Space Error)** calculation: `geometricError × viewportHeight / (distance × 2 × tan(fov/2))`. Threshold from `maximumScreenSpaceError()`. Also checks `maxRenderedTilesForHeight()`, `minimumRenderableZoom()` |
| `markRenderingTransition()` | .cpp:627-650 | Cubic ease-out fade (0→1 over 0.3s). Tracks `fadingNodeCount` (0, 1 if parent fading, 4 if children fading) |
| `animateTransitionOpacity()` | .cpp:669-680 | Cubic ease-out `1 - (1-t)³`. 0.3s duration |
| `traverse()` | .cpp:683-700 | Entry point: calls `traverseImpl()`, accumulates stats |
| `traverseImpl()` | .cpp:706-800+ | Recursive LOD selection. Tests frustum culling (sphere + OBB), altitude visibility, SSE threshold. States: Rendering if no refine, Walkthrough if children exist with fade. Handles `neighborBalancedRendering` |

**Quad tree constants** (anonymous namespace, .cpp:28-68):

| Constant | Line | Value | Purpose |
|----------|------|-------|---------|
| `kOpenGlobusCurrentLodPixels` | 28 | 256.0 | LOD size basis in pixels |
| `kOpenGlobusMinLodPixels` | 29 | 512.0 | Min LOD pixels (oblique view adds) |
| `kOpenGlobusHorizonTangent` | 34 | 0.81 | Cosine threshold for horizon tangent |
| `kMaxRenderedNodes` | 36 | 1000 | Cap on total rendered tiles |
| `kCesiumNativeMaximumSse` | 41 | 12.0 | Base SSE threshold |
| `kCesiumNativeHighAltitudeMaximumSse` | 42 | 9.0 | SSE at 250-20000km |
| `kCesiumNativeSpaceMaximumSse` | 43 | 10.0 | SSE at > 20000km |
| `kCesiumNativeLowAltitudeMaximumSse` | 44 | 6.0 | SSE at < 50km |
| `kCesiumNativeMidAltitudeMaximumSse` | 45 | 8.0 | SSE at 50-250km |
| `kHighAltitudeMaxRenderedNodes` | 50 | 384 | Cap at 1M+ meters altitude |
| `kSpaceViewMaxRenderedNodes` | 51 | 256 | Cap at 8M+ meters altitude |
| `kFadeDuration` | 61 | 0.3 | Transition fade time in seconds |

**Key helper functions** (anonymous namespace):

| Function | Lines | Description |
|----------|-------|-------------|
| `openGlobusGroupBaseY()` | .cpp:72-78 | Computes base Y for OpenGlobus polar group |
| `openGlobusGroupForY()` | .cpp:80-88 | Maps tile Y to mercator/north/south group |
| `childYForTile()` | .cpp:90-100 | Child Y index considering scheme group |
| `tileCornerPoints()` | .cpp:108-130 | Computes 4 corner ECEF positions for a tile |
| `obbFromCorners()` | .cpp:136-157 | Builds OrientedBoundingBox from tile corners |
| `boundingSphereFor()` | .cpp:160-167 | Computes bounding sphere from corners |
| `cameraSlope()` | .cpp:172-180 | Horizon-relative camera angle in [0,1] |
| `openglobusLodSizePixels()` | .cpp:185-200 | LOD pixel size based on camera slope |
| `cesiumTerrainGeometricError()` | .cpp:208-220 | Geometric error for terrain tiles |
| `maxRenderedTilesForHeight()` | .cpp:225-240 | Altitude-based tile cap |
| `minimumRenderableZoom()` | .cpp:246-251 | Min zoom for rendering |
| `shouldApplyEqualZoom()` | .cpp:254-265 | Equal-zoom pass gate |
| `maximumScreenSpaceError()` | .cpp:270-300 | Altitude-dependent SSE threshold |
| `dedupeAndUpdateZoomStats()` | .cpp:311-330 | Removes duplicate TileKeys, updates zoom stats |
| `haveCommonSide()` | .cpp:338-355 | Checks if two tiles share an edge |
| `accumulateNeighborStats()` | .cpp:364-372 | Counts tile neighbor links |
| `applyNeighborBalancePass()` | .cpp:398-430 | Subdivides tiles where neighbor zoom differs >1 |

**SSE slope relaxation:** When `cameraSlope < 0.45`, SSE += 4.0; when `< 0.65`, SSE += 2.0 (in `maximumScreenSpaceError()`).

**Equal-zoom pass** (.cpp:254-265, applied at .cpp:958): Secondary traversal ensures all rendered tiles are at the same zoom level. Horizon-tangent tiles at the max zoom are preserved.

**Neighbor balance** (.cpp:398-430, applied at .cpp:1007): Post-pass subdivides tiles whose neighbor differs by >1 zoom level via `addNeighborBalancedChildren()`.

**Quad tree traversal flow** (`TileQuadTree::compute()`, .cpp:911-1018):
1. Ensure/create root nodes (8 roots arranged around the globe at zoom 0)
2. Reset all node frame states (`resetFrameState()`)
3. Traverse each root with `traverse()` → `traverseImpl()`
4. Deduplicate tiles and compute min/max zoom (`dedupeAndUpdateZoomStats`)
5. Equal-zoom pass (optional, gated by `shouldApplyEqualZoom`)
6. Collect rendered nodes via walkthrough-tree iteration
7. Neighbor balance pass (`applyNeighborBalancePass`)
8. Animate transitions (`updateTileTransitions`)
9. Final deduplication and stats
10. Return `TilePlan`

### SurfaceTile.h (140 lines)

GPU-oriented data structures:
- `SurfaceVertex` — `positionEcef, positionHighEcef, positionLowEcef` (double-split for GPU precision), `normalEcef, uv`
- `SurfaceTileMesh` — `vertices[], indices[], gridSize, winding, sampling, skirtMeta, waterMask`
- `SkirtMetadata` — `noSkirtIndicesBegin/Count, noSkirtVerticesBegin/Count` (cesium-native style)
- `WaterMask` — 256×256 RGBA8 or allLand/allWater flags
- `ImageryAttachment` — Per-tile imagery layer with `texture, uvOffset/Scale, opacity, fallbackSource`

### TileSurface.h / .cpp (header 75, .cpp 500+)

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `textureWindow()` | .cpp:98-138 | Computes UV offset/scale for parent-fallback texture mapping. Accounts for WebMercator-v vs Geographic-v sampling |
| `buildEllipsoidMesh()` | .cpp:140-180 | Regular grid of `(gridSize+1)²` vertices on WGS84 ellipsoid. Two triangles per cell. **WebMercator-v vs Geographic-v** sampling based on bounds |
| `buildTerrainMesh()` | .cpp:183-478 | First checks if terrain tile has raw QuantizedMesh data → direct triangulation (preserves QM's optimized mesh). Otherwise builds regular grid + samples height from TerrainTile. **Equalize border vertices** with parent tile. **Per-vertex normals** via central differences. **Skirt** via cesium-native sorted-edge triangle strips |
| `buildNormalMap()` | .cpp:481-510 | Encodes ECEF normals → RGBA8: `rgb = normal * 0.5 + 0.5` |

**Key skirt algorithm** (.cpp:349-410):
- Skirt height = `5.0 × maxGeometricError × tileBounds.width()` (.cpp:366-367)
- maxGeometricError = `ellipsoid.getMaximumRadius() * 0.25 / 65.0` (cesium-native `calcQuadtreeMaxGeometricError`, .cpp:361)
- Overlap offset = `0.0001 × tile angular extent`
- Edge sort order: West (south→north), South (east→west), East (north→south), North (west→east)
- `addSkirtEdge()` lambda (.cpp:386+) builds bottom-of-skirt vertices offset by `skirtHeight` below surface, creates triangle strip between surface-edge and skirt-edge vertices

### TileRequestScheduler.h / .cpp (header 95, .cpp 110)

Priority queue-based async tile loader. Min-heap for `priority` (0=highest). `maxConcurrent=4`. Uses `CancellationToken` per request group. `requestTile()` checks if already in-flight or pending with higher priority.

### LayerTilePlan.h / .cpp (header 105, .cpp 40)

`TilePlanGroupBuilder::computeGrouped()` — computes one TilePlan per unique `TileGroupKey` (scheme+zoom+viewport).

### CrsProfile.h / .cpp (header 70, .cpp 60)

Two implementations: `WebMercatorProfile` (EPSG:3857, ±20037508.34m) and `WGS84GeographicProfile` (EPSG:4326, ±180°/±90°).

---

## 5. Camera

### Camera.h / .cpp (header 60, .cpp 170)

| Item | Lines (.cpp) | Description |
|------|-------------|-------------|
| Constructor | .cpp:18-31 | `pos=(0,0,7e6)`, `dir=(0,0,-1)`, FOV=60°, near=1.0, far=1e12 (reverse-Z). `target_` initialized to NaN to signal "no target set" |
| `setView()` | .cpp:33-38 | Sets position, direction, up. Invalidates target to NaN |
| `lookAt()` | .cpp:40-44 | Sets position → target → direction via `setOrientation(target-position, up)` |
| `setPerspective()` | .cpp:46-55 | Validates FOV in (0,π), near/far: 0<near<far |
| `viewMatrix()` | .cpp:56-66 | Uses target if set (not NaN), else position+direction. Wraps `glm::lookAt` |
| `projectionMatrix()` | .cpp:74-94 | **Reverse-Z** with `GL_GEQUAL` depth function. `P[2][2]=near/(far-near)`, `P[2][3]=-1`, `P[3][2]=far*near/(far-near)`. Depth: 1m→1.0, 1e12m→0.0 |
| `viewProjectionMatrix()` | .cpp:100-102 | `projectionMatrix() * viewMatrix()` |
| `frustum()` | .cpp:106-109 | `Frustum::fromViewProjection(viewProjectionMatrix())` |
| `getPickRay()` | .cpp:113-130 | Unprojects NDC x,y using inverse VP matrix. Near=1m→ndc_z=1, Far=1e12→ndc_z=0. Returns normalized Ray |
| `getHeight()` | .cpp:134-137 | Camera altitude above WGS84 ellipsoid via `cartesianToCartographic(position_).height()` |
| `getNormalMatrix()` | .cpp:142-155 | 3×3 upper-left of view matrix, column-major, 9 floats |
| `setOrientation()` | .cpp:158-167 | Normalizes direction, computes right=cross(direction,up), recomputes up=cross(right,direction) |

### CameraController.h / .cpp (header 120, .cpp 620)

**Anchor-based drag controller** — aligns with OpenGlobus `TouchNavigation`. Key constants in anonymous namespace:
- `kMaxInertiaAngularVelocityRadPerSec = 5.0` — `.cpp:21`
- `kInertiaDampingPerSecond = 3.0` — `.cpp:22`
- `kVelocitySmoothing = 0.35` — `.cpp:23`
- `kMinAltitudeMeters = 50.0` — `.cpp:26`
- `kMinDistanceEarthRadii` — `.cpp:28` (≈ 1.000008)
- `kMaxDistanceEarthRadii = 30.0` — `.cpp:31`
- `kTouchJerkLimit = 0.3` — `.cpp:32`
- `kTouchInertiaDecayStep = 0.007` — `.cpp:33`
- `kTouchMinSlope = 0.1` — `.cpp:34`
- `kPinchIntentThresholdPixels = 4.0` — `.cpp:35`
- `kPinchTiltThresholdPixels = 10.0` — `.cpp:36`
- `kPinchTiltRadiansPerPixel = 0.0015` — `.cpp:37`
- `kPinchTiltMaxStepRadians = 0.08` — `.cpp:38`
- `kPinchRotateThresholdRadians = 0.003` — `.cpp:39`
- `kPinchAnchorFollow = 0.12` — `.cpp:40`

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| Constructor | .cpp:89-97 | Default camera: Chongqing (106.508°E, 29.617°N, 1500m) |
| `onDragStart()` | .cpp:116-128 | Grabs surface point via pick ray. Records start position + timestamp |
| `onDragMove()` | .cpp:130-137 | `applyAnchorDrag()` — rotates globe so grabbed point follows finger |
| `onDragEnd()` | .cpp:139-145 | Starts inertia if final velocity > threshold. Computes angular velocity from last drag segment |
| `onPinchGesture()` | .cpp:146-230 | 3DTilesRendererJS-style: zoom (clamped scale), rotate (`rotationRadians`), tilt (`centerDeltaY`). Uses `kPinchIntentThresholdPixels`, `kPinchRotateThresholdRadians`, `kPinchTiltThresholdPixels` |
| `update()` | .cpp:292-340 | Inertia damping via `exp(-kInertiaDampingPerSecond * dt)`. Clamps to `kMaxInertiaAngularVelocityRadPerSec`. Orbit mode applies rotation quaternion to camera. Calls `clampEyeAltitude` after each move |
| `viewDistance()` | .cpp:379-395 | Places camera `distanceMeters` from target along surface normal, clamped to `[kMinDistanceEarthRadii, kMaxDistanceEarthRadii]` |
| `clampEyeAltitude()` | .cpp:484 | Enforces `kMinAltitudeMeters` above terrain/ellipsoid via terrain height function |

**Inertia model:** Exponential damping: `velocity *= exp(-kInertiaDampingPerSecond * dt)`, capped at `kMaxInertiaAngularVelocityRadPerSec`. Velocity smoothing via `kVelocitySmoothing` IIR filter on drag deltas.

**Pinch intent classification:** Scale vs rotation vs tilt. Scale wins if `|log(scale)| > 0.015`. Rotation wins if `|rotation| × 300 > verticalPixels × 1.25`. Tilt activates when `|centerDeltaY| > kPinchTiltThresholdPixels`.

---

## 6. Scene

### Scene.h / .cpp (header 160, .cpp 906)

**Scene** owns all subsystems:

| Subsystem | Lines (.cpp) | Key details |
|-----------|-------------|-------------|
| Constructor | .cpp:55-73 | Creates Camera, CameraController, DebugOverlay, InputManager, PickingService, SelectionManager, TimeController, SkyGradient, AtmosphereBackgroundPass, SkyBox. Globe mesh `createMesh(96, 48)`. Calls `configureCameraSurfacePicker()`, `setupSelectionCallbacks()`, `setupInputCallback()` |
| `setRenderDevice()` | .cpp:81-115 | Creates `Renderer`, initializes debug overlay, atmosphere pass, skybox. Handles null device (context loss) |
| `update()` | .cpp:119-200 | Updates controller, environment (sun direction, sky gradient → clear color), layer stack, terrain. Runs `updateSurfaceCommandDiagnostics()` |
| `render()` | .cpp:201-500+ | 6 phases: (1) SkyBox command, (2) Atmosphere command, (3) SurfaceTile commands from layer stack, (4) Vector layer commands, (5) Debug overlay commands, (6) Sort + validate + submit. Diagnostics captured per frame |
| `pick()` | .cpp:627-708 | Ellipsoid + terrain height + vector feature picking. Returns nearest PickResult |
| `configureCameraSurfacePicker()` | .cpp:710-760 | Injects terrain-aware surface picker + terrain height clamp into CameraController |

**Additional methods:** `updateInteractionFocus()`, `pickInteractionFocus()`, `onInputEvent()`, `onHover()`, `onSelect()`, `clearSelection()`, `addLayer()`, `removeLayer()`, `moveLayer()`, `addVectorLayer()`, `removeVectorLayer()`, `setTerrainLayer()`, `setTerrainEnabled()`, `setViewport()`, `setTime()`, `advanceTime()`, `sunDirection()`, `setDebugOverlayEnabled()`, `debugOverlayEnabled()`.

**FrameState.h** (80 lines):
- `lightDir` — sun direction (ECEF unit vector)
- `clearR/G/B/A` — from SkyGradient
- `hasInteractionFocus` — focus direction for view-importance tile prioritization
- `Diagnostics` — 60+ fields for FPS, draw calls, tile counts, timing breakdowns

### Frustum.h / .cpp (header 90, .cpp 140)

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `fromViewProjection()` | .cpp:23-71 | Extracts 6 frustum planes from VP matrix (Gribb-Hartmann). **Reverse-Z**: Near plane = `row3` (z_clip > 0), Far plane = `row4 - row3` (z_clip < w_clip) |
| `containsPoint()` | .cpp:76-83 | All 6 planes test with epsilon tolerance |
| `intersectsSphere()` | .cpp:85-102 | Signed distance vs radius+epsilon for all 6 planes |
| `intersectsOBB()` | .cpp:104-112 | Delegates to `obb.intersectPlane(plane)` per plane |
| `computeVisibility(BoundingSphere)` | .cpp:115-128 | Returns CullingResult: `Outside` (dist < -r), `Intersecting` (dist < r), `Inside` |
| `computeVisibility(OBB)` | .cpp:130-142 | Same tri-state for OBB via `obb.intersectPlane()` result |

**CullingResult** enum: `Outside=-1`, `Intersecting=0`, `Inside=1`. Used by `TileNode::traverseImpl()` for subtree culling optimization: when parent is `Inside`, children skip plane tests.

---

## 7. renderer

### RenderDevice.h (145 lines)

Abstract interface with `Backend` enum (`Metal`, `OpenGLES`, `Vulkan`). Resource desc structs: `TextureDesc`, `BufferDesc`, `ShaderDesc`, `FramebufferDesc`. Resource base classes: `Texture`, `Buffer`, `ShaderProgram`, `Framebuffer`.

### RenderCommand.h / .cpp (header 130, .cpp 130)

**Command kind order** (`mvpRenderOrder()`):
```
SkyBackground      = 0
AtmosphereBackground = 5  (currently via sorted kind)
GlobeSurface        = 10
SurfaceTile         = 10
VectorOverlay       = 30
DebugOverlay        = 40
```

**Key state fields:**
- `depthFunction`: `GreaterEqual` (reverse-Z) or `Always` (SurfaceTile)
- `depthTest`: bool, `depthWrite`: bool
- `blend`: bool, `blendSrc`/`blendDst`
- `hasSurfaceTileUniforms` — hot path with fixed arrays (avoids per-tile allocation)

**Validation** (`validateMvpRenderCommands()`):
- SurfaceTile must have `depthFunction=Always`, `depthWrite=true`, non-zero `generation`
- GlobeSurface must have `depthFunction=GreaterEqual, depthWrite=true, blend=false`
- AtmosphereBackground must have `depthFunction=GreaterEqual, depthWrite=false, blend=true`
- All non-Unknown commands must be `pass="color"`

### Renderer.h / .cpp (header 100, .cpp 370)

**Shader sources (GLSL ES 3.0 + MSL):**

| Shader | Built-In | Key Features |
|--------|----------|--------------|
| **Globe vertex** | `.cpp:16-30` | `u_model` + `u_modelViewProjection`, passes `v_normal` and `v_texcoord` |
| **Globe fragment** | `.cpp:32-48` | Procedural ocean/land: `ocean=(0.05,0.26,0.58)`, `land=(0.18,0.48,0.24)`. Diffuse lighting `0.22 + coef * 0.88` |
| **Tile vertex** | `.cpp:55-76` | Computes camera-relative position, geodetic normal via `kInvRadiiSq`. UV via `u_tileUV + a_texcoord * u_tileUV.zw` |
| **Tile fragment** | `.cpp:78-105` | Texture color → saturation boost `*1.08` → contrast `*1.06` → diffuse `*(0.45+diffuse*0.55)` → optional water mask blend → **exponential fog** `exp(-(d·density)²)` → opacity |
| **Instanced tile vertex** | `.cpp:107-158` | Instance attributes: `tileRect`, `textureRect`, `localOrigin`, `cameraRelativeOrigin`, corner positions. Grid UV from shared unit-quad vertex buffer |
| **Color shader** (vector) | `.cpp:184-198` | Simple `a_position` + `u_modelViewProjection` × `u_color` |
| **Tile shared geometry** | `.cpp:228-247` | `makeTileGeometry(32)` — 33×33 unit grid, 2 triangles per cell |

**Static earth model matrix:** `.cpp:365-371` — `scale(6378137.0)`.

**SurfaceTile command builder** (`.cpp:314-346`): 
- vertex stride = 20 (pos 12 + uv 8, normal computed in shader)
- `depthFunction = Always`
- `depthWrite = true`
- Only blends when `surfaceTileOpacity < 0.999` or `surfaceTransitionOpacity < 0.999`

### TileTextureCache.h / .cpp (header 80, .cpp 90)

**LRU cache** — `std::list` + `std::unordered_map`. Default capacity: 64 MB. `get()` splices to front; `put()` evicts LRU to make room. Key format: `cacheDomain/schemeId/z/x/y`.

---

## 8. platform/android

### RenderDeviceGLES.h / .cpp (header 110, .cpp 520)

**GL wrappers:**

| Class | Lines | Description |
|-------|-------|-------------|
| `GLTexture` | .h:87-97, .cpp:22-28 | Holds `GLuint id`, deletes on destructor |
| `GLBuffer` | .h:99-110, .cpp:30-40 | Holds `GLuint id` + `target` (ARRAY/ELEMENT), deletes on destructor |
| `GLShaderProgram` | .h:112-125, .cpp:42-59 | Compile vs + fs → link program. Caches uniform locations |

**`submit()`** (.cpp:140-370): Per-command state tracking to minimize GL calls:
- Caches: `currentProgram`, `currentArrayBuffer`, `currentElementArrayBuffer`, `currentTexture0/1`, 12 attrib enables, `depthTestEnabled`, `depthWriteEnabled`, `depthFuncCur`, `blendEnabled`, `cullFaceEnabled`
- SurfaceTile instanced: sets up 12 attrib pointers (grid UV + 9 instance attribs)
- SurfaceTile non-instanced: vertex stride 20 (pos3 + uv2)
- Globe: vertex stride 32 (pos3 + normal3 + texcoord2)
- **Reverse-Z**: `glDepthFunc(GL_GEQUAL)`, `glClearDepthf(0.0f)`, `glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)`
- Blending: `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`
- Polygon offset: `glPolygonOffset(-1.0f, -1.0f)` for blend-enabled commands

**`beginFrame()`** (.cpp:120-138): Restores state: `glDepthMask(GL_TRUE)`, disables BLEND, disables POLYGON_OFFSET_FILL, clears with `glClearColor(0.1f, 0.3f, 0.6f, 1.0f)` (TODO: pass frameState clear color).

---

## 9. layers

### BasemapLayer.h (180 lines)

**Key data structures:**
- `SurfaceGpuMesh` — vertex buffer, index buffer, waterMaskTexture, `localOriginEcef`, metadata
- `ImageryAtlas` — atlas texture, per-tile slot tracking, LRU eviction
- `PendingUpload` — deque for async decode → GPU upload (shared_ptr guarded)

**State machine per tile:**
```
Missing → Requested (async) → [decode] → PendingUpload → Texture cached → Ready
                                                            ↓
                                                       Parent-fallback active while loading
```

**Key methods (in .cpp, 2070 lines):**
| Method | Description |
|--------|-------------|
| `update()` | If standalone: computes TilePlan via TileQuadTree. Orchestrates plan → request → upload |
| `applyPlan()` | Injects shared TilePlan from BasemapLayerStack. Drives `rebuildLayerPlan()` |
| `loadMissingTiles()` | Iterates layerPlan_.desiredTiles, checks cache, issues `loadTile()` |
| `buildRenderCommands()` | For each RenderTileRef: finds SurfaceGpuMesh, creates RenderCommand with texture, UV transform, opacity, uniforms |
| `getOrCreateSurfaceGpuMesh()` | Cache of `SurfaceGpuMesh` keyed by tile + terrain generation. Builds mesh via `TileSurface::buildEllipsoidMesh()` or `buildTerrainMesh()` |
| `findFallbackTexture()` | Walks up parent chain for parent-fallback imagery |
| `applyAncestorMeetsSseFallback()` | cesium-native: retains ancestor tiles when children haven't finished loading |
| `applyCesiumNativeKicking()` | cesium-native: removes tiles from render set when loading has stalled too long |

**Imagery atlas:** 2D atlas texture, tile-sized slots, LRU replacement. Enables batching of multiple tile textures into a single GL texture.

### BasemapLayerStack.h / .cpp (header 120, .cpp 190)

**`update()` flow** (.cpp:59-138):
1. Group layers by `schemeId`
2. For each group: check cached plan validity (camera movement threshold: `positionDelta > 2.0m` or `directionDot < 0.99995`)
3. If camera moving, reuse plan on even frames; recompute on odd frames
4. Compute via `TileQuadTree::compute()`
5. Apply plan to all layers in group via `layer->applyPlan()`
6. All layers call `loadMissingTiles()`

**CachedSchemePlan** — stores TilePlan + viewport + camera position/direction. Invalidates on viewport change or camera movement.

### TerrainLayer.h / .cpp (header 125, .cpp 230)

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `update()` | .cpp:86-145 | Processes pending uploads. Recomputes TilePlan (alternates with basemap: terrain on odd frames). Prioritized tile requests via `terrainRequestPriority()` |
| `sampleHeight()` | .cpp:48-50 | Delegates to `findBestTile()` → bilinear interpolation |
| `requestCandidatesForFrame()` | .cpp:171-218 | Sorts visible tiles by priority: `centerPenalty × distance × 0.6 + anchorPenalty × distance × 0.4 + z × 1000` |
| `isTilePossiblyAvailable()` | .cpp:227-238 | Walks ancestor chain checking `emptyTiles_` set (cesium-native availability) |

**In-flight limits:** max 96-128 tiles. Max requests per update: 2-4. Upload budget: 1-3ms/frame.

### VectorLayer.h / .cpp (header 120, .cpp 420)

**Tessellation methods:**

| Type | Algorithm | Lines (.cpp) |
|------|-----------|-------------|
| Point | Screen-space billboard: 4 vertices oriented to camera | .cpp:225-240 |
| LineString | Subdivided arc on ellipsoid surface (16 segments between control points) | .cpp:242-266 |
| Polygon | **Ear-clipping** triangulation in local ENU tangent plane. Falls back to fan triangulation | .cpp:268-320 |

**`buildRenderCommands()`** (.cpp:315-420): Per-feature, creates vertex/index buffers (dynamic, per-frame `frameBuffers_`), sets color shader uniforms, emits `VectorOverlay` RenderCommand.

---

## 10. Environment

### AtmosphereParameters.h (120 lines)

**Default Earth parameters** (aligned with OpenGlobus `DEFAULT_PARAMS`):
- `atmosHeight=100000m`, `rayleighScaleHeight=7994m`, `mieScaleHeight=1200m`
- `rayleighSeaLevelScattering=0.056`, `mieSeaLevelScattering=0.0045`
- `groundAlbedo=0.022`
- Rayleigh coefficients (3.9, 10.8, 20.0) × 1e-6 × sea level scattering
- Mie scattering=1.6e-6, extinction=1.85e-6
- Ozone center=25000m, half-width=15000m
- `sunAngularRadius=0.04685`, `sunIntensity=0.78`

**Mars parameters** also defined.

### AtmosphereBackgroundPass.h / .cpp (header 65, .cpp 250)

**Fullscreen quad pass** with physical scattering shader:
- View ray reconstructed from camera basis: `rayDir = normalize(forward + right×ndcX×tan(fov/2) + up×ndcY×tan(fov/2))`
- **Ray-sphere intersection** with inner (planet) and outer (atmos shell) spheres
- **8-sample optical depth integration** along view ray: Rayleigh scale height=8000m, Mie=1200m
- Rayleigh phase: `0.75 × (1 + cos²θ)`
- Mie phase: Henyey-Greenstein `g=0.76`, softened for mobile
- **Sun disk**: angular radius, limb darkening, corona glow
- **Horizon glow**: exponential with screen-space y-coordinate
- **Space factor**: smoothstep between 120km and 900km altitude
- **State**: `depthTest=true`, `depthWrite=false`, `blend=true` (SrcAlpha, OneMinusSrcAlpha)
- Command kind: `AtmosphereBackground` (order 20 in pipeline)

### SkyBox.h / .cpp (header 100, .cpp 210)

**Procedural starfield** with two modes:
1. Cubemap (if paths provided) — not yet fully implemented
2. **Procedural starfield** — built-in GLSL shader with:
   - 3-layer artistic stars (large/medium/accent) at different scales
   - Star tinting: warm (1.0, 0.82, 0.48) to cool (0.55, 0.78, 1.0) to rose
   - Diamond + halo + ray spike star shapes
   - Painted galactic ribbon with Perlin noise
   - Horizon fade + night gradient
   - `xyww` depth trick: `gl_Position = clipPos.xyww` forces far plane

**State:** `depthTest=false`, `depthWrite=false`, `blend=true`  
**Night factor:** 0=transparent (day), 1=full stars (night). Computed as `exp(sunElevation × 8)` for night, `spaceFactor` for high altitude.

### SkyGradient.h / .cpp (header 65, .cpp 210)

**CPU-side Rayleigh + Mie scattering** (no LUT):
- Computes `zenithColor`, `horizonColor`, `ambientColor` from sun direction and camera altitude
- Rayleigh phase: `3/(16π) × (1 + cos²θ)`
- Mie phase: Henyey-Greenstein g=0.76
- Optical depth integral: `τ = β₀ × H × (exp(-h_start/H) - exp(-h_end/H))`
- Slant path via Chapman function: `τ_slant = τ_vertical / cos(zenith)`
- **Sunrise/sunset**: warm tone enhancement when elevation < 17°
- **Night ramp**: `exp(sunElevation × 8)` darkening below -0.05 rad

### SunDirection.h / .cpp (header 55, .cpp 85)

**Meeus approximation** (stjarnhimlen.se tutorial, matches OpenGlobus exactly):
- Computes sun ECEF direction from Julian Date
- Steps: days since J2000 → mean anomaly → eccentric anomaly → ecliptic coordinates → equatorial rotation → sidereal rotation → ECEF
- Accuracy: ~0.5°

### TimeController.h / .cpp (header 45)

Unified time source: `julianDate()` (TT), `unixTimestamp()`, `advanceSeconds()`. Static helpers: `unixToJulian()`, `julianToUnix()`, `currentJulianDate()`.

---

## 11. Terrain

### TerrainTile.h / .cpp (header 45, .cpp 65)

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `sampleHeight()` | .cpp:14-55 | Bilinear interpolation on heightmap grid. Applies **skirt adjustment** for Mapbox Terrain-RGB (514×514 → data in [1,512]). No-data fallback to parent. OpenGlobus `skipPositiveHeights` at low zoom |

### QuantizedMeshParser.h / .cpp (header 55, .cpp 300)

**Quantized-Mesh-1.0** parser. Binary format: header (92 bytes) + U/V/Height zigzag-delta uint16 arrays + triangle indices (zigzag-delta, 16 or 32 bit) + edge indices + extensions.

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `parseAndRasterize()` | .cpp:14-155 | Parses QM, rasterizes irregular triangulation onto **regular grid** via barycentric point-in-triangle test. Nearest-vertex fallback for outside-mesh points |
| `parseToSurfaceTileMesh()` | .cpp:157-310 | Direct conversion preserving QM's optimized triangulation. Parses extensions: **oct-encoded normals** (ID=1), **water mask** (ID=2). Builds ECEF vertices, face-averaged or oct normals, cesium-native skirt |

**Header format:** 11×8 bytes (doubles) + 1×4 bytes (uint32 vertexCount) = 92 bytes. Optional 4-byte padding to 96 bytes.  
**Extension IDs:** 1 = oct-encoded normals, 2 = water mask (65536 bytes RG or 1 byte allLand/allWater).

---

## 12. Providers

### ImageryProvider.h (55 lines)

Abstract base: `id()`, `schemeId()`, `minZoom()`, `maxZoom()`, `tileWidth()`, `tileHeight()`, `buildUrl(key)`, `supportsTile(key)`, `providerKeyForTile(key)` (for grouped-Y mapping), `requestTile()` (async with CancellationToken), `decodeTile()`.

### XYZImageryProvider.h / .cpp (header 95, .cpp 170)

**URL template** with replacements: `{z}`, `{x}`, `{y}`, `{groupedY}`, `{tileGroup}` (mercator/north/south), `{s}` (subdomain 0-3 or 1-4).

Provider maps logical TileKey → provider TileKey via `providerKeyForTile()`. HTTP via `PlatformBridge` (Android JNI) or libcurl. Decode via `stb_image`.

### TerrainProvider.h (70 lines)

Abstract base: `schemeId()`, `minZoom/maxZoom`, `tileSize()`, `buildUrl()`, `requestTile()` (async), `decodeTile()`. Defines `DecodedHeightmap` struct with `heights[]`, `noDataValues`, `rawData` (preserved for QM triangulation), `sampleBilinear()`.

### QuantizedMeshTerrainProvider.h / .cpp (header 80, .cpp 170)

URL replacement: `{z}`, `{x}`, `{y}` (with optional flipY). HTTP via `PlatformBridge` or libcurl. **Shared LRU cache** (`HttpCache::shared()`). Decodes via `QuantizedMeshParser::parseAndRasterize()` and preserves `rawData`. Default grid size: 65 (64×64 height samples).

### HeightmapTerrainProvider.h (header file)

Basic heightmap terrain provider (not yet read; referenced in filesystem tree).

---

## 13. Interaction

### InputEvent.h (100 lines)

**Normalized input event.** Fields: `type` (PointerDown/Move/Up, PinchStart/Move/End, Cancel, Key), `screenX/Y` (physical pixels, origin top-left), `devicePixelRatio`, `pointerType` (Touch/Mouse/Pen), `buttons`, `pointerCount`, `modifiers` (shift/ctrl/alt/meta), `timestamp` (monotonic seconds), pinch data (`pinchScale`, `rotationRadians`, `centerDeltaX/Y`, `hasPointerPair`, `pointer0/1 X/Y`).

### InputManager.h / .cpp (header 100, .cpp 200)

**Gesture recognizer** with state machine:
```
Idle → OneFingerPending → OneFingerDrag (threshold 8px)
     → TwoFingerWaiting → TwoFingerZoom or TwoFingerRotate
```

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `process()` | .cpp:25-91 | Routes pointer and pinch events through state machine |
| `beginTwoFinger()` | .cpp:148-165 | Waits for intentional movement before locking to Zoom or Rotate (3DTilesRendererJS style) |
| `updateTwoFinger()` | .cpp:167-185 | Classifies intent: scale vs rotate, filters events |
| `finishPointerGesture()` | .cpp:117-143 | Detects click (no drag), double-click (350ms window, 8px threshold) |

### PickingService.h / .cpp (header 70, .cpp 220)

| Method | Lines (.cpp) | Description |
|--------|-------------|-------------|
| `pick()` | .cpp:27-104 | Tests ellipsoid hit + vector feature intersection. Polygon: Möller–Trumbore ray-triangle. LineString: closest point on ray-segment (Eberly formula with parameter clamping + tolerance 10× pixel size) |
| `pickEllipsoid()` | .cpp:106-122 | `Ellipsoid::rayIntersection()` |
| `pickTerrain()` | .cpp:124-152 | Ellipsoid hit then queries `terrainSampler()` for height |
| `rayTriangleIntersection()` | .cpp:11-37 | **Möller–Trumbore** algorithm |

### SelectionManager.h / .cpp (header 100, .cpp 100)

Manages hover/selected feature sets. Callback-driven: updates VectorLayer styles without geometry rebuild. Supports: `onSelect()` (replace), `onSelectAdd()` (Shift), `onSelectToggle()` (Ctrl/Meta).

---

## 14. Globe + Engine

### Globe.h / .cpp (header 35, .cpp 70)

**`createMesh(96, 48)`:** WGS84 ellipsoid tessellation with `polarRadiusRatio=0.9966471893352525`. Vertices from south pole to north pole. CCW winding. Normals computed from ellipsoid gradient `(2x, 2y, 2z/prr²)`.

### Engine.h / .cpp (header 125, .cpp 140)

**Top-level API.** Wraps Scene + RenderDevice lifecycle. Legacy drag methods convert to InputEvent.

`render()` flow:
1. Auto delta-seconds (steady_clock fallback 1/60)
2. `device->beginFrame()` (clear reverse-Z)
3. `scene->update(deltaSeconds)`
4. `scene->render()` (build + sort + validate + submit)
5. `device->endFrame()`

---

## 15. Supporting Systems

### DebugOverlay.h / .cpp

LayerTilePlan-aware debug overlay: green = exact, amber = parent fallback, cyan = requested/missing, red = desired-only.

### threading/CancellationToken.h

Cancellation token for async tile requests. `createChild()` for scoped cancellation.

### data/GeoJsonParser.h / .cpp

Parses GeoJSON to `GeoFeature` list with rings (Cartographic arrays).

### style/OverlayStyle.h / .cpp

Styled geometry types: `PointStyle`, `LineStyle`, `PolygonStyle`. Interaction state overrides: `InteractionStyleOverride` with `colorShift` and `scaleMultiplier`.

### platform/bridge/PlatformBridge.h

Abstract interface for HTTP GET + image decode. Android implementation in `AndroidPlatformBridge.cpp` (JNI). iOS uses `CurlPlatformBridge`.

### core/cache/HttpCache.h / PersistentCache.h

Shared LRU HTTP cache for terrain provider.

### core/async/AsyncSystem.h

Thread pool for async terrain tile requests.

---

## 16. Render Pipeline Order

| Phase | Kind | Depth Test | Depth Func | Depth Write | Blend | Cull Face |
|-------|------|------------|------------|-------------|-------|-----------|
| 1. SkyBox | `SkyBackground` | false | n/a | false | true (SrcAlpha/OneMinusSrcAlpha) | false |
| 2. Atmosphere | `AtmosphereBackground` | true | GreaterEqual | false | true | false |
| 3. Globe mesh | `GlobeSurface` | true | GreaterEqual | true | false | true |
| 4. Surface tiles | `SurfaceTile` | true | **Always** | true | conditional* | true |
| 5. Vector overlays | `VectorOverlay` | true | GreaterEqual | (default) | (default) | (default) |
| 6. Debug overlay | `DebugOverlay` | (per config) | — | — | — | — |

**SurfaceTile depthFunction=Always**: Surface tiles are the authoritative terrain coverage. Parent/child fallback and transition tiles are nearly coplanar, so they write depth but do not reject each other by reverse-Z comparison. *Blend is enabled only when `surfaceTileOpacity < 0.999` or `surfaceTransitionOpacity < 0.999`.*

**Reverse-Z clear:** `glClearDepthf(0.0f)` — 0 = farthest. `glDepthFunc(GL_GEQUAL)` — greater depth = closer.

---

## 17. Cross-Subsystem Contracts

### TilePlan flow

```
Camera (position, direction, FOV, viewport)
  ↓
TileQuadTree::compute()
  → SSE calculation per node
  → Frustum culling (BoundingSphere + OBB)
  → Altitude visibility (horizon culling)
  → Equal-zoom pass (optional)
  → Neighbor balance pass
  ↓
TilePlan (visibleTiles[], zoom, transitions)
  ↓ per-group (BasemapLayerStack)
LayerTilePlan (desiredTiles[], requestTiles[], renderTiles[], fallbackTiles[])
  ↓
BasemapLayer::loadMissingTiles()
  → ImageryProvider::requestTile(key)
  → async decode → TileTextureCache::put()
  ↓
BasemapLayer::buildRenderCommands()
  → findSurfaceGpuMesh() → TileSurface::buildEllipsoidMesh() or buildTerrainMesh()
  → makeSurfaceTileCommand() → RenderCommand
```

### Terrain integration

```
TerrainLayer::update()
  → TileQuadTree::compute() (alternates with basemap on odd frames)
  → TerrainProvider::requestTile()
  → QuantizedMeshParser::parseAndRasterize() + preserve rawData
  ↓
TerrainTile::sampleHeight() — used by:
  1. PickingService::pickTerrain()
  2. CameraController terrainHeightFunc (camera floor clamp)
  3. BasemapLayer::getOrCreateSurfaceGpuMesh() — TileSurface::buildTerrainMesh()
```

### Camera interaction flow

```
InputEvent (platform) → InputManager → Gesture callback
  ↓
Scene::setupInputCallback()
  → CameraController::onDragStart/Move/End
  → CameraController::onPinchGesture
  ↓
  → Camera::setView()
  → Camera::lookAt()
```

### Environment → FrameState

```
TimeController (Julian Date)
  ↓
SunDirection::compute(jd) → Vec3 sunDirECEF
  ↓
SkyGradient::update(sunDir, cameraAltitude)
  → zenithColor, horizonColor, ambientColor
  ↓
FrameState.lightDir = sunDir
FrameState.clearR/G/B = horizonColor
```

### Pick → Selection

```
Scene::pick(screenX, screenY)
  → PickingService::pickTerrain() (ellipsoid + height)
  → PickingService::pick() (vector feature ray intersection)
  → nearest result wins
  ↓
Scene::onHover() / onSelect()
  → SelectionManager → callback
  → VectorLayer::setFeatureState() → style override
```

---

## 18. Key Constants

| Constant | Value | File |
|----------|-------|------|
| WGS84 a | 6378137.0 | `Ellipsoid.cpp:318` |
| WGS84 b | 6356752.314245 | `Ellipsoid.cpp:318` |
| WGS84 f | 1/298.257223563 | implicit |
| Earth polar radius ratio | 0.9966471893352525 | `Globe.h:33` |
| Max WebMercator lat | 1.4844222297453324 rad (~85.0511°) | `TileScheme.cpp:15` |
| kMinAltitudeMeters | 50.0 | `CameraController.cpp:26` |
| Near plane | 1.0 | `Camera.cpp:26` |
| Far plane | 1e12 | `Camera.cpp:27` |
| Reverse-Z near depth | 1.0 (GL_GEQUAL: greater wins) | `Camera.cpp:74-94` |
| Reverse-Z far depth | 0.0 (clear value) | `RenderDeviceGLES.cpp:125` |
| Default FOV | 60° | `Camera.cpp:24` |
| SSE thresholds | 6.0/8.0/9.0/10.0/12.0 | `TileQuadTree.cpp:41-45` |
| Max rendered tiles | 1000 | `TileQuadTree.cpp:36` |
| High-altitude tile cap | 384 | `TileQuadTree.cpp:50` |
| Space-view tile cap | 256 | `TileQuadTree.cpp:51` |
| Fade duration | 0.3s | `TileQuadTree.cpp:61` |
| Drag threshold | 8px | `InputManager.h:86` |
| Double-click window | 0.35s | `InputManager.h:87` |
| Tile texture cache | 64 MB | `TileTextureCache.h:20` |
| Tile grid size | 33×33 (32 segments) | `Renderer.cpp:559` |
| Globe segments | 96×48 | `Scene.cpp:71` |
| Skirt height factor | 5.0 × maxGeometricError | `TileSurface.cpp:366-367` |
| maxGeometricError formula | R × 0.25 / 65.0 | `TileSurface.cpp:361` |
| Overlap offset factor | 0.0001 × tile extent | `TileSurface.cpp:349-410` |
| Atmosphere height | 100,000 m | `AtmosphereParameters.h:12` |
| Rayleigh scale height | 7994 m | `AtmosphereParameters.h:15` |
| Mie scale height | 1200 m | `AtmosphereParameters.h:18` |
| Sun angular radius | 0.04685 rad | `AtmosphereParameters.h:63` |
| Sun intensity | 0.78 | `AtmosphereParameters.h:66` |
| Inertia damping | 3.0 per second (exponential) | `CameraController.cpp:22` |
| Max inertia angular velocity | 5.0 rad/s | `CameraController.cpp:21` |
| Touch inertia decay step | 0.007 | `CameraController.cpp:33` |
| Pinch tilt threshold | 10.0 px | `CameraController.cpp:36` |
| Pinch tilt max step | 0.08 rad | `CameraController.cpp:38` |
| Pinch rotate threshold | 0.003 rad | `CameraController.cpp:39` |
| Pinch intent threshold | 4.0 px | `CameraController.cpp:35` |
| Velocity smoothing | 0.35 (IIR filter) | `CameraController.cpp:23` |
| Max terrain inflight | 96-128 | `TerrainLayer.cpp:109` |
| Terrain per-frame upload | 1-2 tiles | `TerrainLayer.cpp:245` |
| Terrain upload budget | 1-3 ms | `TerrainLayer.cpp:247` |
| kJ2000 (Julian Date) | 2451545.0 | `SunDirection.cpp:20` |
| Obliquity J2000 | 23.4392911° | `SunDirection.cpp:21` |
| AU to meters | 1.4959787e11 | `SunDirection.cpp:22` |

---

## Known TODOs & Issues

| File | Issue |
|------|-------|
| `RenderDeviceGLES.cpp:352` | TODO: pass `frameState.clearR/G/B` from Engine after `beginFrame()` reorder |
| `AtmosphereBackgroundPass.cpp` | Not yet tied into optical-depth LUT optimization |
| `SkyBox.cpp` | Cubemap loading is platform-specific — texture set to nullptr (in shader setup) |
| `BasemapLayer.cpp` (general) | Imagery atlas has placeholder implementation; single-texture path dominant |
| `Camera.cpp:113-130` | `getPickRay()` unproject uses reverse-Z NDC convention tied to camera near/far |
| `TileQuadTree.cpp` (general) | Multiple altitude/SSE constants tuned empirically for specific test cases |
