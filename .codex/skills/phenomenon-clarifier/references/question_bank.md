# Question Bank

Use these only as needed. Ask one to three questions at a time.

## Minimal Universal Questions

- What did you expect to happen?
- What actually happened?
- Can you provide a log, screenshot, or screen recording path?

## Intermittent Problems

- Roughly how often does it happen?
- What is the shortest sequence that seems to trigger it?
- Does restarting the app reset the problem?

## Regression Problems

- When was the last known good state?
- Which files changed recently?
- Is there a commit, diff, or branch where it started?

## Visual Problems

- Is the issue visible in a still screenshot, or only in motion?
- Does it affect the whole frame or a specific area?
- Does waiting for tile loading change the result?

## Gesture Problems

- Which finger count and gesture phase triggers it?
- Does the camera jump, drift, clamp, rotate, or zoom incorrectly?
- Is the anchor under the finger preserved?
- What are camera height, heading, pitch, and anchor position before and after the issue?
- Does the issue happen when changing from pinch to drag, drag to pinch, or releasing fingers?
- Does near-ground clamping change the behavior?

## Android Problems

- Is there a logcat file?
- Did the app crash, freeze, or keep running with wrong output?
- Does it reproduce after reinstalling the app?
- Can you capture a short screen recording from the same run as the logcat?
- Is the issue visible immediately after launch or only after gestures/tile loading?

## gis-md Camera / Globe State

- What approximate camera height is involved?
- Is the view near the ground, mid altitude, or space view?
- Does heading, pitch, target, or height change unexpectedly?
- Does picking return no hit, a wrong hit, or a delayed hit?

## gis-md Tiles / Terrain

- Is the artifact a seam, crack, missing tile, blurry tile, wrong tile, height spike, or flat terrain?
- Does waiting for loading fix it?
- Does it appear at a specific zoom or provider?
- Are there provider errors in the log?

## gis-md Performance

- Is the main symptom low FPS, jank, input latency, slow loading, memory growth, or thermal/battery impact?
- Does it happen during launch, gesture, tile loading, terrain decode, texture upload, or steady state?
- Do we have before/after logs or an A/B run directory?
- What changed in rendered tile count, loading tile count, request count, memory, or frame time?
- Is the issue visible on Android device only, or also in desktop/macOS examples?
- If the bottleneck is unknown, which stage should we instrument first: input, camera, tile traversal, request scheduling, parsing, terrain, texture upload, render submission, platform/GPU calls, or debug overlay?
