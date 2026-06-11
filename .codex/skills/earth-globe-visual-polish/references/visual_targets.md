# Visual Targets

## Good

- The globe reads clearly at first glance.
- The earth limb and horizon have natural depth.
- Near-ground views are not overexposed, crushed, or visually noisy.
- Tile loading remains diagnosable.
- Terrain and imagery retain useful detail.
- Debug text is readable without dominating the scene.
- Android MinimalGlobe looks like a serious globe engine demo.

## Bad

- Web product homepage styling.
- Decorative gradient backgrounds.
- Large card panels covering the globe.
- UI polish that hides tile, terrain, LOD, provider, or picking defects.
- Placeholder-looking controls or empty panels.
- Claims of visual improvement without screenshot, log, build, test, or device verification.

## Review Questions

- Does the change improve the globe itself, or only decorate the shell around it?
- Can tile and terrain failures still be seen during diagnosis?
- Does the first Android screenshot look intentional?
- Did the implementation preserve cesium-native algorithm behavior where relevant?
- Did it use OpenGlobus only for interaction or presentation behavior not covered by cesium-native?
- For gesture behavior, was the project-local interaction contract stated and tested instead of pretending an external reference exists?
