---
name: earth-globe-visual-polish
description: Use when improving visual quality, UI polish, globe appearance, map rendering aesthetics, Android MinimalGlobe presentation, debug overlay readability, sky/atmosphere/lighting, tile loading visuals, terrain appearance, or interaction feedback in the gis-md earth engine project.
metadata:
  short-description: Polish gis-md globe visuals
---

# Earth Globe Visual Polish

## Project Rule

This project uses `/Users/ldy/Desktop/work/cesium-native` as the primary algorithm reference and `/Users/ldy/Desktop/work/openglobus` as an additional globe behavior and presentation reference.

For any change involving algorithms for map, globe, coordinates, tiles, terrain, camera, picking, rendering, LOD, providers, atmosphere, sky, overlays, culling, projections, ellipsoids, or quantized-mesh:

1. Read `/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md`.
2. Locate and read the relevant cesium-native C++ source files.
3. Preserve cesium-native algorithm structure, state semantics, input units, numeric tolerances, and boundary behavior unless impossible.
4. If OpenGlobus covers interaction or presentation behavior that cesium-native does not, read `/Users/ldy/Desktop/work/openglobus/AI_INDEX.md` and the relevant OpenGlobus source.
5. If local behavior intentionally differs from cesium-native or OpenGlobus, document the difference in code or tests.

Reference priority:

1. cesium-native: algorithms, numeric behavior, geospatial math, tile selection, culling, quantized-mesh, raster overlays, SSE, bounding volumes.
2. OpenGlobus: globe interaction, visual presentation, WebGL rendering organization, UI controls, behavior not covered by cesium-native.
3. Local gesture design: touch, drag, pinch, rotate, inertia, anchors, and near-ground gesture constraints are currently project-designed behavior, not forced external alignment.
4. Local project docs/tests: only when they do not conflict with the higher-priority references or the explicit gesture design.

Experience and strategy come first. Before choosing an implementation, make the intended user experience and product strategy explicit: what should feel stable, readable, responsive, diagnostic, or visually trustworthy. Algorithm alignment supports that strategy; do not cite a reference implementation as a substitute for deciding the desired gis-md experience.

Performance is a first-class constraint. Visual polish must consider FPS, frame time, memory, GPU upload cost, tile request pressure, provider IO, main-thread blocking, and Android battery/thermal impact. Prefer measurable before/after evidence for performance-sensitive changes.

When the performance bottleneck is unclear, add temporary segmented instrumentation before optimizing. Measure stage timings and counts around input processing, camera update, tile traversal, request scheduling, data parsing, terrain processing, texture upload, render command submission, and platform/GPU calls. Optimize only after the slow segment is identified.

## Gesture System Exception

The gesture system currently has no suitable external reference target.

For changes involving touch input, drag, pinch zoom, rotation, inertia, gesture anchors, or near-ground gesture constraints:

- Do not force alignment to cesium-native or OpenGlobus.
- Define the desired interaction feel and policy first: stable anchor, predictable zoom, bounded near-ground behavior, clear release/inertia behavior, and recoverable edge cases.
- Define the intended interaction model before changing code.
- Specify state variable semantics, input units, coordinate spaces, anchor behavior, and boundary conditions.
- Add or update focused tests for the local interaction contract when practical.
- Validate on Android device when behavior affects MinimalGlobe touch interaction.

Relevant local files usually include:

- `scaffold/src/earth_engine/interaction/InputManager.*`
- `scaffold/src/earth_engine/camera/CameraController.*`
- `scaffold/examples/android/MinimalGlobe/GLESView.*`
- `scaffold/tests/unit/interaction/*`
- `scaffold/tests/unit/camera/*`

## Visual Direction

Target a professional GIS/globe engine demo:

- clean, cinematic globe
- natural sky and lighting
- readable terrain and imagery
- smooth tile refinement without hiding LOD bugs
- compact, legible debug UI
- diagnostic overlays that support engine work

Do not apply generic web landing-page aesthetics to this project.

## Required Workflow

