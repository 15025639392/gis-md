# Verification Contract

Select checks from the affected behavior. Do not use a broad build as a substitute for a targeted behavioral test.

## Native Algorithm Or Behavior

- Run the focused native target with `cd scaffold && ./test_native.sh <test_target>`.
- Add or adapt cesium-native cases when algorithm behavior is aligned.
- Record input, expected output, boundary conditions, state semantics, and tolerance.
- Run a relevant regression filter or the full native suite when shared behavior changed.

## Rendering And Visual Behavior

- Inspect the rendered output, screenshot, or recording.
- Check space view, horizon, near-ground view, and loading transitions when applicable.
- Confirm the change does not hide missing tiles, LOD transitions, terrain gaps, provider failures, or picking errors.
- Record whether GPU uploads, render submission, or frame-time spikes changed.

## Android And Gestures

- Use the `run-android-device` skill for physical-device deployment.
- Verify expected gesture state, anchor behavior, transition, release, and boundary handling.
- Record the local interaction contract.
- Inspect logcat and visible output from the same run.

## Performance

- Define a repeatable scenario and before/after conditions.
- Measure the narrowest relevant signals: FPS, frame time, stage timing, memory, network/IO, request count, decode work, GPU upload, main-thread blocking, power, or thermal behavior.
- When the slow stage is unknown, instrument input, camera, traversal, scheduling, parsing, terrain, upload, render submit, and platform/GPU calls before optimizing.
- Prefer removing wasted work over lowering visible quality.

## Independent Review

The verifier must:

- read the task goal, target experience, hypotheses, and decision log
- inspect the diff without assuming the supported hypothesis is correct
- run or inspect every required verification check
- look for behavior regressions, missing tests, and unsupported conclusions
- report pass, fail, or inconclusive
- avoid repairing the implementation

On failure, return the task to `implement` or `discriminate`.

## Completion Evidence

Record completed checks using stable names, for example:

```json
{
  "verification": {
    "required": [
      "focused-native-tests",
      "tile-regression-tests",
      "android-device",
      "performance-comparison",
      "independent-diff-review"
    ],
    "completed": [
      "focused-native-tests",
      "tile-regression-tests",
      "android-device",
      "performance-comparison",
      "independent-diff-review"
    ],
    "outcome_confirmed": true,
    "review_status": "passed",
    "notes": [
      "Target behavior reproduced before and no longer reproduces after."
    ]
  }
}
```
