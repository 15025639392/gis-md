---
name: earth-task-orchestrator
description: Coordinate persistent, evidence-driven work in the gis-md repository. Use for continuous multi-turn execution, complex bug root-cause diagnosis, large-codebase understanding, cross-module implementation, performance investigations, globe/tiles/terrain/camera/rendering work, or tasks likely to require competing hypotheses, reference alignment, subagents, and independent verification. Do not use for simple questions or obvious isolated edits.
---

# Earth Task Orchestrator

Maintain engineering investigation state outside the conversation, delegate independent evidence gathering, enforce phase gates before source edits, and verify outcomes before completion.

## Start

1. Classify the task as `analysis` or `implementation`.
2. Define the goal and target experience before running exploratory commands.
3. Initialize persistent state:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py init \
  --mode implementation \
  --goal "<observable outcome>" \
  --target-experience "<expected user or engine behavior>"
```

4. Read `references/task-state-schema.md` before the first transition.
5. Read `references/verification-contract.md` before implementation or completion.
6. Keep the normal task plan synchronized with the persistent state. The state file is the source of truth for facts, hypotheses, decisions, verification, and the next action.

## State Machine

Use these phases:

```text
intake -> model -> explore -> discriminate
discriminate -> implement -> verify -> review -> done
discriminate -> review -> done
```

Use the shorter path only for analysis tasks. Move backward when evidence invalidates the current model. Never skip directly to `implement`.

Transition with:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py transition --to explore
```

The command rejects transitions whose evidence gates are incomplete.

## Investigation Contract

Before diagnosis:

- Define the problem, target experience, known facts, and unknowns.
- Preserve at least two competing hypotheses while the root cause is uncertain.
- Record support, counterevidence, a minimal falsifier, confidence, and status for each hypothesis.
- Treat logs, tests, and reference implementations as evidence, not conclusions.
- Record rejected paths so later turns do not repeat them.
- Keep raw logs and broad exploration output out of the main thread; store distilled evidence with source paths.

Patch state with JSON:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py patch \
  --json '{"next_action":"Read the tile selection call chain"}'
```

Append structured evidence:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py append \
  --field known_facts \
  --json '{"claim":"...","evidence":"...","source":"path:line"}'
```

## Project Routing

For map, coordinate, tile, terrain, camera, picking, rendering, LOD, provider, projection, ellipsoid, quantized-mesh, SSE, bounding-volume, or globe algorithms:

1. Delegate local call-chain mapping to `earth-cartographer`.
2. Delegate cesium-native source and test alignment to `cesium-aligner`.
3. Read `/Users/ldy/Desktop/work/cesium-native/AI_INDEX.md` before relevant source and `test/Test*.cpp`.

For interaction, presentation, WebGL organization, UI behavior, or behavior not covered by cesium-native:

1. Delegate to `earth-behavior-analyst`.
2. Use `/Users/ldy/Desktop/work/openglobus/AI_INDEX.md` only where cesium-native does not define the behavior.
3. Treat gesture behavior as a local interaction contract unless the project later selects an external reference.

For uncertain or high-impact conclusions, delegate the current state and strongest hypothesis to `earth-skeptic`.

For implementation tasks, delegate independent read-heavy work in parallel when useful. Keep the main agent responsible for synthesis and decisions. Use one writer only; use the built-in `worker` or edit locally after entering `implement`.

This skill explicitly authorizes subagents for bounded, independent exploration, reference alignment, counteranalysis, and verification.

## Context Discipline

- Keep the main thread focused on requirements, state transitions, decisions, and final results.
- Require subagents to return facts, source locations, uncertainties, and recommended falsifiers instead of raw command output.
- Update the state after evidence changes, before compaction, before source edits, after tests, and before the final response.
- Recover the current state at any time with:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py context
```

- Pause or block explicitly instead of pretending completion:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py pause --reason "<user requested pause>"
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py block --reason "<external blocker>"
```

When the user explicitly abandons or replaces the goal in the same thread, use
`task_state.py cancel --reason "<why>"`. Do not cancel merely to bypass a phase
gate. Start the replacement task with `init`; cancelled state is archived.

## Edit Gate

Do not modify project source before entering `implement`.

Before entering `implement`:

- Preserve at least two hypotheses.
- Give every hypothesis a falsifier.
- Mark at least one hypothesis `supported`.
- Weaken or reject competing hypotheses with counterevidence.
- State the intended change and why it tests or resolves the supported cause.

The project Hook denies `apply_patch`, `Edit`, `Write`, and high-confidence
shell source edits while an orchestrated task is outside `implement`, paused,
or blocked. If verification fails, transition back to `implement` before
editing again.

If the user changes an analysis request into an implementation request, use
`task_state.py set-mode --mode implementation`; do not patch `task_mode`.

## Verification Gate

Before entering `verify`, record changed files and required verification.

Verification must cover the strongest applicable checks:

- focused native tests
- regression tests
- build or static validation
- Android device behavior
- screenshots or logs for visual behavior
- FPS, frame time, memory, IO, request, upload, or power evidence for performance-sensitive changes
- independent diff and behavior review

Delegate final verification to `earth-verifier` when the task has meaningful risk. The verifier must not repair the implementation.

Mark completed checks and outcome:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py patch \
  --json '{"verification":{"required":["focused-tests","diff-review"],"completed":["focused-tests","diff-review"],"outcome_confirmed":true,"review_status":"passed"}}'
```

Transition to `done` only after all required checks pass, blockers are empty, and the conclusion records why the result satisfies the target experience.

## Final Response

Report:

- target experience or behavior
- root cause or supported conclusion
- key competing hypothesis and why it lost
- implementation strategy and tradeoff
- reference basis
- verification performed
- performance impact judgment
- remaining uncertainty

For gesture changes, also report the local interaction contract.