1. Identify the affected visual area: atmosphere, tile imagery, terrain, debug overlay, Android shell, gesture feedback, interaction feedback, or renderer output.
2. If the change touches algorithmic behavior, use cesium-native `AI_INDEX.md` first, then read the specific source files.
3. If the change touches globe interaction or presentation behavior not covered by cesium-native, use OpenGlobus `AI_INDEX.md`, then read the specific source files.
4. Inspect the corresponding local files under `scaffold/src/earth_engine` or `scaffold/examples`.
5. Make the smallest visual change that improves the target area without changing unrelated engine behavior.
6. Verify with the strongest practical check: unit tests, local build, Android device run, screenshots, or logs.
7. Before final response, inspect visual output when the change affects rendering or Android presentation.
8. For performance-sensitive visual changes, compare before/after frame time, FPS, tile counts, memory, request volume, or logs when practical.
9. If performance impact is unclear, add segmented timing/count logs first, then use the logs to choose the smallest optimization.

For additional aesthetic targets, read `references/visual_targets.md`.

## Atmosphere, Sky, And Lighting

When changing sky, atmosphere, sun direction, or background:

- Prefer physically plausible color transitions.
- Check horizon, limb, space-view, and near-ground views when possible.
- Avoid saturated fantasy gradients.
- Avoid pure black backgrounds except for explicit diagnostics.
- Keep time-of-day and sun state semantics clear.

Relevant local files usually include:

- `scaffold/src/earth_engine/environment/AtmosphereBackgroundPass.*`
- `scaffold/src/earth_engine/environment/SkyGradient.*`
- `scaffold/src/earth_engine/environment/SkyBox.*`
- `scaffold/src/earth_engine/environment/SunDirection.*`
- `scaffold/src/earth_engine/environment/TimeController.*`

## Tiles, Imagery, And Terrain

When changing tile or terrain visuals:

- Do not mask cracks, missing tiles, LOD popping, or provider failures with purely cosmetic cover-ups.
- Preserve tile state transitions and provider semantics.
- Keep debug imagery useful for diagnosing tile boundaries, zoom levels, missing textures, and terrain gaps.
- Prefer cesium-native loading, refinement, SSE, culling, and quantized-mesh behavior when a behavior comparison exists.
- Use OpenGlobus for presentation or interaction details not represented in cesium-native.
- Watch performance pressure: tile traversal count, request bursts, texture cache churn, GPU uploads, terrain decode cost, and frame-time spikes.

Relevant local files usually include:

- `scaffold/src/earth_engine/tiling/*`
- `scaffold/src/earth_engine/terrain/*`
- `scaffold/src/earth_engine/providers/*`
- `scaffold/src/earth_engine/renderer/*`

## Debug Overlay

Debug overlays should be compact, legible, and non-invasive.

Rules:

- Use high contrast text.
- Avoid covering the globe center.
- Keep metrics aligned and scannable.
- Use consistent spacing.
- Do not add decorative panels.
- Make important state visible when relevant: zoom, tile count, camera height, provider state, frame state, and errors.

Relevant local files:

- `scaffold/src/earth_engine/debug/DebugOverlay.*`
- `scaffold/src/earth_engine/style/OverlayStyle.*`

## Android MinimalGlobe

For Android demo visual changes:

- Use the `run-android-device` skill when deploying, launching, or validating on the connected Android phone.
- Capture logs and screenshots when practical.
- Check gesture responsiveness after visual changes.
- Check both near-ground and high-zoom views when relevant.

Relevant local files:

- `scaffold/examples/android/MinimalGlobe/*`
- `scaffold/build_and_install.sh`

## Forbidden Defaults

Do not introduce:

- web landing page aesthetics
- purple or blue gradient blob backgrounds
- excessive rounded cards
- decorative UI panels
- fake placeholder controls
- one-color visual themes
- unreadable debug text
- visual changes that hide tile, terrain, LOD, provider, or picking problems

## Verification

Before final response, report:

- strategy used: target experience/behavior, key tradeoff, reference basis, and why this approach was chosen
- what visual area changed
- which cesium-native source area was checked for algorithmic behavior
- which OpenGlobus source area was checked, if relevant
- what verification was run
- whether screenshots, logs, or Android device output were inspected
- any intentional difference from cesium-native or OpenGlobus
- for gesture changes, the local interaction contract that was designed or preserved
- for performance-sensitive changes, the metric or observation used to judge performance impact
