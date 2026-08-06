# AI_INDEX — 3D Globe Rendering Engine (earth_engine)

> **Project root:** `scaffold/src/earth_engine/`
> **Language:** C++17, GLSL ES 3.0, MSL
> **Build system:** CMake, vcpkg
> **Dependencies:** glm, stb_image, libcurl (optional), nlohmann_json (tests)
> **Architecture:** cesium-native–aligned `Tileset` + glTF-content 3D-Tiles model (migrated from the removed BasemapLayerStack/TileQuadTree/SurfaceTileMesh model). Interaction & atmosphere aligned to openglobus.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [core/geodesy — Ellipsoid, Cartographic, Transforms, Projections, S2](#2-coregeodesy-ellipsoid-cartographic-transforms-projections-s2)
3. [core/math — Vec3, Mat4, bounding volumes, culling, intersection](#3-coremath-vec3-mat4-bounding-volumes-culling-intersection)
4. [core/async, cache, resources, net, gltf](#4-coreasync-cache-resources-net-gltf)
5. [tiling — Tileset: selection / traversal / LOD](#5-tiling-tileset-selection-traversal-lod)
6. [tiling — content lifecycle / loading / caching / GPU upload](#6-tiling-content-lifecycle-loading-caching-gpu-upload)
7. [tiling — raster overlay mapping](#7-tiling-raster-overlay-mapping)
8. [tiling — glTF geometry to GPU render prep (+ content loaders)](#8-tiling-gltf-geometry-to-gpu-render-prep-content-loaders)
9. [providers — imagery + terrain + raster overlay tile providers](#9-providers-imagery-terrain-raster-overlay-tile-providers)
10. [terrain — TerrainTile, DecodedHeightmap](#10-terrain-terraintile-decodedheightmap)
11. [camera — Camera, CameraController](#11-camera-camera-cameracontroller)
12. [scene — Scene, coordinators, FrameState, Frustum, render pipeline](#12-scene-scene-coordinators-framestate-frustum-render-pipeline)
13. [renderer — Renderer, RenderDevice, RenderCommand, streaming, texture](#13-renderer-renderer-renderdevice-rendercommand-streaming-texture)
14. [platform — GLES, Metal, platform + curl bridges](#14-platform-gles-metal-platform-curl-bridges)
15. [layers — RasterOverlay, ActivatedRasterOverlay, VectorLayer, CreditSystem](#15-layers-rasteroverlay-activatedrasteroverlay-vectorlayer-creditsystem)
16. [environment — Atmosphere, SkyBox, SkyGradient, Sun, Time](#16-environment-atmosphere-skybox-skygradient-sun-time)
17. [interaction — InputManager, PickingService, SelectionManager](#17-interaction-inputmanager-pickingservice-selectionmanager)
18. [Engine + sdk](#18-engine-sdk)
19. [Render Pipeline Order & Depth/Blend](#19-render-pipeline-order-depthblend)
20. [Cross-Subsystem Contracts](#20-cross-subsystem-contracts)
21. [Key Constants Table](#21-key-constants-table)

---

## 1. Architecture Overview

The `earth_engine` module (`/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine`) is a C++17 3D globe/terrain rendering engine. Shaders are authored in GLSL ES 3.0 (Android/GLES) and MSL (iOS/macOS/Metal). The build is driven by CMake with vcpkg-managed dependencies; the core third-party libraries are glm (math), stb_image (image decode), libcurl (network/tile fetch), and nlohmann_json (glTF / tileset / layer JSON parsing).

### Component tree

```
Engine  (Engine.h/.cpp — thin lifecycle + surface + input facade)
  owns RenderDevice*  (injected; platform GLES or Metal)
  └── Scene  (scene/Scene.h/.cpp — owns Camera, CameraController, Renderer, SceneRenderPipeline)
        ├── SceneTilesetCoordinator      → primary terrain Tileset + N content Tilesets
        ├── SceneLayerCoordinator        → VectorLayer stack (points/lines/polygons, feature state)
        ├── SceneInteractionCoordinator  → input / picking / hover / selection
        ├── SceneEnvironmentCoordinator  → TimeController, SunDirection, SkyGradient, SkyBox, AtmosphereBackgroundPass
        ├── SceneTelemetryCoordinator    → Diagnostics, engine timing, PresentationTrace
        └── SceneFrameUpdateCoordinator / SceneRenderPipeline  (per-frame update + command build)
        │
        └── Tileset(s)  (tiling/Tileset.h/.cpp — cesium-native Tileset equivalent)
              ├── TileScheme                 → QuadtreeTilingScheme / OctreeTilingScheme (geometric error, bounds)
              ├── selection / traversal      → TileSelection* policies + TileSelectionTraversalExecutor
              │                                (frustum/occlusion/fog culling, SSE refinement, kick)  ──▶ TilePlan
              ├── content lifecycle          → TileContentLifecycleManager, TileLoadQueue,
              │                                TileContentRuntime, GpuUploadQueue (async CPU→GPU),
              │                                TileContentCacheManager (byte-budget LRU)
              ├── RasterOverlay mapping       → ActivatedRasterOverlay → RasterMappedToTilesetTile,
              │                                 upsampled-child coordination, per-tile texture upload
              └── glTF render prep            → GltfContentProvider → GltfModel →
                                                GltfRenderGeometryBuilder / GltfRenderResourcePreparer →
                                                RenderCommand (all terrain renders via the glTF path)
              │
              ├── Providers  (providers/) — feed tile content
              │     ├── TerrainProvider: Heightmap
              │     └── ImageryProvider: XYZ / TMS / WMS / WMTS / BingMaps / GoogleMapTiles / Debug
              │         → RasterOverlayTileProvider → RasterOverlayTile
              └── Content loaders  (content/) — GltfContentProvider, EllipsoidTerrainContentProvider,
                    GltfTerrainUpsampler

Renderer  (renderer/) — RenderCommand list model, streaming/stable command set
  └── RenderDevice  (abstract) ──┬── RenderDeviceGLES   (platform/android — GLSL ES 3.0)
                                 └── RenderDeviceMetal  (platform/ios — MSL)

SDK entry:  sdk/EarthEngineSdkFacade — installs terrain + raster overlays + glTF content + camera/time
            into an existing Engine from an EarthSceneConfig.
```

### Frame loop

`Engine::render(deltaSeconds)` runs a fixed four-phase pipeline, each phase timed into `Diagnostics`. First it calls `device->beginFrame()`, which sets up the render target and issues a reverse-Z depth clear (near=150, far=1e12 per-camera defaults). Then `Scene::update()` runs the `SceneFrameUpdateCoordinator`: it advances the camera/controller, updates the environment (time → sun direction → sky gradient), and drives `Tileset::update()` — the selection traversal that culls and refines the tile tree into a `TilePlan`, enqueues async content loads, drains the GPU upload queue, and evicts against the byte budget. Next `Scene::render()` invokes the `SceneRenderPipeline`, which builds the frame's `RenderCommand` list in a strict order: sky → atmosphere → tile/glTF surface commands (with a full-globe fallback command only when no terrain surface is present) → vector layers → apply MVP uniforms → depth/blend sort → validate → `renderer.submit(commands)` → `releaseRenderReferences()` (reference counts keep tiles alive until the GPU has consumed them). Finally `device->endFrame()` presents the drawable, and `Scene::finishEngineFrame()` records the total CPU frame cost.

### Design philosophy

The engine is built on extreme decomposition: rather than a few large classes, responsibilities are split into a large number of small, single-responsibility policy / coordinator / runner / manager classes (the `tiling/` directory alone holds ~268 files, e.g. one class each for root selection, refinement, kick, culling, reuse, plan-append, and traversal-context building). This is deliberate — the tiling and selection logic is a faithful port of cesium-native's traversal and load algorithms, and fine-grained decomposition preserves that algorithmic fidelity while keeping each piece independently unit-testable. The interaction, camera, and atmosphere/sky subsystems are instead aligned to openglobus (reverse-Z planet camera defaults, atmosphere and sky-gradient model), giving a hybrid lineage: cesium-native for tiling/streaming, openglobus for camera and environment.

### Recent architecture migration

The engine was migrated away from its original custom globe stack — `BasemapLayerStack` and `TileQuadTree` (both **removed**) — to a cesium-native-aligned `Tileset` + glTF-content 3D-Tiles model. Terrain now flows through the glTF render path: heightmap terrain is converted into glTF render geometry and drawn as `GltfPrimitive` render commands, rather than through a bespoke surface-mesh renderer.

Update (2026-07-01): the `GlobeMesh` fallback and the `SurfaceTileMesh`/`TileSurface` geometry path were **removed** (build + 137/137 tests green). What this changed:
- `GlobeMesh` / `Globe.{h,cpp}` / `GlobeVertex` — **deleted**. `Renderer::initialize()` no longer takes a mesh or builds globe buffers/shader; `makeGlobeCommand` and the `RenderCommandKind::GlobeSurface` kind are gone; `SceneRenderPipeline` no longer prepends a fallback-globe command. **Behavior change**: before the first terrain tiles stream in, the view now shows only the clear color (no fallback blue globe).
- `SurfaceTileMesh` / `SurfaceTileMeshWinding` / `SurfaceNormalMap` / `SurfaceTileSampling` — **deleted** from `SurfaceTile.h`. `TileSurface`'s mesh builders (`buildEllipsoidMesh`/`buildTerrainMesh`/`upsampleChildMeshFromParent`/`buildNormalMap`) are gone; the ellipsoid-grid generation was **inlined** into `EllipsoidTerrainContentProvider::buildEllipsoidGrid` (linear-latitude sampling, same ECEF/normal/uv/high-low split). `TileSurface.{h,cpp}` was **kept trimmed** to only the still-live raster-overlay UV helpers (`computeTranslationAndScale`, `textureWindowForNorthWestUv`, `TileTextureWindow`).
- `TileRenderContentState` — `surface_.mesh` + `setSurfaceMesh()`/`surfaceMesh()`/`mutableSurfaceMesh()`/`hasSurfaceMesh()`/`hasTerrainMesh()`/`clearLegacySurfacePayloadPreservingGltfMetadata()` removed; `TileTerrainHeightRangePolicy::applyMeshOrHeightmapRange(SurfaceTileMesh*)` removed. `SurfaceVertex`/`RasterOverlayDetails`/`WaterMask`/`SkirtMetadata` in `SurfaceTile.h` are **retained** (still the glTF vertex + raster-overlay types).
- Sections §8/§12/§13/§14/§18/§19 below were regenerated against the post-refactor source and reflect the removal. A follow-up pass also **deleted** the now-dead `Diagnostics` fields `globeFallbackCommands`/`globeFallbackMaskedTerrainEntries` and `terrainParentFallbackMeshes`/`terrainTransitionSurfaceMeshes`/`ellipsoidSurfaceMeshes` (only ever zeroed, no live readers). Still-live counters (`surfaceMeshCount`/`surfaceMeshBytes`, `terrainSurfaceTileCommands`, `terrainSurfaceCommandsSubmitted`, `quadtree*`, mercator/polar) were kept.

---

## 2. core/geodesy — Ellipsoid, Cartographic, Transforms, Projections, S2

### Cartographic.h / .cpp

Geodetic coordinate (lng/lat = **radian**, height = **meter**). cesium-native `Cartographic` equivalent.

| Item | Lines | Description |
|---|---|---|
| Storage: `lng_`,`lat_`(rad),`height_`(m) | .h:39-41 | Ctor takes radians (.h:13); degrees via factory |
| `fromRadians` / `fromDegrees` | .h:17-19 / .cpp:6-12 | Degrees path uses `MathUtils::degreesToRadians` |
| `longitude/latitude/height` accessors | .h:24-26 | Raw radian/meter getters |
| `longitudeDegrees` / `latitudeDegrees` | .cpp:14-20 | `radiansToDegrees` conversion |
| `operator==` / `!=` | .cpp:22-28 | Exact `==` on all three fields (no epsilon) |
| `operator<<` | .cpp:30-33 | Debug stream in degrees + meters |

### Ellipsoid.h / .cpp

WGS84 reference ellipsoid; cartographic ↔ ECEF (**meters**), Vincenty geodesics. cesium-native `Ellipsoid` + `EllipsoidGeodesic` equivalent.

| Method | Lines | Algorithm / units |
|---|---|---|
| Ctors (a,b) / (rx,ry,rz) | .cpp:24-36 | Precomputes `radiiSquared/oneOverRadii/oneOverRadiiSquared`; `f_=(rx-rz)/rx`, `e2_=2f-f²` |
| `maximumRadius`/`minimumRadius` | .cpp:38-44 | max/min of radii components |
| `cartographicToCartesian` | .cpp:46-54 | normal·radiiSquared scaled by 1/√(n·k), + normal·height |
| `cartesianToCartographic` | .cpp:56-59 | Wraps `try…`; center → `(0,0,0)` fallback |
| `tryCartesianToCartographic` | .cpp:61-79 | Scale to surface → `atan2/asin` on normal; center → `nullopt` |
| `geodeticSurfaceNormal(Cartographic)` | .cpp:81-88 | `(cosLat·cosLng, cosLat·sinLng, sinLat)` normalized |
| `geodeticSurfaceNormal(Vec3)` | .cpp:90-96 | `ecef·oneOverRadiiSquared` normalized |
| `tryScaleToGeodeticSurface` | .cpp:107-168 | Newton iteration on λ, converges when `|func|` ≤ **`kEpsilon12`**; small-norm branch uses **`kEpsilon1`** |
| `scaleToGeodeticSurface`/`projectToSurface` | .cpp:98-105 | Wrap try…; center → `Vec3::zero()` |
| `tryScaleToGeocentricSurface` | .cpp:175-191 | Closed-form β scale; `‖p‖≤kEpsilon12` → `nullopt` |
| `rayIntersection` | .cpp:193-200 | origin + dir·entryDistance from interval |
| `rayIntersectionInterval` | .cpp:202-260 | Unit-sphere quadratic in scaled space; q²>1/<1/=1 branches; cesium `rayEllipsoid` semantics (entry/exit params) |
| `inverse` (Vincenty) | .cpp:262-330 | Iterates λ to **`kEpsilon12`**, cap 1000 iters; returns distance + start/end azimuth (`normalizeTwoPi`) |
| `direct` (Vincenty) | .cpp:332-393 | σ iteration to `kEpsilon12`, cap 1000; returns destination Cartographic + final azimuth |
| `WGS84()` | .cpp:395-398 | **a** = 6378137.0, **b** = 6356752.3142451793 |
| `UNIT_SPHERE()` | .cpp:400-403 | radii (1,1,1); cesium `Ellipsoid::UNIT_SPHERE` |

Constants: **`kEpsilon1`** = 1e-1 (.cpp:10), **`kEpsilon12`** = 1e-12 (.cpp:11); geodesic non-convergence sentinel `sinSigma < 1e-24` (.cpp:288). `GeodesicInverseResult`/`GeodesicDirectResult` structs (.h:10-21).

### Transforms.h / .cpp

Coordinate/frame transforms: up-axis rotations, TRS matrices, reverse-Z projections, ENU↔ECEF. cesium-native `CesiumGeometry::Transforms` + `GlobeTransforms` equivalent.

| Item | Lines | Algorithm / units |
|---|---|---|
| `X/Y/Z_UP_TO_*` static mats | .cpp:37-89 | Fixed 90° axis-swap `dmat4`s |
| `getUpAxisTransform(from,to)` | .cpp:91-116 | Dispatch to the six mats; identity on match |
| `createTranslationRotationScaleMatrix` | .cpp:118-134 | `mat3_cast(rot)·diag(scale)` + translation column |
| `computeTranslationRotationScaleFromMatrix` | .cpp:136-173 | Column-length decompose; det<0 flips rot+scale sign |
| `toRadians`/`toDegrees` | .cpp:175-181 | `·π/180` and inverse |
| `createPerspectiveMatrix` (fov) | .cpp:183-204 | Vulkan reverse-Z; `zFar==inf` → 0 / `zNear` limit |
| `createPerspectiveMatrix` (l/r/b/t) | .cpp:206-228 | Off-center reverse-Z frustum |
| `createOrthographicMatrix` | .cpp:230-252 | Vulkan reverse-Z ortho |
| `createViewMatrix` | .cpp:254-275 | forward=-dir, side=up×forward; look-at |
| `eastNorthUpToFixedFrame` | .cpp:277-314 | Default WGS84; origin-zero and pole special cases; else east=norm(-y,x,0), north=up×east |
| `ecefToEnu` / `enuToEcef` | .cpp:316-324 | WGS84 cartographic→ECEF then ENU-frame (inverse for ecefToEnu) |

Constant: **`kEpsilon14`** = 1e-14 (.cpp:12), used for zero/pole detection. `UpAxis` enum X/Y/Z (.h:13-17).

### Projection.h / .cpp

`Projection = std::variant<GeographicProjection, WebMercatorProjection>` (.h:17); free-function visitors. cesium-native `Projection` variant equivalent.

| Item | Lines | Description |
|---|---|---|
| `projectPosition`/`unprojectPosition` | .cpp:9-39 | `std::visit` dispatch to concrete project/unproject |
| `projectRectangleSimple`/`unproject…` | .cpp:41-73 | Visitor over `Rectangle` overloads |
| `projectRegionSimple` | .cpp:75-85 | Rectangle→`AxisAlignedBox` carrying min/max height |
| `unprojectRegionSimple` | .cpp:87-98 | `AxisAlignedBox`→`BoundingRegion` (rectangle + z-range) |
| `computeProjectedRectangleSize` | .cpp:100-158 | Samples 4 corners + edge midpoints as ECEF, takes max chord; anti-meridian/equator-crossing corrections; returns `dvec2` (meters) |
| `getProjectionEllipsoid` | .cpp:160-172 | Visitor returning underlying `Ellipsoid&` |

### GeographicProjection.h / .cpp

Plate carrée (equirectangular). Longitude/latitude **radians** scaled by ellipsoid `maximumRadius` → **meters**. cesium-native `GeographicProjection` equivalent.

| Item | Lines | Algorithm |
|---|---|---|
| Ctor | .cpp:7-10 | `semimajorAxis_ = maximumRadius`, `oneOverSemimajorAxis_` inverse |
| `project(Cartographic)` | .cpp:12-16 | `(lng·a, lat·a, height)` |
| `project(Rectangle)` | .cpp:18-22 | SW/NE corner projection |
| `unproject(dvec2)` / `unproject(Vec3)` | .cpp:24-37 | `·oneOverSemimajorAxis`; Vec3 preserves z as height |
| `unproject(Rectangle)` | .cpp:39-43 | Inverse of rectangle project |
| `maximumGlobeRectangle` | .cpp:45-50 | `[-π,-π/2, π,π/2]` |
| `computeMaximumProjectedRectangle` | .cpp:52-58 | `±(a·π, a·π/2)` meters |

### WebMercatorProjection.h / .cpp

Spherical Web Mercator (EPSG:3857-style). cesium-native `WebMercatorProjection` equivalent.

| Item | Lines | Algorithm |
|---|---|---|
| Ctor | .cpp:9-12 | Same `semimajorAxis` setup as Geographic |
| `project(Cartographic)` | .cpp:14-20 | `(lng·a, mercatorAngle(lat)·a, height)` |
| `unproject(dvec2)` | .cpp:28-35 | `x·(1/a)` for lng; `mercatorAngleToGeodeticLatitude(y·1/a)` |
| `maximumLatitude` | .cpp:51-53 | `mercatorAngleToGeodeticLatitude(π)` ≈ 1.4844 rad (~85.05°) |
| `maximumGlobeRectangle` | .cpp:55-60 | `[-π, -maxLat, π, maxLat]` |
| `computeMaximumProjectedRectangle` | .cpp:62-66 | Square `±a·π` |
| `mercatorAngleToGeodeticLatitude` | .cpp:68-71 | `π/2 - 2·atan(e^-angle)` (Gudermannian) |
| `geodeticLatitudeToMercatorAngle` | .cpp:73-78 | Clamped to ±maxLat; `0.5·ln((1+sin)/(1-sin))` |

### S2CellID.h / .cpp

64-bit S2 cell identifier: face(3b) + Hilbert position + trailing sentinel bit. cesium-native `CesiumGeospatial::S2CellID` equivalent.

| Method | Lines | Algorithm |
|---|---|---|
| `fromToken` | .cpp:201-217 | Hex-parse (≤16 chars) left-justified into 64 bits |
| `fromFaceLevelPosition` | .cpp:219-232 | `(face<<61) \| (pos<<(lsbShift+1)) \| (1<<lsbShift)`; validates face≤5, level≤30 |
| `fromQuadtreeTileID` | .cpp:234-249 | Hilbert-encode (x,y) or (y,x) by face parity; TileKey overload maps z/x/y |
| `isValid` | .cpp:251-263 | face≤5, nonzero, lowest-bit in `0x1555…` mask |
| `toToken` | .cpp:265-282 | 16-hex string, trailing zeros stripped |
| `getLevel` | .cpp:284-297 | `30 - trailingZeros(lowestOnBit)/2` |
| `getFace` | .cpp:299-301 | `id >> 61` |
| `getCenter` | .cpp:303-317 | Decode Hilbert→(x,y)→ST→UV (`stToUv`)→XYZ→lat/lng (radians) |
| `getVertices` | .cpp:319-339 | Four corner UVs → cartographics (CCW) |
| `getParent`/`getChild` | .cpp:341-352 | Bit-shift lowest-on-bit by 2 |
| `computeBoundingRectangle` | .cpp:354-434 | Level-0 per-face hardcoded rects (pole faces 2/5 use `asin(√(1/3))`); else interval from corner lat/lng with `2·ε` margin, `negativePiToPi` normalization |

Constants (anon ns): **`kMaxLevel`** = 30 (.cpp:17), **`kSwapMask`** = 0x01 (.cpp:18), **`kInvertMask`** = 0x02 (.cpp:19); `kPosToIj`/`kPosToOrientation` Hilbert tables (.cpp:20-31). Helpers: `stToUv` quadratic ST→UV (.cpp:85-91), `faceUvToXyz` 6-face map (.cpp:99-114), `encodeHilbert2D`/`decodeS2Position` (.cpp:44-79).

### S2CellBoundingVolume.h / .cpp

Bounding volume for an S2 cell extruded between min/max height: 6 planes + 8 vertices. cesium-native `CesiumGeospatial::S2CellBoundingVolume` equivalent. Always uses `Ellipsoid::WGS84()`.

| Item | Lines | Algorithm |
|---|---|---|
| Ctor | .cpp:178-201 | Invalid cell → empty; center at mid-height ECEF; builds planes then vertices |
| `computeBoundingPlanes` | .cpp:13-61 | plane[0]=top (surface normal @ center), plane[1]=bottom (offset by max vertex distance), planes[2..5]=4 sides from edge×geodeticNormal |
| `computeIntersection` (3-plane) | .cpp:63-84 | Cramer-style plane triple intersection |
| `computeVertices` | .cpp:86-100 | 8 corners = top/bottom plane ∩ adjacent side-plane pairs |
| `computeDistanceSquaredToPosition` | .cpp:203-290 | Region-classify by selected planes (0/1/2/edge/vertex cases); closest point on face polygon / edge / vertex |
| `intersectPlane` | .cpp:292-311 | Sign test over 8 vertices → +1 / -1 / 0 straddle |
| `contains` | .h:28-30 | `distanceSquared ≤ 1e-8` |

Helpers: `closestPointLineSegment` (.cpp:140-150), `closestPointPolygon` (.cpp:152-174), `computeEdgeNormals` (.cpp:122-138).

### EllipsoidTangentPlane.h / .cpp

Plane tangent to ellipsoid at a surface-projected origin; ENU-frame-derived x/y axes. cesium-native `CesiumGeospatial::EllipsoidTangentPlane` equivalent. Defaults to WGS84.

| Item | Lines | Algorithm |
|---|---|---|
| Ctor(origin) | .cpp:11-16 | Delegates via `computeEastNorthUpToFixedFrame` |
| Ctor(ENU Mat4) | .cpp:18-34 | Extracts origin (col 3), xAxis (col 0), yAxis (col 1), plane normal (col 2) |
| `getZAxis` | .h:27 | Returns `plane_.getNormal()` |
| `projectPointToNearestOnPlane` | .cpp:36-51 | Ray along normal → `IntersectionTests::rayPlane` (retries reversed ray); returns `dvec2(x·v, y·v)` |
| `computeEastNorthUpToFixedFrame` | .cpp:53-63 | `tryScaleToGeodeticSurface`; near-center → `std::invalid_argument` |

### BoundingRegionBuilder.h / .cpp

Incrementally accumulates a geodetic `Rectangle` + min/max height, with pole-tolerance longitude handling. cesium-native `BoundingRegionBuilder` equivalent.

| Item | Lines | Algorithm |
|---|---|---|
| `BoundingRegion` struct | .h:15-19 | `rectangle`, `minimumHeight`=1, `maximumHeight`=-1 (empty sentinel) |
| Ctor | .cpp:18-25 | poleTolerance = **`MathUtils::Epsilon10`**; bounds from `Rectangle::EMPTY`; heights = max/lowest |
| `toRegion`/`toRectangle` | .cpp:27-39 | Empty longitude range → `Rectangle::EMPTY` + (1,-1) |
| `expandToIncludePosition` | .cpp:49-108 | Always expands lat/height; skips longitude if `isCloseToPole`; chooses west vs east by shorter anti-meridian-aware distance |
| `expandToIncludeRectangle` | .cpp:110-132 | `Rectangle::computeUnion` |
| `expandToIncludeRegion` | .cpp:134-148 | Union rectangle + expand height range |
| `isCloseToPole` | .cpp:12-14 | `π/2 - |lat| < tolerance` |

### QuadtreeGeometricError.h / .cpp

Free functions for terrain quadtree geometric-error / skirt-height heuristics. cesium-native terrain geometric-error helpers.

| Function | Lines | Formula |
|---|---|---|
| `calcQuadtreeMaxGeometricError` | .cpp:14-16 | `maximumRadius · 0.25 / 65` |
| `calcQuadtreeSkirtHeight` | .cpp:18-23 | `5 · maxGeoError · rectangle.width()` |
| `calcLayerJsonTerrainGeometricError` | .cpp:25-31 | `8 · maxGeoError · rectangle.width()` |

Constants: **`kTerrainMapQuality`** = 0.25, **`kTerrainMapWidth`** = 65.0, **`kSkirtHeightMultiplier`** = 5.0, **`kLayerJsonTerrainMultiplier`** = 8.0 (.cpp:7-10).

### SimplePlanarEllipsoidCurve.h / .cpp

Great-circle-plane-through-center curve between two positions; height linearly interpolated. cesium-native `CesiumGeospatial::SimplePlanarEllipsoidCurve` equivalent.

| Item | Lines | Algorithm |
|---|---|---|
| `fromEarthCenteredEarthFixedCoordinates` | .cpp:22-41 | Scales both endpoints to geocentric surface; either fails → `nullopt` |
| `fromLongitudeLatitudeHeight` | .cpp:43-52 | Cartographic→ECEF then ECEF factory |
| `getPosition(pct, addHeight)` | .cpp:54-79 | `angleAxis(pct·totalAngle)`·sourceDir → geocentric surface + up·(lerp heights + addHeight); pct≤0/≥1 short-circuits to endpoints |
| Private ctor | .cpp:81-108 | `sourceHeight/destHeight` = original−scaled length; `totalAngle=acos(dot)`; antipodal (`dot≈-1`) → orthogonal axis + π; else axis = source×dest |
| `orthogonalAxis` helper | .cpp:14-18 | Picks unitX/unitY by `|dir.x|<0.9` to avoid degeneracy |

Antipodal tolerance `|dot+1| ≤ 1e-15` (.cpp:102).

---

## 3. core/math — Vec3, Mat4, bounding volumes, culling, intersection

### Vec3.h / .cpp

| Item | Lines | Description |
|---|---|---|
| `Vec3` class | .h:10-55 | double-precision Cartesian vector wrapping `glm::dvec3 v_` (.h:54). Internal storage double; cesium-native `glm::dvec3` equivalent. |
| `raw()` accessors | .h:24-25 | Expose underlying `glm::dvec3` for GLM interop |
| Arithmetic / compare ops | .h:28-46 | `+ - * / += -=`, unary `-`, `== !=` inline via glm |
| `length` / `lengthSquared` | .cpp:6-12 | `glm::length` / `glm::dot(v,v)` |
| `normalized` | .cpp:14-16 | `glm::normalize` |
| `dot` / `cross` / `distanceTo` | .cpp:18-28 | `glm::dot` / `glm::cross` / `glm::distance` |
| static `zero/unitX/unitY/unitZ` | .h:48-51 | axis constructors |
| `operator*(double,Vec3)`, `operator<<` | .cpp:30-36 | scalar-lhs multiply, stream fmt |

### Mat4.h / .cpp

| Item | Lines | Description |
|---|---|---|
| `Mat4` class | .h:13-47 | 4×4 **column-major** double matrix wrapping `glm::dmat4 m_` (.h:46). Default ctor = identity (`m_(1.0)`, .cpp:8). |
| `operator(row,col)` | .cpp:10-11 | element access transposed to `m_[col][row]` (column-major storage, row/col API) |
| `operator*(Mat4)` | .cpp:13 | `m_ * rhs.m_` |
| `operator*(Vec3)` / `transformPoint` | .cpp:15-22 | homogeneous w=1, divides by `r.w` |
| `transformVector` | .cpp:24-26 | w=0, no translation |
| `inverse` / `transpose` | .cpp:28-29 | `glm::inverse` / `glm::transpose` |
| `translation/scale/rotationX/Y/Z` | .cpp:31-36 | via `glm::translate/scale/rotate` |
| `data()` | .h:43 | raw `const double*` to `&m_[0][0]` (column-major upload) |

### BoundingSphere.h

Header-only, `constexpr`. cesium-native `CesiumGeometry::BoundingSphere` equivalent.

| Item | Lines | Description |
|---|---|---|
| ctor / `getCenter` / `getRadius` | .h:17-21 | `constexpr`, stores `center_`, `radius_` (.h:60-61) |
| `intersectPlane` | .h:27-32 | returns -1 Outside / 0 Intersecting / +1 Inside vs `Plane::getPointDistance` |
| `computeDistanceSquaredToPosition` | .h:36-41 | 0 if inside; else `(dist-radius)^2` |
| `contains` | .h:44-46 | point within radius |
| `transform` | .h:50-57 | radius scaled by **max** of transformed axis lengths (non-uniform scale), matches cesium-native |

### AxisAlignedBox.h / .cpp

cesium-native `AxisAlignedBox` equivalent.

| Item | Lines | Description |
|---|---|---|
| ctor (min/max XYZ) | .cpp:7-25 | derives `lengthX/Y/Z_` and `center_` (0.5·(max+min)) in init list |
| min/max/length/center accessors | .h:20-30 | stored fields .h:37-46 |
| `contains` | .cpp:27-31 | inclusive per-axis range test |
| `fromPositions` | .cpp:33-60 | per-axis `std::min/max` reduce; empty → default box (.cpp:35-37) |

### BoundingCylinderRegion.h / .cpp

`3DTILES_bounding_volume_cylinder` region; approximated by a tight OBB (`box_`, .h:52), matching cesium-native. All query ops delegate to the inner `OrientedBoundingBox`.

| Item | Lines | Description |
|---|---|---|
| ctor | .h:18-24 / .cpp:79-95 | stores translation/rotation/height/radialBounds/angularBounds; `angularBounds` defaults `[-π, π]` (.h:23-24). Builds `box_` via `computeBoxFromCylinderRegion`. |
| `computeBoxFromCylinderRegion` (anon) | .cpp:17-75 | computes x/y extremes from angular bounds (handles π/2, -π/2 crossings and zero-crossing sign flips .cpp:29-60), z from `height*0.5`; builds AABB from 4 corner positions → `OBB::fromAxisAligned` → TRS transform via `Transforms::createTranslationRotationScaleMatrix` |
| delegated ops | .h:26-44 | `getCenter/intersectPlane/computeDistanceSquaredToPosition/contains/toOrientedBoundingBox` forward to `box_` |
| `transform` | .cpp:97-121 | composes TRS with incoming matrix, decomposes via `Transforms::computeTranslationRotationScaleFromMatrix`, scales height by `scale.z`, radialBounds by `max(scale.x,scale.y)` (.cpp:114) |

### CullingVolume.h / .cpp

4-side-plane frustum (left/right/top/bottom); no near/far. cesium-native `CullingVolume` equivalent.

| Item | Lines | Description |
|---|---|---|
| struct + default planes | .h:9-13 | 4 `Plane`s default `{unitZ,0}` |
| `normalizedPlane` (anon) | .cpp:12-15 | divides a,b,c,d by `sqrt(a²+b²+c²)` |
| `fromPerspectiveFieldOfView` | .cpp:19-55 | builds 4 planes from position/direction/up + h/v FOV; near via `nextafter(positionLength)` epsilon (.cpp:31-34) |
| `fromPerspectiveOffCenter` | .cpp:57-75 | infinite-far perspective × view matrix → `fromClipMatrix` |
| `fromOrthographicOffCenter` | .cpp:77-99 | infinite-far ortho × view; near ignored (only side planes stored, .cpp:86-88) |
| `fromClipMatrix` | .cpp:101-124 | Gribb-Hartmann plane extraction: rows (3±0),(3∓0),(3∓1),(3±1) via `operator(row,col)` |

### IntersectionTests.h / .cpp

Static-only (ctor `= delete`, .h:20). cesium-native `IntersectionTests` equivalent.

| Method | Lines | Algorithm |
|---|---|---|
| `solveQuadratic` (anon) | .cpp:28-57 | discriminant `b²-4ac`; sorts roots; det==0 rejects root 0 |
| `component` (anon) | .cpp:17-26 | index → Vec3 x/y/z |
| `rayPlane` | .cpp:60-75 | denom = normal·dir, reject `<1e-15` (**`epsilon15`**=1e-15, .cpp:61); reject t<0 |
| `rayEllipsoid` | .cpp:77-137 | scales ray by inverse radii, quadratic in unit-sphere space; returns `RayEllipsoidIntersectionInterval` |
| `rayTriangle` / `rayTriangleParametric` | .cpp:139-204 | Möller–Trumbore; `cullBackFaces` branch; **`epsilon8`**=1e-8 (.cpp:157) |
| `rayAABB` / `rayAABBParametric` | .cpp:206-248 | slab method per axis; skip near-parallel `<1e-6` (**`epsilon6`**=1e-6, .cpp:218) |
| `rayOBB` / `rayOBBParametric` | .cpp:250-283 | transforms ray into OBB local frame (inverse rotation from normalized half-axes), delegates to `rayAABBParametric` |
| `raySphere` / `raySphereParametric` | .cpp:285-308 | quadratic via `solveQuadratic(1,b,c)` |
| `pointInTriangle` (2D) | .cpp:310-333 | perpendicular-dot sign test |
| `pointInTriangle` (3D + barycentric) | .cpp:335-389 | area-ratio; degenerate reject `<1e-8` (.cpp:352); fills barycentric coords |

### AttributeCompression.h

Header-only static utility. cesium-native `AttributeCompression` equivalent.

| Item | Lines | Description |
|---|---|---|
| `octDecodeInRange<T>` | .h:16-35 | oct-encoded unit normal decode: `fromSNorm` x/y, `z=1-|x|-|y|`, fold when z<0 using `signNotZero`, `glm::normalize`. `T` restricted to unsigned via `enable_if` (.h:16-17) |
| `octDecode(uint8_t,uint8_t)` | .h:37-40 | 8-bit convenience, `kRangeMax`=255 (.h:38) |
| `decodeRGB565` | .h:42-55 | unpack 16-bit RGB565; `kNormalize5`=1/31, `kNormalize6`=1/63 (.h:45-46) |

### ClipTriangleAtAxisAlignedThreshold.h / .cpp

Clips a triangle against an axis-aligned scalar threshold; cesium-native `clipTriangleAtAxisAlignedThreshold` equivalent. Used in terrain/mesh clipping.

| Item | Lines | Description |
|---|---|---|
| `InterpolatedVertex` | .h:10-24 | edge-interpolated vertex `{first,second,t}`; `==` compares t with `std::numeric_limits<double>::epsilon()` (.h:18) |
| `TriangleClipVertex` | .h:26 | `std::variant<int, InterpolatedVertex>` — original index or interpolated |
| `clipTriangleAtAxisAlignedThreshold` | .cpp:5-100 | `keepAbove` picks `<`/`>` comparison (.cpp:18-26); branches on `numberOfBehind` 0/1/2/3; emits kept-side polygon with edge-interpolated vertices; skips `ratio==1.0` degenerate cuts; all-behind (==3) emits nothing |

---

## 4. core/async, cache, resources, net, gltf

### AsyncSystem.h

cesium-native `CesiumAsync` analog (ThreadPool/Future/AsyncSystem). Header-only, all inline.

| Item | Lines | Description |
| --- | --- | --- |
| `ThreadPool` | .h:18-84 | Fixed worker pool over a `std::queue<std::function<void()>>` + `mutex_`/`cv_`. Ctor defaults `numThreads` to `hardware_concurrency()` (min 1) (.h:20-39). Dtor sets `stop_`, notifies, joins all (.h:41-50). Non-copyable (.h:52-53). |
| `ThreadPool::enqueue` | .h:56-74 | Wraps callable in `packaged_task`, returns `std::future<R>`. Throws `std::runtime_error` if enqueued after stop (.h:67-69). |
| `Future<T>` | .h:88-134 | `std::future<T>` wrapper held via `shared_ptr` (.h:133). `get()` (.h:95-98), non-blocking `isReady()` via `wait_for(0)` (.h:101-104), `operator bool` = valid (.h:130). |
| `Future::then` | .h:107-128 | **CAVEAT: spawns a detached `std::thread` per call** (.h:113-126) that blocks on `shared->get()` then runs `func`; propagates exceptions via `set_exception`. Not pooled — unbounded thread creation under chained/fan-out use. |
| `AsyncSystem::pool()` | .h:139-142 | Meyers-singleton `ThreadPool` sized to `hardware_concurrency()` (min 1). |
| `AsyncSystem::run` | .h:145-149 | Enqueue on shared pool, return `Future<R>`. |

Threading: `enqueue` work runs on pooled workers; `Future::then` continuations do NOT — each `.then` leaks a one-shot detached thread.

### HttpCache.h

Thread-safe LRU HTTP-response cache. cesium-native `CachingAssetAccessor` analog. Header-only, all inline; single `mutex_` guards all maps.

| Item | Lines | Description |
| --- | --- | --- |
| `CachedResponse` | .h:20-29 | status, headers (vec of pairs), body bytes, `expiryTime` (`time_t`, 0 = none). `isExpired()` = `expiryTime>0 && now>=expiryTime` (.h:26-28). |
| `HttpCache` ctor / `shared()` | .h:35-36, .h:145-148 | **`maxEntries`** default = **2000** (.h:35, singleton .h:146). |
| Body-only compat API | .h:41-57 | Deprecated `get`/`put(url,bytes)` wrappers over full-response API. |
| `getResponse` | .h:62-73 | Lookup; erases + returns nullptr if expired; else `touch()` LRU, returns copy as `shared_ptr<const>`. |
| `putResponse` | .h:76-91 | Upsert; `evictOne()` when at `maxEntries_` (.h:85); then `persistAsync` outside lock (.h:90). |
| `remove` / `prune` / `clear` / `contains` / `size` | .h:94-142 | `prune()` drops all expired (.h:104-118); `contains` erases-on-expiry side effect (.h:129-139). |
| `persistAsync` | .h:171-179 | If `PersistentCache::cacheDir()` set and body non-empty, enqueues disk write on `AsyncSystem::pool()`. |
| `touch` / `evictOne` | .h:154-169 | `lru_` = `std::list<string>` MRU-front; `lruMap_` iterators; `evictOne` drops LRU back. |

### PersistentCache.h

File-per-URL byte cache backing `HttpCache`. All static; header-only.

| Item | Lines | Description |
| --- | --- | --- |
| `setCacheDir` / `cacheDir` | .h:18-19 | Static `cacheDir_` (inline, .h:75); empty = disabled. |
| `load` | .h:22-34 | `fopen`/`fseek`/`fread` whole file; empty vec on miss or when dir unset. |
| `save` | .h:37-46 | Writes bytes; calls `ensureDir()` first. Called from worker thread via `HttpCache::persistAsync`. |
| `prewarm` | .h:51-57 | **No-op** — templated but body only `(void)onEntry`; on-demand load only. |
| `filePath` | .h:60-65 | **CAVEAT: filename = `std::hash<string>(url)` → `"%016zx.bin"`** (.h:61-63). Non-cryptographic, process-salted hash risks collisions/aliasing between distinct URLs and is not stable across runs/platforms. |
| `ensureDir` | .h:67-73 | **CAVEAT: no-op** — `call_once` with empty body; no `mkdir`. If `cacheDir_` doesn't already exist, every `save` silently fails (fopen returns null). |

### SharedAssetDepot.h

Generic LRU asset depot + in-flight request coalescing. cesium-native `CesiumAsync::SharedAssetDepot` analog. Header-only template `<TAsset, TKey, KeyTraits>`; `KeyTraits::toString(key)` required. Single `mutex_`.

| Item | Lines | Description |
| --- | --- | --- |
| Types | .h:31-36 | `AssetPtr` = `shared_ptr<const TAsset>`; `Waiter` = `function<void(AssetPtr)>`; `InFlightEntry{vector<Waiter>}`. |
| `get` / `put` / `remove` | .h:43-82 | `put` upserts, updates `cacheBytes_`, `pruneToBudget()` (.h:53-66). `get` touches LRU (.h:43-50). |
| `invalidateAll` / `contains` | .h:85-99 | Epoch-style bulk clear (also clears `inFlight_`). |
| `startInFlight` | .h:105-117 | Returns true for first caller (issue request); subsequent callers append `Waiter` and get false (coalesce). |
| `finishInFlight` / `abortInFlight` | .h:120-142 | Moves waiters out under lock, `put`s result, invokes each waiter outside... — note: `put(key,result)` re-locks `mutex_` after copying waiters (waiter callbacks at .h:134-136 run unlocked). `abortInFlight` = finish with nullptr. |
| `isInFlight` | .h:145-149 | Membership check. |
| Budget | .h:154-193 | **`maxCacheBytes_`** default = **16 MiB** (`16*1024*1024`, .h:193). `pruneToBudget` evicts LRU-back while over (.h:170-181). |
| `assetSize` | .h:183-185 | **CAVEAT: shallow — returns `sizeof(TAsset)`** regardless of heap-allocated contents (.h:184). Byte budget grossly under-counts real memory for assets owning vectors/textures/etc. |

### FrameResourceBudget.h / .cpp

Per-frame issue/finalize rate limiter across named lanes. No cesium-native 1:1; governs terrain/content/raster request + finalize pacing. Not thread-safe (single-frame, main-thread).

| Item | Lines | Description |
| --- | --- | --- |
| `FrameResourceLane` | .h:7-15 | TerrainRequest, ContentRequest, RasterRequest, TerrainFinalize, ContentFinalize, RasterTextureUpload, TerminalState. |
| `FrameResourcePriority` | .h:17-21 | Preload=0, Normal=1, Urgent=2. **Currently ignored** — `canIssue`/`canFinalize` take priority param but discard it (.cpp:23, .cpp:95). |
| `FrameResourceBudgetConfig` | .h:23-41 | Per-lane limits. **`maxNetworkRequestsPerFrame`**=20 (.h:27), **`maxNetworkInflight`**=20 (.h:32); terrain/content + raster overrides default 0 → fall back to the 20 defaults (.h:28-34). `maxMainThreadFinalizesPerFrame`=1 (.h:35), `maxTerminalStateTransitionsPerFrame`=64 (.h:36), `maxRasterUploadsPerFrame`=1 (.h:37). Note: per-lane cap, **not** a global network sum cap (comment .h:24-26). |
| `FrameResourceBudgetSnapshot` | .h:43-67 | Read-model of counters + resolved limits; built in `snapshot()` (.cpp:156-192). |
| `beginFrame` | .cpp:7-20 | Resets all counters, stores config. |
| `canIssue` / `tryIssue` | .cpp:22-75 | Terrain+Content share the `terrainContentNetworkRequestsIssued_` pool vs `networkRequestLimit` (.cpp:26-33); Raster uses its own counter (.cpp:34-36). `tryIssue` also bumps `contentNetworkRequestsIssued_` and global `networkRequestsIssued_`. Finalize/upload/terminal lanes always issuable (.cpp:37-41). |
| `hasNetworkInflightCapacity` | .cpp:77-92 | `currentInflight + fanout <= networkInflightLimit(lane)`; no-lane overload delegates to TerrainRequest lane (.cpp:85-92). |
| `canFinalize` / `tryFinalize` | .cpp:94-145 | Gated first by `mainThreadTimeExpired()` (.cpp:98). RasterTextureUpload vs `maxRasterUploadsPerFrame`; Terrain/ContentFinalize share `mainThreadFinalizesUsed_` vs `maxMainThreadFinalizesPerFrame`; TerminalState vs its cap; request lanes defer to `canIssue`. |
| `recordElapsed` / `mainThreadTimeExpired` | .cpp:147-154 | Accumulates `mainThreadElapsedMs_` (lane arg unused); expired when `mainThreadTimeMs>0 && elapsed>=it`. |
| `positiveUnits` | .cpp:194-196 | `max(1, estimatedUnits)` — fanout/cost floors at 1. |
| `networkRequestLimit` / `networkInflightLimit` | .cpp:198-242 | Resolve per-lane limit with 0→default fallback; non-network lanes return the network default. |

### Uri.h / .cpp

Pure-string URI parse/compose/resolve/template. cesium-native `CesiumUtility::Uri` analog; no external URL lib. Replaces 3 call sites: `TileMapServiceUrl::resolveRelativeUrl`, `ImplicitTileIdUtilities::resolveUrl`+`substituteTemplateParameters`, `GltfContentProvider::resolveContentUrl` (.h:15-19).

| Method | Lines | Algorithm |
| --- | --- | --- |
| `Parsed` | .h:23-31 | scheme/authority/path/query/fragment + `hasScheme`/`hasAuthority`. |
| `parse` | .cpp:22-84 | Split fragment→query→scheme (via `://`)→authority (to next `/`)→path; empty path defaults `"/"`. Scheme must start alpha + all `isSchemeChar`. |
| `compose` | .cpp:86-99 | `scheme://authority` + path + `?query` + `#fragment`. |
| `hasScheme` | .cpp:101-108 | True if chars before first `:` are all `isSchemeChar` (colon not at 0). |
| `resolve` | .cpp:110-171 | RFC-3986-ish: absolute/protocol-relative passthrough (.cpp:116-123), fragment-only (.cpp:126), query-only (.cpp:133), absolute vs relative path merge + `normalizePath` (.cpp:143-157). `useBaseQuery` merges base query. |
| `substituteTemplateParameters` | .cpp:173-196 | Replaces `{placeholder}` via callback; unclosed `{` emitted literally (.cpp:186-189). |
| `addQuery` | .cpp:198-212 | Appends `?`/`&` + percent-encoded `key=value`. |
| `percentEncode` / `percentDecode` | .cpp:214-247 | Encode all but `isUnreserved` (alnum + `-._~`, .cpp:12-15). Decode requires `i+2 < size` — **CAVEAT: a `%XX` in the final 2 bytes is not decoded** (off-by-one at .cpp:231, passes `%` through literally). |
| `normalizePath` | .cpp:249-294 | Segment stack handling `..`/`.`/dup slashes; preserves leading + trailing slash. |
| `mergeQuery` | .cpp:296-340 | Relative params first, then base params whose key not overridden. |

### AccessorView.h

Type-safe strided view over glTF binary buffers. cesium-native `CesiumGltf::AccessorView<T>` + `AccessorTypes` analog. Header-only.

| Item | Lines | Description |
| --- | --- | --- |
| `AccessorViewStatus` | .h:14-25 | Valid + 9 error states (invalid accessor/bufferView/buffer index, too-small, WrongSizeT, InvalidType/ComponentType/ByteStride). |
| `AccessorView<T>` | .h:36-76 | Holds `pData_/stride_/offset_/size_/status_`. Default ctor = `InvalidAccessorIndex` (.h:41-43); status-only ctor (.h:45-46); data ctor sets `Valid` (.h:48-51). |
| `operator[]` | .h:53-58 | Bounds-checked (`throw std::range_error`); returns `*reinterpret_cast<const T*>(pData_ + i*stride_ + offset_)`. |
| accessors / `operator bool` | .h:60-68 | `size/status/stride/offset`, `data()` = `pData_+offset_`; `bool` = Valid && non-null. |
| `AccessorTypes` | .h:84-94 | `#pragma pack(1)` SCALAR/VEC2-4/MAT2-4 element shapes matching glTF binary layout. |
| `CesiumImpl::createAccessorView` | .h:98-114 | Dispatches type code 0-6 (SCALAR..MAT4) to a callback-constructed `AccessorView<AccessorTypes::*<TElement>>`; default → `InvalidType`. Type codes are the local 0-6 enum, not glTF spec strings. |

Note: none of these modules touch the async-terrain GPU-upload path (`TerrainGpuVertex`/`Renderer::terrainShader`/`GltfDrawCommandBuilder`); `AccessorView` is the glTF read path feeding geometry builders upstream of that draw refactor.

---

## 5. tiling — Tileset: selection / traversal / LOD

### Tileset.h / .cpp

cesium-native `Tileset` equivalent. Owns a unified quadtree of terrain + raster-overlay tiles, drives per-frame selection/traversal, and produces a `TilePlan`. Replaces the removed `TileQuadTree`.

**Public API**

| Method | Lines | Description |
| --- | --- | --- |
| ctor (scheme, overlays, device, options) | .h:95-98 | Primary ctor; delegates to private ctor with `TilesetTerrainProviders(nullptr)` (.cpp:41-50) |
| ctor (+ contentProvider) | .h:99-103 | 3D-Tiles content path; wraps provider in `TilesetTerrainProviders` (.cpp:115-126) |
| `update(FrameState, IPrepareRendererResources*)` | .h:106-107 / .cpp:267-279 | Per-frame entry; delegates to `TilesetUpdateFrameFacade::update`; logs if >5ms |
| `buildRenderCommands(Renderer&, RenderCommandList&)` | .h:108 / .cpp:281-310 | `++frameNumber_`, `renderCommands_.beginFrame(...)`, then `TilesetRenderFrameExecutor::buildRenderCommands` over `tilePlan_` |
| `releaseRenderReferences()` | .h:125 / .cpp:312-318 | Called by Scene after `renderer_->submit()`; drops the ref added in buildRenderCommands via `TileRenderReferenceReleaser::release` |
| `tilePlan()` / `tileScheme()` | .h:110-111 | Const accessors to the frame selection result |
| `sampleHeight(lngRad, latRad)` | .h:120 / .cpp:260-265 | Best-loaded terrain height in meters (0 if none); via `LoadedTerrainHeightSampler` |
| `setOcclusionCallback` / `clearOcclusionCallback` | .h:131-132 / .cpp:151-157 | cesium-native `TileOcclusionRendererProxyPool` input hook |
| `pendingRequests` / `totalBytesUsed` / `loadDiagnostics` | .h:113-115 / .cpp:139-171 | Diagnostics |

**Private frame plumbing:** `makeContentRuntimeRequestFrame` (.cpp:173-190), `makeContentRuntimeUploadFrame` (.cpp:192-207), `requestMissingContent` (.cpp:209-217), `processPendingLoads` (.cpp:219-229), `drainGpuUploadQueue` (.cpp:231-238 — async CPU→GPU pipeline, called after processPendingLoads each frame), `checkSingleTileOcclusion`/`checkOcclusion` (.cpp:244-258, callback else `TileSoftwareOcclusionPolicy::check`).

**`TilesetOptions`** (.h:54-87), cesium-native `TilesetOptions` subset. Key defaults: **`maximumScreenSpaceError`** = 16.0, **`maximumSimultaneousTileLoads`** = 20, **`loadingDescendantLimit`** = 20, **`culledScreenSpaceError`** = 64.0, **`maximumCachedBytes`** = 512 MiB, `enableFrustumCulling`/`enableOcclusionCulling`/`delayRefinementForOcclusion`/`enableFogCulling`/`enforceCulledScreenSpaceError`/`preloadAncestors`/`preloadSiblings`/`renderTilesUnderCamera` = true, `enableLodTransitionPeriod` = false. Embeds a 21-entry `fogDensityTable` (.h:74-86). **`kMaximumCachedBytes`** = 512 MiB duplicated as static constexpr (.cpp/.h:180).

Members of interest (.h:170-208): `tilePlan_`, `tileRegistry_`, `contentLifecycle_`, `gpuUploadQueue_` (async pipeline), `selectionReuseState_`, `loadQueue_`, `selectionCounters_`, `lastCameraPosition_`/`lastCameraDirection_` (view-weighted priority). Friends: `TilesetTestAccess`, `TilesetSelectionFrameFacade`, `TilesetUpdateFrameRuntime` (.h:135-137) — selection/update logic reaches into privates.

### TilesetUpdateFrameRuntime.h / .cpp

Drives one `update()` frame; runs as friend of `Tileset`. `run(tileset, frameState, pPrepRenderer)` (.h:17-21 / .cpp:20-136).

| Step | Lines | Action |
| --- | --- | --- |
| generation bump | .cpp:26 | `++tileset.generation_` each frame so RenderCommand validator accepts commands |
| frame work | .cpp:28-98 | `TileFrameWorkCoordinator::run(...)` with lambdas: processPendingLoads, markContentResourcesDirty, hasPendingWork, `TileRenderPlanFrameRefresher::refresh`, `TilesetSelectionFrameFacade::selectTiles`, ensureTile, unloadTileContent, requestMissingContent |
| drain GPU uploads | .cpp:99-110 | `tileset.drainGpuUploadQueue(pPrepRenderer)` after selection; logs if >1ms (Android) |
| result | .cpp:112-135 | Packs `TileUpdateDebugLogInput` (visible count, load queue size, compute/prefetch/request/upload ms, reuse mode/reason) |

**`kPostInteractionResourceSmoothingSeconds`** = 1.25 (.cpp:16).

### TileUpdateSelectionWorkRunner.h

Header-only template orchestrating the selection sub-phase, invoked from `TileFrameWorkCoordinator`. `run(input, refreshRenderEntries, selectTiles, ensureTile, unloadTileContent, requestMissingTiles)` (.h:58-127).

- **Reuse branch** (.h:73-83): if `reusedSelection`, bump `tilePlan.frameId`, reset counters, and only refresh render entries — skips traversal. Else `selectTiles(frameState)` + `selectionReuseState_.commit(...)`.
- **Prefetch** (.h:92-114): non-reuse only; `TileSelectionRasterOverlayPreparer::processingOrder` → `TileRasterOverlayFrameProcessor::prefetchSelection`. Comment (.h:85-91) explains reuse skips redundant overlay prefetch (cesium-native `updateTileOverlays` runs outside selection).
- **Request** (.h:116-124): `requestMissingTiles(loadQueue.requests(), &budget)`; records outcome into reuse state.

Returns `TileUpdateSelectionWorkResult` with `computeMs`/`prefetchMs`/`requestMs` and reuse mode/reason.

### TilesetSelectionFrameFacade.cpp (traversal entry point)

`selectTiles(tileset, frameState)` (.cpp:16-90) wires the traversal. Runs `TileSelectionFrameRunner::run` with: state reset (`TileSelectionStateResetter`), root ensure via `contentAccess_.ensureTile`, **per-root visit** (.cpp:40-71) that builds the `TileSelectionTraversalContext` (via `TileSelectionTraversalContextBuilder`) then calls `TileSelectionTraversalExecutor::visitTileIfNeeded(ctx, root, selectorFrame, depth=0, ancestorMeetsSse=false)`, and finalize (`TileSelectionFrameFinalizationRunner`). Occlusion binding routes back to `Tileset::checkOcclusion` (.cpp:48-51).

### TileSelectionTraversalContext.h / TileSelectionTraversalContextBuilder.h / .cpp

The C-function-pointer "vtable" threaded through recursive traversal. **TYPE-UNSAFE**: dispatch goes through raw `void* userData` + `void (*)(void*, ...)` function pointers (.h:25-64), not virtuals. Two distinct user-data pointers — `userData` (→ `TileSelectionTraversalContextBinding`, i.e. plan/loadQueue/occlusion) and `contentAccessUserData` (→ `TileContentAccess`, i.e. ensureChildren/canRefine); passing the wrong one silently reinterprets the pointer.

| Fn ptr member | Target (Builder .cpp) | Purpose |
| --- | --- | --- |
| `queueTileLoadFn` | .cpp:14-25 | queue load via `TileSelectionPlanAppender::queueTileLoad` |
| `addTileToCurrentPlanFn` | .cpp:27-42 | append selected tile to plan |
| `ensureTileChildrenFn` | .cpp:44-47 | `TileContentAccess::ensureTileChildren` (uses `contentAccessUserData`) |
| `canRefineFn` | .cpp:49-51 | `TileContentAccess::canRefine` (uses `contentAccessUserData`) |
| `checkOcclusionFn` | .cpp:53-57 | routes to binding → `Tileset::checkOcclusion` |
| `hasLodTransitionRenderContentFn` | .cpp:59-66 | render-kind + renderable |
| `createSingleTileDetailsFn` / `createCulledTileDetailsFn` | .cpp:68-87 | build `TileTraversalDetails` |

Thin inline forwarders (.h:66-109) call the pointers. `TileSelectionTraversalContextBinding` (Builder.h:22-33) holds `tilePlan`, `loadQueue`, `options`, `rasterOverlays`, `contentAccess`, plus `occlusionUserData` + its fn ptr.

### TileSelectionTraversalExecutor.h / .cpp

Core recursive selection (cesium-native `Tileset::_visitTileIfNeeded` / `_visitTile`).

**`visitTileIfNeeded`** (.cpp:36-110): computes camera cartographic, prepares visibility via `TileSelectionVisitPreparation::prepare` (frustum/fog/SSE using options), sets `selection.inFrustum`/`cameraInside`/`priority`, updates cull/fog/culledVisited counters. On `visitOutcome.shouldExit` marks Culled / resets SSE / queues load and returns culled-or-empty details. Otherwise `++visited` and recurses into `visitTile`.

**`visitTile`** (.cpp:112-290): the fan-out body.
1. Prepare raster overlay (`TileSelectionRasterOverlayPreparer::prepare`) and compute `renderable` (not virtual root && overlay-renderable) → `tile.updateTraversalRenderability` (.cpp:122-134).
2. Refinement: `canRefine(tile)` then `TileSelectionRefineFlowPolicy::evaluate`; if `shouldCheckOcclusion`, calls `checkOcclusion` and re-evaluates, updating occluded / waitingForOcclusionResults counters (.cpp:136-168).
3. Pre-traversal (`TileSelectionPreTraversalPolicy::plan`): may queue urgent load; **`finishAsSingleTile`** → `addTileToCurrentPlan` + return (leaf/SSE-met case) (.cpp:171-193).
4. `ensureTileChildren`; on `retryLater` queue urgent load. Additive parent added to plan if requested (.cpp:195-211).
5. **Fan-out** (.cpp:213-228): records `firstRenderedDescendant = visibleTiles.size()` and `loadQueueBeforeChildren`, then `TileSelectionChildTraversal::visitChildren(tile.children, …)` recursing `visitTileIfNeeded(depth+1)`.
6. Post-traversal (`TileSelectionPostTraversalPolicy::evaluate` + `commitPlan`) then `TileSelectionPostTraversalCommitter::commit` — applies kick (`kickVisitedDescendants`, .cpp:24-32 recursively demotes child selection states), load-queue restore, renderable-replacement, ancestor preload (.cpp:230-289).

**Traversal fan-out order:** roots visited in `chooseRoots` order (see RootPolicy); within a tile, children visited in stored `tile.children` vector order (`TileSelectionChildTraversal::visitChildren` .h:14-27, skips null children, merges child `TileTraversalDetails` via `TileTraversalDetailsPolicy::mergeChild`). Depth-first, pre-order plan append for additive parents, post-order for refined replacements.

### TileSelectionRefinementPolicy.h / .cpp

Pure refine-vs-render decision logic (cesium-native `_meetsSSE`/occlusion gating).

| Method | Lines | Algorithm |
| --- | --- | --- |
| `initialRefineDecision` | .cpp:5-13 | `refine = unconditionallyRefine \|\| (!meetsSse && !ancestorMeetsSse)` |
| `shouldCheckOcclusion` | .cpp:15-27 | occlusion enabled && refine && !unconditional && (tile not refined last frame OR child not refined last frame) |
| `occlusionAction` | .cpp:29-45 | `Occluded`→StopForOccluded; `OcclusionUnavailable` + delay + not previously refined→StopForUnavailable |
| `applyOcclusionAction` | .cpp:47-58 | stop → `refine=false, meetsSse=true` |
| `continueDeeperDecision` | .cpp:60-77 | non-renderable refined ancestor keeps descending; `queueUrgent = !ancestorMeetsSse` |
| `shouldPreloadRefinedAncestor` | .cpp:79-83 | `preloadAncestors && !queuedForLoad` |

### TileSelectionCullingPolicy.h / .cpp

Frustum/fog culling + culled-SSE (cesium-native `_frustumCull`/`_fogCull`).

| Method | Lines | Logic |
| --- | --- | --- |
| `shouldUseChildrenBounds` | .cpp:9-16 | Replace-refine + children + no unconditional child |
| `anyViewVisibleInFog` | .cpp:18-30 | per-view `TileSelectionMetrics::isVisibleInFog(distance, density)` |
| `evaluateFrustum` | .cpp:32-44 | not-visible → culled=Frustum; `shouldVisit=false` only if `enableFrustumCulling` |
| `evaluateFog` | .cpp:46-61 | not-in-fog → culled=Fog; visit suppressed only if `enableFogCulling` |
| `planCulledTileLoad` | .cpp:63-80 | forbidHoles+Replace→Normal; else preloadSiblings→Preload |
| `meetsScreenSpaceError` | .cpp:82-93 | culled → `!enforce \|\| sse < culledSSE`; else `sse < maxSSE` |

### TileSelectionKickPolicy.h / .cpp

"Kick" (demote refined subtree to parent when descendants aren't ready). cesium-native `_kickDescendantsAndRenderTile`.

| Method | Lines | Logic |
| --- | --- | --- |
| `shouldKickDescendants` | .cpp:5-29 | kick if (non-ready descendant && none rendered last frame) OR (fading-in) AND (`notYetRenderableCount > loadingDescendantLimit` OR renderable) |
| `shouldRestoreChildLoadQueueAndLoadParent` | .cpp:31-41 | not-rendered-last-frame && over limit && !external && !unconditional |
| `planAfterKick` | .cpp:43-68 | builds `TileSelectionKickPlan{restoreChildLoadQueueAndLoadParent, addRenderableReplacementToPlan(=non-Add && renderable), preloadParent}` |

### TileSelectionReusePolicy.h / .cpp

Frame-to-frame selection reuse (skip full traversal when the view is unchanged). cesium-native has no direct equivalent — a local optimization.

- `selectorViewsEquivalent` (.cpp:54-80): per-view position within **1e-3** m, direction lenSq within **1e-12**, projection matrices equal within **1e-12**, matching viewport height.
- `classifyReuseWithReason` (.cpp:87-153): returns `None`/`Strict`/`Stale` with a reject reason. Rejects on: no reusable selection, viewport changed, overlay-signature change, selector moved with stale disabled, stale age exceeded (`maxStaleFrameAge`, default 1), stale view too different (`stalePositionToleranceMeters`=100, `staleDirectionToleranceSquared`=1e-4), resource-revision change, fading tiles. Note (.cpp:144-149): pending network/upload work does NOT block strict reuse (avoids re-traversing large static tile sets on Android).
- `canReuseSelection` (.cpp:155-158): `classifyReuse != None`.

`TileSelectionReuseMode {None, Strict, Stale}` (.h:10-14); `TileSelectionReuseRejectReason` (.h:16-29); `TileSelectionReuseInput` tolerances/flags (.h:37-58).

### TileSelectionInputMetrics.h / .cpp — the SSE formula

Per-tile per-view metrics feeding traversal.

**Screen-space error** `screenSpaceErrorForView(geometricError, projectionMatrix, viewportHeightPixels, distance)` (.cpp:41-58): projects a point at `(0,0,-distance)` and one offset by `geometricError` in Y through the projection matrix, perspective-divides both, and returns
`|(errorOffsetNdc − centerNdc).y| · viewportHeightPixels · 0.5` — the NDC-projection form of cesium-native `computeScreenSpaceError` (returns 0 if `geometricError ≤ 0`; distance clamped to ≥ 1e-7).

`summarizeForViews` (.cpp:60-91): per view accumulates `priority = min(...)` and `screenSpaceError = max(...)` across all `SelectorView`s; distances via `distanceToTile` (→ `TileBoundsMetrics::approximateDistanceToTileBounds`), center via bounding volume else tile bounds. `TileSelectionInputSummary{distances, priority, screenSpaceError}` (.h:13-17).

### TilePriorityMetrics.h / .cpp

`computeTilePriority(tileCenter, cameraPosition, cameraDirection, distance)` (.cpp:7-20): **`(1 − dot(tileDir, cameraDir)) · distance`**, lower = higher priority (view-direction-weighted). Returns `double::max` if tile within **1e-5** m of camera.

### TileLoadPriorityPolicy.h / .cpp

`TileLoadPriorityGroup {Preload=0, Normal=1, Urgent=2}` (.h:9-13). `hasHigherPriority` (.cpp:5-14): higher group wins; within a group, lower numeric priority wins. Header-only `selectHighestPriority` / `sortByPriority` templates (.h:24-52). `toFramePriority` maps to `FrameResourcePriority` (.cpp:16-27).

### TilePlan.h / .cpp — the traversal output

cesium-native `ViewUpdateResult` equivalent (the removed `TileQuadTree`'s output). `struct TilePlan` (.h:126-183) is the per-frame selection result the renderer consumes without reselecting LOD.

Key vectors: `visibleTiles` (`vector<TileKey>` — selected tiles), `tilesFadingOut` / `tileTransitions` (`vector<TileTransition>` — LOD fade), **`renderEntries`** (`vector<TileRenderEntry>` — resolved draw list), `selectionRecords` (`vector<TileSelectionRecord>`), `frameCredits`. Plus ~50 int diagnostic counters (rendering/refined/kicked/occluded, render-entry command draw/miss/deferred tallies).

- `TileRenderEntry` (.h:49-76): `selectedKey` vs `renderKey` differ only for clipped ancestor fallback; `reason` (`TileRenderEntryReason {Direct, AncestorFallback, SynchronousPrep, FadingOut}`), `opacity`, `surfaceClipEnabled` + `surfaceClipUv[4]`; helpers `isAncestorFallback`/`isFadingOut`/`renderPass`.
- `TileSelectionState {NotVisited, Culled, Rendered, Refined, RenderedAndKicked, RefinedAndKicked}` (.h:78-85) with `selectionWasKicked` / `originalSelectionState` / `kickSelectionState` transition helpers (.h:87-114).
- `TileTransition` (.h:13-17): `{key, opacity, fadingNodeCount}`.
- `.cpp`: `TilePlanBuilder::parentKey` (.cpp:19-37) — parent-of-tile with a special `OpenGlobus-Earth` 3-group Y-remap; else `TileKey::parent()`.

### TileFrameState.h / .cpp

`TileFrameState::collectInactiveTiles(tiles, frameNumber)` (.cpp:8-20): returns `{cacheKey, tile*}` for every tile whose `lastUsedFrame() != frameNumber` (unload candidates). `TileFrameInactiveEntry` (.h:12-15).

### TileTraversalDetails.h

`TileTraversalDetails{allAreRenderable=true, anyWereRenderedLastFrame=false, notYetRenderableCount=0}` (.h:10-14) — bubbled up during fan-out. `TileTraversalDetailsPolicy` static helpers: `forSingleTile`, `forCulledTile`, `mergeChild` (.h:16-34). cesium-native `Tile::TraversalDetails` equivalent.

### QuadtreeTilingScheme.h / .cpp

cesium-native `QuadtreeTilingScheme` equivalent. `Rectangle` + `rootTilesX/Y` + `schemeId`. `tileCountX/Y(level) = rootTiles << level` (.cpp:16-22). `positionToTile(x,y,level)` (.cpp:24-46, `nullopt` if outside rectangle, clamps to last tile). `tileToRectangle(TileKey)` (.cpp:48-61). Y increases north/south by rectangle convention.

### OctreeTilingScheme.h / .cpp

cesium-native `OctreeTilingScheme` equivalent for 3D voxel/implicit octrees. `OctreeTileID{level,x,y,z}` (.h:10-21). `AxisAlignedBox` + `rootTilesX/Y/Z`. `tileCountX/Y/Z(level) = rootTiles << level` (.cpp:14-24). `positionToTile(Vec3,level)` (.cpp:26-56); `tileToBox(OctreeTileID)` (.cpp:58-77) — note it builds the box from origin-relative sizes (min = size·index).

### TileBoundingVolume.h / .cpp

cesium-native `BoundingVolume` variant. `TileBoundingVolumeKind {Region, Sphere, Box, CylinderRegion, S2Cell}` (.h:16-22). Factory ctors `fromRegion`/`fromLooseRegion`/`fromSphere`/`fromBox`/`fromCylinderRegion`/`fromS2Cell` (.h:49-103). Regions stored radians/meters and NOT transformed by tile transform (`transform()` .h:105-129 returns `*this` for Region and S2Cell). `.cpp`: `toOrientedBoundingBox` (.cpp:30-51) and `estimateGlobeRectangle` (.cpp:53-147, per-kind; sphere/box that contain origin → `Rectangle::MAXIMUM`). **`kPi`** = 3.14159… (.cpp:15).

### TileKey.h / .cpp / TileCacheKey.h / TileID.h / .cpp

- **`TileKey`** (.h:11-26): `{schemeId, z, x, y}` — the quadtree tile identity. `parent()` (.cpp:6-9, `z-1, x>>1, y>>1`), `invertedX/Y`. `std::hash<TileKey>` specialization (.h:33-44, boost-style mix). Comment (.h:9-10): cache key must additionally include provider/layer/style/version.
- **`TileCacheKey::forTile`** (.h:9-14): `schemeId + "/z/x/y"` string — the cache map key (distinct from `TileKey`).
- **`TileID`** (.h:23-27): `std::variant<std::string(url), TileKey, OctreeTileID, UpsampledQuadtreeNode>` — cesium-native `TileID`. `UpsampledQuadtreeNode{tileID}` (.h:11-21). `TileIdUtilities::createTileIdString` (.cpp:5-32, `std::visit`) and `isLoadable` (.cpp:34-37, non-empty url or non-string).

### ImplicitTileIdUtilities.h / .cpp

cesium-native `ImplicitTilingUtilities` equivalent (24 KB .cpp). Static-only class (deleted ctor). Overloaded for both `TileKey` (quadtree) and `OctreeTileID` (octree): `children`, `resolveUrl` (`{level}/{x}/{y}[/{z}]` templates), `computeBoundingVolume` (OBB / cylinder / region / S2Cell / `TileBoundingVolume`), `computeRegionBoundingVolume`, `parentId`, `subtreeRootId`, `absoluteTileIdToRelative`, `mortonIndex` / `relativeMortonIndex`, `levelDenominator(level)` (.h:21-85). Morton/subtree math backs 3D-Tiles 1.1 implicit tiling.

### CrsProfile.h / .cpp

Coordinate-reference-system descriptor associated with a `TileScheme` (does NOT do coordinate transforms — those live in `Transforms`/`TileScheme`). Abstract `CrsProfile` (.h:12-49): `id`/`name`/`unit` (`{Degree, Meter}`)/`isGeographic`/`isProjected`/`xRange`/`yRange`/axis-direction. Two built-ins (.cpp): `webMercator()` (EPSG:3857, meters, ±20037508.342789244) and `wgs84Geographic()` (EPSG:4326, degrees, ±180/±90) as static singletons.

### TileLodTransitionController.h / TileLodTransitionFrameUpdater.h

LOD cross-fade (cesium-native `TilesetOptions::enableLodTransitionPeriod` machinery).

- `TileLodTransitionController::updateTransitions` (.h:24-144, header-only template): clears fade lists; when `enableLodTransitionPeriod` is false, forces all visible tiles to `lodTransitionFadePercentage=1.0` and returns (.h:42-52). Otherwise steps fade by `transitionDelta = deltaSeconds / lodTransitionLength`, moving tiles no longer selected into `fadingKeys`, populating `tilesFadingOut`/`tileTransitions` with `renderOpacity = 1 − fade`, and fading-in newly visible tiles. `wasRenderedInPreviousSelection` gates fade-out (.h:159-165).
- `TileLodTransitionFrameUpdater::update` (.h:18-27): frame-level façade over the controller given `TilesetTileRegistry` + overlays + options.

### TileOcclusionResolver.h / TileOcclusionCallback.h / TileOcclusionState.h

- `TileOcclusionState {NotOccluded, Occluded, OcclusionUnavailable}` (.h:5-9).
- `TileOcclusionCallback = std::function<TileOcclusionState(const TilesetTile&)>` (.h:11-12) — platform/renderer hook.
- `TileOcclusionResolver::check` (.h:10-46, header-only template): returns tile's own occlusion if Occluded/Unavailable/leaf; else if any child is null or `unconditionallyRefine` returns `NotOccluded` (children can't form a reliable finite union); else aggregates children — any `NotOccluded` child → NotOccluded, any Unavailable → Unavailable, else Occluded. cesium-native `TileOcclusionRendererProxy` union logic.

### TileSelectionRootPolicy.h / .cpp

Root fan-out set (determines top-level traversal order). `chooseRoots(schemeId, explicitRoots, useVirtualTerrainRoot)` (.cpp:32-59): explicit roots (from `tileset.json`) win; else virtual terrain root for `Geographic-TMS`/`XYZ-WebMercator`; `Geographic-TMS` → two level-0 roots `(0,0,0),(0,1,0)`; `OpenGlobus-Earth` → three roots `(0,0,0),(0,0,1),(0,0,2)`; default single `(0,0,0)`. `virtualTerrainRootKey` = `{schemeId, -1, 0, 0}` (.cpp:5-8); `isVirtualTerrainRoot` (.cpp:10-13). The `visitTile` renderable check excludes virtual roots so they never draw.

### TileSelectionPostTraversalPolicy.h / .cpp

Post-fan-out decision combining kick + preload. `evaluate` (.cpp:7-43): runs `TileSelectionKickPolicy::shouldKickDescendants`, sets `wasReallyRenderedLastFrame`, and either builds a kick plan or a `preloadRefinedAncestor` flag. `commitPlan` (.cpp:45-70): translates into a `TileSelectionPostTraversalCommitPlan` (kick → trim rendered descendants, return single-tile details, restore child load queue, queue parent Normal/Preload, add renderable replacement; else mark tile Refined + preload). Consumed by `TileSelectionPostTraversalCommitter` in the executor.

---

**Async GPU-upload note (2026-08-06 复核):** the 32-byte `TerrainGpuVertex` async upload path is **complete, not WIP** — produce + upload + draw all wired. It is **config-gated, and off in the shipped demo**: the entry gate needs `hasTerrainWaterMaskMetadata` + complete `terrainGpuVertexBytes`, whose only producer is the upsampler path, which `decoupleImageryFromGeometry=true` disables (`TilesetOptions` struct default is `false`, so an embedder that does not override it **does** run this path). Machine-checked by `contracts::Id::GpuUploadQueueFifo` under `Gate::ImageryDrivenUpsample` — device-verified coverage 0 with `decouple=true`, 152 with `decouple=false`. See `GpuUploadQueue.h` and `content/GltfTerrainUpsampler.h` for the survival conditions.

⚠️ Thread attribution: both `push` and `tryPop` run **on the main thread, in the same frame** (push in `processPendingUploads`, pop in the later `drainGpuUploadQueue`). Worker threads do the earlier vertex conversion, not the enqueue. The one real benefit of the split is the drain-side per-frame upload cap (`maxUploadsPerFrame`=4, interaction floor 2 / idle floor 4) with spillover to the next frame.

---

## 6. tiling — content lifecycle / loading / caching / GPU upload

### TileContentRuntime.h / .cpp

Thin façade forwarding frame work to `TileContentLifecycleManager`, binding `contentAccess_`/`meshPreparation_`/`resourceInvalidator_` collaborators into the lifecycle's template callbacks.

| Item | Lines | Description |
|---|---|---|
| `TileContentRuntimeRequestFrame` | .h:26-38 | Per-frame inputs for load-request pass (tiles map, provider, device, `pPrepRenderer`, budgets/limits). |
| `TileContentRuntimeUploadFrame` | .h:40-51 | Per-frame inputs for upload/drain pass; carries `GpuUploadQueue* gpuUploadQueue` = async CPU→GPU pipeline (.h:45; config-gated, see §5 note). |
| ctor(lifecycle, contentAccess, meshPreparation, resourceInvalidator) | .h:55-59 / .cpp:11-19 | Holds four refs, no ownership. |
| `requestMissingTiles` | .cpp:21-46 | Forwards to `lifecycle_.requestMissingTiles`; binds `prepareUpsampleSourceTile`→`meshPreparation_.prepareUpsampleSourceTile` and `ensureTile`→`contentAccess_.ensureTile`. |
| `processPendingUploads(interactionActive, resourceSmoothingActive)` | .cpp:48-76 | Forwards to `lifecycle_.processPendingUploads`; binds `ensureTile`, `ensureTileChildren`, `markResourcesDirty`. |
| `drainGpuUploadQueue(maxUploadsPerFrame)` | .cpp:78-101 | Forwards to `lifecycle_.drainGpuUploadQueue`; consumes async-prepared GPU data. Called once/frame after `processPendingUploads`. |
| `markResourcesDirty` | .cpp:103-105 | Delegates to `resourceInvalidator_`. |

### TileContentLifecycleManager.h / .cpp

Owns the two long-lived pieces of load state — `TileLoadLifecycle loadLifecycle_` and `TileEmptyContentRegistry emptyContentRegistry_` — and packs frame args into `TilesetContentLifecycleContext` / `TilesetContentUploadContext` before dispatching to the static `TilesetContentLifecycleCoordinator`.

| Item | Lines | Description |
|---|---|---|
| `loadLifecycle()` / `emptyContentRegistry()` accessors | .h:24-32 | Expose the two owned members. |
| `drainGpuUploadQueue<...>` (template) | .h:38-70 | Builds upload context, forwards to coordinator. |
| `requestMissingTiles<...>` (template) | .h:72-104 | Builds `TilesetContentLifecycleContext`, forwards to coordinator. |
| `processPendingUploads<...>` (template) | .h:106-144 | Builds `TilesetContentUploadContext`, forwards to coordinator. |
| `makeContext` | .h:147-170 | Packs load-request context; `maximumSimultaneousTileLoads` default 20. |
| `makeUploadContext` | .h:172-196 | Packs upload context including `gpuUploadQueue`. |
| `~TileContentLifecycleManager` → `shutdown()` | .cpp:6-8, 18-20 | Calls `loadLifecycle_.markDestroyingCancelAndWait()`. |
| `pendingRequests` / `hasPendingWork` | .cpp:10-16 | Delegate to `loadLifecycle_`. |

### TilesetContentLifecycleCoordinator.h

Static, header-only orchestrator. Three entry points mirroring cesium-native `TilesetContentManager` phases. Uses `TileFrameBudgetFallback` to synthesize a local `FrameResourceBudget` when caller passes `budget==nullptr`.

| Item | Lines | Description |
|---|---|---|
| `TilesetContentLifecycleContext` | .h:34-46 | Load-request context. `maximumSimultaneousTileLoads`=20, `smoothedMainThreadUploadLimit`=1 defaults. |
| `TilesetContentUploadContext` | .h:48-61 | Upload context; `gpuUploadQueue` = async CPU→GPU pipeline (.h:55; config-gated, see §5 note). |
| `requestMissingTiles<...>` | .h:65-94 | Fallback budget via `TileFrameBudgetFallback::requestConfig`; delegates to `TileMissingRequestScheduler::request` with `TileCacheKey::forTile` keyer. |
| `processPendingUploads<...>` | .h:99-193 | Fallback budget via `uploadConfig`; delegates to `TilePendingUploadFrameProcessor::process`. The `ensureGltfResources` lambda (.h:132-191) chooses async vs sync upload per tile. |
| — async terrain branch | .h:143-184 | If terrain render content with non-empty `terrainGpuVertexBytes` + water-mask metadata + queue+device present, sets `asyncGpuUploadPending=true`, extracts pre-built 32-byte vertex bytes into `GpuReadyPrimitive` (stride `sizeof(TerrainGpuVertex)`=32, .h:168), sets `meta.useTerrainVertexFormat=true`, pushes `PendingGpuUpload` to queue. No CPU rebuild — bytes produced during decode. |
| — sync fallback | .h:185-190 | Non-terrain: `GltfRenderResourcePreparer::prepare` (synchronous). |
| `drainGpuUploadQueue<...>` | .h:199-261 | Pops uploads (up to `maxUploadsPerFrame`); guards on tile still present + `asyncGpuUploadPending` still set (.h:216-228); calls `GltfRenderResourcePreparer::uploadToGpu` (.h:240) then `TileContentUploadCommitter::finishRenderResourcePreparation` (.h:246); erases the lifecycle upload key via `TilePendingUploadCompletion::eraseUpload`. |

### TileLoadLifecycle.h / .cpp

The shared load-state hub, guarding **the lifecycle mutex** (`mutex_`) + `condition_` around `TilePendingRequestState requestState_` (in-flight network requests) and `TilePendingLoadQueue pendingLoads_` (completed results awaiting commit). Distinct from the second mutex inside `GpuUploadQueue`.

| Item | Lines | Description |
|---|---|---|
| `TileLoadLifecycleCounts` | .h:13-19 | Diagnostics snapshot (requests + terrain/content upload & terminal counts). |
| `mutex()` / `condition()` / `requestState()` / `pendingLoads()` | .h:23-29 / .cpp:5-31 | Accessors to the guarded members. |
| `markDestroyingCancelAndWait` | .cpp:33-41 | Sets destroying, clears pending loads, waits on `condition_` until `requestState_.empty()`, then clears. |
| `pendingRequestCount` | .cpp:43-46 | Locked read of `totalRequestCount`. |
| `counts` | .cpp:48-59 | Locked aggregate of request + queue domain counts. |
| `hasPendingWork` | .cpp:61-64 | `!requestState_.empty() || pendingLoads_.hasWork()`. |
| `containsWorkForCacheKey` / `...AnyCacheKey` | .cpp:66-83 | Locked membership check across both structures. |
| `cancelAndEraseCacheKey` | .cpp:85-90 | Cancels request token + erases from queue. |

### GpuUploadQueue.h

Thread-safe FIFO carrying CPU-prepared bytes across one main-thread frame hop — the **second mutex** in the pipeline (independent of the lifecycle mutex); it guards against the unload path's `eraseCacheKey`/`clear`, not against worker producers. Complete but config-gated off in the demo; see the §5 note and §20.

| Item | Lines | Description |
|---|---|---|
| `PendingGpuUpload` | .h:15-19 | `{TileKey tileKey; std::string cacheKey; GpuReadyData data;}`. `cacheKey` used for `eraseUpload`. |
| `push` | .h:26-29 | Worker-thread enqueue under `mutex_`. |
| `tryPop` | .h:32-38 | Main-thread FIFO pop (`std::optional`). |
| `hasWork` / `size` / `clear` | .h:41-54 | Non-blocking state queries + drain. |
| `mutex_`, `queue_` | .h:57-58 | `std::deque<PendingGpuUpload>` guarded by its own mutex. |

### GpuReadyData.h

CPU-prepared payload consumed on the main thread (GL/Metal context).

| Item | Lines | Description |
|---|---|---|
| `GpuReadyPrimitive` | .h:14-48 | vertexBytes + `vertexStride` (**32 TerrainGpuVertex / 120 GltfGpuVertex**, .h:17), indices, `TextureData` (.h:25-31), `metadata` (`GltfPrimitiveRenderResources`), `sortCenterEcef`, optional `InstanceData`. |
| `GpuReadyData` | .h:51-54 | `std::vector<GpuReadyPrimitive> primitives; valid()==!primitives.empty()`. |

### TileLoadState.h / TileLoadStatus.h / TileLoadStatePredicates.h

The content state machine enums.

| Item | Lines | Description |
|---|---|---|
| `TileLoadState` | TileLoadState.h:5-13 | Ordered enum: **Unloading=-2, FailedTemporarily=-1, Unloaded=0, ContentLoading=1, ContentLoaded=2, Done=3, Failed=4**. Ordering used by predicate comparisons. cesium-native `TileLoadState` equivalent. |
| `TileContentKind` | TileLoadState.h:15-20 | `Unknown/Empty/External/Render`. |
| `TileLoadStatus` | TileLoadStatus.h:8-15 | `Renderable/Empty/External/RetryLater/Failed/Cancelled`; cesium-native `TileLoadResultState` split. `isSuccessfulTileLoadStatus` (.h:17-29) = Renderable/Empty/External. |
| `canRefreshContentMetadataBeforeContentAccepted` | TileLoadStatePredicates.h:8-22 | True only for Unloaded/ContentLoading. |
| `hasResolvedAvailabilityBoundaryContent` | TileLoadStatePredicates.h:24-27 | `state > ContentLoading`. |

### TileContentStateTransition.h

Free-function state mutators applied to `(TileLoadState&, TileContentKind&)` pairs (and `TileRenderContentState&` / `TileSelectionFrameState&`). No branching — pure assignments.

| Method | Lines | Transition |
|---|---|---|
| `markLoading` | .h:12-16 | → ContentLoading, kind Unknown. |
| `markRenderLoaded` | .h:18-22 | → ContentLoaded, kind Render. |
| `markRenderDone` | .h:24-30 | `markRenderContentReady()` → Done, Render. |
| `markRenderFailedTemporarily` | .h:32-40 | Clears mesh/gltf-ready flags → FailedTemporarily. |
| `markFailedTemporarily` / `markFailedPermanently` | .h:42-52 | → FailedTemporarily / Failed. |
| `markEmptyLoaded` / `markEmptyDone` | .h:54-64 | kind Empty → ContentLoaded / Done. |
| `markExternalDone` | .h:66-72 | kind External, sets `unconditionallyRefine=true` → Done. |
| `markUnloading` / `markUnloaded` | .h:74-84 | → Unloading / Unloaded (+ `clearFrameRenderability`). |

### TileContentRuntimeState.h

Cross-frame per-tile lifecycle aggregate embedded in `TilesetTile`.

| Item | Lines | Description |
|---|---|---|
| fields | .h:16-28 | `renderContent`, `loadState`(Unloaded default), `contentKind`(Unknown), `contentUpsampleKind`, `rasterDetailSourceProjection`. cesium-native `UpsampledQuadtreeNode` marker via `contentUpsampleKind`. |
| upsample predicates/mutators | .h:30-65 | `derivesTerrainFromParent`, `isTerrainAvailabilityUpsample`, `isRasterDetailUpsample`, `markTerrainAvailabilityUpsample`, `markRasterDetailUpsample`, `clearUpsampleKind`. |
| `TileContentUpsampleKind` | TileContentUpsampleKind.h:5-9 | `None/TerrainAvailability/RasterDetail`. |

### TileLoadState.h loadState machine — pipeline overview

Flow: **request** (`TileLoadScheduler`→`TileLoadRequestDispatcher`, async network) → completion callback enqueues into `TilePendingLoadQueue` (upload vs terminal) → **commit** (`TilePendingUploadFrameProcessor`→`TilePendingLoadCommitCoordinator`, main thread) → sync GPU upload OR **async**: dispatch to `GpuUploadQueue`, drained by `drainGpuUploadQueue` (config-gated, off in demo) → **cache/unload** (`TileContentCacheManager` byte-budget eviction).

### TileLoadTypes.h

Shared value types for the load pipeline.

| Item | Lines | Description |
|---|---|---|
| `TileLoadRequest` | .h:17-21 | `{key, group=Normal, priority}`. |
| `TileLoadRequestOutcome` | .h:23-26 | `{issued, blockedByInflight}`. |
| `TileLoadedContent` | .h:28-71 | glTF model + transform + metadata + availability updates. `satisfiesContentTerrainPayloadContract` (.h:48-62) enforces model/metadata `rasterOverlayDetails` parity for terrain. |
| `TileLoadDomain` | .h:73-81 | `TerrainContent/Content`; `isContentLoadDomain` true for both. |
| `TileLoadResult` | .h:83-178 | Factory-built result. `fromContentResult` (.h:123-152) demotes Renderable-with-null-model → Failed and enforces terrain payload contract. `shouldUpload()`=Renderable+has-payload (.h:163-166). cesium-native `TileLoadResult` equivalent. |
| `PendingTileLoad` | .h:180-223 | Queue element: `{domain,key,cacheKey,group,priority,result}`. |

### TileLoadScheduler.h

Static, header-only. Sorts requests by priority, then per-request classifies and dispatches under the lifecycle mutex. Enforces network in-flight capacity via `FrameResourceBudget`.

| Item | Lines | Description |
|---|---|---|
| `TileLoadSchedulerInput` | .h:25-29 | `{lifecycle, budget, contentProvider}`. |
| `requestMissingTiles<...>` | .h:38-177 | Main loop. Skips if destroying (.h:51-56), empty cacheKey, already-in-flight (`containsWorkForCacheKey`), or empty tile. |
| — upsample branch | .h:80-127 | `TerrainContentUpsample`: prepares source tile, builds gltf via `TileGltfTerrainUpsampledChildMaterializer`, `queueUpsampledLoad`. |
| — content branch | .h:129-173 | Checks `hasNetworkInflightCapacity` (lane Terrain/ContentRequest by `providesTerrainQuadtree`); sets `blockedByInflight` and breaks on saturation (.h:141-150); dispatches `requestContent`. |
| `requestOptionsForTile` | .h:180-194 | Sets `generateTerrainRasterOverlayDetails` when a child is terrain-availability upsample. |
| `shouldStopAfterDispatch` | .h:196-199 | Stops on Destroying/Blocked. |

### TileLoadRequestPlanner.h / .cpp

Pure classifier: `TileLoadRequestSnapshot` → `TileLoadRequestKind`.

| Item | Lines | Description |
|---|---|---|
| `TileLoadRequestKind` | .h:8-12 | `Skip/TerrainContentUpsample/Content`. |
| `TileLoadRequestSnapshot` | .h:14-21 | Tile presence, upsample kind, provider support/quadtree, has-render-content, loadState. |
| `classify` | .cpp:5-31 | Skip if already loaded (state ∉ {Unloaded, FailedTemporarily}); else upsample kind → upsample; else provider-supported → Content (Skip if already has render content and not FailedTemporarily). |

### TileLoadRequestDispatcher.h / .cpp

Issues one load and wires its async completion callback back into `pendingLoads`. All state mutation under the lifecycle mutex.

| Method | Lines | Description |
|---|---|---|
| `TileLoadDispatchResult` | .h:19-24 | `Issued/Skipped/Blocked/Destroying`. |
| `requestContent<OnIssuedFn>` | .h:39-124 | Guards destroying/dup/capacity; `budget.tryIssue` (.h:71-76); `requestState.beginContentRequest` with a `CancellationToken`; calls `provider.requestTileContent`; completion callback (.h:90-120) normalizes result per domain and calls `enqueueCompletedLoadResult`, then `completeContentRequest` + `condition.notify_all`. |
| `queueUpsampledLoad` | .cpp:7-51 | Synchronous variant for upsampled results: normalizes, routes to `addTerminalResult` or `addUpload` by `shouldUploadForDomain`. |
| `enqueueCompletedLoadResult` | .h:127-147 | `result.shouldUpload()` → `addUpload`, else `addTerminalResult`. |
| `toHttpPriority` | .h:149-159 | Preload→Low, Normal→Normal, Urgent→High. |

### TileLoadQueue.h / .cpp

Simple main-thread request accumulator (distinct from `TilePendingLoadQueue`). Dedup by key, upgrade priority in place.

| Item | Lines | Description |
|---|---|---|
| `queue` | .cpp:9-28 | If key exists, upgrade group/priority via `TileLoadPriorityPolicy::hasHigherPriority`; else push. |
| `erase` / `eraseIf` / `clear` / `resize` | .h:15-24 / .cpp:30-50 | `resize` truncates only (never grows). |
| `front` / `requests` / iterators | .h:28-32 / .cpp:60-74 | Read access. |

### TilePendingLoadQueue.h / .cpp

Completed-load holding area: separate `uploads_` (need GPU) and `terminalResults_` (empty/external/failed) deques, plus `uploadKeys_` set for O(1) in-flight dedup. Priority-selected drains gated by `FrameResourceBudget`.

| Item | Lines | Description |
|---|---|---|
| `addUpload` | .cpp:45-62 | Skips if a terminal result already holds the key; inserts into `uploadKeys_` + `uploads_`. |
| `addTerminalResult` | .cpp:64-81 | Skips if `uploadKeys_` holds key or dup terminal exists. |
| `eraseUploadKey` | .cpp:83-86 | Erases only the `uploadKeys_` claim (used by async completion to release the tile-protection claim without dropping data). |
| `eraseCacheKey` | .cpp:88-106 | Full erase from all three structures. |
| `containsCacheKey` | .cpp:19-43 | Membership across set + both deques. |
| `takeHighestPriorityTerminalResult` | .cpp:144-161 | Selects best, gated by `budget.tryFinalize(TerminalState,...)`. |
| `takeHighestPriorityUpload` | .cpp:163-200 | Skips non-Urgent when `interactionActive`; gated by `tryFinalize` on `ContentFinalize`/`TerrainFinalize` lane (`uploadLaneForDomain`, .cpp:11-15). |
| domain counters | .cpp:120-142, 202-211 | `countDomain` for terrain/content upload & terminal diagnostics. |

### TilePendingLoadProcessor.h

Static drain loop: terminal results first, then uploads; each dispatch time-recorded into the frame budget lane.

| Item | Lines | Description |
|---|---|---|
| `TilePendingLoadProcessorInput` | .h:14-19 | `{lifecycle, budget, interactionActive, elapsedOverrideMs}`. |
| `processPendingLoads<...>` | .h:23-87 | Loop 1 (.h:30-53): pop terminal under lifecycle lock, `processTerminalResult`, `recordElapsed(TerminalState)`. Loop 2 (.h:55-84): pop upload, `processUpload`, record on Content/TerrainFinalize lane. Returns `changed`. |

### TilePendingUploadFrameProcessor.h

Binds `TilePendingLoadCommitCoordinator` commit functions and forwards to `TilePendingLoadProcessor`.

| Item | Lines | Description |
|---|---|---|
| `TilePendingUploadFrameProcessorInput` | .h:23-33 | Provider/device/`pPrepRenderer`/overlays/registry + interaction & smoothing flags. |
| `process<...>` | .h:41-82 | `processTerminalResult`→`commitTerminalResult`; `processUpload`→`commitUpload`. |

### TilePendingLoadCommitCoordinator.h

The commit stage: turns a `PendingTileLoad` into committed tile state, running raster-overlay mapping and render-content preparation. Contains the async-upload fork point.

| Item | Lines | Description |
|---|---|---|
| `captureInitialBoundingVolumes` | .h:28-39 | Snapshots initial bounding volumes into metadata (for failure restore). |
| `commitTerminalResult<...>` | .h:44-68 | Ensures tile, delegates to `TileTerminalLoadCommitter::commitTerminalResult`, applies `ensureChildren`/`markResourcesDirty`. |
| `commitUpload<...>` | .h:74-153 | Ensures tile (erase upload claim if gone, .h:87-93); fail path via `shouldFailUploadForDomain` (.h:95-116); else applies availability updates + `TileContentUploadCommitter::prepareRenderContent` + `ensureGltfResources`. |
| — async keep-alive | .h:135-138 | If `asyncGpuUploadPending`, returns WITHOUT erasing the upload key — the pending upload claim protects the tile from unload until `drainGpuUploadQueue` completes. |
| — sync finish | .h:140-152 | Else `finishRenderResourcePreparation` + `applyCommitAction` + erase upload. |
| `applyCommitAction` | .h:159-170 | Fires `ensureChildren` / `markResourcesDirty` per action flags. |

### TileContentUploadCommitter.h / .cpp

Two-phase render-content commit + failure rollback.

| Method | Lines | Description |
|---|---|---|
| `TileContentUploadCommitAction` | .h:15-18 | `{ensureChildren, resourcesDirty}`. |
| `prepareRenderContent` | .cpp:46-70 | Releases old overlay refs, `TileContentUploadPolicy::prepareGltfRenderContent`, then `ensureProjectionDetailsFromActiveOverlays` when model present. Mirrors cesium-native `RasterOverlayCollection::addTileOverlays`. |
| `finishRenderResourcePreparation` | .cpp:72-83 | On failure: restore initial bounding volumes, release overlays, `markGltfRenderResourcesFailed`. Returns `{resourcesReady, true}`. |
| `effectiveContentBoundingVolumeForLoad` / `restoreInitialBoundingVolumesAfterResourceFailure` | .cpp:15-42 | Bounding-volume selection + rollback helpers. |

### TileContentUploadPolicy.h / .cpp

| Method | Lines | Description |
|---|---|---|
| `prepareGltfRenderContent` | .cpp:9-21 | `renderContent.prepareGltfContent(model, transform)`, set terrain flag, apply metadata, `tile.markRenderContentLoaded()`. |
| `markGltfRenderResourcesFailed` | .cpp:23-27 | `clearGltfContent()` + `tile.markRenderContentFailedTemporarily()`. |

### TilePendingRequestState.h / .cpp

Tracks in-flight network requests and their cancellation tokens (guarded externally by the lifecycle mutex). Terrain vs content request keys tracked separately for diagnostics.

| Item | Lines | Description |
|---|---|---|
| `PendingRequestCounts` | .h:12-16 | `{terrainRequests, contentRequests, totalRequests}`. |
| `beginTerrainRequest` / `beginContentRequest` | .cpp:35-56 | Reject if destroying/empty/dup; insert key + store `CancellationToken`. |
| `completeTerrainRequest` / `completeContentRequest` | .cpp:58-70 | Erase from sets + token map. |
| `cancelAndErase` | .cpp:72-80 | Cancels token, then erases. |
| `markDestroyingAndCancelRequests` | .cpp:82-88 | Sets `destroying_`, cancels every token. |
| `clearAfterCallbacksComplete` | .cpp:90-97 | Once `pendingRequests_` drained, clears content keys/tokens and unsets destroying. |

### TilePendingUploadCompletion.h / .cpp

| Method | Lines | Description |
|---|---|---|
| `eraseUpload` | .cpp:7-12 | Locks lifecycle mutex, calls `pendingLoads().eraseUploadKey(cacheKey)` — releases the async tile-protection claim after `drainGpuUploadQueue` finishes or aborts. |

### TileContentCacheManager.h / .cpp

Byte-budget eviction owner. Holds `totalBytesUsed_`, `TileUnloadQueue unloadQueue_`, `cacheBytesDirty_`. cesium-native `Tileset::_unloadCachedTiles` equivalent.

| Item | Lines | Description |
|---|---|---|
| accessors | .h:24-28 | `totalBytesUsed`, `unloadQueue`, `cacheBytesDirty`. |
| `updateTotalBytesUsed` | .cpp:10-14 | `TileCacheMetrics::estimateTotalBytes(tiles, {})` (lifecycle param unused). |
| `markEligibleForUnloading` / `markIneligibleForUnloading` | .cpp:16-25 | Push/remove from `unloadQueue_` via `TileIndexState`. |
| `eraseTileIndexState` | .cpp:27-41 | Erases cache-key state across unload queue, empty registry, load queue, lifecycle. |
| `unloadTileContent` | .cpp:43-53 | Delegates to `TileContentUnloadCoordinator::unloadContent` (no terrain heightmap cache: `nullptr`). |
| `unloadCachedBytes<ClearChildrenFn>` | .h:55-129 | Byte-budget eviction. When `resourceSmoothingActive` && over budget: defer non-Unloading tiles to avoid single-frame spike (.h:71-93). Else `TileCacheUnloadCoordinator::run` with active-work guard `TileSubtreeWorkTracker::hasActiveContentWork`; refreshes `totalBytesUsed_` if `shouldRefreshTotalBytes`. |

### TileContentUnloadCoordinator.h

Static unload logic per `TileContentKind`. Returns `TileCacheUnloadContentResult` (Keep/Remove/RemoveAndClearChildren).

| Item | Lines | Description |
|---|---|---|
| `unloadContent` (3-arg / 5-arg) | .h:20-96 | Keep if ContentLoading (.h:46-48); release overlay refs; External w/ refs → Keep. Render kind: if `Unloading` with content-loading upsampled child protection → Keep and release main-thread resources (.h:66-81); else `releaseRenderContentResources` + erase terrain cache. Finally `markContentUnloaded`. |
| `shouldReleaseRenderResourcesForProtectedUnload` | .h:99-103 | True for ContentLoaded/Done. |

### TileCacheOwnershipManager.h / .cpp

Facade binding `TileContentCacheManager` to the live `tiles` map, frame flags, and byte budgets by reference; wires recursive child clearing.

| Method | Lines | Description |
|---|---|---|
| ctor | .cpp:15-29 | Holds refs to cache, lifecycle, load queue, tiles map, `resourceSmoothingActiveForFrame`, `maximumCachedBytes`, `tileCacheUnloadTimeLimit`. |
| `updateTotalBytesUsed` / `markEligible/Ineligible` / `eraseTileIndexState` | .cpp:31-50 | Forward to cache manager with `tiles_`. |
| `clearChildrenRecursively` | .cpp:52-65 | `TileSubtreeRemovalCoordinator::clearChildrenRecursively` with `TileCacheKey::forTile` + `eraseTileIndexState`. |
| `unloadTileContent` | .cpp:67-81 | Unloads; on `RemoveAndClearChildren` recurses; marks resources dirty if not Keep. |
| `unloadCachedBytes` / `unloadConfiguredCachedBytes` | .cpp:83-101 | Drive eviction with `tileCacheUnloadTimeLimit_` + smoothing flag; the latter uses `maximumCachedBytes_`. |

### TileMeshPreparationManager.h / .cpp

Prepares upsample-source ancestor tiles and content-terrain frame meshes; queues follow-on tile loads. Constructor's `hasTerrainQuadtree`/`device`/`rasterOverlays` params are currently unused (vestigial from the removed `SurfaceTileMesh` model, .cpp:15-17).

| Method | Lines | Description |
|---|---|---|
| `prepareRenderableTile` → `prepareContentTerrainFrame` | .cpp:22-38 | `TileMeshFrameEnsurer::ensureContentTerrain`, marks resources dirty. |
| `prepareUpsampleSourceTile` | .cpp:40-56 | `TileUpsampleSourcePreparer::prepareSourceTile`; binds ancestor-prepare + `queueTileLoad`. |
| `queueTileLoad` | .cpp:62-67 | `loadQueue_.queue(key, group, priority)`. |

### GltfRenderResourcePreparer.h — async CPU/GPU split

Declarations only here (impl elsewhere). Two-phase split for the async pipeline. cesium-native `prepareInLoadThread` / `prepareInMainThread` split.

| Method | Lines | Description |
|---|---|---|
| `prepare` | .h:18-20 | Legacy synchronous path (still used for animation + non-terrain content). |
| `prepareCpuWork` / `prepareCpuWorkFromModel` | .h:25-36 | Phase 1: SurfaceVertex→GPU bytes + texture decode → `GpuReadyData`. ⚠️ Labelled "worker thread" but **both call sites are on the main thread**; only the `FromModel` overload (owned copy) is safe to move off it. |
| `uploadToGpu` | .h:41-44 | Phase 2 (main thread): create GL/Metal buffers from `GpuReadyData`; called by `drainGpuUploadQueue`. |

### Async terrain GPU-upload path — complete, config-gated

The 32-byte `TerrainGpuVertex` async upload path is end-to-end (2026-07-01): produce + upload + draw all wired (`terrainShader` + `makeTerrainPrimitiveCommand`). Not WIP. It simply **does not execute under the demo's `decoupleImageryFromGeometry=true`** — see the note at the end of §5 for the gate chain and the contract that proves liveness either way.

| Fact | Location | Status |
|---|---|---|
| `TerrainGpuVertex` = 32 bytes (pos3+nrm3+tex2) | GltfRenderGeometryBuilder.h:31-39 (`static_assert sizeof==32`) | done |
| `GltfGpuVertex` = 120 bytes | GltfRenderGeometryBuilder.h:14-27 (`static_assert sizeof==120`) | reference format |
| `GltfPrimitive::terrainGpuVertexBytes` pre-built during decode | content/GltfModel.h:113-116 | produce done |
| `asyncGpuUploadPending` flag | TileRenderContentState.h:116 | done |
| `useTerrainVertexFormat` (32 vs 120 selector) | TileRenderContentState.h:111 | set in coordinator (.h:176) |
| Async push/upload/drain | TilesetContentLifecycleCoordinator.h:143-261 | upload side done |
| DRAW side wired (2026-07-01) | `GltfDrawCommandBuilder.cpp:70` branches on `useTerrainVertexFormat` → `renderer.makeTerrainPrimitiveCommand` (stride 32, `kind=GltfPrimitive`, `shader=terrainShader`) | **done** (compiles + unit-tested both backends; pixel-verify pending a reachable QM terrain server) |
| `Renderer::terrainShader()` defined + terrain shaders added | `kTerrainVertex/FragmentGLSL` (Renderer.cpp:855-...), `kTerrainVertex/FragmentMSL` (Metal, buffers ≤23), `terrainShader()` getter + `makeTerrainPrimitiveCommand` | **done** |

Files read and verified: all listed content-lifecycle files under `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/tiling/`, plus `content/GltfModel.h` and `renderer/Renderer.h`. Line numbers above are current as read.

---

## 7. tiling — raster overlay mapping

### RasterMappedToTilesetTile.h / .cpp

cesium-native `RasterMappedTo3DTile` equivalent — links one geometry tile to one imagery tile with a 3-state attachment machine. Raw-pointer accessors are non-owning; shared handles keep tile/texture lifetime stable.

| Item | Lines | Description |
| --- | --- | --- |
| `enum State` | .h:44-48 | `Unattached=0 / TemporarilyAttached=1 / Attached=2` (mirrors cesium-native AttachmentState) |
| `enum MoreDetail` | .h:51-55 | `No / Yes / Unknown` (mirrors RasterOverlayTile::MoreDetailAvailable) |
| `enum ReadyTileSource` | .h:57-61 | `None / Real / Ancestor` — provenance of `_pReadyTile` |
| `struct SourceTileList` | .h:63-73 | Source imagery tile grid: `sourceZoom`, `sourceBounds`, `sourceKeys`, `minX/minY/maxX/maxY` |
| Invariants | .h:35-40 | `_pLoadingTile`=desired (may be Loading/Failed); `_pReadyTile`=rendered (Loaded/Done); both-null=Unattached, both-set=TemporarilyAttached, ready-only=Attached; `originalFailed_` set-once |
| `update()` | .h:89-100 / .cpp:139-469 | 7-step flow; params: geometryKey, overlayDetails, targetScreenPixels x/y, provider, prepRenderer, missingProjections, parentTile, overlayIndex, hasRenderContentDetails, boundingVolumeRectangle |
| — stale-handle drop | .cpp:160-179 | If ready/loading tile no longer owned by provider (option/coverage change) → releaseTileReferences or drop loading handle + recompute state |
| — Step 1 attached fast path | .cpp:181-202 | If `Attached`: revert to Unattached if renderer resources gone; else markUsed + return MoreDetail from `_pReadyTile->isMoreDetailAvailable()` |
| — Step 2 failure fallback | .cpp:204-237 | On loading-tile `Failed`, set `originalFailed_`, walk parent geometry chain via `findParentTileOverlayPreferLoading`, adopt ancestor's loading/ready tile |
| — projection/rectangle lookup | .cpp:239-255 | `findRectangleForProjection` reads `textureCoordinateID` + invertedV; falls back to boundingVolumeRectangle when no render-content details |
| — placeholder retry | .cpp:257-267 | Placeholder loading tile cleared once `provider.isReady()` |
| — map-to-geometry | .cpp:273-338 | If no loading tile: provider-not-ready→placeholder (texCoordID=-1); render-content+geometryRectangle→`provider.mapRasterTilesToGeometryTile`; missing projection→record in `missingProjections` at index-after-list + placeholder; else boundingVolume path |
| — Step 3 loading→ready promote | .cpp:346-380 | On `Loaded`/`Done`: detach old ready, `_pReadyTile=_pLoadingTile`, cache texture, `computeTranslationAndScale`. Gated by `canPromoteReadyRaster` (needs prepRenderer OR Unattached OR no ready) |
| — Step 4 ancestor substitution | .cpp:382-419 | While loading, walk parent chain via `findLoadedTileOverlay` for a ready ancestor tile; adopt as `_pReadyTile` (source=Ancestor), recompute UV for CURRENT child bounds |
| — Step 5 failed-no-fallback valve | .cpp:421-434 | Failed loading tile + no ancestor + no ready → mark ready (texture null) `Attached` so raster failure never blocks geometry |
| — Step 6 attach | .cpp:437-452 | `_pReadyTile->loadInMainThread()`; if prepRenderer + renderer resources → `attachRasterInMainThread(geometryKey, overlaySlot, tile, tex, offset/scaleUV)`; state=Temporarily/Attached |
| — Step 7 return MoreDetail | .cpp:454-468 | loading→Unknown; else MoreDetail from ready tile unless `originalFailed_` |
| `isMoreDetailAvailable()` | .cpp:471-479 | `!loading && !originalFailed && ready && ready.moreDetail==Yes` |
| `hasPendingNonPlaceholderLoadingTile()` | .cpp:481-485 | Loading tile present and not Placeholder |
| `detachFromTile()` | .cpp:491-504 | `detachRasterInMainThread(geometryKey, overlaySlot)` (skips Failed tiles / z<0), state→Unattached |
| `releaseTileReferences()` / `clearTileOwnershipState()` | .cpp:506-528 | Detach + drop both handles, reset UV/flags/texCoordID/slot |
| `loadThrottled()` | .cpp:530-542 | markUsed + `provider.loadTileThrottled(loadingTile, budget)`; no-op for null/placeholder |
| `computeTranslationAndScale()` | .h:122-124 / .cpp:544-568 | Delegates to `TileSurface::computeTranslationAndScale`; non-inverted V passes through `textureWindowForNorthWestUv`. Pure rectangle-ratio, no tile-size assumption (edge bleed handled by GL CLAMP_TO_EDGE) |
| UV fields | .h:189-191 | `rasterUV = geometryUV * scale + offset`; `offsetU/V_`, `scaleU/V_` |
| `textureCoordinateID_` / `overlaySlot_` | .h:204,208 | Per-projection texcoord index (cesium `_textureCoordinateID`) vs. resource-notification layer slot — distinct identities |

Free helpers (.cpp:15-134): `addProjectionToList` (dedup append), `findLoadedTileOverlay` (ancestor ready lookup, same-owner check via `hasSameOverlayOwner`), `findParentTileOverlayPreferLoading`, `findRectangleForProjection`, `isCurrentProviderTile`.

### SurfaceRasterBinding.h / .cpp

Decides whether a mapped raster is drawable and produces the render-time binding (texture + UV transform). "Drawable" = ready tile is a real Loaded/Done raster with an owned texture.

| Item | Lines | Description |
| --- | --- | --- |
| `enum SurfaceRasterBindingKind` | .h:12-16 | `None / RealTile / AncestorTile` |
| `struct SurfaceRasterBinding` | .h:18-26 | `kind`, non-owning `tile`, `tileHandle` (shared), `offset/scaleU/V` |
| `isLegalSurfaceRasterTile()` | .cpp:7-14 | Tile non-null, has texture, state Loaded/Done |
| `chooseSurfaceRasterBinding()` | .cpp:16-39 | Uses `mapped->getReadyTile()` only; copies UV; `kind` = Ancestor if `getReadyTileSource()==Ancestor` else RealTile. Placeholders/failed/no-texture → `{}` |
| `rasterOverlayBindingAllowedByPolicy()` | .cpp:41-59 | Requires visible overlay + texture; `BaseImagery` role always allowed; `SkipUntilReady` fallback policy requires `getLoadingTile()==nullptr` |

### TileRasterOverlayState.h / .cpp

Per-tile collection of `RasterMappedToTilesetTile` mappings (one slot per active overlay), plus missing-projection accumulation and mapping-identity invalidation. Lives on `TilesetTile.rasterOverlayState`.

| Item | Lines | Description |
| --- | --- | --- |
| `mappings_` / `missingProjections_` | .h:70-71 | Vector of unique_ptr mappings; deferred missing projections |
| `ensureMappingSlots` / `resizeMappingSlots` | .h:32-38 / .cpp:42-54 | Grow to `count`; shrink releases + resets tail slots |
| `mappingAt` / `ensureMapping` | .cpp:8-30 | Bounds-checked accessor; lazy-construct slot |
| `releaseMapping` | .cpp:32-40 | `releaseTileReferences` + reset slot |
| `hasReadyMapping()` | .cpp:56-59 | Cover-ready: `getReadyTile()!=nullptr` (failed ready still counts — required imagery must not permanently block geometry) |
| `hasDrawableReadyMapping()` | .cpp:61-65 | Drawable-ready: `chooseSurfaceRasterBinding(...).kind != None` |
| `synchronizeMappingIdentity()` | .cpp:67-82 | On identity change, `releaseAndClearReferences` then store new identity |
| `releaseReferences` / `releaseAndClearReferences` | .cpp:84-100 | Release handles; latter also clears vector + missing projections + identity |
| `forEachMapping()` | .h:51-56 | Visits each slot (nullable) — used by ancestor lookups in RasterMappedToTilesetTile |

### TileRasterOverlayPrefetcher.h / .cpp

Per-tile prefetch driver: syncs mapping slots to the active-overlay list, runs `RasterMappedToTilesetTile::update` for each overlay in priority order, then throttled-loads the resulting loading tiles. Selection-phase (no renderer resources; `pPrepRenderer` optional).

| Item | Lines | Description |
| --- | --- | --- |
| `struct TileRasterOverlayPrefetchAction` | .h:14-16 | `unloadTileContent` — signals caller to unload+reload tile when projections are missing |
| `prefetch()` | .h:20-27 / .cpp:20-141 | Static entry point |
| — done-tile guard | .cpp:29-33 | `doneTileCannotHoldRasterOverlays` → release all + return |
| — slot sync | .cpp:34-44 | `synchronizeMappingIdentity` (signature) + `resizeMappingSlots` + `clearMissingProjections`; empty overlays → return |
| — mapping context | .cpp:48-50 | `TileRasterOverlayMappingPolicy::contextFor(tile)` |
| — per-overlay loop | .cpp:52-139 | For each `overlayProcessingOrder` index: skip invisible (release slot), `ensureTileProvider(device)`, compute geometry/boundingVolume/target rectangles, `computeDesiredScreenPixels`, then `mapped.update(...)` |
| — MSE note | .cpp:85-96 | Passes TILESET MSE (16.0), NOT overlay MSE (2.0), into `computeDesiredScreenPixels`; overlay MSE divided again downstream — using overlay MSE twice underestimates zoom by ~3 levels (16/2=8) |
| — missing projection handling | .cpp:123-131 | If Done tile → `action.unloadTileContent=true`; else `releaseAndClearReferences`; return early |
| — throttled load | .cpp:133-138 | Non-placeholder loading tile → `mapped.loadThrottled(provider, &frameResourceBudget)` |

### TileRasterOverlayReadinessPolicy.h / .cpp

Pure policy helpers for overlay eligibility, required-readiness gating, and priority ordering.

| Method | Lines | Description |
| --- | --- | --- |
| `doneTileCannotHoldRasterOverlays()` | .cpp:14-19 | True if tile `Done` but not Render content OR `!hasRasterOverlayHostContent()` |
| `requiredOverlaysReady()` | .cpp:21-36 | For every visible overlay with `blocksCompleteRenderable()`, require `hasReadyMapping(i)` |
| `processingOrder()` | .cpp:38-57 | Stable-sort indices by overlay `priority()` descending (null → `RasterOverlayPriority::Low`) |

### TileRasterOverlayDetailsGenerator.h / .cpp

Generates `RasterOverlayDetails` (projected rectangles + per-vertex overlay UVs) for a tile's glTF render content, from a bounding region or tight model bounds. This is the geometry side of the geometry↔imagery mapping: it writes glTF TEXCOORD sets consumed later as `geometryUV`.

| Method | Lines | Description |
| --- | --- | --- |
| `projectRegionRectangle()` | .cpp:222-236 | Split at antimeridian, then Geographic passthrough or `projectRectangleSimple(WebMercator, ...)` |
| `computeTightModelBoundingRegion()` | .cpp:238-275 | Iterates glTF primitive vertices (skipping skirt verts via `contributesToComputedBounds`), converts ECEF→cartographic, expands `BoundingRegionBuilder` |
| `projectEffectiveContentBoundingVolumeRectangle()` | .cpp:277-292 | Projects tile's content/bounding volume Region rectangle (nullopt if not Region kind) |
| `ensureProjectionDetailsFromRegion()` | .cpp:294-335 | For a Region volume: write texcoords + append projection/rectangle at next texcoord index |
| `ensureProjectionDetailsFromModelBounds()` | .cpp:337-376 | Same via tight model bounds when no Region volume |
| `ensureProjectionDetailsFromActiveOverlays()` (2 overloads) | .cpp:378-424 | For each ready provider's projection, generate details from Region or model bounds; returns count generated |
| `writeGltfOverlayTexCoords()` (file-local) | .cpp:163-218 | Per primitive/vertex: world position → cartographic → `projectPositionForOverlayClosestToRectangle` → clamped `(x-west)/width`,`(y-south)/height` into `vertexTexCoords[texCoordIndex]`. Guarded by **`kGltfMaxTexCoordSets`** = 8 (GltfModel.h:20) |
| `projectPositionForOverlayClosestToRectangle()` (file-local) | .cpp:131-161 | Antimeridian disambiguation: picks ±2π longitude with smaller `computeSignedDistance` to rectangle (uses `Epsilon5`, `OnePi`, `TwoPi`) |
| `applyGeneratedProjectionDetails` / `mergeBoundingRegion` (file-local) | .cpp:35-78 | Merge generated projection+rectangle+invertedV(false)+region into existing details |

### TileRasterOverlayDetailsDeriver.h

Header-only. Derives child `RasterOverlayDetails` from parent details by remapping the parent's per-projection overlay rectangles into the child's sub-rectangle — used for upsampled/refined tiles without re-projecting vertices.

| Method | Lines | Description |
| --- | --- | --- |
| `deriveChildFromParent()` | .h:15-83 | Empty parent → `setGeographicRectangle(childBounds)`. Else copy projections/invertedV, and per projection interpolate child rectangle from parent overlay rectangle using `relativeX/relativeY` of projected child bounds within projected parent bounds |
| `projectBounds()` | .h:86-97 | Geographic passthrough or `projectRectangleSimple(WebMercator)` |
| `relativeX` / `relativeY` | .h:99-112 | Normalized 0..1 position; `relativeX` adds `TwoPi` across antimeridian |
| `mixHorizontal()` | .h:118-129 | Longitude lerp with antimeridian wrap + `convertLongitudeRange` for Geographic |

### TileRasterOverlayFrameProcessor.h

Header-only frame orchestrator. Drives per-frame overlay uploads and selection-phase prefetch across all visible/requested tiles.

| Method | Lines | Description |
| --- | --- | --- |
| `struct TileRasterOverlayUploadResult` | .h:22-25 | `processedUploads`, `resourcesDirty` |
| `processPendingUploads()` | .h:29-44 | Sums `overlay->processPendingUploads(interactionActive, &budget)` across overlays; `resourcesDirty = processed>0` |
| `prefetchSelection()` (template) | .h:46-149 | Ensures + priority-sorts visible tiles (`TileLoadPriorityPolicy::sortByPriority`), dedups via `prefetchedTiles`, calls `TileRasterOverlayPrefetcher::prefetch` per tile |
| — Done-tile skip | .h:87-94 | Done tile with existing mappings (`mappingCount()>0`) skips full prefetch (initial mapping built at 0→1 transition) |
| — unload/reload | .h:104-110,138-147 | On `action.unloadTileContent` → `unloadTileContent(tile)` + `queueReload(key, group, priority)` |
| — load-request pass | .h:113-148 | Sorts load requests, budget-gated by `FrameResourceLane::RasterRequest`, prefetches non-dup request tiles |

### TileRasterOverlayMappingPolicy.h

Header-only. Resolves which rectangle/projection context a tile presents to `RasterMappedToTilesetTile::update` — render-content details vs. bounding-volume fallback.

| Item | Lines | Description |
| --- | --- | --- |
| `struct TileRasterOverlayMappingContext` | .h:10-20 | `hasRenderContentDetails`, `mapsLoadedRenderContent`, `waitForContentTerrainDetails`, `overlayDetails*` (empty-singleton via `details()`) |
| `contextFor()` | .h:23-39 | `hasRenderContentDetails` = committed render content + `hasRasterOverlayDetailsContent()`; `mapsLoadedRenderContent` = + `hasRenderableTerrainContent()` |
| `geometryRectangle()` | .h:41-47 | `details().findRectangleForOverlayProjection(projection)` when render-content details exist, else null |
| `boundingVolumeRectangle()` | .h:49-60 | nullopt when render-content details / waiting-for-terrain; else `projectEffectiveContentBoundingVolumeRectangle` |
| `targetRectangle()` | .h:62-71 | geometryRectangle → boundingVolumeRectangle → `tile.bounds` (for screen-pixel sizing) |

### TileRasterOverlaySignature.h

Header-only FNV-style hashing of the active-overlay list for change detection (mapping invalidation, config revisions).

| Item | Lines | Description |
| --- | --- | --- |
| **`kFnvOffset`** = 1469598103934665603 (.h:85), **`kFnvPrime`** = 1099511628211 (.h:86) | | FNV-1a constants |
| `revision()` | .h:16-24 | XOR-fold of each overlay `revision()` |
| `selectionResourceRevision()` | .h:26-31 | Currently passthrough of `baseResourceRevision` (overlays ignored) |
| `configuration()` | .h:33-62 | Mixes size, visible, blocksCompleteRenderable, role, priority, fallbackPolicy, opacity×1e6, provider-ready |
| `mappingIdentity()` | .h:64-72 | Mixes size + each overlay pointer — identity used by `synchronizeMappingIdentity` |
| `hasPendingWork()` | .h:74-82 | Any overlay `hasPendingWork()` |
| `mix()` | .h:88-91 | boost-style hash-combine (`0x9e3779b97f4a7c15`) |

### RenderContentRasterOverlayStateUpdater.h / .cpp

Command-build-phase counterpart to the prefetcher: updates mappings WITH renderer resources (`&renderer` as `IPrepareRendererResources`), so it actually attaches textures, and decides whether to spawn raster-overlay-upsampled children.

| Item | Lines | Description |
| --- | --- | --- |
| `struct RenderContentRasterOverlayUpdateAction` | .h:14-17 | `unloadTileContent`, `createRasterOverlayUpsampledChildren` |
| `update()` | .cpp:17-127 | Same slot-sync + per-overlay loop as prefetcher but passes `&renderer` so Step-6 attach runs; tracks `firstMoreDetailAvailable` / `firstUnknownAvailability` |
| — attach path | .cpp:92-104 | `overlay.update(...)` with renderer → attaches ready textures; `loadThrottled` per overlay |
| — missing projection | .cpp:105-108 | Sets `unloadTileContent` and returns |
| — upsample decision | .cpp:121-125 | `createRasterOverlayUpsampledChildren` = Done tile + more-detail-available AND no earlier Unknown-availability overlay |

### RasterOverlayScreenSpaceMetrics.h / .cpp

Computes the target screen-pixel dimensions for a geometry rectangle, driving imagery zoom-level selection in the mapping step.

| Method | Lines | Description |
| --- | --- | --- |
| `struct RasterTargetScreenPixels` | .h:8-11 | `x`, `y` (default 256) |
| `computeDesiredScreenPixels(bounds, geomErr, mse)` | .cpp:87-105 | `diameterMeters × mse / geometricError`, clamped ≥1; geometricError≤0 → default |
| `computeDesiredScreenPixels(bounds, projection, geomErr, mse)` | .cpp:107-132 | Geographic → ellipsoid-distance variant; WebMercator → `computeProjectedRectangleSize(WebMercator,...)` |
| `computeProjectedRectangleSizeMeters()` (file-local) | .cpp:28-83 | cesium-native `Projection::computeProjectedRectangleSize`: max of edge distances plus midpoint checks for wide/equator-straddling rectangles (`signsDifferOrTouchesZero`) |

### TileSelectionRasterOverlayPreparer.h

Header-only. Selection-traversal facade: gates whether prefetch can be skipped, answers renderability, and kicks off selection-phase prefetch (no renderer resources).

| Method | Lines | Description |
| --- | --- | --- |
| `canSkipReadyOverlayPrefetch()` | .h:23-50 | Skip only if `requiredOverlaysReady` AND no required overlay has `hasPendingNonPlaceholderLoadingTile()` or `isMoreDetailAvailable()` |
| `isCompleteRenderable()` / `isRenderable()` | .h:52-68 | `TileRenderablePolicy::isCompleteRenderable(tile.renderableSnapshot(requiredRasterOverlaysReady))` |
| `processingOrder()` | .h:70-74 | Delegates to `TileRasterOverlayReadinessPolicy::processingOrder` |
| `prepare()` | .h:76-106 | Guard `canPrepareRasterOverlays()`; early-out on skip; else `TileRasterOverlayPrefetcher::prefetch` (prepRenderer=null) |

### TileRasterUpsampledChildCoordinator.h / .cpp

Materializes upsampled child geometry tiles when a raster overlay has more detail than the current geometry (`createRasterOverlayUpsampledChildren` action). Delegates to `TileRasterUpsampledChildMaterializer`.

| Item | Lines | Description |
| --- | --- | --- |
| **`kTerrainMapQuality`** = 0.25 (.cpp:15), **`kTerrainMapWidth`** = 65.0 (.cpp:16) | | cesium-terrain geometric-error constants |
| `cesiumTerrainGeometricError()` | .cpp:18-24 | `8.0 × (semiMajorAxis × 0.25 / 65.0) × bounds.width()` |
| ctor | .cpp:28-32 | Holds `TileContentAccess&` + `TileContentResourceInvalidator&` |
| `createRasterOverlayUpsampledChildren()` | .cpp:34-48 | `TileRasterUpsampledChildMaterializer::materialize(tile, geomError, ensureTile, pPrepRenderer)`; on change → `markResourcesDirty()` |

---

Notes:
- UV convention: native overlay UV has V growing south→north; `computeTranslationAndScale` (TileSurface.cpp:82-102) is pure rectangle-ratio (`offsetU=(geoWest-imgWest)/imgWidth`, `scaleU=geoWidth/imgWidth`, analogous V). Non-inverted-V mappings flip V via `textureWindowForNorthWestUv` (TileSurface.cpp:104-109: `offsetV = 1 - offsetV - scaleV`) to the renderer's north=V0 convention. No tile-size/edge-bleed baked into UVs — handled at GL via CLAMP_TO_EDGE.
- The async terrain GPU-upload path (32-byte `TerrainGpuVertex`) is now end-to-end: produce/upload done, and the DRAW side is wired (2026-07-01 — `GltfDrawCommandBuilder` branches on `useTerrainVertexFormat` to a stride-32 `terrainShader` command). None of the raster-overlay files above depend on the terrain draw path; `RenderContentRasterOverlayStateUpdater` takes `Renderer&` only for `IPrepareRendererResources` attach/detach.
- `TileSurface`/`TileSurface.h` is now trimmed to ONLY the raster-overlay UV helpers `computeTranslationAndScale`/`textureWindowForNorthWestUv` (returning `TileTextureWindow`), used here in the raster path; the former `SurfaceTileMesh`/`buildEllipsoidMesh`/`buildTerrainMesh` mesh builders were removed (2026-07-01).

---

## 8. tiling — glTF geometry to GPU render prep (+ content loaders)

### GltfRenderGeometryBuilder.h / .cpp

Static geometry-conversion helpers: parsed `GltfModel` primitives → GPU-ready interleaved vertex/instance byte arrays. All positions are relativized against a `renderLocalOrigin` (Vec3, ECEF) for float precision. cesium-native `RenderResourcesPreparer` geometry stage.

| Item | Lines | Description |
| --- | --- | --- |
| **`GltfGpuVertex`** = 120 B | .h:14-23 | Interleaved: `pos[3]`,`nrm[3]`,`texcoord01[4]`,`color[4]`,`tangent[4]`,`texcoord23/45/67[4]` — POSITION+NORMAL+8 TEXCOORD sets+COLOR_0+TANGENT. `static_assert(sizeof==120)` (.h:25-27). |
| **`TerrainGpuVertex`** = 32 B | .h:31-35 | Lightweight terrain-only: `pos[3]`,`nrm[3]`,`texcoord[2]` (POSITION+NORMAL+TEXCOORD_0). `static_assert(sizeof==32)` (.h:37-39). |
| **`GltfGpuInstance`** = 100 B | .h:41-44 | Per-instance `model[16]` (mat4) + `normal[9]` (mat3); EXT_mesh_gpu_instancing-style. |
| `transformPoint` | .cpp:10-15 | Homogeneous `dmat4 * point`, perspective-divide by `w`. |
| `primitiveCentroid` | .cpp:17-26 | Mean of `vertices[i].positionEcef`. |
| `primitiveSortCenterEcef` | .cpp:28-46 | Translucency sort center; averages over instances if instanced. |
| `primitiveUsesSplitBlendInstances` | .cpp:48-53 | Instanced AND (Blend alpha OR `transmissionFactor>0`) → split into per-instance draws. |
| `primitiveRenderResourceCount` | .cpp:55-63 | 0 if empty verts/indices; else instance count (split-blend) or 1. |
| `modelUsesSplitBlendInstances` | .cpp:65-71 | `any_of` over primitives. |
| `localOrigin` | .cpp:73-101 | Vertex-weighted ECEF centroid over model (post `contentTransform`); honors `model.preferredLocalOriginEcef`. |
| `primitiveHasTexCoordSet` / `texCoordForVertex` | .cpp:103-130 | Per-set UV lookup; set 0 falls back to `vertices[i].uv`; else `{0,0}`. |
| `buildVertices` | .cpp:146-223 | Produces `GltfGpuVertex[]`. `instanced` (or explicit `keepInstanceLocalVertices`) keeps vertex-local coords (matrix applied per-instance); otherwise applies `contentTransform` and subtracts `localOrigin`. Normal via `inverseTranspose(dmat3)`, tangent transformed+normalized only when non-instanced. |
| `buildTerrainVertices` | .cpp:225-254 | Produces `TerrainGpuVertex[]` — always non-instanced: `(contentTransform*pos)-localOrigin`, normal-matrix normal, texcoord0 only. |
| `buildInstances` | .cpp:256-290 | Per-instance `GltfGpuInstance`: `contentTransform*instance.transform`, origin-relativized translation column, `inverseTranspose` normal mat3 (guarded by `|det|>1e-14`). Column-major float packing. |

### GltfRenderResourcePreparer.h / .cpp

Turns a tile's `GltfModel` into `GltfPrimitiveRenderResources` (GPU buffers/textures). Three APIs: legacy synchronous `prepare` (also handles animation buffer updates), and the two-phase async `prepareCpuWork`/`prepareCpuWorkFromModel` (worker) + `uploadToGpu` (main-thread GL). cesium-native `IPrepareRendererResources::prepareInMainThread` split into worker/main halves.

| Method | Lines | Description |
| --- | --- | --- |
| `toTextureFilter` / `toTextureWrap` | .cpp:21-37 | glTF sampler enums → `TextureDesc` enums. |
| `createGltfGpuTexture` | .cpp:39-94 | Uploads one `GltfTexture`; expands 3-ch→RGBA8, passes 1-ch as R8, validates pixel-buffer size; builds `TextureDesc` incl. mipmap/filters/wrap. |
| `makeGltfTextureBinding` | .cpp:96-116 | Model `GltfTextureBinding` → runtime `TextureBinding` (texture ptr, texCoord, offset/scale vec4, `sin/cos` of rotation). |
| `prepare` (sync) | .cpp:121-593 | Legacy path. Reuse-check: if resource count matches and all buffers ready, either reuse, or (animated+changed) re-`buildVertices` into existing dynamic VBOs via `device->updateBuffer` (.cpp:154-208), or clear+rebuild. Full build: textures then per-primitive `appendPrimitiveResource` lambda (.cpp:236-456) copying the entire material/PBR/water-mask metadata set and validating every texture binding; split-blend instances emit one resource per instance (.cpp:458-488). **Terrain branch** (.cpp:491-555): if `hasTerrainWaterMaskMetadata && !instanced`, builds `TerrainGpuVertex` VBO with `useTerrainVertexFormat=true` and minimal material. Marks tile done/failed-temporarily (.cpp:588-592). |
| `useTerrainFormat` gate | .cpp:234, 615, 763 | `= primitive.hasTerrainWaterMaskMetadata` — the single switch selecting 32 B terrain vs 120 B glTF vertices. |
| `prepareCpuWork` | .cpp:599-746 | Phase 1 (worker): reads tile model, builds vertex bytes (terrain 32 B or glTF 120 B), instance bytes, copies indices, decodes textures (skipped for terrain — raster overlay owns them), fills `GpuReadyPrimitive.metadata` (GPU ptrs left null). Returns `GpuReadyData` or nullopt. |
| `prepareCpuWorkFromModel` | .cpp:748-891 | Same as above but takes a caller-owned deep-copied `model`+explicit `transform`/`localOrigin` — thread-safe variant. |
| `uploadToGpu` | .cpp:897-1008 | Phase 2 (main/GL): creates VBO/IBO/instance buffers + textures from `GpuReadyData`, sets bindings, appends resources, clears `asyncGpuUploadPending`, marks tile done/failed. Note terrain texture-binding block (.cpp:979-983) is a no-op stub — terrain textures are owned by the raster-overlay system. |

### GpuReadyData.h

CPU→GPU handoff struct for the async path (built on worker, consumed on GL thread).

| Item | Lines | Description |
| --- | --- | --- |
| `GpuReadyPrimitive` | .h:14-48 | `vertexBytes`+`vertexStride` (**32 or 120**, .h:17)+`vertexCount`, `indices`/`indexCount`, decoded `TextureData` (R8/RGBA8, .h:25-31), `metadata` (`GltfPrimitiveRenderResources` with null GPU ptrs), `sortCenterEcef`, optional `InstanceData` (bytes/count/stride, .h:42-47). |
| `GpuReadyData` | .h:51-54 | `vector<GpuReadyPrimitive> primitives`; `valid()` = non-empty. |

### GltfDrawCommandBuilder.h / .cpp

Emits per-primitive `RenderCommand`s from prepared `GltfPrimitiveRenderResources`, binding materials, textures, water mask, and mapped raster overlays. cesium-native draw-call assembly.

| Item | Lines | Description |
| --- | --- | --- |
| `GltfDrawCommandBuildContext` | .h:16-21 | `frameNumber`, `generation`, `transitionOpacity`, optional `surfaceClipUv`. |
| `alphaModeUniform` | .cpp:14-24 | Opaque→0, Mask→1, Blend→2. |
| `renderPrimitiveType` | .cpp:26-41 | `GltfPrimitiveMode` → `RenderCommand::PrimitiveType` (Fan→Triangles). |
| `build` | .cpp:45-396 | Per resource: chooses `makeGltfPrimitiveInstancedCommand` vs `makeGltfPrimitiveCommand` (.cpp:61-73); sets frame/gen, `terrainRenderContent` flag, `u_modelOrigin`=renderLocalOrigin, world sort center, opacity, surface-clip UV for terrain (.cpp:91-100); emits full PBR uniform block + texture-transform uniforms + 15-slot texture array (.cpp:269-284); water-mask slot `kGltfWaterMaskTextureSlot` (.cpp:285-307); binds up to `kMaxGltfRasterOverlays` overlay textures at `kGltfRasterOverlayTextureBase+i` with per-overlay UV/opacity/texCoord (.cpp:319-384); blend/depth for translucent (.cpp:385-393). |
| Terrain draw wired (2026-07-01) | .cpp:70-77 | When `primitive.useTerrainVertexFormat`, emits `renderer.makeTerrainPrimitiveCommand` (stride 32, `kind=GltfPrimitive`, `shader=terrainShader`) instead of `makeGltfPrimitiveCommand`; GLES keys the 32B layout on stride, Metal on the terrain PSO (`PipelineLayout::Surface`). Compiles + unit-tested both backends; pixel-verify pending a reachable QM terrain server. |

### TileRenderContentState.h

Owns a tile's render-side content: residual surface/heightmap payload, glTF model, and prepared GPU resources. Header-only. cesium-native `TileRenderContent` + `RenderResources`. Note: mesh-model members (`SurfaceTileMesh`/`GlobeMesh` era) are **gone**; what remains is the heightmap + raw GPU-buffer bucket plus glTF resources.

| Item | Lines | Description |
| --- | --- | --- |
| `SurfaceDrawableSource` | .h:17-23 | enum: None / HeightmapTerrain / AncestorUpsample / EllipsoidFallback / **GltfContent** (the only live terrain source; all terrain now flows through glTF). |
| `TileSurfaceContentState` | .h:25-37 | Residual surface bucket: `heightmap` (`DecodedHeightmap`), raw `gpuVertexBuffer`/`gpuIndexBuffer`, `localOrigin` (reused as glTF render origin), height range, `horizonOcclusionPoint`, `meshReady`, `surfaceDrawable`, `surfaceSource`. No `mesh` member — the `SurfaceTileMesh` field is removed. |
| `GltfPrimitiveRenderResources` | .h:42-111 | Renderer-side per-primitive resources: VBO/IBO/instance buffer, 15 `TextureBinding`s, terrain water-mask fields (.h:67-75), counts, `primitiveMode`, `sortCenterEcef`, `animationRevision`, full PBR/extension factor set. **`useTerrainVertexFormat`** = false default (.h:110) — true ⇒ 32 B vertex. cesium-native `TileRenderContent::getRenderResources` equivalent. |
| `asyncGpuUploadPending` | .h:115 | Set when CPU work dispatched to worker; GPU upload pending next frame. |
| ready/source queries | .h:117-165 | `hasGltf*`, `isGltfRenderReady`, `isRenderContentReady`, `hasRenderableTerrainContent`, `isTerrainRenderContent`, `currentSurfaceSource`, `needsHeightmapSurfaceReplacement`. |
| heightmap accessors | .h:148-178 | `retainedHeightmap` (read/mutable), `hasRetainedHeightmap`, `set/clearRetainedHeightmap`; terrain height range (.h:153-157, 209-218). |
| surface GPU accessors (residual) | .h:245-251, 284-301 | `surfaceVertexBuffer`/`surfaceIndexBuffer`/`surfaceWaterMaskTexture`, `setSurfaceGpuBuffers`/`setSurfaceWaterMaskTexture` — raw buffers (no mesh type); all no-op when `isGltfOwnedContentState()`. |
| glTF accessors | .h:117-122, 253-278, 303-318 | `hasGltfContent/Model`, `gltfContent`, `gltfModelForRead`, `gltfTransform`, `gltfPrimitiveResourcesForDraw`, `gltfTextureResourcesForBinding`, `gltfPrimitiveResourceForBuild/ReadAt`, `setGltfLocalOrigin`, `addGltfPrimitive/TextureResource`. |
| `renderLocalOrigin` | .h:252 | Returns `surface_.localOrigin` — shared origin for both surface and glTF relativization. |
| `rasterOverlayDetails` / `credits` | .h:228-244 | Proxy to `gltfModel->rasterOverlayDetails` / `credits`. |
| `estimateRetainedBytes` | .h:332-369 | CPU+GPU byte accounting across glTF model, surface buffers, water-mask texture, glTF textures/buffers, heightmap. |
| build/clear helpers | .h:371-475 | `clearGltfPrimitiveResources`, `clearGltfGpuResources`, `beginGltfGpuResourceBuild`, `releaseGpuResources`, `clearSurfaceMeshResources`, `clearRenderContent`, `prepareGltfContent` (resets surface payload, seeds origin from `preferredLocalOriginEcef`), `clearGltfContent`; `isGltfOwnedContentState` guard (.h:478-481). |

### DecodedHeightmapSampler.h / .cpp (tiling/)

Bilinear height lookup into a `DecodedHeightmap` at a geodetic point, mapping lon/lat → tile UV. Used by upsampling/ellipsoid-fallback height sampling.

| Method | Lines | Description |
| --- | --- | --- |
| `sampleHeight` | .cpp:10-37 | `u=(lon-west)/width`, `v=(north-lat)/height`; rejects (returns 0) if outside `[0,1]±ε` else clamps; **`kTileCoordinateEpsilon`** = 1e-12 (.cpp:19); `sampleBilinear` then `isNoData`→0. |

### content/GltfModel.h / .cpp

Full glTF/GLB/B3DM in-memory model + parser + animation runtime. `.cpp` is ~8.96k lines (parser, decoders, skinning, morph, animation). cesium-native `CesiumGltf::Model` + reader.

| Item | Lines | Description |
| --- | --- | --- |
| **`kGltfMaxTexCoordSets`** = 8 | .h:20 | Max TEXCOORD sets packed into `GltfGpuVertex`. |
| Enums | .h:22-46 | `GltfAlphaMode`, `GltfPrimitiveMode`, `GltfTextureFilter`, `GltfTextureWrap`. |
| `GltfImage`/`GltfSampler`/`GltfTexture`/`GltfTextureTransform`/`GltfTextureBinding` | .h:48-78 | Texture-side data. `KHR_texture_transform` offset/scale/rotation. |
| `GltfInstance` | .h:83-87 | Per-instance `transform`+`featureId`+`featureProperties` (EXT_mesh_gpu_instancing / EXT_instance_features). |
| `GltfVertexSkinning` / `GltfMorphTarget` / `GltfPrimitiveRuntime` | .h:89-109 | Skinning joints/weights, morph deltas, animation base state. |
| `GltfPrimitive` | .h:111-181 | Geometry+material: `vertices` (`SurfaceVertex`), **`terrainGpuVertexBytes`** (.h:116) pre-built 32 B format, `vertexTexCoords[8]`, colors, tangents, `indices`, `instances`, `skirtMetadata`, terrain water-mask fields (`hasTerrainWaterMaskMetadata`, `terrainOnlyWater/Land`, translation/scale, .h:129-135), full PBR + KHR extension factor/texture set. |
| `terrainGpuVertexBytes` | .h:113-116 | 32 B `TerrainGpuVertex` fast-path field. No longer pre-built by anything (was QM-only); terrain now always routes through the CPU vertex path (`prepareCpuWork`/sync `prepare`). |
| `GltfNodeRuntime`/`GltfSkinRuntime`/animation runtimes | .h:183-238 | Node hierarchy transforms, skins, animation samplers/channels (Linear/Step/CubicSpline). |
| `GltfModel` | .h:240-272 | `primitives`, `textures`, `nodes`, `skins`, `animations`, `credits`, `rasterOverlayDetails`, `terrainWaterMask`, `preferredLocalOriginEcef`, animation state. |
| `GltfModel::vertexCount/indexCount/byteSize` | .cpp:8561-8608 | Aggregate accessors. |
| `GltfModel::hasRuntimeAnimation` | .cpp:8610 | True if animation channels present. |
| `GltfModel::rebuildRuntime` | .cpp:8658 | Rebuilds node/skin/animation runtime; must run before `terrainGpuVertexBytes` build so positions are ECEF. |
| `GltfModel::updateAnimation` | .cpp:8662 | Advances animation, returns changed flag; drives sync-`prepare` dynamic buffer updates. |
| `GltfParser::parse` (4 overloads) | .cpp:8774-8798+ | GLB/glTF parsing with optional external-resource resolver, image decoder, `GltfParserOptions`. |

### content/GltfContentProvider.h / .cpp

Content providers producing `TileContentLoadResult` (glTF models, not meshes). `.cpp` ~4.0k lines. cesium-native `TilesetContentLoader` family.

| Item | Lines | Description |
| --- | --- | --- |
| `TilesetContentTileMetadata` | .h:28-41 | Per-tile: keys, bounds/bounding volumes, `transform`, `geometricError`, `refine`, `unconditionallyRefine`. |
| `TileContentLoadResult` | .h:43-113 | Result: `status`, `gltfModel`, `contentTransform`, `metadata`, `terrainRenderContent`, availability updates. Factories: `render`, `renderTerrain` (.h:63; sets `terrainRenderContent=true`, wires rasterOverlayDetails), `empty`/`retryLater`/`failed`/`cancelled`/`external`. |
| `TilesetContentProvider` (interface) | .h:119-174 | Virtual: `supportsTile`, `rootTiles`, `tileMetadata`, `childTiles`, `providesTerrainQuadtree`, `availabilityState`, `requestTileContent`, `decodeContent`, diagnostics. cesium-native `TilesetContentLoader`. |
| `SingleGltfContentProvider` | .h:179-231; .cpp:3312-3419 | Single glTF/GLB/B3DM tile. `decodeContent` (.cpp:3312) → `GltfParser::parse`; `setEastNorthUpPlacementDegrees` (.cpp:3341) places model via ENU frame. |
| `TilesetJsonContentProvider` | .h:237-312; .cpp:3427-3960 | 3D Tiles `tileset.json`: `parseTilesetJson` (.cpp:3708) parses transforms/refine/GE/bounding volumes/URIs incl. external tilesets; `decodeRenderableContent` (.cpp:3676) parses glTF w/ up-axis transform. cesium-native `TilesetJsonLoader`. |
| `GltfParser::parse` sites | .cpp:2366, 2386, 3173 | glTF decode entry points inside decode helpers. |

### content/EllipsoidTerrainContentProvider.h / .cpp

Fallback terrain provider: synthesizes a smooth-ellipsoid glTF grid mesh per tile (no real heights). Now **self-contained** — inlines grid generation via a local `buildEllipsoidGrid` (no `TileSurface` call). cesium-native `EllipsoidTilesetLoader`.

| Item | Lines | Description |
| --- | --- | --- |
| ctor | .h:13-16; .cpp:261-267 | `schemeId="XYZ-WebMercator"`, `maximumLevel=14`, `gridSize=16`. |
| `providesTerrainQuadtree` | .h:24 | Returns true. |
| `splitHighLow` / `setLocalPosition` | .cpp:57-76 | ECEF high/low split (`kSplit`=65536.0) into `SurfaceVertex.positionHigh/LowEcef`. |
| `rewriteProjectionTexCoords` | .cpp:88-122 | Reprojects UV0 into WebMercator/Geographic raster space from `runtime.baseVertices`. |
| `makeRasterOverlayDetails` | .cpp:124-137 | Projection/rectangle/inverted-V + bounding region (min/max height = 0). |
| **`buildEllipsoidGrid`** | .cpp:148-200 | **Inlined** ellipsoid-grid generator (ported from former `TileSurface::buildEllipsoidMesh`): `(gridSize+1)²` vertices, **linear latitude** (north at v=0, south at v=1), geodetic ECEF + high/low split + surface normal + unit-UV; winding `(a,c,b,b,c,d)`. |
| `makeEllipsoidTerrainModel` | .cpp:202-257 | Builds grid → one `GltfPrimitive` (Triangles, metallic 0 / roughness 1), root node at `localOrigin`, reprojects UVs, relativizes positions, `rebuildRuntime()`. Does NOT set `hasTerrainWaterMaskMetadata` ⇒ renders through the **120 B glTF** vertex path, and does NOT pre-build `terrainGpuVertexBytes`. |
| `tileMetadata` / `childTiles` / `rootTiles` / `availabilityState` | .cpp:283-336 | Quadtree navigation + availability; virtual-terrain-root handling. |
| `requestTileContent` / `decodeContent` | .cpp:338-377 | Returns `renderTerrain` result (with terrain height range + rasterOverlayDetails); `decodeContent` always fails (content synthesized in `requestTileContent`). |

### content/GltfTerrainUpsampler.h / .cpp

Generates a child terrain glTF by subdividing/upsampling a parent terrain model for a quadrant (used when a child tile lacks its own terrain but a raster overlay needs finer geometry). `.cpp` ~834 lines. cesium-native `upsampleGltfForRasterOverlays`.

| Item | Lines | Description |
| --- | --- | --- |
| `upsampleForRasterOverlay` | .h:12-16; .cpp:807 | Subdivides `parentModel` into `childID` quadrant; `textureCoordinateIndex`, `hasInvertedVCoordinate`. Sets child `hasTerrainWaterMaskMetadata=true` (.cpp:761) and propagates water-mask translation/scale (halved scale, offset accumulation, .cpp:771-780). Does NOT pre-build `terrainGpuVertexBytes` (nothing does anymore — see note above), so upsampled terrain still routes through `prepareCpuWork`/sync `prepare`. |

---

## 9. providers — imagery + terrain + raster overlay tile providers

### ImageryProvider.h

Abstract imagery interface. cesium-native imagery data-source equivalent: builds URLs, fetches, decodes to CPU `DecodedImage`; **owns no GPU resources** (.h:16-17).

| Item | Lines | Description |
|---|---|---|
| `ImageryProvider` iface | .h:18-79 | Pure-virtual: `id`, `schemeId`, `minZoom`/`maxZoom`, `tileWidth`/`tileHeight`, `buildUrl(TileKey)`, `requestTile`, `decodeTile` |
| `type()` | .h:26 | Defaults to `"imagery"`; subclasses override (`"xyz-imagery"`, `"tms-imagery"`, …) |
| `supportsTile` (default) | .h:44-48 | schemeId match AND zoom in [min,max]; overridable |
| `providerKeyForTile` | .h:53 | Identity by default; OpenGlobus grouped-Y provider remaps here |
| `TileCallback` | .h:63-64 | `(TileKey, unique_ptr<DecodedImage>)` invoked on background thread |
| `requestTile` | .h:66-70 | Async; takes `CancellationToken`, `HttpRequestPriority` (default Normal) |
| `decodeTile` | .h:77-78 | Sync decode of raw bytes (test/debug) |

### XYZImageryProvider.h / .cpp

Standard XYZ/`{z}{x}{y}{s}` provider; base class for TMS/WMS/WMTS/Bing/Google. HTTP GET → stb_image RGBA decode. Uses `PlatformBridge` when set (Android native curl), else `CurlMultiRequestScheduler::shared()` (.cpp:382-506).

| Item | Lines | Description |
|---|---|---|
| ctor(urlTemplate, attribution) | .cpp:238-241 | Template + credit; setters configure zoom/tileSize/scheme |
| `id()` | .cpp:259-263 | `"xyz-" + hash(urlTemplate)` |
| `setOpenGlobusGroupedY` | .cpp:279-284 | Enables 3-band grouped-Y (mercator/north/south), forces schemeId `"OpenGlobus-Earth"` |
| `supportsTile` | .cpp:290-306 | Scheme+zoom+bounds check; grouped-Y allows `y` in `[0, 3·2^z)` |
| `providerKeyForTile` | .cpp:308-319 | Grouped-Y: subtract `2·2^z` / `2^z` to fold polar bands into base tile |
| `buildUrl` | .cpp:321-369 | Substitutes `{z}{x}{y}{s}` + extended placeholders |
| placeholder set | .cpp:342-367 | `reversez`, `reversex`, `reversey`, `west/south/east/northdegrees`, `minimum/maximumx/y` (projected), `width`, `height`, `groupedy`, `tilegroup`, subdomain `s` (`(x+y)%4`, +1 if `"0{s}"`) |
| `substituteUrlTemplateParameters` | .cpp:197-230 | `{...}` scan, case-insensitive names, `[UNKNOWN PLACEHOLDER]` on miss |
| scheme tile counts | .cpp:74-85 | `Geographic-TMS` x = `2^(z+1)`; else `2^z`; y = `2^z` |
| degrees/projected rects | .cpp:124-187 | Per-scheme (Geographic-TMS / TMS-WebMercator / default WebMercator) rectangle math; **`kWgs84MaximumRadius`** = 6378137.0 (.cpp:117,173) |
| `requestTile` | .cpp:371-507 | Bridge path (.cpp:382-445) and CurlMulti path (.cpp:447-506); decode dispatched via `AsyncSystem::run` |
| `decodeTile` | .cpp:524-549 | Prefers `PlatformBridge::decodeImage`, else `stbi_load_from_memory(...,4)` → RGBA |
| `requestDiagnostics` | .cpp:509-522 | Atomics started/completed + transport max active |
| Android failure log cap | .cpp:42 | **`kMaxAndroidFailureLogs`** = 24 |

### TileMapServiceImageryProvider.h / .cpp

TMS provider; cesium-native `TileMapServiceTileProvider` semantics. Subclass of XYZ; parses `tilemapresource.xml` for level→tileset URL mapping.

| Item | Lines | Description |
|---|---|---|
| `TileMapServiceMetadata` | .h:21-31 | fileExt, tile W/H, min/max level, schemeId, degrees-vs-projected bbox flag, `projectedCoverageRectangle`, `tileSets` |
| `TileMapServiceImagerySource` | .h:15-19 | Bundles provider + `TileScheme` + optional coverage `Rectangle` |
| ctor | .cpp:11-23 | Constructs XYZ base with empty template; applies metadata scheme/zoom/tileSize |
| `buildUrl` / `supportsTile` | .cpp:31-40 | Delegate to `tileMapServiceTileUrlForKey`; returns empty/false when no tileset for level |
| `createTileMapServiceImagerySource` | .cpp:59-82 | Parse XML → metadata → coverage/scheme/provider |

### TileMapServiceUrl.h / .cpp

Free functions: TMS XML/URL resolution + `tilemapresource.xml` parsing (hand-rolled XML), cesium-native `TileMapServiceRasterOverlay` aligned.

| Function | Lines | Description |
|---|---|---|
| `tileMapServiceXmlUrl` | .cpp:429-461 | Resolve `tilemapresource.xml` relative to endpoint, preserving `?`/`#` |
| `tileMapServiceTileBaseUrl` | .cpp:463-487 | Base URL for resolving TileSet hrefs (trailing slash added like Cesium) |
| `tileMapServiceTileUrl` | .cpp:489-497 | `<tileSetUrl>/<x>/<y><ext>` resolved against base |
| `tileMapServiceTileUrlForKey` | .cpp:499-522 | Level index = `z - minimumLevel`; `nullopt` if out of tileset range |
| `tileMapServiceGeographic/Resolved…CoverageRectangle` | .cpp:524-546 | Unproject bbox; default = scheme max rectangle |
| `tileMapServiceXmlIsLoadable` | .cpp:556-576 | Preflight: needs `<TileSets>` + SRS ∈ {4326, 3857, 900913} |
| `parseTileMapServiceMetadata` | .cpp:578-622 | Profile/SRS → scheme (.cpp:359-392); BoundingBox → projected rect; TileFormat; per-`TileSet order/href` min/max level |
| `resolveRelativeUrl` / `normalizePath` | .cpp:26-107 | RFC-ish relative URL + `..`/`.` path normalization |
| XML helpers | .cpp:109-357 | `attributeValue/Uint32/Double`, `rootContent`, `directChildTag(s)`, `directChildElementText/Content` |

### WebMapServiceImageryProvider.h / .cpp

WMS `GetMap` provider (cesium-native WMS aligned). Subclass of XYZ; forces `Geographic-TMS` scheme. Loading/decoding inherited.

| Item | Lines | Description |
|---|---|---|
| `WebMapServiceImageryOptions` | .h:9-17 | version (`1.3.0`), layers, format (`image/png`), min/max level, tile W/H |
| ctor | .cpp:266-278 | Forces `Geographic-TMS`; clamps zoom/tileSize |
| `buildUrl` | .cpp:290-329 | Computes lat/lon bbox from key; sets `crs/styles/transparent/service` (no-overwrite), `request=GetMap`/`version`/`bbox`(S,W,N,E)/`layers`/`format`/`width`/`height` (overwrite) |
| URL query split/join | .cpp:183-250 | `splitUrl`, `setQueryValue`, `joinUrl` |
| `validateWebMapServiceCapabilities` | .cpp:331-417 | Checks Service/Name; MaxWidth/MaxHeight/LayerLimit vs options |
| `webMapServiceCapabilitiesUrl` | .cpp:419-426 | Builds `GetCapabilities` URL |

### WebMapTileServiceImageryProvider.h / .cpp

WMTS provider (cesium-native WMTS aligned). Subclass of XYZ; supports RESTful template and KVP fallback.

| Item | Lines | Description |
|---|---|---|
| `WebMapTileServiceImageryOptions` | .h:12-25 | format, subdomains, layer, style, tileMatrixSetId, optional tileMatrixLabels/dimensions, schemeId, level/size |
| `shouldUseKvp` | .cpp:65-68 | KVP unless URL has ≥1 placeholder other than `{s}` |
| ctor | .cpp:111-124 | Applies scheme/zoom/size; picks REST vs KVP |
| `invertedY` | .cpp:80-85 | `2^level - 1 - y` |
| `tileMatrixLabel` | .cpp:70-78 | Uses labels array if provided, else level number |
| `buildUrl` | .cpp:136-183 | REST: substitutes `{Layer}{Style}{TileMatrix}{TileRow}{TileCol}{TileMatrixSet}` (+`{s}`, dims). KVP: appends `request=GetTile&version=1.0.0&service=WMTS&…` |
| `substituteTemplateParameters` | .cpp:31-63 | `{...}` substitution with `escapeUriComponent` on values; leaves unknown placeholders intact |

### BingMapsImageryProvider.h / .cpp

Bing Maps quadkey provider. Subclass of XYZ; requires metadata fetch to configure imageUrl/subdomains/credits.

| Item | Lines | Description |
|---|---|---|
| metadata structs | .h:14-56 | `BingMapsImageryOptions`, `BingMapsCredit`+`CoverageArea`, `BingMapsMetadata`, `…ParseResult`, `BingMapsImagerySource` |
| `buildUrl` | .cpp:297-328 | Substitutes `{quadkey}` (from `tileXYToQuadKey`), `{subdomain}` (`(level+x+y)%N`), `{culture}`; appends `n=z` if missing |
| `tileXYToQuadKey` | .cpp:330-347 | Interleaves x/y bits high→low into base-4 quadkey string |
| `bingMapsMetadataUrl` | .cpp:349-363 | `REST/v1/Imagery/Metadata/<mapStyle>?incl=ImageryProviders&key=…&uriScheme=https` |
| `parseBingMapsMetadata` | .cpp:365-424 | JSON parse of imageUrl/subdomains/zoomMax/credits |
| `createBingMapsImagerySource` | .cpp:426+ | Builds provider + scheme from metadata |

### GoogleMapTilesImageryProvider.h / .cpp

Google Map Tiles (2D) provider. Subclass of XYZ; session-token workflow + per-viewport availability ranges.

| Item | Lines | Description |
|---|---|---|
| session/viewport structs | .h:18-74 | `…ExistingSessionOptions` (apiBase `https://tile.googleapis.com/`), `…NewSessionOptions`, viewport rects, `GoogleMapTilesTileRange`, `…ImagerySource` |
| provider members | .h:76-114 | availability ranges, complete-availability ranges, async credit requests |
| `buildUrl` | .cpp:536-549 | `v1/2dtiles/{z}/{x}/{invertedY}?session=…&key=…` |
| `requestTile` | .cpp:551-629 | Skips if scheme unsupported; requests only if `isTileKnownAvailable`; short-circuits null when in complete-availability range but not present |
| `googleMapTilesCreateSessionUrl` / `…Payload` | .cpp:157-192 | POST session creation |
| `parseGoogleMapTilesCreateSessionResponse` | .cpp:194-249 | Parse session token / expiry / tile size |
| `googleMapTilesViewportUrl` / `parse…ViewportResponse` | .cpp:251-337 | Viewport availability (maxZoomRects) fetch/parse |
| credits | .cpp:339-410, 647 | `parse…Copyright`, `combineGoogleMapTilesCredits`, async `loadCredits` |
| availability | .cpp:412-465, 631-762 | Viewport→tile ranges, `addAvailableTileRanges`, `applyViewportAvailability`, `isTileKnownAvailable`, `isTileInCompleteAvailabilityRange` |

### DebugImageryProvider.h / .cpp

Networkless debug provider; synthesizes deterministic checkerboard tiles with z/x/y labels. Directly implements `ImageryProvider` (not XYZ).

| Item | Lines | Description |
|---|---|---|
| fixed config | .h:14-21 | scheme `XYZ-WebMercator`, zoom [0,18], 256×256 |
| `buildUrl` | .cpp:11-16 | `debug://z/x/y` (unused) |
| `requestTile` | .cpp:18-26 | Synchronous `generateTile` → callback (no network) |
| `generateTile` | .cpp:34-194 | Hash-colored 32px checkerboard, white 1px border, 3×5 bitmap-digit z/x/y labels |
| `decodeTile` | .cpp:28-32 | Returns nullptr (no real decode) |

### TerrainProvider.h / .cpp

Abstract terrain interface + `DecodedHeightmap`. cesium-native terrain data-source equivalent; owns no GPU resources. `HeightmapTerrainProvider` is the live subclass.

| Item | Lines | Description |
|---|---|---|
| `DecodedHeightmap` | .h:27-53 | tileSize, `heights` (row-major N→S), min/max, `noDataValues`, `heightFactor` |
| `isNoData` | .cpp:7-15 | `height > 50000` (OpenGlobus RgbTerrain) or sentinel match |
| `sampleBilinear` | .cpp:17-43 | Bilinear over regular grid; clamps u,v to [0,1] |
| `TerrainTileLoadResult` | .h:55-91 | status + heightmap; `successWithHeightmap`/`empty`/`retryLater`/`failed`/`cancelled` factories |
| `TerrainProvider` iface | .h:95-166 | `id`, `schemeId`, zoom, `tileSize`, `buildUrl`, `requestTile` (`TerrainCallback`), `decodeTile` |
| `availabilityState` (default) | .h:127-134 | cesium-native `tileIsAvailableInLayer` equivalent: Available/Unknown/NotAvailable |
| `estimatedRequestFanout` / `isAvailabilityBoundaryLevel` | .h:142-147 | Fan-out estimate for frame budget; sparse-terrain subtree boundary hook |

### HeightmapTerrainProvider.h / .cpp

RGB-encoded heightmap terrain (Terrarium / Mapbox Terrain-RGB). Live `TerrainProvider` subclass; HTTP → stb_image → decode.

| Item | Lines | Description |
|---|---|---|
| `Encoding` | .h:27-30 | `Terrarium` (R·256+G+B/256−32768) or `MapboxTerrainRgb` (−10000 + RGB·0.1); formulas .h:15-20 |
| config setters | .h:50-56 | zoom range, maxNativeZoom, encoding, tileSize, heightFactor, noDataValues |
| `buildUrl` | .cpp:64-77 | Simple `{z}/{x}/{y}` string replace |
| `requestTile` | .cpp:79+ | Bridge/curl path with `HttpCache::shared()` hit path; decodes to `DecodedHeightmap` |
| `decodeTile` | later | RGB→height per encoding (uses `EARTH_ENGINE_HAS_STB_IMAGE`) |

### BlockingHttpFetcher.h / .cpp

Generic blocking HTTP GET (setup-time). Supports `file://`, `PlatformBridge`, and `CurlMultiRequestScheduler`; caches via `HttpCache::shared()`. Used by TMS/WMS/Bing imagery setup and (formerly) the terrain layer.json fetch. Renamed from `QuantizedMeshLayerJsonFetcher` when the quantized-mesh terrain path was removed; the class itself is unchanged (generic, not terrain-specific).

| Item | Lines | Description |
|---|---|---|
| `fetchBlocking` | .cpp:44-117 | Cache hit → return; `file://` read; bridge path blocks on `condition_variable` with **20s** deadline (.cpp:88-89, 113), cancellable via predicate |
| `isCesiumSuccessfulHttpStatus` | .cpp:17-19 | status 0 or 2xx (0 = local/file success) |
| `cacheKeyFor` | .cpp:21-36 | URL + serialized request headers |

### ProviderRequestDiagnostics.h + ProviderRequestDiagnosticsAggregator.h / .cpp

Per-provider request counters and their sum/merge.

| Item | Lines | Description |
|---|---|---|
| `ProviderRequestDiagnostics` | ProviderRequestDiagnostics.h:5-17 | started/completed/failed, worker-blocking active/peak, external-resource variants, `maximumTransportActiveRequests` (−1 = unknown) |
| `Aggregator::add` | ProviderRequestDiagnosticsAggregator.cpp:7-34 | Sums counters, `max`es peaks and transport max |

### RasterOverlayTile.h / .cpp

cesium-native `RasterOverlayTile` equivalent: one imagery tile with its own async load lifecycle. Owned by `RasterOverlayTileProvider` via `shared_ptr`; raw accessors are non-owning.

| Item | Lines | Description |
|---|---|---|
| `LoadState` | .h:26-33 | Placeholder(−2)/Failed(−1)/Unloaded(0)/Loading(1)/Loaded(2)/Done(3) — values match cesium-native |
| `MoreDetailAvailable` | .h:36-40 | No/Yes/Unknown |
| real vs placeholder ctor | .h:46-53 / .cpp:9-23 | Real starts Unloaded; placeholder starts Placeholder |
| `setTexture` / `markLoadedWithoutTexture` | .cpp:28-40 | Owns GPU `Texture` via `unique_ptr`; sets opaque `rendererResources_` handle; empty-image case marks Loaded without texture |
| `loadInMainThread` | .cpp:42-53 | Loaded→Done transition (cesium-native GPU-resource step) |
| `getTargetScreenPixelsX/Y` | .h:80-85 | cesium-native `getTargetScreenPixels` (default 256×256) |
| mapped-raster fields | .h:137-174 | Mapped (non-source) tile: source zoom/bounds/keys + min/max XY of source-tile plan |
| atlas UV | .h:176-184 | Atlas offset/scale U/V |
| `lastUsedFrame` | .h:214 | Used by provider `trimUnusedTiles` eviction |

### RasterTextureUploader.h

Resource-prep boundary: provider owns tile lifecycle + decoded CPU imagery; uploader turns pixels into a GPU `Texture`. Interface only.

| Item | Lines | Description |
|---|---|---|
| `RasterTextureUploadOptions` | .h:10-18 | `generateMipmaps`, `enableEdgeBleed` (1px edge-bleed border, cesium-native seam fix; UV inset handled by `RasterMappedToTilesetTile::computeTranslationAndScale`) |
| `RasterTextureUploader` iface | .h:25-34 | `maxTextureSize`, `uploadRasterTexture(DecodedImage, options)` → `unique_ptr<Texture>` |

### RasterOverlayTileProvider.h / .cpp (~3700 lines)

cesium-native `RasterOverlayTileProvider` equivalent. Owns raster tile cache, async load dispatch, GPU-upload scheduling, throttling, source-tile depot, and frame-based trimming. Maps geometry rectangles → provider quadtree source imagery (cesium-native `mapRasterTilesToGeometryTile`). Provider still stops at CPU imagery; GPU upload delegated to injected `RasterTextureUploader`.

| Method | Lines | Description |
|---|---|---|
| ctor / dtor | .h:51-55 / .cpp:2347 | Takes `ImageryProvider&`, `TileScheme&`, optional uploader (null = headless test); dtor drains async state |
| `getTile` | .cpp:2696-2832 | Get/create cached tile by key; returns shared placeholder when not ready; stamps `frameNumber_` |
| `mapRasterTilesToGeometryTile` | .cpp:2728... | cesium-native equivalent: geometry rect → quadtree source plan; exact single-source → direct tile, else composed mapped tile |
| `buildQuadtreeSourcePlan` | .cpp:1446... | Choose source zoom (SSE/texture-size driven) + source-key rectangle |
| `resolveTile` | .cpp:2833-2904 | Best available tile ≤ desiredZoom over bounds; nullptr if none |
| `loadTile` / `loadTileThrottled` | .cpp:2905-2970 | Start async load (Loading + HTTP); throttled by `maximumSimultaneousTileLoads` (=20, .h:189) |
| `loadMappedRasterTile` / `loadSourceTileList` / `loadSourceImageSet` | .cpp:2971-3277 | Fetch/compose overlapping source quadtree tiles for a mapped tile |
| `issueMappedSourceImageSet` | .cpp:3278-3444 | Dispatch source-tile requests through shared depot |
| `composeQuadtreeSourceImagesWithDetails` | .cpp:2294-2329 | Composite source images into target rect; propagate MoreDetailAvailable/credits/diagnostics |
| `projectedVForLatitude` | .cpp:2330-2346 | Latitude → projected V within bounds |
| `processPendingUploads` | .cpp:3445-3624 | Main-thread: drain `pendingUploads`, GPU-upload via uploader, Loaded→Done; frame-budget aware |
| `hasPendingWork` | .cpp:3625-3680 | HTTP/source-fanout/upload outstanding |
| `trimUnusedTiles` | .cpp:3681... | Evict tiles by `lastUsedFrame`; advances `frameNumber_` |
| `refreshSourceAssetDepot` | .cpp:2577-2695 | Rebuild shared source-tile depot on option change |
| `requestDiagnostics` | .cpp:2859... | Aggregates imagery-provider + raster-source request counters |

| State/struct | Lines | Description |
|---|---|---|
| `QuadtreeSourceImage` / `CompositeImageResult` | .h:60-93 | Per-source decoded image; composed output + detail/credits |
| `RasterSourceTileMapping` / `RasterTileMapping` | .h:95-115 | Source-tile plan (keys + min/max XY) and mapping result (direct vs composed) |
| `ProviderAsyncState` | .h:394-447 | Shared state that **outlives the provider** for in-flight callbacks (cesium-native depot lifetime): `pendingUploads`, source depot cache/in-flight/LRU, active mapped source sets, atomics, `revision`, async-destruction promise |
| `SourceTileAsset` / `InFlightSourceTileAsset` | .h:368-384 | cesium-native `SharedAssetDepot`: source imagery shared by `TileKey`, generation-tracked |
| defaults | .h:456-459, 408 | `maximumScreenSpaceError_`=2.0, `maximumTextureSize_`=2048, `subTileCacheBytes`=16 MiB |

Note: none of the raster-overlay path is affected by the (now-vestigial) async terrain GPU-upload machinery. That path (32-byte `TerrainGpuVertex`; `GltfDrawCommandBuilder` branches on `useTerrainVertexFormat` to a stride-32 `terrainShader` command) lives in the terrain/renderer modules but is no longer fed by anything since the quantized-mesh terrain path was removed — providers here stop at CPU-decoded `DecodedImage`/`DecodedHeightmap`.

---

## 10. terrain — TerrainTile, DecodedHeightmap

The quantized-mesh terrain path (`QuantizedMeshParser`, `QuantizedMeshAvailability`, `QuantizedMeshContentLoader`, `QuantizedMeshTerrainProvider`) and the Cesium ion terrain integration have been **fully removed**. Heightmap terrain (CPU-baked regular-grid, see `HeightmapTerrainProvider` in §9) is now the only terrain source; `EllipsoidTerrainContentProvider` remains as its ellipsoid fallback. This section now covers only the surviving terrain height-sampling type and the generic quadtree geometric-error helpers (§2, `QuadtreeGeometricError.h/.cpp`).

### TerrainTile.h / .cpp

Height-sampling tile wrapping a `DecodedHeightmap` (regular grid, distinct from the QM mesh path). Not part of the glTF render path — used for terrain height queries. UV convention: v north→south (row-major).

| Item | Lines | Description |
| --- | --- | --- |
| ctor | .h:19-21 / .cpp:21-26 | takes `TileKey`, `TileScheme` (→ `bounds_` via `tileToRectangle`), owns `DecodedHeightmap` |
| `key()` / `bounds()` / `heightmap()` / `valid()` | .h:23-26 | accessors; `valid()` = heightmap present and internally valid |
| `sampleHeight()` | .h:33-34 / .cpp:28-64 | bilinear sample with optional `parentTile` no-data fallback |
| `clampTileCoordinate()` | .cpp:9-17 | anon helper; rejects outside `[−ε, 1+ε]`, clamps to `[0,1]` |

**`kTileCoordinateEpsilon`** = 1e-12 (.cpp:10) — absorbs ECEF↔cartographic round-trip ulps at tile edges. Sampling detail: u/v from bounds (.cpp:36-37, v = north→south), parent fallback on invalid coord or no-data (.cpp:44-47, :53-60); **OpenGlobus skipPositiveHeights**: parent zoom ≤ 8 with positive height → sea level 0 (.cpp:57-59).

Note: the shared `TileAvailabilityState` enum (NotAvailable / Available / Unknown) previously lived in `terrain/QuantizedMeshAvailability.h`. It was relocated to a neutral header, `tiling/TileAvailabilityState.h`, when the quantized-mesh path (and the availability-update mechanism built around it — `QuantizedMeshAvailabilityUpdate`, `TileAvailabilityUpdateCommitter`) was removed; the enum is still used by the Ellipsoid/Heightmap/Composite terrain providers.

---

## 11. camera — Camera, CameraController

### CameraController.h / .cpp

Anchor-based globe camera controller (openglobus-aligned). Single-finger drag grabs a surface point and keeps it under the finger; two-finger pinch zooms/rotates/tilts around a surface anchor below the pinch center. Two modes: `orbitMode_` (synthetic orbit around earth center from `rotation_`+`distance_`) and free ECEF camera (`orbitMode_=false`, default after construction).

| Item | Lines | Description |
|------|-------|-------------|
| `SurfacePicker` typedef | .h:26 | `std::function<bool(x,y,Vec3&)>` — Scene-injected terrain pick; falls back to internal WGS84 sphere pick |
| `TerrainHeightFunc` typedef | .h:31 | `std::function<double(const Vec3& ecef)>` — height above WGS84 for collision clamping |
| Public API | .h:36-70 | `onDragStart/Move/End`, `onPinchGesture/End`, `update`, `setDistance`, `rotation`/`setRotation`, `viewDistance` |
| Private helpers | .h:73-89 | `intersectGrabSphere`, `pickSurfacePoint`, `grabSurfacePoint`, `applyAnchorDrag`, `keepAnchorAtScreenPoint`, `rotateCameraAroundPoint`, `rotateCameraVerticalAroundPoint`, `clampEyeAltitude` |
| State | .h:91-125 | `rotation_` (dquat), `distance_` (earth radii), `orbitMode_`, drag state, grabbed anchor (`grabbedNormal_`/`grabbedPoint_`/`grabbedRadiusMeters_`), inertia state, pinch anchor state |

Tuning constants (.cpp:21-39): **`kMaxInertiaAngularVelocityRadPerSec`** = 5.0, **`kInertiaDampingPerSecond`** = 3.0, **`kVelocitySmoothing`** = 0.35, **`kEarthRadiusMeters`** = 6378137.0, **`kMinAltitudeMeters`** = 50.0 (visual floor), **`kMaxDistanceEarthRadii`** = 30.0, **`kTouchJerkLimit`** = 0.3, **`kTouchInertiaDecayStep`** = 0.007, **`kTouchMinSlope`** = 0.1, **`kPinchIntentThresholdPixels`** = 4.0, **`kPinchTiltThresholdPixels`** = 10.0, **`kPinchTiltRadiansPerPixel`** = 0.0015, **`kPinchTiltMaxStepRadians`** = 0.08, **`kPinchRotateThresholdRadians`** = 0.003, **`kPinchAnchorFollow`** = 0.12. `kMinDistanceEarthRadii` derived from (earthRadius+minAlt)/earthRadius (.cpp:27-29).

| Method | Lines | Algorithm |
|--------|-------|-----------|
| `cartographicNormal` (anon) | .cpp:41-49 | lng/lat degrees → unit ECEF normal |
| `defaultViewRotation` (anon) | .cpp:51-64 | Initial `rotation_` aiming eye over lng=105,lat=35 (East Asia) so XYZ Web Mercator imagery is visible frame 1 |
| `clampEyeToMinAltitude` (anon) | .cpp:66-86 | Projects eye to WGS84 surface, adds `max(terrainHeight,0)+kMinAltitudeMeters`; pushes eye out along geodetic normal if below floor |
| ctor | .cpp:90-101 | Inits `rotation_=defaultViewRotation()`, then `lookAt` over Chongqing (106.508,29.617, eye@1500m), sets `orbitMode_=false` |
| `onDragStart` | .cpp:116-128 | `grabSurfacePoint`; resets inertia; records timestamp |
| `onDragMove` | .cpp:130-137 | `applyAnchorDrag` when grabbed |
| `onDragEnd` | .cpp:139-144 | Clears drag/grab; inertia already set by last `applyAnchorDrag` |
| `onPinchGesture` | .cpp:146-284 | Interrupts drag inertia; first call picks surface anchor at pinch center + earth-up normal at screen center. Per-frame: jerk-clamped scale drives dolly along view dir (clamped to `kMaxDistanceEarthRadii`), rotate intent → `rotateCameraAroundPoint`, tilt intent (`|dy|` dominant) → `rotateCameraVerticalAroundPoint`, then `keepAnchorAtScreenPoint`; center-pan blends anchor by `kPinchAnchorFollow`. No-anchor branch (.cpp:270-283) dollies along view dir |
| `onPinchEnd` | .cpp:286-290 | Clears pinch state/inertia |
| `update` | .cpp:292-360 | Touch inertia: cubic-eased slerp of `touchInertiaRotation_`→identity, decayed by `kTouchInertiaDecayStep` (.cpp:293-316); angular inertia: `angleAxis(v·dt)`, exp damping (.cpp:317-332). `orbitMode_` off → `syncDistanceFromCamera` and return; on → rebuild eye = `-rotation_·(+Z)·distance·R`, `lookAt(earthCenter)` (.cpp:334-359) |
| `setDistance` | .cpp:362-370 | Enables `orbitMode_`; clamps to [`kMinDistanceEarthRadii`,`kMaxDistanceEarthRadii`] |
| `setRotation` | .cpp:372-377 | Enables `orbitMode_`; normalizes quat |
| `viewDistance` | .cpp:379-400 | Keeps target→eye bearing, places eye at clamped distance from target, `lookAt`; sets `orbitMode_=false` |
| `applyRotationAroundAxis` | .cpp:402-412 | `orbitMode_` → compose into `rotation_`; else `applyCameraRotation` |
| `applyCameraRotation` | .cpp:414-421 | Rotates eye/dir/up about earth center, composes `rotation_`, syncs distance |
| `rotateCameraAroundPoint` | .cpp:423-436 | Rotates eye about arbitrary `center` by `angleAxis` |
| `rotateCameraVerticalAroundPoint` | .cpp:438-478 | Tilt about `camera_->right()` around center; guards against up flipping and against dropping below `minSlope` |
| `syncDistanceFromCamera` | .cpp:480-482 | `distance_ = |eye|/kEarthRadiusMeters` |
| `clampEyeAltitude` | .cpp:484-486 | Wraps `clampEyeToMinAltitude` with `terrainHeightFunc_` |
| `keepAnchorAtScreenPoint` | .cpp:488-513 | Pick ray → grab sphere; rotates so screen point aligns to `anchorNormal` via cross-product axis, `atan2(len,dot)` angle |
| `intersectGrabSphere` | .cpp:519-539 | Ray/sphere (radius `grabbedRadiusMeters_`) intersection; nearest positive root |
| `pickSurfacePoint` | .cpp:541-552 | Tries injected `surfacePicker_` (terrain), falls back to grab-sphere pick |
| `grabSurfacePoint` | .cpp:554-568 | Picks anchor, stores `grabbedRadiusMeters_`/`grabbedPoint_`/`grabbedNormal_` |
| `applyAnchorDrag` | .cpp:570-618 | Picks new screen point → rotation from `targetPoint` normal to `grabbedNormal_` (cross/`atan2`), `applyCameraRotation`, clamps altitude; feeds smoothed angular inertia and sets `touchInertiaRotation_`/`touchInertiaScale_=1.0` |

Terrain wiring: `setSurfacePicker`/`setTerrainHeightFunc` (.cpp:108-114) are Scene-injected; picker feeds `pickSurfacePoint`, height func feeds altitude clamping. When unset, both fall back to bare WGS84 sphere at `grabbedRadiusMeters_`.

### Camera.h / .cpp

Perspective camera in ECEF/world meters; screen coords are physical viewport pixels, origin top-left. Uses reverse-Z projection matched to OpenGlobus `PlanetCamera`.

| Item | Lines | Description |
|------|-------|-------------|
| Accessors | .h:16-23 | `position`/`direction`/`up`/`right`, `verticalFovRadians`/`nearPlaneMeters`/`farPlaneMeters` |
| `isOrthographic` | .h:26 | Always false (perspective only) |
| `getHeight` | .h:29, .cpp:138-141 | Height above WGS84 via `cartesianToCartographic` |
| `getNormalMatrix` | .h:32, .cpp:143-158 | 3×3 (viewMatrix rotation part, 9 floats, column-major) |
| `setView` | .h:34, .cpp:29-33 | Sets position + orientation; invalidates `target_` (NaN sentinel) so viewMatrix uses position+direction |
| `lookAt` | .h:35, .cpp:35-39 | Sets position/target/orientation from `target-position` |
| `setPerspective` | .h:37, .cpp:41-54 | Validates FOV∈(0,π), 0<near<far |
| `viewMatrix` | .h:41, .cpp:56-65 | `glm::lookAt`; center = `target_` if valid, else `position+direction` |
| `projectionMatrix` | .h:42, .cpp:67-98 | Reverse-Z perspective (see below) |
| `viewProjectionMatrix` | .h:44, .cpp:100-103 | `proj * view` |
| `frustum` | .h:46, .cpp:105-109 | `Frustum::fromViewProjection` |
| `getPickRay` | .h:49, .cpp:111-136 | NDC from pixels; unproject near(z=1)/far(z=0) clip via inverse VP; returns `Ray(nearWorld, normalize(far-near))` |
| `setOrientation` (private) | .h:55, .cpp:160-168 | Orthonormalizes: `dir` normalized, `right=dir×up`, `up=right×dir` |

Constructor defaults (.cpp:18-27): position=(0,0,7e6), direction=(0,0,-1), up=(0,1,0), `target_.x`=NaN (invalid), FOV=60°, **near=1.0**, **far=1e12** (OpenGlobus `PlanetCamera` reverse-Z defaults). **`kMinViewportPixels`** = 1.0 (.cpp:15).

Reverse-Z projection (.cpp:67-98): maps `z_eye=near(1)→z_ndc=1`, `z_eye=far(1e12)→z_ndc=0`. Requires depth clear=0.0, depth func GreaterEqual. Matrix: `P[0][0]=f/aspect`, `P[1][1]=f` (`f=1/tan(fov/2)`), `proj[2][2]=n·invRange`, `proj[2][3]=-1` (w_clip=-z_eye), `proj[3][2]=r·n·invRange` with `invRange=1/(far-near)`. `getPickRay` mirrors this (near clip z=1.0, far clip z=0.0, .cpp:125-127).

---

## 12. scene — Scene, coordinators, FrameState, Frustum, render pipeline

### Scene.h / .cpp

`Scene` is the top-level 3D scene facade. Owns Camera/CameraController/Renderer/render pipeline and delegates all domain work to 5 coordinators + a render pipeline. cesium-native has no direct `Scene` equivalent; this is the engine's own composition root.

| Item | Lines | Description |
|---|---|---|
| Owned coordinators (5) | .h:109-121 | `layers_` (SceneLayerCoordinator), `tilesets_` (SceneTilesetCoordinator), `interaction_` (SceneInteractionCoordinator), `environment_` (SceneEnvironmentCoordinator), `telemetry_` (SceneTelemetryCoordinator) |
| `renderPipeline_` / `frameRuntime_` | .h:104-105 | Owned `SceneRenderPipeline` + `SceneFrameRuntime` (holds FrameState + RenderCommandList + frame/time counters) |
| ctor | .cpp:21-43 | Constructs coordinators; sets reverse-Z perspective near=**150.0**, far=**1e12** (.cpp:30-34, "OpenGlobus PlanetCamera reverse-Z defaults"); wires feature-state callback interaction→layers |
| `setRenderDevice` | .cpp:49-69 | Creates Renderer + SceneRenderPipeline, calls **`renderer_->initialize()` (no-arg)**, inits environment GPU resources; null device tears both down. **No globe mesh built or passed** — `GlobeMesh`/`Globe::createMesh` deleted |
| `update(dt)` | .cpp:79-93 | Phase 1. Builds `SceneFrameUpdateInput` via `frameRuntime_.makeFrameUpdateInput` and calls `SceneFrameUpdateCoordinator::update` (static) |
| `render()` | .cpp:130-147 | Phase 2. Guards on `renderer_`/`renderPipeline_`/`isReady()`; builds `SceneRenderPipeline::Context` from frameState + coordinator getters; `beforeSubmit` lambda = `updatePresentationTrace`; feeds result diagnostics back to telemetry |
| `interactionContext()` | .cpp:220-226 | Assembles `SceneInteractionContext` (camera, controller, primary tileset, vector layers) per call for pick/input via `frameRuntime_.makeInteractionContext` |
| Tileset API | .cpp:157-176 | `setTileset`→primary (+re-configures surface picker), `addTileset`→content; `hasTerrain()` = primary present |

No `globeMesh_` member and no `struct GlobeMesh` forward-decl remain (both deleted post-refactor). Two-phase flow: `update(dt)` mutates FrameState + runs tileset selection; `render()` reads the same FrameState, builds ordered RenderCommands, submits. No rendering in update; no selection in render. Behavior change: with no fallback-globe path, nothing is drawn before tiles load (clear color only).

### FrameState.h

Per-frame POD context. Computed by the update phase (`SceneFrameStateBuilder`), consumed read-only by render pipeline / layers / uniform updater. cesium-native `ViewState`-adjacent but engine-specific.

| Field | Lines | Description |
|---|---|---|
| `frameId`, `timeSeconds`, `deltaSeconds` | .h:19-21 | Monotonic frame counter; seconds since engine start; last-frame delta |
| `mode` | .h:17,22 | `Mode::Mode3D` only (single enumerator) |
| `camera` | .h:24 | Non-owning `const Camera*` |
| `selectorViews` | .h:26 | `std::vector<SelectorView>` — culling frustums for tileset selection |
| `lightDir` | .h:29 | Anonymous struct `{x,y,z}` sun dir, defaults **0.35/0.45/0.82**, env-filled |
| `clearR/G/B/A` | .h:32 | Sky clear color, env-filled (default **0.02/0.02/0.08/1.0**) |
| viewport | .h:34-36 | `viewportWidthPixels`, `viewportHeightPixels`, `devicePixelRatio` |
| interaction focus | .h:38-39 | `hasInteractionFocus`, `interactionFocusDirection` (drives smoothing anchor) |

### SceneFrameRuntime.h / .cpp

Holds the mutable per-frame containers and factory methods that pack coordinator inputs. Keeps FrameState/RenderCommandList/frameId/elapsedTime out of Scene itself.

| Item | Lines | Description |
|---|---|---|
| State | .h:68-73 | `frameState_`, `renderCommands_` (RenderCommandList), `frameId_`, `elapsedTime_`, selector-override flag+vector |
| `setViewport` | .cpp:9-16 | Writes viewport px + dpr into frameState_ |
| `set/clearSelectorViewOverride` | .cpp:18-27 | Test/debug hook to inject fixed culling frustums |
| `makeFrameUpdateInput` | .cpp:29-58 | Packs `SceneFrameUpdateInput` (frameState + diagnostics + camera + controller + prep-renderer + tilesets + frameId/elapsedTime-by-ref + interaction focus + timeController + skyGradient) |
| `makeInteractionContext` | .cpp:60-73 | Packs `SceneInteractionContext` (camera, controller, viewport, terrain tileset, vector layers, elapsedTime) |

### SceneFrameUpdateCoordinator.h / .cpp

Static orchestrator of the update phase. Input is `SceneFrameUpdateInput` (by-ref frameState/diagnostics/frameId/elapsedTime). cesium-native `Tileset::updateView` sits at the tail.

| Step | Lines | Description |
|---|---|---|
| `SceneFrameUpdateInput` | .h:20-38 | POD of by-ref frameState, diagnostics, `frameId&`, `elapsedTime&` + camera/controller/prep-renderer/tilesets + interaction-focus + timeController/skyGradient |
| reset+framerate | .cpp:22-26 | `SceneFrameDiagnostics::resetPerFrame` + `updateFrameRate(dt)` |
| camera update | .cpp:28-34 | `cameraController->update(dt)`, timed → `diagnostics.cameraUpdateMs` |
| build FrameState | .cpp:36-54 | `elapsedTime += dt`, `++frameId`, `SceneFrameStateBuilder::build(...)`; env timing → `environmentUpdateMs` |
| tileset update | .cpp:56-61 | `tilesets.update(frameState, pPrepRenderer)` → terrain/content update ms |
| perf log | .cpp:63-83 | `Scene.update.total`; Android per-section breakdown when total > **30.0** ms |

### SceneFrameStateBuilder.h / .cpp

Populates FrameState scalars, selector views, interaction-focus TTL, and environment lighting. Static.

| Item | Lines | Description |
|---|---|---|
| `build` | .cpp:57-80 | Sets frameId/time/delta/camera; calls `SceneSelectorViewBuilder::populate`; `updateInteractionFocus`; returns `environmentUpdateMs` |
| **`kInteractionFocusTtlSeconds`** = 2.5 | .cpp:15 | Focus expires when `timeSeconds - focusTime > 2.5` |
| `updateInteractionFocus` | .cpp:17-30 | Latches focus dir only within TTL else zeros it |
| `updateEnvironment` | .cpp:32-53 | No-op unless timeController+skyGradient+camera; `SunDirection::compute(julianDate)`, `geodeticSurfaceNormal` local up, `skyGradient->update`; writes `lightDir` + `clearR/G/B` from horizon color; timed |

### SceneTilesetCoordinator.h / .cpp

Owns the primary (terrain) tileset + N content tilesets; fans update/occlusion-callback across them. cesium-native multi-`Tileset` container.

| Item | Lines | Description |
|---|---|---|
| State | .h:42-44 | `primary_`, `contentTilesets_` vector, `occlusionCallback_` |
| `setPrimary`/`addContent` | .cpp:13-26 | Store + `applyOcclusionCallback` |
| `set/clearOcclusionCallback` | .cpp:28-51 | Propagate callback to primary + all content |
| `update` | .cpp:53-82 | `primary_->update` timed → `terrainUpdateMs`; loops content → `contentTilesetUpdateMs`; Android logs any content tileset update > **5.0** ms |
| `SceneTilesetUpdateResult` | .h:14-17 | `terrainUpdateMs`, `contentTilesetUpdateMs` |

### SceneEnvironmentCoordinator.h / .cpp

Owns environment subsystems and exposes them to the update/render phases.

| Item | Lines | Description |
|---|---|---|
| Owned | .h:37-40 | `TimeController`, `SkyGradient`, `AtmosphereBackgroundPass`, `SkyBox` |
| `initializeRenderResources` | .cpp:18-22 | Inits atmosphere pass + skybox GPU resources |
| time API | .cpp:24-34 | `setTime`/`time`/`advanceTime` delegate to TimeController Julian date |
| `sunDirection` | .cpp:36-38 | `SunDirection::compute(julianDate)` |

### SceneInteractionCoordinator.h / .cpp

Owns InputManager/PickingService/SelectionManager; bridges gestures→camera, click→selection, and maintains interaction-focus. Per-call context via `SceneInteractionContext` (.h:22-30).

| Item | Lines | Description |
|---|---|---|
| Owned | .h:77-81 | `inputManager_`, `pickingService_`, `selectionManager_`, `focusState_`, transient `activeInputContext_` |
| `configureCameraSurfacePicker` | .cpp:20-38 | Installs camera controller surface-picker (→pickInteractionFocus) + terrain-height func (→`SceneTerrainQuery::sampleHeight`) |
| `pick` / `pickInteractionFocus` | .cpp:40-60 | Delegate to `ScenePickingCoordinator` static |
| `onInputEvent` | .cpp:62-73 | `updateInteractionFocus` then sets `activeInputContext_` and runs `inputManager_->process` |
| `setupInputCallback` | .cpp:118-129 | InputManager gesture cb → `SceneInputCoordinator::handleGesture` |

### SceneInputCoordinator.h / .cpp

Static gesture→camera/selection mapper. Also updates interaction-focus. Contexts: `SceneInputCoordinatorContext` (.h:21-27), `SceneInteractionFocusState` (.h:15-19).

| Item | Lines | Description |
|---|---|---|
| `handleGesture` | .cpp:45-104 | Drag→onDragStart/Move/End; Pinch→onPinchGesture/onPinchEnd; DoubleClick→`viewDistance(pos, distance*0.57)` or `setDistance(dist*0.7)`; Click→selectFromClick |
| `selectFromClick` | .cpp:9-27 | shift→onSelectAdd; ctrl/meta→onSelectToggle; else onSelect; invalid→clearSelection |
| `updateInteractionFocus` | .cpp:106-125 | Only for pointer/pinch events; picks focus point, stores normalized dir + elapsed time + hasFocus |

### SceneLayerCoordinator.h / .cpp

Owns the vector-layer list; initializes on render device; applies feature state (hover/select).

| Item | Lines | Description |
|---|---|---|
| `addVectorLayer`/`removeVectorLayer` | .cpp:22-41 | Init layer with device; erase by `id()` |
| `applyFeatureState` | .cpp:43-66 | Maps FeatureState→"hover"/"selected" string, calls `layer->setFeatureState` |

### ScenePickingCoordinator.h / .cpp

Static picking against terrain + vector layers. Context: `ScenePickingContext` (.h:14-21).

| Item | Lines | Description |
|---|---|---|
| `pick` | .cpp:9-47 | `pickTerrain` (lng/lat height sampler from SceneTerrainQuery) then vector `pick`; keeps nearest of the two |
| `pickInteractionFocus` | .cpp:49-71 | Terrain-only pick; outputs `worldPosition` if valid |

### SceneTelemetryCoordinator.h / .cpp

Owns `Diagnostics` + `PresentationTrace`; forwards engine timing and rebuilds trace.

| Item | Lines | Description |
|---|---|---|
| `recordEngineTiming`/`finishEngineFrame` | .cpp:7-18 | Delegate to `SceneFrameDiagnostics` |
| `replaceRenderDiagnostics` | .cpp:20-23 | Overwrites `diagnostics_` with render-phase result |
| `updatePresentationTrace` | .cpp:25-36 | Rebuilds via `ScenePresentationTraceBuilder::build` |

### SceneSelectorViewBuilder.h / .cpp

Static; fills `frameState.selectorViews`. Input `SceneSelectorViewBuildInput` (.h:12-18).

| Item | Lines | Description |
|---|---|---|
| `populate` | .cpp:8-36 | If override set, copies override views; else builds one `SelectorView` from camera position/direction, `projectionMatrix(w,h)`, `Frustum::fromViewProjection(proj*view)`, viewport height |

### SceneRenderPipeline.h / .cpp

Turns FrameState into ordered RenderCommands, sorts/validates, aggregates diagnostics, submits. Context struct `SceneRenderPipeline::Context` (.h:32-44). Owns `tileCommandSet_` (RenderCommandStreamingSet) + `tileCommandCandidates_` for stable-key streaming.

Render flow in `render()` (.cpp:101-174):

| Order | Method | Lines | Builds |
|---|---|---|---|
| 0 | `reserveCommands` | .cpp:176-189 | Reserve = **4 + vectorLayers*4 + Σ tileset renderEntries** |
| 1 | `buildStableLayerCommands` | .cpp:260-297 | Terrain + content tileset `buildRenderCommands`; prefixes stableKey (`terrain:` / `content:N:`); streams through `tileCommandSet_.update` → `layerCommandsMs` |
| 2 | `buildSkyCommands` | .cpp:191-229 | SkyBox command; nightFactor from sun elevation (`exp(elev*8)` below -0.05) max spaceFactor (smoothstep of `(height-120000)/780000`) |
| 3 | `buildAtmosphereCommands` | .cpp:231-258 | AtmosphereBackgroundPass command from camera basis + sun dir + gradient params |
| 4 | `buildLayerCommands` | .cpp:299-316 | Inserts streamed tile cmds, then visible vector layers. **No fallback-globe command** — `makeGlobeCommand`/`GlobeSurface` removed; nothing is drawn if no tile commands exist |
| 5 | `applyMvpUniforms` | .cpp:318-325 | `SceneRenderCommandUniformUpdater::apply` |
| 6 | `sortAndValidate` | .cpp:327-376 | Sort if `mvpRenderOrder` inversion or translucent gltf; `updateSurfaceCommandGeneration`; `validateMvpRenderCommands` throws `std::runtime_error` on failure |
| 7 | `aggregateDiagnostics` | .cpp:378-393 | `SceneFrameDiagnosticsAggregator::aggregateRenderFrame` + terrain render-entry counters + `terrainSurfaceCommandsSubmitted` (`countTerrainSurfaceCommands`) |
| — | beforeSubmit → submit | .cpp:132-141 | `beforeSubmit()` (presentation trace) then `renderer.submit(commands)`; `releaseRenderReferences` on all tilesets (.cpp:399-408) |

Terrain surface = `GltfPrimitive` cmds carrying `terrainRenderContent` (`isTerrainSurfaceCommand`, .cpp:26-29). QM terrain now draws via the 32-byte `TerrainGpuVertex` path (`makeTerrainPrimitiveCommand`, stride 32, `terrainShader`; 2026-07-01); ellipsoid-fallback terrain still uses the 120-byte `GltfGpuVertex` glTF path. `Renderer::terrainShader()` is defined (`kTerrainVertex/FragmentGLSL` + MSL).

### SceneRenderCommandUniformUpdater.h / .cpp

Static; writes per-command MVP + light-dir uniforms from FrameState camera.

| Branch | Lines | Description |
|---|---|---|
| skip globe | .cpp:31-33 | **Residual** guard: `owner=="globe"` commands are skipped. No producer emits them anymore (globe fallback removed), so this branch is now dead but still present |
| SurfaceTile | .cpp:35-50 | **Vestigial** `RenderCommandKind::SurfaceTile` (enum order 10, RenderCommand.h:25) path: `mvp = proj*view*translate(surfaceTileOrigin)` into `surfaceModelViewProjection`, sets `surfaceLightDir`; no live producer |
| Gltf(+Instanced) | .cpp:52-81 | `model = translate(u_modelOrigin)`; `u_modelViewProjection = viewProj*model`; `u_lightDir`; translucent sort depth from `worldSortCenter·cameraDir` |
| default | .cpp:83-94 | Fills `u_modelViewProjection`=viewProj if empty; `surface_tile` owner gets `u_lightDir` |

### Frustum.h / .cpp

Six world-space planes from a view-projection matrix; cesium-native `CullingVolume`/`Plane` equivalent. `CullingResult` tri-state (.h:11-16), `FrustumPlane` (.h:19-24), `SelectorView`-consumed.

| Item | Lines | Description |
|---|---|---|
| `makePlane` | .cpp:11-17 | Normalizes (a,b,c,d); throws `std::invalid_argument` on zero-length normal |
| `fromViewProjection` | .cpp:25-63 | Extracts L/R/B/T + reverse-Z Near/Far: Near=`row4-row3`, Far=`row3` (reverse-Z [0,1] depth; comment notes standard [-1,1] would swap, .cpp:54-56) |
| `containsPoint` / `intersectsSphere` | .cpp:69-97 | Inside test with optional epsilon; sphere overloads for `Vec3` + `BoundingSphere` |
| `intersectsOBB` | .cpp:99-105 | Per-plane OBB `intersectPlane` |
| `computeVisibility` (sphere/OBB) | .cpp:107-126 | Tri-state Outside/Intersecting/Inside for subtree cull-skip |

### SelectorView.h

cesium-native `TilesetFrameState::frustums` equivalent — one culling view. Fields (.h:12-18): `position`, `direction`, `frustum` (Frustum), `projectionMatrix` (Mat4), `viewportHeightPixels`. Empty vector ⇒ no selection.

### Diagnostics.h

Large runtime-diagnostics POD (.h:9-173) exposed via `Scene::diagnostics()`. Groups: frame timing (fps/frameTimeMs/engine\*Ms/scene\*Ms .h:10-23), draw/tile counts (.h:24-28), load-queue + network/main-thread budgets (.h:29-55), terrain/content/raster provider pipelines (.h:56-88), terrain render-entry accounting, imagery attachments, and a partly-**vestigial** quadtree/surface block (`quadtree*`, `surfaceMeshCount`/`surfaceMeshBytes`, `terrainSurfaceTileCommands`) retained from the removed TileQuadTree/SurfaceTileMesh model — still declared but not fed by live architecture. The dead globe-fallback counters (`globeFallbackCommands`/`globeFallbackMaskedTerrainEntries`) and `*SurfaceMeshes` counters were **deleted** (they were only ever zeroed).

### PresentationTrace.h

Frame-snapshot for golden/regression harness (no rendering role). Structs: `PresentationCameraTrace` (.h:14-30), `PresentationSelectorViewTrace` (.h:32-37), `PresentationRenderEntryTrace` (.h:39-49), `PresentationTilesetTrace` (.h:51-80, per-tileset render-entry accounting), `PresentationCommandTrace` (.h:82-101, per-command kind/owner/stableKey/surface\* fields + generation), `PresentationTrace` aggregate (.h:103-108). Built by `ScenePresentationTraceBuilder` via SceneTelemetryCoordinator.

---

Supporting (not in the assigned list but present in the folder): `EngineTimingScope.h` (enum: BeginFrame/SceneUpdate/SceneRender/EndFrame), `SceneTerrainQuery.h/.cpp` (lng/lat + ECEF height samplers over the terrain tileset, .h:13-16), `SceneFrameDiagnostics.*`, `SceneFrameDiagnosticsAggregator.*`, `SceneRenderDiagnostics.*`, `SceneTilesetDiagnostics.*`, `ScenePresentationTraceBuilder.*`, `Camera.h/.cpp`.

---

## 13. renderer — Renderer, RenderDevice, RenderCommand, streaming, texture

### RenderDevice.h

| Item | Lines | Description |
| --- | --- | --- |
| `RenderDevice` | .h:25-68 | Pure-virtual GPU backend abstraction; core never touches Metal/GLES/Vulkan directly. Impls in `platform/ios`, `platform/android`, `platform/macos`. |
| `Backend` enum | .h:29 | `Metal`, `OpenGLES`, `Vulkan`. |
| Capability queries | .h:32-37 | `backendType`, `maxTextureSize`, `maxDrawBuffers`, `supportsFloatTextures`, `supportsInstancing`, `rendererString`. |
| Resource creation | .h:40-54 | `createTexture`, `updateTextureRegion`, `createBuffer`, `updateBuffer`, `createShader`, `createFramebuffer`. |
| Frame ops | .h:57-59 | `beginFrame` / `submit(const RenderCommandList&)` / `endFrame`. |
| Lifecycle | .h:63-67 | `onSurfaceCreated` / `onSurfaceChanged(w,h)` / `onSurfaceDestroyed`. |
| `TextureDesc` | .h:74-85 | Format {RGBA8,RGB8,R8,Depth32F}, Filter, Wrap, `maxAnisotropy`. |
| `BufferDesc` | .h:87-92 | Usage {Static,Dynamic}, Type {Vertex,Index,Uniform}. |
| `ShaderDesc` | .h:94-97 | `vertexSource` / `fragmentSource` (MSL or GLSL ES). |
| `FramebufferDesc` | .h:99-105 | color/depth/samples. |
| GPU resource base classes | .h:111-132 | `Texture`, `Buffer`, `ShaderProgram`, `Framebuffer` (abstract handles). |

### RenderCommand.h / .cpp

Fat, backend-neutral draw descriptor produced by command factories and consumed by `RenderDevice::submit`. cesium-native `DrawCommand`-analogous but far wider (holds all surface + glTF PBR + raster-overlay uniforms inline).

| Item | Lines | Description |
| --- | --- | --- |
| Slot constants | .h:14-19 | **`kMaxSurfaceImageryOverlays`** = 4 (.h:14), **`kGltfRasterOverlayTextureBase`** = 15 (.h:15), **`kMaxGltfRasterOverlays`** = 4 (.h:16), `kGltfWaterMaskTextureSlot` = 19 (.h:17-18), **`kGltfInstanceMatrixStride`** = 100 (.h:19). |
| `RenderCommandKind` | .h:21-29 | Unknown, SkyBackground, AtmosphereBackground, SurfaceTile, GltfPrimitive, GltfPrimitiveInstanced, VectorOverlay. **No `GlobeSurface`** — the fallback-globe kind was removed with the globe-mesh deletion. |
| `RenderCommand` struct | .h:33-138 | Identity (`owner`, `pass`, `stableKey`, `frameId`, `generation`, `terrainRenderContent`), raw GPU handles + `resourceKeepAlive` shared_ptrs, draw params (`vertexStride` 0=auto/32=surface/120=glTF, `instanceStride`), render state, translucent sort (`worldSortCenter`/`translucentSortDepth`), generic `uniforms` map, and fixed-storage hot-path surface/glTF uniform arrays (.h:93-137). |
| Surface fixed uniforms | .h:93-122 | `surfaceModelViewProjection`, tile/clip UV, per-overlay UV+opacity, fog, water-mask, geometry/texture zoom, skirt index counts — avoids per-tile map/string alloc. |
| glTF raster-overlay fixed uniforms | .h:126-137 | `_CESIUMOVERLAY_n`-style UVs/opacities/texCoordSets + water-mask; separate from surface samplers. |
| `RenderCommandValidationError` | .h:143-147 | `{commandIndex, owner, message}`. |
| `mvpRenderOrder(kind)` | .h:150, .cpp:134-151 | Draw-order map: Sky 0, SurfaceTile 10, GltfPrimitive(Instanced) 15, Atmosphere 20, VectorOverlay 30, Unknown 100. No GlobeSurface entry remains. |
| `validateMvpRenderCommands` | .h:154-156, .cpp:153-283 | Hard contract for MVP chain. Returns `optional<...Error>` (empty=valid). Enforces monotonic pass order (.cpp:164-168), color pass, per-kind fixed depth/write/cull/blend, non-zero `generation`, matching `frameId`, back-to-front translucent glTF sort (.cpp:213-230), instanced requires count+buffer+stride (.cpp:251-264). **Terrain-primary exemption**: `owner=="terrain_primary_surface"` overlay allows all-off state (.cpp:123-130,173-183). Contract is enforced by throw at call site `SceneRenderPipeline.cpp:369-372` (`std::runtime_error` on any error). |
| `sortMvpRenderCommands` | .h:158, .cpp:285-290 | `stable_sort` by `mvpCommandLess`: order → opaque-before-translucent glTF → translucent back-to-front by `translucentSortDepth` (.cpp:91-114). |

### RenderCommandStreamingSet.h / .cpp

Diffs per-frame candidate commands into long-lived slots keyed by `stableKey` (avoids treating tile rendering as fully rebuilt immediate-mode list).

| Item | Lines | Description |
| --- | --- | --- |
| `update(candidates, frameId, activeCommands&)` | .h:20-22, .cpp:7-43 | Empty-`stableKey` candidates pass through as transient (appended directly, .cpp:16-19); keyed candidates upsert into `commands_` map + record `lastActiveFrameId`, dedup active keys (.cpp:21-27); entries not touched this frame are evicted (.cpp:29-35); survivors appended to `activeCommands` in streaming order (.cpp:37-42). |
| accessors | .h:24-25 | `storedCommandCount` (map size), `activeCommandCount` (active keys). |

### Renderer.h / .cpp

Platform-agnostic renderer owning shared GPU resources (shaders, geometry) via `Impl` (pImpl). Implements `IPrepareRendererResources` only as lifecycle notification (no raster state retained). Post-refactor the globe-mesh path is gone: no globe shader/VBO/IBO, no `makeGlobeCommand`, no globe getters — `Impl` (.cpp:1911-1928) now holds only surfaceTile/placeholder/gltf/gltfInstanced/color resources. ctor/dtor .cpp:1934-1941.

| Method | Lines | Notes |
| --- | --- | --- |
| `initialize()` (no-arg) | .h:30, .cpp:1943-2019 | Compiles surfaceTile/gltf/gltfInstanced/color shaders, builds shared 64×64 tile IBO (`makeTileGeometry(64)`, .cpp:1998; vertices discarded via `(void)tileVerts`, VBOs per-tile now), 1×1 neutral placeholder tex {174,184,170,255} (.cpp:1961-1971). No `GlobeMesh` argument and no globe buffer upload. Metal skips GLSL surfaceTile shader (.cpp:1951) and treats gltf/gltfInstanced compile failure as non-fatal (.cpp:1978-1995). |
| `submit` | .cpp:2021-2024 | Forwards to `device->submit` if initialized. |
| `dispose` | .cpp:2026-2035 | Resets surfaceTile/tileIndex/placeholder/gltf/gltfInstanced/color resources. |
| Shared-resource getters | .cpp:2039-2049 | `colorShader`, `tileIndexBuffer`/`tileIndexCount`, `surfacePlaceholderTexture`, `gltfShader`, `gltfInstancedShader`. The `globeShader`/`globeVertexBuffer`/`globeIndexBuffer`/`globeIndexCount` getters are **removed**. |
| `terrainShader()` | .h:105 / **.cpp:4418 defined** | 32-byte terrain vertex, no PBR extensions. DRAW side wired: `GltfDrawCommandBuilder.cpp:133` calls `makeTerrainPrimitiveCommand` (.cpp:4477). ⚠️ This row previously read "declared, NO definition, linking would fail" — false since at least 2026-07-01; corrected 2026-08-06. |
| `makeSurfaceTileCommand(tex,vb,ib,idxCount)` | .cpp:2053-2078 | `SurfaceTile`, **`vertexStride`=32** POSITION(12)+NORMAL(12)+TEXCOORD_0(8) (.cpp:2065), UInt32 indices, falls back to shared `tileIndexBuffer`/`tileIndexCount` when `ib`==null (.cpp:2063-2064), sets `hasSurfaceTileUniforms`. |
| `makeGltfPrimitiveCommand(vb,ib,idxCount,vtxCount)` | .cpp:2080-2209 | `GltfPrimitive`, **`vertexStride`=120** POSITION/NORMAL + TEXCOORD_0..7 + COLOR_0(16) + TANGENT(16) (.cpp:2093), seeds the full PBR uniform set (base/metallicRoughness/specular-glossiness/transmission/clearcoat/sheen/anisotropy factors, tex-transform offset/scale/rotation defaults .cpp:2157-2207, per-overlay raster UVs .cpp:2147-2156, water-mask, `u_clipUV`). |
| `makeGltfPrimitiveInstancedCommand(...)` | .cpp:2211-2230 | Delegates to `makeGltfPrimitiveCommand`, then sets `GltfPrimitiveInstanced`, `gltfInstancedShader`, `instanceBuffer`/`instanceCount`, **`instanceStride`=`kGltfInstanceMatrixStride`=100** (mat4 relative model 64B + mat3 normal 36B) (.cpp:2228). Current-branch WIP: surface instancing GPU batch path. |
| `earthModelMatrix()` (static) | .cpp:2232-2238 | Scale by **`kEarthRadius`** = 6378137.0f (.cpp:2233); unit sphere → ECEF meters. |
| `attachRasterInMainThread` / `detachRasterInMainThread` | .cpp:2242-2257 | No-op notification hooks; raster ownership stays in `RasterMappedToTilesetTile` / `SurfaceRasterBinding`. |

`makeTileGeometry(gridSize)` (.cpp:1853-1883) builds a UV-only grid VBO (`TileVertex{{u,v}}`) + `uint32_t` index buffer; `initialize()` keeps only the IBO. The `GlobeMesh`/`GlobeVertex` types and `globe/Globe.{h,cpp}` are **deleted** — there is no fallback ellipsoid background; before tiles load the screen shows clear color only. Terrain flows entirely through the Tileset + glTF-content path.

### IPrepareRendererResources.h

| Item | Lines | Description |
| --- | --- | --- |
| `IPrepareRendererResources` | .h:23-58 | cesium-native `IPrepareRendererResources` equivalent; abstract raster-texture lifecycle notification only — explicitly NOT the source of truth for surface visibility / ancestor fallback / drawable raster binding (.h:14-17). |
| `attachRasterInMainThread` | .h:40-46 | cesium-native `attachRasterInMainThread`; called from `RasterMappedToTilesetTile::update()` Step 6 with geometry key, overlay slot, ready raster tile, GPU texture, UV translation/scale. |
| `detachRasterInMainThread` | .h:55-57 | cesium-native `detachRasterInMainThread`; `noexcept`, no-op-safe; called on eviction / tile replacement / loading→ready promotion. |

### TileTextureCache.h / .cpp

LRU GPU-texture cache keyed `TileKey → Texture`, non-thread-safe (main/render thread only).

| Item | Lines | Description |
| --- | --- | --- |
| ctors | .h:21-25, .cpp:8-17 | Default **`maxBytes`** = 64 MB (.h:22,25); optional `cacheDomain` (defaults to `"default"`, .cpp:9); `device` param accepted but unused (`(void)device`, .cpp:16). |
| `get` | .cpp:23-31 | Miss → nullptr; hit splices entry to LRU front. |
| `put` | .cpp:33-65 | Size = `width*height*4` (RGBA, .cpp:37); replaces existing key, evicts LRU tail until it fits, inserts at front. |
| `contains` / `evict(targetBytes)` / `clear` | .cpp:67-84 | `evict` drops LRU tail until `totalBytes_ ≤ targetBytes`. |
| `makeCacheKey` | .cpp:86-92 | `cacheDomain/schemeId/z/x/y`. |

### RenderDeviceRasterTextureUploader.h / .cpp

Concrete `RasterTextureUploader` backed by a `RenderDevice`.

| Item | Lines | Description |
| --- | --- | --- |
| `RenderDeviceRasterTextureUploader` | .h:8-20 | `final` impl; holds non-owning `RenderDevice*`. |
| `maxTextureSize` | .cpp:10-12 | Delegates to device (0 if null). |
| `addOnePixelBorder` (file-static free fn) | .cpp:16-57 | cesium-native edge-bleed: pads decoded image to (w+2)×(h+2), replicating edge rows/cols/corners so `GL_LINEAR` at tile seams stays continuous. |
| `uploadRasterTexture` | .cpp:59-102 | Requires `bytesPerChannel==1` (.cpp:62); applies 1px border only when `options.enableEdgeBleed` (.cpp:70,77-85); builds `TextureDesc` (RGBA8 if 4 channels else RGB8, Linear, `maxAnisotropy`=4.0, Clamp) and `device_->createTexture`. |

### providers/RasterTextureUploader.h

| Item | Lines | Description |
| --- | --- | --- |
| `RasterTextureUploadOptions` | .h:10-18 | `generateMipmaps`=true; `enableEdgeBleed`=false (.h:17) — when set, texture gets a 1px repeated-edge border and UVs must be nudged 1px inward by `RasterMappedToTilesetTile::computeTranslationAndScale`. |
| `RasterTextureUploader` | .h:25-34 | Resource-prep boundary for raster imagery: `maxTextureSize()` + `uploadRasterTexture(DecodedImage&, options&)`. Provider owns tile lifecycle/CPU imagery; uploader owns decoded-pixels → GPU-texture step. |

---

## 14. platform — GLES, Metal, platform + curl bridges

### RenderDeviceGLES.h / .cpp

OpenGL ES 3.0 backend implementing `renderer/RenderDevice.h`. Assumes caller owns/activates EGL context. Header declares device + internal GL resource wrappers `GLTexture`/`GLBuffer`/`GLShaderProgram` (.h:61-98); `GLShaderProgram::uniformLocation` caches locations in `unordered_map` (.cpp:70-78).

| Item | Lines | Description |
| --- | --- | --- |
| Capability queries | .cpp:90-115 | `maxTextureSize`/`maxDrawBuffers` via `glGetIntegerv`; `supportsFloatTextures`/`supportsInstancing` hardcoded `true` (GLES 3.0 core); `rendererString` from `GL_RENDERER` |
| `createTexture` | .cpp:121-195 | Format map RGBA8/RGB8/R8/Depth32F; anisotropy via `GL_EXT_texture_filter_anisotropic` (ext-guarded, .cpp:23-27,163-171); wrap map (.cpp:173-187); mipmap gen |
| `updateTextureRegion` | .cpp:197-230 | Bounds-checked `glTexSubImage2D`; rejects `rowBytes != width*4`; returns `glGetError()==GL_NO_ERROR` |
| `createBuffer` / `updateBuffer` | .cpp:232-267 | Index→`GL_ELEMENT_ARRAY_BUFFER` else `GL_ARRAY_BUFFER`; Dynamic→`GL_DYNAMIC_DRAW`; `updateBuffer` bounds-checked `glBufferSubData` |
| `createShader` | .cpp:269-345 | Compile VS+FS, log via `__android_log_print`, link, delete shaders |
| `createFramebuffer` | .cpp:347-350 | Returns `nullptr` (MVP uses default FBO) |
| `beginFrame` | .cpp:356-373 | **Reverse-Z setup**: restores `glDepthMask(TRUE)`, disables blend/polygon-offset; `glClearColor(0.1,0.3,0.6,1)`; `glClearDepthf(0.0)` (clear to farthest); `glDepthFunc(GL_GEQUAL)`; cull back. Stale-depth comment (.cpp:358-360) |
| `submit` | .cpp:375-949 | Redundancy-cached program/VBO/IBO/texture + 15 attrib-enable flags; per-command dispatch below. Batch-end attrib/buffer/texture-unit teardown (.cpp:877-933). Perf log every 120 submits or ≥25ms (.cpp:935-948) |
| `endFrame` | .cpp:951-955 | **No-op — `glFlush()` removed**; `eglSwapBuffers` (external) implicitly syncs, avoids blocking CPU→GPU parallelism |
| `onSurfaceCreated`/`Changed`/`Destroyed` | .cpp:961-979 | State init (reverse-Z `GL_GEQUAL`), viewport cache, no-op destroy (EGL ctx may be dead) |

Per-command command-kind counters in `submit` (.cpp:420-437) tally only `SurfaceTile` / `GltfPrimitive[Instanced]` / `VectorOverlay` / `Sky+AtmosphereBackground` / `Unknown` — the `GlobeSurface` kind no longer exists in `RenderCommandKind`.

**Stride-based vertex-layout dispatch** in `submit` (.cpp:472-671) — no VAOs; per-command `glVertexAttribPointer` keyed on `cmd.vertexStride`:
- stride 32 OR glTF (kind `GltfPrimitive[Instanced]` && stride==**120**): attrib0 POSITION, 1 NORMAL, 2 TEXCOORD (4 floats for glTF, else 2); glTF adds attribs 10-14 (COLOR_0/TANGENT/TEXCOORD sets 2-7) (.cpp:476-524)
- `GltfPrimitiveInstanced` + `instanceStride==kGltfInstanceMatrixStride`: attribs 3-9 from `instanceBuffer` with `glVertexAttribDivisor(...,1)` — instance model matrix (4×vec4) + normal matrix (3×vec3) (.cpp:525-575)
- stride 20: terrain tile pos(12)+uv(8), normal in shader (.cpp:576-598)
- stride 8/12: vector/sky/atmosphere vec2/vec3 (.cpp:599-634)
- **stride 0 (final `else`, .cpp:635-671): vestigial 32-byte pos+normal+uv fallback** (`kSurfaceStride=32`) — no live command emits stride 0; kept only as a defensive default. No globe layout remains.
- Sampler bindings hardcode units: `u_tileTexture`=0, overlays 1..4, waterMask=5, glTF material samplers 0-14, glTF raster overlays base `kGltfRasterOverlayTextureBase`(15), glTF water mask `kGltfWaterMaskTextureSlot`(19) (.cpp:695-724; constants from `renderer/RenderCommand.h:14-19`)
- SurfaceTile uniforms block (.cpp:727-770): MVP has RTC baked in (u_tileOrigin removed, .cpp:754-755); generic `cmd.uniforms` map dispatched by element count (.cpp:771-795)

`Renderer::terrainShader()` is now **defined** (`kTerrainVertex/FragmentGLSL` + `kTerrainVertex/FragmentMSL`); the draw side is wired — this stride-32 GLES dispatch consumes the 32-byte `TerrainGpuVertex` terrain commands emitted by `GltfDrawCommandBuilder` via `makeTerrainPrimitiveCommand`.

### AndroidPlatformBridge.h / .cpp

`PlatformBridge` impl for Android. Network delegates to native `CurlPlatformBridge` (member `Impl::networkBridge`, .cpp:60-64); JNI used only for image decode + platform capability. `AndroidPlatformBridge_InitJvm` stashes `JavaVM*` global (.cpp:24-28).

| Item | Lines | Description |
| --- | --- | --- |
| JNI thread helpers | .cpp:32-54 | `getJniEnv` attaches current thread on `JNI_EDETACHED`; `detachJni`; `clearPendingJniException` |
| `onEnterBackground` | .cpp:76-78 | Forwards to `networkBridge.onEnterBackground()` (cancels queued curl requests) |
| `get`/`post`/`maximumActiveRequests` | .cpp:81-108 | Forward to `impl_->networkBridge` (curl) |
| `cacheDirectory`/`documentsDirectory` | .cpp:110-116 | Hardcoded `/data/data/com.earthengine.minimalglobe/{cache,files}` |
| `decodeImage` | .cpp:118-218 | JNI → `BitmapFactory.decodeByteArray` as ARGB_8888, locks pixels, requires `ANDROID_BITMAP_FORMAT_RGBA_8888`, row-copies to `DecodedImage` |
| `log`/`deviceInfo`/`getToken` | .cpp:220-234 | `__android_log_print`; `platform="Android"`, cpuCores=4; token stubbed `""` |

### ios/RenderDeviceMetal.h / .mm

Metal 2 backend (iOS/macOS) via `CAMetalLayer`. PImpl (.mm:62-75). Internal wrappers `MetalTexture` (tex+sampler), `MetalBuffer`, `MetalShaderProgram` (opaque+blended PSO pair + depth state) (.mm:20-56).

| Item | Lines | Description |
| --- | --- | --- |
| `makeDepthState` | .mm:77-85 | **Reverse-Z**: `MTLCompareFunctionGreaterEqual` (matches OpenGlobus `reverseDepth`); ctor precomputes readWrite/readOnly/disabled states (.mm:114-116) |
| Ctor | .mm:109-126 | Bridges `CAMetalLayer*`, creates command queue, depth states, `linearClampSampler` (maxAnisotropy=4) |
| Capabilities | .mm:133-151 | `maxTextureSize`=4096, `maxDrawBuffers`=4 hardcoded; `rendererString` from device name |
| `createTexture`/`updateTextureRegion` | .mm:157-224 | RGBA8/R8 pixel formats; per-texture sampler; `replaceRegion` bounds-checked |
| `createBuffer`/`updateBuffer` | .mm:226-249 | `MTLResourceStorageModeShared`; `updateBuffer` `memcpy` into `.contents` |
| `createShader` | .mm:251-473 | Compiles combined VS+FS MSL source; entry-point sniffing selects 6 `PipelineLayout` variants (Surface/Tile/Gltf/GltfInstanced/Color/DebugLine, .mm:279-322 — **no globe-shader detection**); builds `MTLVertexDescriptor` per layout; emits paired opaque(blend=NO)+blended(blend=YES) PSOs |
| `beginFrame` | .mm:483-526 | `nextDrawable`; clearColor (0.1,0.3,0.6,1); **reverse-Z `clearDepth=0.0`**; retains drawable via `objc_setAssociatedObject` |
| `submit` | .mm:528-805 | Per-command PSO/depth-state select; vertex buffer @0, instance buffer @7; **fixed uniform indices 0..83** (see below); texture+sampler binding 0..kGltfWaterMaskTextureSlot; cull/blend; indexed/instanced draw |
| `endFrame` | .mm:807-823 | End encoding, `presentDrawable`, `CFRelease`, commit |
| `onSurfaceChanged` | .mm:833-847 | Sets `drawableSize`; allocates `Depth32Float` depth texture (RenderTarget, Private) |

Vertex descriptors (.mm:325-433): DebugLine stride 8; Tile stride 20 (pos+uv); Color stride 12 (pos); Surface stride 32 (pos+normal+uv); Gltf/GltfInstanced stride 120 (attribs 0-2 + 10-14); GltfInstanced adds bufferIndex-7 attribs 3-9 (`kGltfInstanceMatrixStride`=**100**, `MTLVertexStepFunctionPerInstance`).

**Fixed uniform-buffer index scheme** (.mm:556-738) — hand-numbered `setVertexBytes`/`setFragmentBytes`: vertex MVP=1, model/tileBounds=2, tileUV=3, cameraRelativeOrigin=4; fragment `u_lightDir`=0 … glTF material factors and `u_mappedRaster*`/`u_gltfWaterMask*` uniforms densely packed to index **83** (.mm:738). SurfaceTile fast-path uses struct fields (`surfaceModelViewProjection`, `surfaceTileUv`, `surfaceCameraRelativeOrigin`, `surfaceLightDir`, opacities) instead of the `cmd.uniforms` map (.mm:565-620). `allowsNextDrawableTimeout` is set on the layer in the macOS demo (`examples/macos/MinimalGlobe/MetalView.mm`), not here.

Note: per-backend uniform binding is duplicated — GLES binds uniforms by name lookup, Metal by these fixed indices; the two lists must be kept in sync manually (duplication risk). Same 6-layout / stride tables are mirrored across GLES `submit` and Metal `createShader`.

### macos/MacPlatformBridge.h / .mm

macOS `PlatformBridge`: NSURLSession network / Keychain auth / CoreGraphics decode. PImpl `MacPlatformBridgeImpl` holds `NSURLSession` (.mm:13-15). `MacHttpRequest` wraps `NSURLSessionDataTask` for RAII cancel (.mm:23-34).

| Item | Lines | Description |
| --- | --- | --- |
| `get` | .mm:57-99 | `dataTaskWithURL` completion handler; `shared_ptr` callback; `-1` on bad URL |
| `cacheDirectory`/`documentsDirectory` | .mm:101-117 | `NSSearchPathForDirectoriesInDomains`, fallback `/tmp` |
| `decodeImage` | .mm:119-149 | `CGImageSource` → RGBA8 via `CGBitmapContext` (`kCGImageAlphaPremultipliedLast`) |
| `deviceInfo`/`log` | .mm:151-160 | `NSLog`; `platform="macOS"` |
| `getToken` | .mm:162-182 | Keychain `SecItemCopyMatching`, service `com.earthengine.provider.<id>` |

Note: `post` not overridden (header only declares `get`, .h:21-24) — inherits `PlatformBridge` default that immediately calls back `-1` (`bridge/PlatformBridge.h:112-122`).

### bridge/PlatformBridge.h

Platform-capability abstraction; engine core depends on this, not iOS/Android SDKs. Structs `DecodedImage` (.h:14-20), `DeviceInfo` (.h:23-32); enums `LogLevel`/`NetworkStatus`/`HttpRequestPriority` (.h:34-42); `HttpRequestOptions` {priority, headers} (.h:44-53).

| Item | Lines | Description |
| --- | --- | --- |
| `PlatformBridge` interface | .h:58-103 | Pure-virtual system/net/file/decode/log/device/token; `get` required, `post` virtual with default, `maximumActiveRequests` default `-1` |
| `HttpRequest` | .h:105-110 | RAII cancel handle |
| `post` default impl | .h:112-122 | Inline; invokes callback `(-1, {})` and returns `nullptr` |

### bridge/CurlPlatformBridge.h / .cpp

Cross-platform `PlatformBridge` on libcurl + stb_image (desktop dev + Android/iOS MVP network). All net methods forward to `CurlMultiRequestScheduler::shared()` process-wide singleton (.cpp:19-53).

| Item | Lines | Description |
| --- | --- | --- |
| `onEnterBackground` | .cpp:21-23 | `shared().cancelQueuedRequests()` |
| `get`/`post`/`maximumActiveRequests` | .cpp:27-53 | Delegate to shared scheduler |
| `cacheDirectory`/`documentsDirectory` | .cpp:55-61 | `/tmp/earth_engine_cache`, `.` |
| `decodeImage` | .cpp:63-85 | `stbi_load_from_memory` forcing 4 channels; `__has_include(<stb_image.h>)`-guarded (`EARTH_ENGINE_HAS_STB_IMAGE`, .cpp:5-10), returns `nullptr` if absent |
| `log`/`deviceInfo`/`getToken` | .cpp:87-101 | stderr; `platform="cross-platform"`, cpuCores=4; token `""` |

### bridge/CurlMultiRequestScheduler.h / .cpp

Process-wide singleton (`shared()`, .cpp:576-579) owning one libcurl `multi` handle + dedicated worker thread. **`kDefaultMaximumActiveRequests`** = 20 (.h:15). Public: `get`/`post`/`getBlocking`/`cancelQueuedRequests`/`shutdown`/`maximumActiveRequests` (.h:26-44).

| Item | Lines | Description |
| --- | --- | --- |
| `CurlGlobalLifetime` | .cpp:41-54 | Function-local static → one `curl_global_init`/`_cleanup` per process |
| `RequestState` | .cpp:59-75 | Per-request: seq, method, upload/response body, headers, `errorBuffer` (`CURL_ERROR_SIZE`), atomic `cancelled` |
| `WakeState` | .cpp:77-82 | Mutex-guarded cv+multi+alive; `weak_ptr` from `RequestHandle` to wake worker safely after shutdown |
| `RequestHandle` | .cpp:84-115 | `HttpRequest` impl; dtor cancels; `wake()` calls `curl_multi_wakeup` |
| `Impl` ctor / dtor | .cpp:117-132 | `curl_multi_init`, spawns `run()` worker; dtor → `shutdown` |
| `request` | .cpp:134-171 | Enqueues into `pending[priorityBucket]` (3 buckets 0-2); immediate `(-1,{})` if stopping |
| `cancelQueuedRequests` | .cpp:203-219 | Swaps out pending buckets, fires `(-1,{})` callbacks |
| `shutdown` | .cpp:221-264 | Idempotent; cancels pending+active, joins worker (guards self-join), fires deferred callbacks |
| priority scheduling | .cpp:297-308 | `popNextPendingLocked` drains highest bucket first (High=2→Low=0) |
| `run` (worker loop) | .cpp:310-353 | start→cancel→start→`curl_multi_perform`→drain; `curl_multi_wait` 50ms timeout; cv-waits when idle |
| `startRequest` | .cpp:385-443 | Configures easy handle: timeout 15s, connect 5s, follow redirects (max 3), UA `earth-md/0.1`, `NOSIGNAL`; POST fields + Content-Type header |
| `drainCompletedRequests` | .cpp:470-536 | Reads `CURLMSG_DONE`; status = httpCode or `-1`; body moved to callback only if status==200; Android-only failure log |
| `getBlocking` | .cpp:608-655 | Sync wrapper over async `get` (default 20s timeout); polls `shouldCancel` every 20ms; cancels on timeout/cancel |

### threading/RenderThreadPlacement.h + android/RenderThreadPlacementAndroid.cpp (+ threading/RenderThreadPlacementNoop.cpp)

Host-facing helper that keeps a caller-owned render thread off the little cluster — the SDK never creates that thread, so this is opt-in, not engine-internal. Cross-platform header, CMake-selected impl (Android real, everything else no-op) like `RenderDevice`/`PlatformBridge`.

| Item | Lines | Description |
| --- | --- | --- |
| `applyToCurrentThread` | Android .cpp:182-209 | Must run *on* the render thread (ADPF registers `gettid()`). `setpriority(-8)`, then three-tier fallback: ① ADPF `APerformanceHint` session (all symbols via `dlsym`, API 33 vs minSdk 26) → ② `uclamp_min` via raw `sched_setattr` syscall (no bionic wrapper) → ③ affinity to the performance cluster. Returns `Status{mode, pinnedCoreCount, threadNice}` |
| `reportActualWorkDurationMs` | Android .cpp:211-218 | Per-frame CPU work; **must exclude vsync wait** (`eglSwapBuffers`) or the system reads "always exactly on budget" and never boosts. No-op unless ① is live |
| `pinCurrentThreadToPerformanceCores` | Android .cpp:73-110 | ③'s core set from runtime `cpuinfo_max_freq` (anything above the lowest tier), never hardcoded core ids; bails on unreadable topology or homogeneous SoCs |
| `release` | Android .cpp:230-235 | Closes the ① session (bound to the registering tid) before the thread dies; dtor calls it; ②③ die with the thread |

Measured (PHK110 / api 36, release A/B): before = 91% of frames on cpu0-3, engine 21-22ms, ~30fps; after = 100% on cpu4-7, engine 6.6-9.4ms, locked 60fps. On that device ① is unimplemented by the PowerHAL and ② returns EPERM, so it lands on ③. `nice` alone does nothing for placement (it only weights timeslices inside one runqueue). Demo call sites: `examples/android/MinimalGlobe/GLESView.cpp` (thread entry / per-frame / thread exit), which logs `RenderThreadPlacement mode=… pinnedCores=… nice=…` plus the per-frame `cpu=` / `hint=` fields.

### stb_image_impl.cpp

Single translation unit defining `STB_IMAGE_IMPLEMENTATION` (avoids multiple-definition link errors); `__has_include(<stb_image.h>)`-guarded (.cpp:2-5).

**File paths (all under `/Users/ldy/Desktop/work/gis-md/scaffold/src/earth_engine/platform/`):** `android/RenderDeviceGLES.{h,cpp}`, `android/AndroidPlatformBridge.{h,cpp}`, `ios/RenderDeviceMetal.{h,mm}`, `macos/MacPlatformBridge.{h,mm}`, `bridge/PlatformBridge.h`, `bridge/CurlPlatformBridge.{h,cpp}`, `bridge/CurlMultiRequestScheduler.{h,cpp}`, `stb_image_impl.cpp`, `android/RenderThreadPlacementAndroid.cpp` (paired with `../../threading/RenderThreadPlacement.h` + `../../threading/RenderThreadPlacementNoop.cpp`). Shared render constants live in `../../renderer/RenderCommand.h:14-19`. Undefined `terrainShader()` decl at `../../renderer/Renderer.h:57` (no .cpp definition). Post-refactor: no `RenderCommandKind::GlobeSurface`, no globe shader/mesh path in either backend.

---

## 15. layers — RasterOverlay, ActivatedRasterOverlay, VectorLayer, CreditSystem

### RasterOverlay.h / .cpp

cesium-native `RasterOverlay` equivalent. Pure **config** layer: owns imagery data source + tile scheme + loading options. Runtime state lives in `ActivatedRasterOverlay` / `RasterOverlayTileProvider`. Non-copyable. Referenced by provider via `getOwner()`.

| Item | Lines | Description |
|---|---|---|
| `RasterOverlayRole` enum | .h:14-18 | BaseImagery / AnnotationOverlay / DataOverlay |
| `RasterOverlayPriority` enum | .h:20-24 | Low=0 / Normal=1 / High=2 |
| `RasterOverlayFallbackPolicy` enum | .h:26-31 | ExactOrAncestor / AncestorOrPlaceholder / PreviousAttached / SkipUntilReady |
| `Options` struct | .h:42-72 | see constants below |
| ctor(provider, scheme, options) | .h:77-79 / .cpp:9-29 | ownership of provider+scheme transferred; `id_` = `provider_->id()` (.cpp:15) |
| ctor option clamping | .cpp:16-28 | invalid values reset to defaults; opacity `std::clamp` 0..1 |
| `getProvider` / `getTileScheme` / `getOptions` | .h:87-96 | mutable + const accessors |
| `getID` | .h:99 | overlay id (from provider id) |
| `visible` / `setVisible` | .h:101-102 | forwards `options_.visible` |
| `opacity` / `setOpacity` | .h:104-105 / .cpp:33-35 | setter `std::clamp` 0..1 |
| `role` / `priority` / `fallbackPolicy` / `blocksCompleteRenderable` | .h:107-114 | forward `options_` |

Option defaults: **`maximumSimultaneousTileLoads`** = 20 (.h:44, cesium default .cpp:17); **`maximumScreenSpaceError`** = 2.0 (.h:47); **`subTileCacheBytes`** = 16 MiB (.h:50); **`maximumTextureSize`** = 2048 (.h:53); `coverageRectangle` = `Rectangle::MAXIMUM` (.h:60); `visible`=true / `opacity`=1.0 (.h:63-64); `role`=BaseImagery, `priority`=High, `fallbackPolicy`=AncestorOrPlaceholder, `blocksCompleteRenderable`=true (.h:67-71).

### ActivatedRasterOverlay.h / .cpp

cesium-native `ActivatedRasterOverlay` equivalent. Wraps a `RasterOverlay&` (must outlive) with runtime state: lazily builds the real `RasterOverlayTileProvider` (+ `RenderDeviceRasterTextureUploader`) on first `ensureTileProvider(device)`; a `placeholderProvider_` (null uploader) exists from construction. Non-copyable.

| Method | Lines | Description |
|---|---|---|
| ctor(overlay) | .h:24 / .cpp:8-20 | builds `placeholderProvider_` with `nullptr` uploader; sets owner, `maximumSimultaneousTileLoads`, `maximumScreenSpaceError` from overlay options |
| `ensureTileProvider(RenderDevice*)` | .h:33 / .cpp:24-44 | lazily creates real provider; builds `RenderDeviceRasterTextureUploader` only if `device` non-null (.cpp:28-31); sets owner + throttle + SSE; then `syncProviderOptionsFromOverlay()` |
| `getTileProvider` | .h:36-37 | real provider, null until ensured |
| `getPlaceholderTile` | .h:40 / .cpp:46-52 | uses `placeholderProvider_` else `tileProvider_`; returns `provider->getPlaceholderTile().get()` |
| `processPendingUploads(bool, FrameResourceBudget*)` | .h:42 / .cpp:54-62 | syncs then forwards to provider; 0 if no provider |
| `hasPendingWork` / `revision` | .h:44-45 / .cpp:64-70 | forward provider or false/0 |
| `setFrameNumber` / `trimUnusedTiles` / `getCachedTileCount` | .h:46-48 / .cpp:72-88 | sync + forward |
| `setMaximumSimultaneousTileLoads(int)` | .h:52 / .cpp:90-100 | `n>0 ? n : 20`; applies to both providers |
| `getThrottledTilesCurrentlyLoading` | .h:53 / .cpp:102-105 | forward, 0 if none |
| `visible` / `opacity` | .h:55-56 / .cpp:107-113 | delegate to `overlay_` |
| `syncProviderOptionsFromOverlay` (private) | .h:65 / .cpp:115-130 | re-derives throttle from overlay (`>0 ? : 20`), calls `applyOwnerOptions()` on both providers |

Default `maximumSimultaneousTileLoads_` = 20 (.h:70).

### CreditSystem.h / .cpp

Copyright attribution, aligned with cesium-native `CesiumUtility::CreditSystem`. Refcounted HTML strings with frame-diff snapshots. Non-copyable.

| Item | Lines | Description |
|---|---|---|
| `CreditFilteringMode` enum | .h:14-18 | None=0 / UniqueHtmlAndShowOnScreen=1 / UniqueHtml=2 |
| `Credit` struct | .h:21-31 | value handle `{id, generation}`, `==`/`!=` |
| `CreditsSnapshot` struct | .h:34-37 | `currentCredits` + `removedCredits` vectors |
| `Record` (private) | .h:79-85 | html, showOnScreen, referenceCount, shownLastSnapshot, generation |
| `allocateIndex` (private) | .h:87 / .cpp:11-20 | reuses `freeIndices_` back, else grows `records_` |
| `isValid` (private) | .h:88 / .cpp:22-25 | idx in range and generation != UINT32_MAX (tombstone) |
| `createCredit(html, showOnScreen=false)` | .h:51 / .cpp:27-46 | O(n) dedup on (html, showOnScreen) → reuse; else allocate, generation bump, `++nextId_` |
| `shouldBeShownOnScreen` / `setShowOnScreen` | .h:54-57 / .cpp:48-56 | guarded by `isValid` |
| `getHtml` | .h:60 / .cpp:58-61 | returns `kInvalidCreditMsg` if invalid |
| `addCreditReference` / `removeCreditReference` | .h:64-68 / .cpp:63-83 | generation-checked; refcount saturates at INT32_MAX / floors at 0 |
| `getSnapshot(mode=UniqueHtml)` | .h:72-73 / .cpp:85-143 | rebuilds current (refcount>0, filtered per mode), diffs against prior snapshot for `removedCredits`; returns ref valid until next call |

`kInvalidIndex` = UINT32_MAX (.h:76-77); `kInvalidCreditMsg` = "Error: Invalid Credit, cannot get HTML string." (.cpp:8-9). Note snapshot filtering and dedup are O(n²) linear scans.

### VectorLayer.h / .cpp

GeoJSON vector layer: owns `GeoFeature` set + `OverlayStyle`, tessellates each feature to ECEF, emits `RenderCommand`s. Interaction state (hover/selected) switches style via uniform without rebuilding geometry. Non-copyable. Not a cesium-native mirror. Comments are Chinese.

| Item | Lines | Description |
|---|---|---|
| `TessellateResult` (private struct) | .h:88-94 | interleaved xyz `vertices`, `indices`, `centroid`, counts |
| ctor(layerId, features, style, RenderDevice*) | .h:33-36 / .cpp:181-188 | moves in features + style |
| dtor | .cpp:190-192 | calls `dispose()` |
| `setOpacity` | .cpp:194-196 | clamps `style_.layer.opacity` 0..1 |
| `setStyle` | .cpp:198-201 | replaces style, clears `tessCache_` |
| `setFeatureState` / `clearAllStates` / `featureState` | .cpp:203-219 | empty state erases entry |
| `findFeature` | .h:71 / .cpp:221-226 | O(n) scan by `f.id` |
| `initialize(device)` | .h:76 / .cpp:228-232 | sets device, `initialized_=true` |
| `dispose` | .h:84 / .cpp:234-237 | clears `tessCache_` |
| `tessellateFeature` | .h:97 / .cpp:243-265 | cache lookup, dispatch by `feature.type`, cache result |
| `tessellatePoint` | .h:100 / .cpp:267-283 | centroid = ECEF of first coord; placeholder vertex, quad indices (billboard built at draw time) |
| `tessellateLine` | .h:102 / .cpp:285-327 | subdivides each segment along ellipsoid (`subdivideArc`, 8 segs), LineStrip indices |
| `tessellatePolygon` | .h:104 / .cpp:329-361 | outer ring → ECEF, ear-clip `triangulatePolygon` |
| `resolveStyleOverride` | .h:107 / .cpp:367-376 | maps feature state → `InteractionStyleOverride`, else `normalOverride` |
| `resolveColor` | .h:111 / .cpp:378-397 | base color per geometry variant + `colorShift`, alpha × layer opacity |
| `buildRenderCommands(FrameState, Renderer&, RenderCommandList&)` | .h:79 / .cpp:403-592 | per-feature: creates dynamic vertex/index buffers (lifetime in `frameBuffers_`), sets `u_color`+`u_modelViewProjection`, `shader = renderer.colorShader()` |

Free helpers (.cpp): `geoToECEF` via `Ellipsoid::WGS84().cartographicToCartesian` (.cpp:29-31); `subdivideArc` slerp + `scaleToGeodeticSurface`, default 16 segs (.cpp:37-50); ear-clipping `isEar`/`signedArea2D`/`projectToLocalTangentPlane`/`triangulatePolygon` with fan-triangulation fallback (.cpp:56-173); `kQuadIndices` = {0,1,2,0,2,3} (.cpp:23). Point path emits a camera-facing billboard quad in world space at draw time (`vertexStride`=12, .cpp:439-515); Line/Polygon upload `tess.vertices` directly (.cpp:517-587). `u_modelViewProjection` = camera view-projection (`vpUniform`, .cpp:413-424).

---

## 16. environment — Atmosphere, SkyBox, SkyGradient, Sun, Time

### AtmosphereParameters.h

Plain-data struct of Bruneton-2008-style scattering coefficients; aligns with openglobus `atmos.ts` `DEFAULT_PARAMS`. Analytic (no LUT) — struct kept to allow future LUT path.

| Item | Lines | Description |
| --- | --- | --- |
| `struct AtmosphereParameters` | .h:10-83 | Scale heights, sea-level scattering coeffs, ozone, radii |
| **`atmosHeight`** = 100000.0 m | .h:12 | Atmosphere shell thickness (100 km) |
| **`rayleighScaleHeight`** = 7994.0 m | .h:15 | Rayleigh density scale height (~8 km) |
| **`mieScaleHeight`** = 1200.0 m | .h:18 | Mie/aerosol scale height |
| `rayleighSeaLevelScattering` = 0.056, `mieSeaLevelScattering` = 0.0045 | .h:22,25 | openglobus `RAYLEIGH_SCALE`/`MIE_SCALE` |
| **`groundAlbedo`** = 0.022 | .h:28 | Secondary-scatter ground reflectance |
| `equatorialRadius` = 6378137.0, `bottomRadius` = 6356752.314 | .h:31,35 | WGS84 a / b; atmosphere measured from bottom radius |
| `RayleighCoefficients {r=3.9,g=10.8,b=20.0}` | .h:39-43 | Per-wavelength (680/550/440 nm) |
| `MieCoefficients {scattering=1.6,extinction=1.85}` | .h:46-49 | |
| `OzoneCoefficients {r=1.25,g=2.1,b=0.09}` | .h:52-56 | + density center 25000 m / half-width 15000 m (.h:59-60) |
| `sunAngularRadius` = 0.04685 rad, `sunIntensity` = 0.78 | .h:63,66 | Sun disk size / intensity multiplier |
| `topRadius()` | .h:74 | `bottomRadius + atmosHeight` |
| `validate()` | .h:77-82 | Clamp scale heights to positive defaults |
| `earthAtmosphereDefaults()` / `marsAtmosphereDefaults()` | .h:86-88 / .h:91-108 | Preset factories (openglobus `DEFAULT_PARAMS`/`MARS_PARAMS`) |

### AtmosphereBackgroundPass.h / .cpp

Full-screen atmospheric-scattering pass, ported from openglobus `Atmosphere.ts` + `atmosphere.frag.glsl`. Runs real-time Rayleigh+Mie+Ozone with an 8-sample optical-depth integral, sun disk + corona/stellar-ray bloom. No precomputed LUT.

| Item | Lines | Description |
| --- | --- | --- |
| `initialize(RenderDevice*)` | .cpp:269-308 | Compiles shader, uploads 4-vertex fullscreen quad (`TriangleStrip`) |
| `buildCommand(camPos,fov,vpW,vpH,camRight,camUp,camForward,sunDir,params,opacity=1)` | .cpp:310-367 | Emits one `RenderCommand`, kind `AtmosphereBackground` (.cpp:323) |
| `isReady()` / `dispose()` | .h:46 / .cpp:369-372 | |
| GLSL vert `kAtmosphereBackgroundVert` | .cpp:20-28 | Draws at `gl_Position.z=0` (reverse-Z far plane) after terrain; composites only sky pixels |
| GLSL frag `kAtmosphereBackgroundFrag` | .cpp:30-259 | View ray rebuilt in ECEF from camera basis (.cpp:64-73); ray-sphere shell intersect (.cpp:85-122); **ground-facing rays `discard`** (.cpp:127-129) so ground haze belongs to surface shader |
| Scatter integral | .cpp:135-152 | `SAMPLE_COUNT=8`, `rayleighScaleHeight=8000`, `mieScaleHeight=1200` |
| Phase fns | .cpp:52-62 | `rayleighPhase`; softened Henyey-Greenstein `miePhase` (g=0.76) |
| Sun disk + corona + stellar rays | .cpp:190-249 | Limb darkening, screen-space cross/diagonal flares gated by `spaceFactor` |
| Alpha composite | .cpp:251-257 | `skyAlpha` fades to 0.18 in space; limb + sun alpha |

Render command state (.cpp:322-337): `pass="color"`, depthTest on / depthWrite off, alpha blend (SrcAlpha/OneMinusSrcAlpha), no cull. Uniform `u_bottomRadius` is the local surface radius from `Ellipsoid::projectToSurface(cameraPos)` (.cpp:352-357), `u_topRadius = bottom + atmosHeight`.

### SkyBox.h / .cpp

Sky-box background, ported from openglobus `SkyBox.ts` + `skybox.ts`. Two modes: 6-face cubemap (daytime) or procedural starfield (night); uses `gl_Position.xyww` depth trick to force far plane. Only starfield path is functional — cubemap texture load is a platform stub (`cubemapTexture_ = nullptr`, .cpp:238).

| Item | Lines | Description |
| --- | --- | --- |
| `struct CubemapPaths` | .h:26-33 | 6 optional face paths; nullptr → procedural |
| `setCubemapPaths` / `setSize`(=10000 m) | .cpp:210-215 / .h:45 | `useCubemap_` set if any face non-null |
| `initialize(RenderDevice*)` | .cpp:217-257 | Picks cubemap vs starfield shader; scales 36-vertex cube by `size_` |
| `buildCommand(viewMatrix,projMatrix,isOrthographic,nightFactor)` | .cpp:259-317 | Kind `SkyBackground` (.cpp:266); strips view translation (.cpp:284-288), computes `proj*viewRot` col-major (.cpp:294-302) |
| GLSL cubemap vert/frag | .cpp:15-42 | `samplerCube` sample; `xyww` far-plane |
| GLSL starfield vert/frag | .cpp:45-171 | `hash`/`noise3D`, `artStarLayer` 3 layers (.cpp:150-152), painted Milky-Way ribbon (.cpp:144-148); `u_nightFactor` scales stars & alpha (0=day transparent, 1=night) |
| `kCubeVertexCount` = 36 | .cpp:196 | 6 faces × 2 tris |

Command state (.cpp:265-280): `pass="color"`, depthTest **off**, depthWrite off, alpha blend, no cull. `u_time` hard-wired to 0 (.cpp:314) — no animation.

### SkyGradient.h / .cpp

CPU analytic Rayleigh+Mie+Ozone color solver. Produces zenith / horizon / ambient colors + sun elevation from sun dir, local up, and camera altitude. Horizon color feeds `FrameState` clear color; ambient feeds lighting.

| Method | Lines | Algorithm |
| --- | --- | --- |
| `SkyGradient()` / `(params)` / `setParameters` | .cpp:67-78 | Default `earthAtmosphereDefaults()`; `params.validate()` |
| `update(sunDirECEF,localUpECEF,camAltMeters=0)` | .cpp:80-247 | Core solve |
| — daylight factor | .cpp:89-91 | `sunElevation=asin(sun·up)`; `daylight=smoothstep(-0.06,0.04,elev)` |
| — sea-level β / optical depth | .cpp:100-118 | `betaR/M/O`; vertical optical depth 0→atmosHeight |
| — zenith color | .cpp:125-162 | Phase × extinction × ozone × `sunIntensity` × daylight; `expose(·,330)` |
| — horizon color | .cpp:164-223 | `horizMassFactor=38.0` (.cpp:181) air mass; secondary ground-albedo scatter (.cpp:201-204); `expose(·,30)`; sunrise/sunset warm-tint boost for elev<~17° (.cpp:211-222) |
| — ambient color | .cpp:225-246 | `(0.6·zenith+0.4·horizon)·0.35`, min 0.005; night falloff `exp(elev·8)` when elev<-0.05 |
| `zenithColor/horizonColor/ambientColor/sunElevation` accessors | .h:38-47 | RGBA/RGBA/RGB arrays |
| helpers `rayleighPhase`/`miePhase`(g=0.76)/`ozoneDensity`/`opticalDepthVertical`/`smoothstep`/`expose` | .cpp:18-59 | `kInv4Pi` (.cpp:15); Mie denom clamp `1e-6` |

### SunDirection.h / .cpp

Static astronomical sun-direction solver (~0.5° accuracy). Ported to match openglobus `astro/earth.ts` `getSunPosition` (stjarnhimlen.se). Input Julian Date, output ECEF unit vector (geocenter→sun).

| Method | Lines | Algorithm |
| --- | --- | --- |
| `compute(julianDate)` | .cpp:32-84 | Perihelion lon, eccentricity, mean anomaly → eccentric/true anomaly → ecliptic lon → equatorial → sidereal Z-rotation `theta` (.cpp:65-66) → normalized ECEF |
| `elevation(julianDate)` | .cpp:86-89 | `asin(dir.z)` (relative to equatorial plane) |
| `cosIncidence(jd,lngRad,latRad)` | .cpp:91-98 | `max(0, sunDir · WGS84 geodeticSurfaceNormal)` for diffuse |
| **`kJ2000`** = 2451545.0 | .cpp:20 | J2000.0 epoch |
| **`kObliquityDeg`** = 23.4392911 | .cpp:21 | J2000 obliquity |
| **`kAuToMeters`** = 1.4959787e11 | .cpp:23 | AU → m |
| `rev()` | .cpp:24-28 | Wrap angle to [0,360) |

### TimeController.h / .cpp

Single time source for the environment system (sun, stars). Stores Julian Date (TT); Unix conversions.

| Item | Lines | Description |
| --- | --- | --- |
| `TimeController()` | .cpp:30-31 | Inits to `currentJulianDate()` |
| `julianDate()` / `setJulianDate` | .h:21 / .cpp:33-35 | |
| `unixTimestamp()` / `setUnixTimestamp` | .cpp:41-43 / .cpp:37-39 | |
| `advanceSeconds(s)` | .cpp:45-47 | `jd += s/86400` |
| `secondsPerDay` = 86400.0 | .h:33 | |
| `unixToJulian` / `julianToUnix` / `currentJulianDate` | .cpp:11-13 / 15-17 / 19-24 | Free fns |
| **`kJdUnixEpoch`** = 2440587.5 | .cpp:8 | JD of Unix epoch |

### FrameState / render-pass wiring (consumers)

These environment types are owned by `scene/SceneEnvironmentCoordinator` (.h:37-40: `TimeController`, `SkyGradient`, `AtmosphereBackgroundPass`, `SkyBox` as `unique_ptr`s; `sunDirection()` = `SunDirection::compute(timeController->julianDate())`, SceneEnvironmentCoordinator.cpp:37).

Data flow into `FrameState` — `scene/SceneFrameStateBuilder.cpp:33-52`:
- `sunDir = SunDirection::compute(timeController->julianDate())` (.cpp:39); `skyGradient->update(sunDir, WGS84 normal, camera height)` (.cpp:41-43).
- `frameState.lightDir` ← sunDir (.cpp:44-47).
- `frameState.clearR/G/B` ← `skyGradient->horizonColor()` (.cpp:48-51) — horizon color is the frame clear color.

Pass emission — `scene/SceneRenderPipeline.cpp`:
- `buildSkyCommands` (.cpp:204-242): computes `nightFactor` from `skyGradient->sunElevation()` (`exp(elev·8)` below -0.05, .cpp:226-232) max'd with `spaceFactor` (altitude 120k→900k, .cpp:233-236); pushes `SkyBox::buildCommand` with rotation-only view+proj.
- `buildAtmosphereCommands` (.cpp:244-271): rebuilds `sunDir` from `frameState.lightDir` (.cpp:255-257); pushes `AtmosphereBackgroundPass::buildCommand` using camera basis (`right/up/direction`) and `skyGradient->parameters()`.
- Command ordering (`renderer/RenderCommand.h:23-24`): `SkyBackground` order 0, `AtmosphereBackground` order 5 — skybox first, atmosphere over it, both before/behind terrain per depth rules noted above.

---

## 17. interaction — InputManager, PickingService, SelectionManager

### InputEvent.h

Normalized cross-platform input event. All platform input (iOS `UITouch`/`UIEvent`, Android `MotionEvent`, mouse) converted to `InputEvent` by the platform adapter before reaching the engine.

| Item | Lines | Description |
| --- | --- | --- |
| `Type` enum | .h:18-27 | `PointerDown/Move/Up`, `PinchStart/Move/End`, `Cancel`, `Key` (uint8_t) |
| `PointerType` enum | .h:29-33 | `Touch`, `Mouse`, `Pen` |
| `Modifiers` struct | .h:35-42 | `shift/ctrl/alt/meta` bools + `any()` |
| `screenX/screenY` | .h:47-48 | Physical surface pixels (× devicePixelRatio), top-left origin |
| `devicePixelRatio` | .h:51 | Screen density, ref for PickingService/unproject |
| `buttons` / `pointerCount` | .h:56/59 | Button bitmask; active pointer count (1=mouse, 2=pinch) |
| `timestamp` | .h:65 | Monotonic seconds (platform clock, not wall time) |
| pinch fields | .h:69-76 | `pinchScale` (per-frame ratio), `rotationRadians`, `centerDeltaX/Y` |
| pointer-pair fields | .h:80-84 | `hasPointerPair` + `pointer0X/Y`, `pointer1X/Y` raw positions |
| `isPointerEvent()` / `isPinchEvent()` | .h:87-97 | Type-category helpers |

### InputManager.h / .cpp

Gesture recognizer. Consumes normalized `InputEvent` stream; recognizes drag, pinch, click, double-click. **Fully callback-decoupled** — never touches Camera/Selection/GPU directly; notifies owner (Scene) via `Callback`.

| Item | Lines | Description |
| --- | --- | --- |
| `Gesture` enum | .h:22-31 | `DragStart/Move/End`, `PinchStart/Move/End`, `Click`, `DoubleClick`; comments map each to `CameraController::onDrag*`/`onPinch`/pick+onSelect |
| `Callback` type | .h:36 | `std::function<void(Gesture, const InputEvent&)>`; set via `setCallback()` (.h:41) |
| `process()` | .cpp:6-100 | Main event dispatch; early-out if no callback (.cpp:7) |
| `reset()` | .cpp:102-107 | Clears state on interrupt/scene destroy |
| `State` enum | .h:58-63 | `Idle`, `OneFingerPending`, `OneFingerDrag`, `TwoFinger` |
| Pinch passthrough | .cpp:15-51 | Pinch events routed directly; synthesizes `PinchStart` if `PinchMove` arrives without start (.cpp:25-36) |
| Pointer handling | .cpp:54-99 | `PointerDown`→pending, `PointerMove`→drag-threshold check, `PointerUp`→finish |
| Drag detection | .cpp:71-89 | Promotes pending→drag when displacement ≥ `dragThreshold_`; `DragStart` uses start position (.cpp:77-81) |
| `finishPointerGesture()` | .cpp:109-147 | Emits `DragEnd`, or click/double-click; suppresses click after drag/pinch |
| Double-click test | .cpp:128-136 | Within `doubleClickInterval_` AND displacement ≤ `dragThreshold_`; resets timer on double to block triple (.cpp:139) |
| `cancelActiveGesture()` | .cpp:149-165 | Synthesizes `DragEnd`/`PinchEnd` for in-flight gestures |
| **`dragThreshold_`** | .h:86 | = 8.0f px (also reused as double-click position tolerance) |
| **`doubleClickInterval_`** | .h:87 | = 0.35 s |

### PickingService.h / .cpp

Ray-picking service. Generates ray via `Camera::getPickRay()`, tests against ellipsoid + vector-layer features. Hit priority: nearest `VectorFeature` > `Ellipsoid`. Stateless (all methods const/static); **no GPU/framebuffer readback** — pure geometric CPU picking.

| Item | Lines | Description |
| --- | --- | --- |
| `PickResult` struct | .h:16-48 | `screenX/Y`, `HitType` (None/Ellipsoid/Terrain/VectorFeature), `terrainHeight`, `cartographic`, `worldPosition` (ECEF), `layerId`/`featureId`, `distance`, `isValid()` |
| `rayEllipsoidIntersection()` | .cpp:21-45 | WGS84 via `Ellipsoid::rayIntersectionInterval`; uses entry dist, falls back to exit; on-surface origin special-case tol **1e-6** (.cpp:34) |
| `rayTriangleIntersection()` | .cpp:51-69 | Delegates to `IntersectionTests::rayTriangleParametric` (Möller–Trumbore); cesium-native `IntersectionTests::rayTriangle` equivalent |
| `pick()` | .cpp:75-251 | Ellipsoid fallback first, then ray-vs-feature; keeps nearest by `distance` |
| — Polygon path | .cpp:103-138 | ECEF-projects ring[0], fan-triangulates around v0, ray-triangle test |
| — LineString path | .cpp:139-246 | Ray-segment shortest-distance (constrained Eberly variant); dynamic per-pixel tolerance × 10 (.cpp:143-149); **`kEpsSeg`** = 1e-12 (.cpp:178) |
| `pickEllipsoid()` | .cpp:253-281 | Ray-ellipsoid only; sets Ellipsoid hit + cartographic/distance |
| `pickTerrain()` | .cpp:283-319 | Ellipsoid hit then `terrainSampler(lngRad,latRad)→height`; recomputes ECEF + distance. Note: sampler callback-injected, decoupled from tile/GPU terrain (async terrain GPU-upload path with 32-byte `TerrainGpuVertex` draws via the wired terrain shader path) |

### SelectionManager.h / .cpp

Selection-state manager. Centralizes hovered/selected features, multi-select (Shift add, Ctrl/Meta toggle). **Callback-decoupled** — notifies `VectorLayer` style updates via `StateChangeCallback`; holds no GPU resources and rebuilds no geometry.

| Item | Lines | Description |
| --- | --- | --- |
| `FeatureState` enum | .h:13-17 | `None`, `Hovered`, `Selected` |
| `FeatureRef` struct | .h:20-29 | `layerId`+`featureId` pair; `operator==`, `empty()` |
| `std::hash<FeatureRef>` | .h:34-42 | `h1 ^ (h2 << 1)` for `unordered_set` keying |
| `StateChangeCallback` | .h:57-60 | `void(layerId, featureId, FeatureState)`; set via `setStateChangeCallback()` (.h:65) |
| `onHover()` | .cpp:16-41 | No-op if unchanged; clears old, skips already-selected (shows Selected not Hovered) (.cpp:33-38) |
| `clearHover()` | .cpp:43-45 | `onHover(PickResult{})` |
| `onSelect()` | .cpp:49-56 | Single-select: clears then adds |
| `onSelectAdd()` | .cpp:58-65 | Shift add (skip if already selected) |
| `onSelectToggle()` | .cpp:67-76 | Ctrl/Meta toggle |
| `clearSelection()` | .cpp:78-85 | Emits `None` for all, clears set, marks dirty |
| `firstSelected()` | .cpp:87-93 | Lazily caches `*selected_.begin()`; `mutable cachedFirstSelected_`/`selectedDirty_` (.h:118-119) |
| `addToSelection()` / `removeFromSelection()` | .cpp:97-118 | Set mutation + `setFeatureState` emit; dead-code hover branch at .cpp:106-109 |

---

## 18. Engine + sdk

### Engine.h / .cpp

Top-level platform-facing API: lifecycle + input router. Owns exactly one `Scene` (`std::make_unique<Scene>()`, .cpp:19); holds a **non-owning** `RenderDevice*` (`device_`, .h:134). Copy/move deleted (.h:39-40). Nearly every public method is a thin forward to `scene_`. cesium-native `Cesium3DTilesSelection` app-shell analog.

| Item | Lines | Description |
| --- | --- | --- |
| ctor `Engine(RenderDevice*)` | .h:35, .cpp:17-20 | Stores device_, constructs Scene. |
| dtor | .cpp:22-24 | Calls `onSurfaceDestroyed()`. |
| `onSurfaceCreated()` | .h:45, .cpp:26-35 | `device_->onSurfaceCreated()` then `scene_->setRenderDevice(device_)`; sets `surfaceCreated_` on success. |
| `onSurfaceChanged(w,h,dpr=1)` | .h:48, .cpp:37-40 | Forwards to `device_->onSurfaceChanged` + `scene_->setViewport`. |
| `onSurfaceDestroyed()` | .h:51, .cpp:42-48 | `scene_->setRenderDevice(nullptr)` + `device_->onSurfaceDestroyed()`. |
| `render(deltaSeconds=0)` | .h:57, .cpp:50-112 | Per-frame driver. Guards `surfaceCreated_ && isReady()` (logs BLOCKED, .cpp:51). Auto-computes delta via `steady_clock` when ≤0, fallback 1/60 (.cpp:55-65). Ordered phases each timed via `perf::nowMs()` + `scene_->recordEngineTiming`: `device_->beginFrame` → `scene_->update` → `scene_->render` → `device_->endFrame` (.cpp:67-94). `scene_->finishEngineFrame` + `perf::logTiming` summary (.cpp:96-111). |
| `onInputEvent(InputEvent)` | .h:62, .cpp:114-116 | Forward to `scene_->onInputEvent`. |
| `onDragStart/Move/End` | .h:65-67, .cpp:118-141 | Legacy compat: build `InputEvent` (PointerDown/Move/Up, `PointerType::Touch`) and call `onInputEvent`. |
| `addVectorLayer / removeVectorLayer / vectorLayerCount` | .h:72-78, .cpp:149-159 | Forward to scene_. |
| `setTileset(unique_ptr<Tileset>)` | .h:81, .cpp:161-163 | cesium-native aligned: unified terrain Tileset → `scene_->setTileset`. |
| `addTileset(unique_ptr<Tileset>)` | .h:83, .cpp:165-167 | Parallel 3D Tiles / glTF content Tileset; not terrain-sampled. |
| `setSelectorViewOverride / clearSelectorViewOverride` | .h:87-89, .cpp:169-176 | Override selector frustum list; empty ⇒ no selectable view this frame. |
| `setOcclusionCallback / clearOcclusionCallback` | .h:90-91, .cpp:178-184 | Forward `TileOcclusionCallback`. |
| `hasTerrain()` | .h:94, .cpp:186-188 | `scene_->hasTerrain()`. |
| `pick / onHover / onSelect / clearSelection` | .h:99-108, .cpp:192-206 | Picking + selection forwards. |
| `setTime / time / advanceTime / sunDirection / getClearColor` | .h:113-121, .cpp:210-232 | Environment system. `getClearColor` reads `frameState().clearR/G/B/A` (.cpp:226-232). |
| `diagnostics() / presentationTrace()` | .h:124-126, .cpp:234-240 | Runtime `Diagnostics` + per-frame `PresentationTrace`. |
| `camera() / isReady()` | .h:130-131, .cpp:143-145, 242-244 | `isReady` = `scene_ && scene_->isReady()`. |
| members | .h:134-137 | `RenderDevice* device_` (non-owning), `unique_ptr<Scene> scene_`, `double lastRenderTime_`, `bool surfaceCreated_`. |

Post-refactor: the fallback-globe path is gone. `Renderer::initialize()` no longer builds globe buffers/shader, `SceneRenderPipeline` no longer inserts a fallback-globe command, and `Globe`/`GlobeMesh`/`GlobeVertex` were deleted — before tiles load the frame is clear-color only. The `Diagnostics` globe-fallback counter fields were deleted along with the fallback path.

### EarthEngineSdkFacade.h / .cpp

Thin SDK entry point: `installScene(EarthSceneConfig)` builds providers/overlays/terrain and installs one unified `Tileset` (+ optional glTF Tileset) into an already-created `Engine`. Caller owns Engine/RenderDevice/PlatformBridge; facade owns the created `RasterOverlay` / `ActivatedRasterOverlay` runtime objects (.h:54-56), released on destroy or re-install. Copy/move deleted (.h:32-33).

| Item | Lines | Description |
| --- | --- | --- |
| ctor `(Engine&, RenderDevice&, PlatformBridge&)` | .h:27-29, .cpp:169-174 | Stores references only. |
| `installScene(EarthSceneConfig)` | .h:37, .cpp:178-538 | Move-stores config_, `resetCamera()`, clears overlay vectors, builds raster stack, creates unified Tileset, optional glTF Tileset, sets sim time. See per-kind rows below. |
| `resetCamera()` | .h:39, .cpp:540-552 | Rebuilds camera from `initialCamera` via `Ellipsoid::WGS84().cartographicToCartesian` + `geodeticSurfaceNormal`; `camera().lookAt(camEcef, targetEcef, up)`. No source rebuild. |
| `config()` | .h:41 | Const accessor for stored `EarthSceneConfig`. |
| `addActivatedRasterOverlay(...)` | .h:44-48, .cpp:554-568 | Wraps provider+scheme+options into `RasterOverlay`, then `ActivatedRasterOverlay`; pushes raw ptr into selection vector + owns both uniques. |

Overlay dispatch inside `installScene` (by `ImagerySourceKind`, .cpp:186-495):

| Kind | Lines | Notes |
| --- | --- | --- |
| Debug | .cpp:188-196 | `DebugImageryProvider` + XYZ-WebMercator scheme. |
| TileMapService | .cpp:198-237 | **Blocking** fetch of `tilemapresource.xml` via `BlockingHttpFetcher::fetchBlocking` (.cpp:202); parse via `createTileMapServiceImagerySource`; applies coverage rectangle if present. |
| WebMapService | .cpp:239-288 | **Blocking** GetCapabilities fetch (.cpp:257) + `validateWebMapServiceCapabilities`; Geographic-TMS scheme. |
| WebMapTileService | .cpp:290-330 | Scheme = Geographic-TMS if `wmtsSchemeId=="Geographic-TMS"` else XYZ-WebMercator (.cpp:325-327). No blocking fetch. |
| BingMaps | .cpp:332-400 | Two paths: explicit `urlTemplate` (no fetch, .cpp:333-358) or **blocking** metadata fetch (.cpp:366) + `parseBingMapsMetadata`. |
| GoogleMapTiles | .cpp:402-481 | If no session, **blocking POST** `createSession` via `postBlocking` (.cpp:426) + `parseGoogleMapTilesCreateSessionResponse`; else reuse configured session. Default maxLevel 28 (.cpp:463). |
| Xyz (default/fallback) | .cpp:483-494 | `XYZImageryProvider` + XYZ-WebMercator. |

| Helper | Lines | Description |
| --- | --- | --- |
| `makeSceneTilesetOptions` | .cpp:37-42 | Maps `SceneTilesetConfig` → `TilesetOptions` (main-thread + cache-unload time limits). |
| `makeRasterOverlayOptions` | .cpp:44-59 | `RasterOverlaySourceConfig` → `RasterOverlay::Options` (loads/SSE/zoom/opacity/role/priority/fallback/blocks). |
| `postBlocking(...)` | .cpp:69-110 | **Blocking HTTP POST**: `PlatformBridge::post` + mutex/condition_variable, default timeout 20s (.cpp:74); cancels request on timeout. Same blocking pattern as `fetchBlocking` — setup runs synchronously and stalls the caller thread. |
| `applyConfiguredZoomRange` | .cpp:112-122 | Provider `setZoomRange`; no-op if both zooms ≤0. |
| `createTileSchemeForId` | .cpp:124-130 | `"XYZ-WebMercator"` → XYZWebMercator else GeographicTMS. |
| `SceneTerrainRuntimeSources` / `createTerrainRuntimeSources` | .cpp:155-208 | Terrain sources struct. None ⇒ empty (ellipsoid fallback handled by the engine default); Heightmap builds `HeightmapTerrainProvider` wrapped in `HeightmapTerrainContentProvider`, and — if `config.ellipsoidFallback` — further wraps that in `CompositeTerrainProvider` alongside an `EllipsoidTerrainContentProvider` sharing the same tiling scheme (uncovered regions floor to smooth ellipsoid up to `ellipsoidFallbackMaxZoom`). |
| Unified Tileset build | .cpp:497-508 | `new Tileset(tileScheme, rasterOverlays, &renderDevice_, options, contentProvider)` → `engine_.setTileset`. |
| glTF Tileset build | .cpp:510-535 | If `config_.gltf.enabled`: `SingleGltfContentProvider` at `TileKey{schemeId,level,x,y}`, `setEastNorthUpPlacementDegrees(lon,lat,height,scale)`; empty overlay list; `engine_.addTileset`. |
| sim time | .cpp:537 | `engine_.setTime(config_.fixedSimulationJulianDate)`. |

### EarthSceneConfig.h

Declarative config consumed by `installScene`. No logic; enums + POD structs.

| Item | Lines | Description |
| --- | --- | --- |
| `enum TerrainSourceKind` | .h:14-20 | `None`, `Heightmap` (regular-grid raster-DEM heightmap terrain — the only real terrain source; quantized-mesh and Cesium ion terrain were **removed**). |
| `enum TerrainHeightmapEncoding` | .h:23-28 | `MapboxTerrainRgb`, `Terrarium` — mirrors `HeightmapTerrainProvider::Encoding`. |
| `enum ImagerySourceKind` | .h:18-26 | `Debug, Xyz, TileMapService, WebMapService, WebMapTileService, BingMaps, GoogleMapTiles`. |
| `struct SceneCameraConfig` | .h:28-32 | lon/lat degrees + heightMeters (all default 0). |
| `struct TerrainSourceConfig` | .h:49-73 | kind, urlTemplate, attribution, min/maxZoom, tileSize; ellipsoid-fallback fields `ellipsoidFallback`/`ellipsoidFallbackMaxZoom`/`ellipsoidFallbackGridSize` (.h:60-62); heightmap fields `heightmapEncoding`/`heightmapHeightFactor`/`heightmapNoDataValues`/`heightmapMaxNativeZoom` (.h:67-72). |
| `struct SceneTilesetConfig` | .h:45-48 | `mainThreadLoadingTimeLimit`, `tileCacheUnloadTimeLimit` (both 0.0 ⇒ Tileset defaults). |
| `struct RasterOverlaySourceConfig` | .h:50-98 | Largest struct: imageryKind + shared fields (zoom/SSE/opacity/role/priority/fallback, **`maximumSimultaneousTileLoads`**=20 .h:59, **`maximumScreenSpaceError`**=2.0 .h:60) plus per-provider blocks — WMS (.h:67-69), tile W/H (.h:70-71), WMTS (.h:72-79, default `wmtsSchemeId="XYZ-WebMercator"` .h:76), Bing (.h:80-84, default `bingMapStyle="Aerial"` .h:81), GoogleMapTiles (.h:85-97, default apiBaseUrl .h:85, mapType `"satellite"` .h:89). |
| `struct GltfSourceConfig` | .h:100-112 | `enabled`, `tileSchemeId="Geographic-TMS"`, tileLevel/X/Y (defaults 0/1/0), url, name, lon/lat/height, `uniformScale`=1.0. |
| `struct EarthSceneConfig` | .h:114-127 | Top-level: `initialCamera`, `terrain`, `tileset`, `vector<RasterOverlaySourceConfig> rasterOverlays`, `gltf`, `fixedSimulationJulianDate` (0.0 ⇒ keep epoch). |

---

## 19. Render Pipeline Order & Depth/Blend

### RenderCommand.h / RenderCommand.cpp

Draw order is a fixed integer per `RenderCommandKind` (RenderCommand.h:21-29), resolved by `mvpRenderOrder()` (RenderCommand.cpp:134-151). `sortMvpRenderCommands()` (`.cpp:285-290`) does a `std::stable_sort` on `mvpCommandLess` (`.cpp:91-114`): primary key = order integer, then opaque-before-translucent glTF, then translucent glTF back-to-front by `translucentSortDepth` descending. `validateMvpRenderCommands()` (`.cpp:153-283`) hard-asserts per-kind pass/depth/cull/blend. **`GlobeSurface` is gone** — the `GlobeMesh`/`Globe` fallback path was deleted; live terrain is `SurfaceTile` + glTF `GltfPrimitive`.

| RenderCommandKind | mvpRenderOrder | Lines |
|---|---|---|
| SkyBackground | **0** | .cpp:136-137 |
| SurfaceTile | **10** | .cpp:138-139 |
| GltfPrimitive / GltfPrimitiveInstanced | **15** | .cpp:140-142 |
| AtmosphereBackground | **20** | .cpp:143-144 |
| VectorOverlay | **30** | .cpp:145-146 |
| Unknown / default | **100** | .cpp:147-149 |

Note: the enum comments (RenderCommand.h:23-28) still read "order 5" for Atmosphere and "order 10"/"order 15" — but the live `mvpRenderOrder` returns Sky=0, Surface=10, Gltf=15, **Atmosphere=20** (drawn after tiles), Vector=30. The enum no longer contains a `GlobeSurface` member; the only remaining kinds are `Unknown, SkyBackground, AtmosphereBackground, SurfaceTile, GltfPrimitive, GltfPrimitiveInstanced, VectorOverlay`.

Per-kind fixed render state (defaults set in the Renderer command factories, enforced by `validateMvpRenderCommands`):

| Kind | depthTest | depthWrite | blend | cull | Enforced at |
|---|---|---|---|---|---|
| SurfaceTile | true | true | false* | true* | `.cpp:184-207`; factory Renderer.cpp:2068-2071 |
| SurfaceTile (terrain_primary overlay) | false | false | false | false | `terrainPrimaryOverlayStateAllowed` .cpp:123-130 |
| GltfPrimitive(Instanced) opaque | true | true | false | true | `.cpp:231-250`; factory Renderer.cpp:2096-2099 |
| GltfPrimitive(Instanced) BLEND | true | **false** (depthWrite==!blend) | true | true | `.cpp:235-238` |
| AtmosphereBackground | true | false | true | (cull off) | `requireState` `.cpp:273` |

*SurfaceTile may opt out of cull (skirts/two-sided) via `surfaceTileCullStateAllowed` (`.cpp:116-121`) and may blend only via `surfaceTileBlendAllowed` (`.cpp:60-71`; blends when tile/transition opacity < 0.999 or when instanced). `SkyBackground` has no explicit validation case — it falls through to `default` (`.cpp:276-278`) and only anchors the order floor.

### Renderer.h / Renderer.cpp command factories

`Renderer::initialize()` now takes **no arguments** (Renderer.h:30) and builds only the surfaceTile/placeholder/gltf/gltfInstanced/color resources (Renderer.cpp:1943-2019) — no globe buffers/shader, no `makeGlobeCommand`, no `RenderCommandKind::GlobeSurface`. `initialize()` compiles `surfaceTileShader` (GLES only; Metal uses glTF path), a 1×1 neutral `surfacePlaceholderTexture` (`.cpp:1961-1971`), `gltfShader` + `gltfInstancedShader` (both non-fatal on Metal), the shared 64×64 `tileIndexBuffer` (`.cpp:1997-2008`), and the vector `colorShader`. `dispose()` mirrors this (`.cpp:2026-2035`).

| Factory | Kind / stride | Lines |
|---|---|---|
| `makeSurfaceTileCommand` | SurfaceTile, **vertexStride=32** (POS12+NRM12+UV8), UInt32 indices, falls back to shared `tileIndexBuffer`/`tileIndexCount` when no index buffer passed | .cpp:2053-2078 |
| `makeGltfPrimitiveCommand` | GltfPrimitive, **vertexStride=120** (POS/NRM + TEXCOORD_0..7 + COLOR_0 + TANGENT), seeds full PBR uniform block | .cpp:2080-2209 |
| `makeGltfPrimitiveInstancedCommand` | GltfPrimitiveInstanced, delegates to `makeGltfPrimitiveCommand` then sets `instanceStride=kGltfInstanceMatrixStride`=**100** (mat4 64B + mat3 36B) | .cpp:2211-2230 |

`terrainShader()` is **defined** (Renderer.h:57 getter; `kTerrainVertex/FragmentGLSL` + `kTerrainVertex/FragmentMSL`, Metal buffers ≤23). The draw side is wired (2026-07-01): `GltfDrawCommandBuilder::build` (GltfDrawCommandBuilder.cpp:70-77) branches on `primitive.useTerrainVertexFormat` → `makeTerrainPrimitiveCommand` (stride 32, `shader=terrainShader`) for QM terrain, keeping the existing `terrainRenderContent`/`surfaceClipUv`/raster-overlay population; ellipsoid-fallback terrain still routes through the stride-120 glTF path.

### Reverse-Z convention

Convention: clear depth **0.0** (farthest), compare **GEQUAL** (greater depth = closer). Configured per backend:

| Backend | Setup | Lines |
|---|---|---|
| GLES (`RenderDeviceGLES.cpp`) | `glClearDepthf(0.0f)` + `glDepthFunc(GL_GEQUAL)` in per-frame clear | .cpp:367, .cpp:370 |
| GLES | `glDepthFunc(GL_GEQUAL)` in GL state init | .cpp:966 |
| Metal (`RenderDeviceMetal.mm`) | `makeDepthState`: `depthCompareFunction = enabled ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionAlways` ("Matches OpenGlobus reverseDepth:true") | .mm:77-85 |
| Metal | `passDesc.depthAttachment.clearDepth = 0.0` | .mm:502 |

Metal prebuilds three states (`depthReadWrite` / `depthReadOnly` / `depthDisabled`, .mm:70-72, built at .mm:114-116) and picks one per command from `cmd.depthTest`/`cmd.depthWrite` (.mm:540-542); `depthDisabled` uses `MTLCompareFunctionAlways`.
## 20. Cross-Subsystem Contracts

### Load-bearing ORDER contracts (enforced by call order, not by types)

| Contract | Where | Why |
|---|---|---|
| `renderer.submit(commands)` BEFORE `releaseRenderReferences()` | SceneRenderPipeline.cpp:149 then :153 (`releaseRenderReferences` body .cpp:432-439 calls `tileset->releaseRenderReferences()`) | Render commands hold **raw** `Buffer*/Texture*` plus `resourceKeepAlive` shared_ptrs (RenderCommand.h:46-54). References must survive the submit that consumes them; releasing first would free GPU resources mid-draw. |
| `processPendingLoads(...)` BEFORE `drainGpuUploadQueue(...)` | TilesetUpdateFrameRuntime.cpp:59 then :143 | `processPendingLoads` is what pushes onto `GpuUploadQueue`; the drain in the same frame pops and does the GPU upload. Reversing the order does not error — it just makes every upload lag one frame, which reads as "loading is always half a beat late" and gets misattributed to network or device. **Machine-checked**: `contracts::Id::LoadsBeforeGpuDrain` (Tileset.cpp:279). |
| Ref-count keep-alive until GPU consumption | GpuUploadQueue.h (deque of `PendingGpuUpload`); drain finalizes via `uploadToGpu` + `finishRenderResourcePreparation` | CPU-prepared vertex/index bytes and tile state must stay alive from enqueue through upload. The claim is `asyncGpuUploadPending` + a retained lifecycle upload key; three sites must release it (`TilePendingUploadCompletion::eraseUpload`) or the tile is pinned forever. Not machine-checked. |

### GpuUploadQueue FIFO

`GpuUploadQueue` (GpuUploadQueue.h) is a `std::mutex`-guarded `std::deque<PendingGpuUpload>`: `push()` (`push_back`) then `tryPop()` (`front`/`pop_front`) — strict FIFO, now machine-checked by `contracts::Id::GpuUploadQueueFifo` via two independently-maintained sequence counters (a priority reorder or a container swap trips it on the spot). `drainGpuUploadQueue` pops up to `maxUploadsPerFrame` per frame, skipping uploads whose tile was unloaded (`asyncGpuUploadPending` cleared).

⚠️ **Not worker→main.** Both `push` and `tryPop` run on the **main thread within one frame** — push inside `processPendingUploads`, pop inside the later `drainGpuUploadQueue`. Worker threads produce the vertex bytes earlier (during decode); they never touch this queue. The split buys exactly one thing: the drain-side per-frame upload cap with spillover to the next frame. The queue is also **config-gated off in the demo** — see the note at the end of §5.

### Handoff pipeline

`RasterOverlayTileProvider` → tile content load (`TileLoadResult`, `terrainRenderContent` flag TileLoadTypes.h:42) → `TileContentLifecycleManager` / `TilesetContentLifecycleCoordinator` → render-prep (`GltfRenderResourcePreparer`, `GltfRenderGeometryBuilder`) → GPU upload (`drainGpuUploadQueue` → `uploadToGpu`) → draw (`GltfDrawCommandBuilder::build` → Renderer factory → `RenderCommandList`). CPU geometry build produces `GltfGpuVertex`(120B) or `TerrainGpuVertex`(32B) into `GpuReadyData` (GpuReadyData.h:17 documents the two strides).

### IPrepareRendererResources boundary

`IPrepareRendererResources` (renderer/IPrepareRendererResources.h) is the renderer-decoupling interface (cesium-native `IPrepareRendererResources` equivalent) threaded through scene/tiling as `pPrepRenderer` (SceneTilesetCoordinator.cpp:55, SceneFrameRuntime.cpp:33, drain finalize `.h:247-250`). Renderer implements the notification hooks but **stores no imagery state**: `Renderer::attachRasterInMainThread` / `detachRasterInMainThread` are intentional no-ops (Renderer.cpp:2418-2433) — surface raster ownership lives in `RasterMappedToTilesetTile`/`SurfaceRasterBinding` (cesium-native `RasterMappedTo3DTile` equivalent). Raster attach happens on the **main thread** at upload-finalize time.

### Two-phase update/render FrameState contract

FrameState is mutated during update (tile selection, GPU upload, command build) and read-only during render/submit. `SurfaceTile`/`GltfPrimitive` commands carry `frameId`/`generation` (RenderCommand.h:41-42); `validateMvpRenderCommands(commands, expectedFrameId)` rejects any command whose `frameId != expectedFrameId` or `generation == 0` (RenderCommand.cpp:182-215, :251-258), enforcing that render only reads state produced for the current frame.

## 21. Key Constants Table

### Ellipsoid.h / .cpp

| Constant | Value | Lines |
|---|---|---|
| **`WGS84`** semi-major (x=y radius) | 6378137.0 m | .cpp:396 |
| **`WGS84`** semi-minor (z radius) | 6356752.3142451793 m | .cpp:396 |
| **`UNIT_SPHERE`** | (1.0, 1.0, 1.0) | .cpp:401 |
| **`kEpsilon1`** | 1e-1 (near-center guard, `.cpp:128`) | .cpp:10 |
| **`kEpsilon12`** | 1e-12 (Newton/geodesic convergence, `.cpp:165,177,305,370`) | .cpp:11 |
| geodesic iteration cap | 1000 | .cpp:305, .cpp:370 |

`radiiSquared_`/`oneOverRadii_`/`oneOverRadiiSquared_` precomputed in ctor (.cpp:28-32); `semiMajorAxis`=radii.x, `semiMinorAxis`=radii.z (Ellipsoid.h:30-31). Shaders hardcode the same radii (`kInvRadiiSq`, Renderer.cpp:1061-1064, MSL tile vertex).

### TileSelectionInputMetrics.h / .cpp

SSE (screen-space error) — `screenSpaceErrorForView` (.cpp:41-58): projects two clip points at the tile distance (center and center+geometricError along +Y), divides by w, and scales the NDC Δy by `viewportHeight * 0.5`:

| Item | Value / formula | Lines |
|---|---|---|
| SSE formula | `abs((errorOffsetNdc − centerNdc).y) * viewportHeightPixels * 0.5` | .cpp:51-57 |
| zero-error short-circuit | `geometricError <= 0 → 0` | .cpp:46 |
| distance floor | `max(distance, 1e-7)` | .cpp:48 |
| view aggregation | max SSE / min priority over views | .cpp:81-88 |

### FrameResourceBudget.h / .cpp

`FrameResourceLane` (.h:7-15): TerrainRequest, ContentRequest, RasterRequest, TerrainFinalize, ContentFinalize, RasterTextureUpload, TerminalState. `FrameResourcePriority` Preload=0/Normal=1/Urgent=2 (.h:18-20).

| Config field | Default | Lines |
|---|---|---|
| **`maxNetworkRequestsPerFrame`** | 20 | .h:27 |
| **`maxTerrainContentNetworkRequestsPerFrame`** | 0 (falls back to default when zero) | .h:28 |
| **`maxRasterNetworkRequestsPerFrame`** | 0 (fallback) | .h:29 |
| **`maxNetworkInflight`** | 20 | .h:32 |
| **`maxTerrainContentNetworkInflight` / `maxRasterNetworkInflight`** | 0 (fallback) | .h:33-34 |
| **`maxMainThreadFinalizesPerFrame`** | 1 | .h:35 |
| **`maxTerminalStateTransitionsPerFrame`** | 64 | .h:36 |
| **`maxRasterUploadsPerFrame`** | 1 | .h:37 |

Zero lane-specific limits fall back to the shared default; not a global sum cap (.h:24-26). Lane checks in `tryIssue`/`canIssue` (.cpp:104-111).

### GltfRenderGeometryBuilder.h — GPU vertex formats

| Struct | Size | Layout | Lines |
|---|---|---|---|
| **`GltfGpuVertex`** | **120 B** (`static_assert`) | POS + NRM + TEXCOORD_0..7 (four vec4) + COLOR_0 + TANGENT | .h:14-27 |
| **`TerrainGpuVertex`** | **32 B** (`static_assert`) | POS(12) + NRM(12) + TEXCOORD_0(8) | .h:31-39 |
| `GltfGpuInstance` | 100 B (16 model + 9 normal floats) | matches `kGltfInstanceMatrixStride`=100 (RenderCommand.h:19) | .h:41-44 |

Draw-side strides mirror these: SurfaceTile=**32** (Renderer.cpp:2241), glTF=**120** (Renderer.cpp:2269), instance=**100** (Renderer.cpp:2404). `GpuReadyData.vertexStride` documents `32 (TerrainGpuVertex) or 120 (GltfGpuVertex)` (GpuReadyData.h:17). `TerrainGpuVertex` is produced/uploaded (GltfRenderGeometryBuilder.cpp:225-229, GltfRenderResourcePreparer.cpp:492-501,622-624) **and drawn** via `terrainShader` / `makeTerrainPrimitiveCommand`. ⚠️ The former "never drawn — stride-32 draw path unwired" claim here was false; corrected 2026-08-06.

### Upload / reverse-Z constants

| Item | Value | Lines |
|---|---|---|
| **`Tileset::drainGpuUploadQueue` maxUploadsPerFrame** default | 4 | Tileset.h:161-163 |
| GLES reverse-Z **clear depth** | 0.0 (`glClearDepthf(0.0f)`) | RenderDeviceGLES.cpp:367 |
| GLES depth compare | `GL_GEQUAL` | RenderDeviceGLES.cpp:370, :967 |
| Metal reverse-Z **clear depth** | 0.0 | RenderDeviceMetal.mm:507 |
| Metal depth compare | `MTLCompareFunctionGreaterEqual` | RenderDeviceMetal.mm:82 |
| `kMaxSurfaceImageryOverlays` / `kMaxGltfRasterOverlays` | 4 / 4 | RenderCommand.h:14, :16 |
| `kGltfRasterOverlayTextureBase` | 15 | RenderCommand.h:15 |

Note: there is no existing `AI_INDEX.md` at `scaffold/src/earth_engine/` — these three sections were written to the described house style; verify against the file's actual conventions when appending. All line numbers above were read directly from current source on branch `codex/surface-instancing-gpu-batch`.
