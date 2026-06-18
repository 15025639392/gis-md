# Algorithm Alignment Test Inventory

This inventory records the current cesium-native algorithm tests that have been
ported to gis-md native tests, plus source tests that are not directly portable
until this project exposes equivalent APIs.

## Ported

| cesium-native source | gis-md test target | Covered behavior |
|---|---|---|
| `CesiumGeospatial/test/TestGlobeRectangle.cpp` | `test_rectangle` | `contains` for simple and antimeridian-wrapped rectangles; wrapped longitude span. |
| `CesiumGeometry/test/TestIntersectionTests.cpp` | `test_ellipsoid` | `rayEllipsoid` outside hits, inside hits, tangent miss, parallel miss, and outward miss cases. |
| `CesiumGeometry/test/TestPlane.cpp` | `test_plane` | Plane point-distance formula and point+normal constructor distance sign. |
| `CesiumGeometry/test/TestBoundingSphere.cpp` | `test_bounding_sphere` | Plane classification, distance squared to position, and contains boundary behavior. |
| `CesiumGeometry/test/TestOrientedBoundingBox.cpp` | `test_oriented_bounding_box` | OBB plane classification for face, edge, and corner thresholds; closest-point distance. |
| `CesiumGeospatial/include/CesiumGeospatial/GlobeTransforms.h` behavior | `test_ellipsoid` | `eastNorthUpToFixedFrame` zero-origin and polar special cases; ENU/ECEF roundtrip. |
| `CesiumGeometry/QuadtreeTilingScheme` / terrain layer geodetic root behavior | `test_tile_scheme`, `test_tms_scheme` | WebMercator edge clamping; Geographic TMS 2x1 level zero and edge clamping. |
| `Cesium3DTilesSelection/test/TestTilesetSelectionAlgorithm.cpp` | `test_sse_pipeline` | REPLACE selection fallback, ready children replacement, failed-child empty holes, failed child continuation to grandchildren, ADD parent+descendant selection, ADD failed child holes and siblings, external wrapper unconditional refinement, multi-view largest SSE, multi-frustum culling agreement, unconditionally-refined non-rendering behavior, ADD fade-out transitions, and selector no-frustum behavior. |

## Presentation-Layer Contract Tests

These tests are not direct cesium-native ports. They lock this project's
presentation contract after the cesium-native-style selector has produced a
`TilePlan`.

| gis-md test target | Covered behavior |
|---|---|
| `test_sse_pipeline` | `PresentationTrace` records deterministic camera center longitude/latitude, pitch, heading, viewport, selector views, render entries, and command summary for a fixed one-frame scene. |
| `test_sse_pipeline` | `TilePlan.renderEntries` explicitly links selected tiles to rendered tiles; clipped ancestor fallback preserves `surfaceClipUv` into the emitted `SurfaceTile` command without renderer-side LOD reselection. |

## Not Directly Portable Yet

| cesium-native source | Missing gis-md equivalent |
|---|---|
| `CesiumGeometry/test/TestAvailability.cpp` | Quadtree / octree availability bitset and subtree availability API. |
| `CesiumGeometry/test/TestAxisAlignedBox.cpp` | Dedicated AxisAlignedBox type. Current code uses OBB/sphere paths. |
| `CesiumGeometry/test/TestBoundingCylinderRegion.cpp` | Bounding cylinder region type. |
| `CesiumGeometry/test/TestClipTriangleAtAxisAlignedThreshold.cpp` | Public triangle clipping utility. Related clipping exists inside surface mesh code but is not exposed as a standalone algorithm API. |
| `CesiumGeometry/test/TestCullingVolume.cpp` | Cesium-style CullingVolume API. Current frustum tests cover camera extraction instead. |
| `CesiumGeometry/test/TestRectangle.cpp` | Cesium 2D rectangle type with `computeSignedDistance` and `computeUnion`; gis-md `Rectangle` is geospatial longitude/latitude bounds. |
| `CesiumGeometry/test/TestTransforms.cpp` | Cesium up-axis and projection matrix helpers. gis-md currently exposes ENU/ECEF transforms and camera projection behavior separately. |
| `CesiumGeospatial/test/TestBoundingRegion.cpp` | Dedicated BoundingRegion type with culling and distance APIs. |
| `CesiumGeospatial/test/TestBoundingRegionBuilder.cpp` | BoundingRegionBuilder API. |
| `CesiumGeospatial/test/TestEarthGravitationalModel1996Grid.cpp` | EGM96 geoid grid API. |
| `CesiumGeospatial/test/TestGlobeAnchor.cpp` | GlobeAnchor API. |
| `CesiumGeospatial/test/TestLocalHorizontalCoordinateSystem.cpp` | LocalHorizontalCoordinateSystem API. |
| `CesiumGeospatial/test/TestProjection.cpp` | GeographicProjection/WebMercatorProjection API and projected rectangle sizing API. |
| `CesiumGeospatial/test/TestS2CellBoundingVolume.cpp` | S2 cell bounding volume API. |
| `CesiumGeospatial/test/TestS2CellID.cpp` | S2CellID API. |
| `CesiumGeospatial/test/TestSimplePlanarEllipsoidCurve.cpp` | SimplePlanarEllipsoidCurve API. |

## Migration Rule

When any missing equivalent API is added, first port the corresponding
cesium-native `Test*.cpp` behavior into a gis-md native test target, then
implement or adjust runtime code until the ported test passes.
