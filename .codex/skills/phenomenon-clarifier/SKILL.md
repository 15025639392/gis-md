---
name: phenomenon-clarifier
description: Use when the user describes a vague, hard-to-explain, intermittent, visual, gesture, rendering, Android, map, globe, camera, tile, terrain, or bug phenomenon and the assistant is not yet able to understand or diagnose it precisely. Triggers for phrases like hard to describe, unclear, weird, strange, not sure, sometimes, occasionally, feels wrong, AI does not understand, cannot explain clearly, phenomenon, symptom, bug, issue, problem, glitch, jitter, jump, flicker, blank, crash, stuck, lag, drift, shake, wrong position, or abnormal behavior.
metadata:
  short-description: Clarify vague bugs and phenomena
---

# Phenomenon Clarifier

## Purpose

Turn a vague phenomenon into an observable evidence package before diagnosing or fixing it.

Use this skill when the user's description is ambiguous, incomplete, intermittent, visual, gesture-related, or difficult to explain. Do not guess the root cause from a weak description.

## Core Rule

Ask for the smallest next piece of evidence that would reduce uncertainty the most.

Prefer one to three short questions at a time. Do not overwhelm the user with a long form unless they ask for a template.

This is a project-local skill for `gis-md`. Optimize clarification around the native earth engine, Android MinimalGlobe, globe camera, gesture input, tile/terrain state, renderer output, and OpenGL/Metal platform evidence.

Experience and strategy come first. When clarifying a phenomenon, ask what user-facing experience is desired and which policy should govern ambiguous behavior before pushing the user into implementation details.

Performance is a first-class concern in this project. If a phenomenon involves lag, jank, slow loading, battery/thermal issues, frame drops, memory growth, tile bursts, or delayed input response, clarify it as a performance issue and ask for measurable evidence.

If the performance bottleneck is unclear, propose segmented instrumentation before proposing optimizations. The first diagnostic step should identify which stage is slow: input, camera, tile traversal, request scheduling, parsing, terrain, texture upload, render submission, GPU/platform call, or debug overlay.

## Workflow

1. Restate the phenomenon in one sentence using cautious language.
2. Identify what is missing: reproduction, scope, timing, visual evidence, logs, state values, recent changes, or expected behavior.
3. Ask one to three targeted questions.
4. If the user provides evidence paths, inspect them directly.
5. If the issue is hard to describe visually, ask for a short recording or screenshot plus the nearest log file instead of asking for more prose.
6. Once enough evidence exists, hand off to root-cause diagnosis or propose a minimal falsification experiment.

## First Questions

Choose only the most relevant questions.

For experience or strategy ambiguity:

- What should the user feel or observe when this works correctly?
- Which behavior should win if there is a tradeoff: stability, responsiveness, visual continuity, diagnostic clarity, or strict reference alignment?
- Is this a product behavior decision, a bug against an existing contract, or an unknown that needs a small experiment?

For performance issues:

- Is the symptom FPS drop, frame-time spike, input latency, slow tile loading, memory growth, network/IO burst, GPU upload stall, or thermal/battery degradation?
- Does it happen during launch, tile loading, gesture movement, camera settle, terrain decode, texture upload, or steady state?
- Is there a before/after log, FPS counter, frame-time trace, tile count, memory reading, or A/B run directory?
- If we do not know the slow stage, can we add temporary segmented timing logs around input, camera, tile traversal, scheduling, parsing, terrain, upload, render submit, and platform/GPU calls?

For visual or rendering issues:

- Can you provide a screenshot or screen recording path?
- Does it happen from space view, near-ground view, or both?
- Is it a one-frame flash, continuous flicker, wrong color, blank frame, or geometry distortion?

For gesture issues:

- Which gesture triggers it: single-finger drag, pinch, rotate, fling, or gesture transition?
- Does it happen during the gesture, on release, or after inertia begins?
- Does the touched anchor point stay under the finger, drift, or jump?
- If available, what are the gesture phase, pointer count, anchor lon/lat, camera height, heading, and pitch before and after the jump?

For Android issues:

- Is there a logcat file under `tmp/android-qa/` or `tmp/`?
- Does it reproduce on every run or only after a specific sequence?
- Did the app crash, freeze, render incorrectly, or accept input incorrectly?
- Can you capture a 5-10 second screen recording if the issue is visible only in motion?

For tile, terrain, or LOD issues:

- Is the problem a missing tile, wrong tile, seam, crack, pop, blurry texture, or wrong height?
- At what approximate zoom/camera height does it appear?
- Does it recover after waiting for loading?
- If visible in debug output, what are the zoom, rendered tile count, loading tile count, provider, and camera height?

For camera or coordinate issues:

- What changes unexpectedly: camera height, target position, heading, pitch, zoom, or picked coordinate?
- Is the error stable, drifting, oscillating, or a sudden jump?
- Does it happen near poles, antimeridian, high zoom, or near the ground?
- If a picked coordinate is involved, does screen-to-world picking fail, return the wrong location, or return no hit?

## gis-md State Snapshot

When the phenomenon involves globe motion, rendering, picking, tiles, or terrain, ask for any available subset of this state. Do not require every field.

```text
view:
- camera height:
- heading / pitch / roll:
- approximate location:
- near-ground or space-view:

gesture:
- pointer count:
- gesture phase:
- anchor screen position:
- anchor lon/lat or world position:
- before -> after camera state:

tiles:
- zoom / level:
- rendered tile count:
- loading tile count:
- missing or failed tile count:
- imagery provider:
- terrain provider:

render:
- blank / flicker / seam / crack / wrong color / wrong geometry:
- one frame or continuous:
- GL/Metal error if logged:

performance:
- FPS or frame time:
- memory:
- rendered/loading tile count:
- request count or provider errors:
- decode/upload spikes:
- input latency:
- slow segment if known:

android:
- device:
- app run id or log file:
- screenshot/video path:
```

## Evidence Capture Guidance

If the user cannot describe the issue clearly, guide them toward evidence capture:

- For Android visual/gesture issues: ask for a short screen recording and logcat from the same run.
- For intermittent jumps: ask for the shortest repeated gesture sequence and approximate frequency.
- For rendering artifacts: ask for one screenshot from the bad frame and one from a normal frame if possible.
- For crashes: ask for the first fatal stack trace, not the entire log unless needed.
- For performance or lag: ask whether FPS drops, input latency grows, or tile loading stalls.
- For A/B performance comparisons: ask for both run directories or logs and the scenario used for each run.
- For unknown performance bottlenecks: suggest temporary segmented instrumentation before changing production behavior.

Prefer existing project locations:

```text
tmp/android-qa/
tmp/
```

## Evidence Package

When useful, ask the user to provide this compact package:

```text
phenomenon:

expected:

actual:

reproduction:
1.
2.
3.

frequency:

evidence:
- log:
- screenshot/video:
- perf log / A-B run:

recent changes:

request:
先澄清现象，不要直接修。必要时设计最小证伪实验。
```

## Clarification Summary Format

Before handing off to diagnosis, summarize in this format:

```text
strategy:
- target experience / behavior:
- key tradeoff:
- reference basis:
- verification plan:
- performance signal:

observed phenomenon:

expected behavior:

actual behavior:

trigger / reproduction:

frequency:

affected subsystem:

available evidence:

missing evidence:

performance signal, if relevant:

local interaction contract, if gesture-related:

first minimal falsification experiment:
```

## Project-Specific Evidence

Prefer these evidence locations in this project:

- `tmp/android-qa/*.log`
- `tmp/*.txt`
- Android screenshots or recordings under `tmp/android-qa/`
- relevant files under `scaffold/src/earth_engine/interaction/`
- relevant files under `scaffold/src/earth_engine/camera/`
- relevant files under `scaffold/src/earth_engine/tiling/`
- relevant files under `scaffold/src/earth_engine/terrain/`
- relevant files under `scaffold/src/earth_engine/renderer/`
- Android demo files under `scaffold/examples/android/MinimalGlobe/`
- existing QA logs such as `tmp/minimalglobe-*.txt`

## Avoid

- Do not invent missing reproduction steps.
- Do not turn vague symptoms into a confident root cause.
- Do not ask for everything at once.
- Do not modify code during clarification unless the user explicitly asks for a fix.
- Do not force external reference alignment for gesture behavior; gesture behavior is project-designed unless the project later defines a reference target.

## Hand-Off Criteria

Move from clarification to diagnosis only when at least two of these are available:

- concrete reproduction steps
- log or stack trace
- screenshot or video
- affected file or subsystem
- expected vs actual behavior
- frequency or trigger pattern
- recent code change

When handing off, summarize:

- observed phenomenon
- reproduction confidence
- available evidence
- missing evidence
- first minimal falsification experiment

For gesture issues, include the project-local interaction contract being assumed or ask the user to choose one before diagnosis.
