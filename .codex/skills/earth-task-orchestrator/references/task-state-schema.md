# Task State Schema

The JSON file under `.codex/task-state/<thread-id>.json` is the canonical state for an orchestrated task.

## Status

- `active`: continue working; the Stop Hook may prevent premature completion.
- `paused`: the user intentionally paused work.
- `blocked`: external information or state is required.
- `cancelled`: the user explicitly abandoned or replaced the task.
- `done`: every completion gate passed.

## Phases

1. `intake`: capture request and mode.
2. `model`: define goal, target experience, known facts, and unknowns.
3. `explore`: gather local, reference, test, log, and performance evidence.
4. `discriminate`: compare hypotheses with falsifiers and counterevidence.
5. `implement`: modify one coherent write scope.
6. `verify`: run required checks without repairing source.
7. `review`: synthesize behavior, regression, and performance evidence.
8. `done`: complete and stable.

Analysis tasks use `discriminate -> review -> done`.

## Important Fields

```json
{
  "task_mode": "analysis",
  "status": "active",
  "phase": "model",
  "goal": "",
  "target_experience": "",
  "known_facts": [
    {
      "claim": "",
      "evidence": "",
      "source": ""
    }
  ],
  "unknowns": [],
  "hypotheses": [
    {
      "id": "H1",
      "claim": "",
      "evidence_for": [],
      "evidence_against": [],
      "falsifier": "",
      "confidence": 0.5,
      "status": "active"
    }
  ],
  "dependency_map": [],
  "reference_evidence": [],
  "rejected_paths": [],
  "decisions": [],
  "changed_files": [],
  "risk": {
    "performance_sensitive": false,
    "visual_or_interaction": false,
    "android": false
  },
  "verification_plan": [],
  "verification": {
    "required": [],
    "completed": [],
    "outcome_confirmed": false,
    "review_status": "pending",
    "notes": []
  },
  "conclusion": "",
  "next_action": "",
  "blockers": []
}
```

Hypothesis status values are `active`, `supported`, `weakened`, and `rejected`.

## Transition Gates

### Enter `model`

- `goal` is non-empty.
- `target_experience` is non-empty.

### Enter `explore`

- The model gate passed.
- At least one known fact or unknown is recorded.

### Enter `discriminate`

- Exploration produced local, reference, or dependency evidence.
- At least two hypotheses are recorded.

### Enter `implement`

- `task_mode` is `implementation`.
- At least two hypotheses remain visible.
- Every hypothesis has a minimal falsifier.
- At least one hypothesis is `supported`.
- Every competing hypothesis is weakened or rejected with counterevidence.

### Enter `verify`

- At least one changed file is recorded.
- The required verification list is non-empty.

### Enter `review`

For implementation:

- All required verification checks are completed.
- `outcome_confirmed` is true.
- Performance, Android, or visual checks are present when their risk flags are true.

For analysis:

- `conclusion` is non-empty.
- `verification_plan` explains how to falsify or confirm the conclusion.

### Enter `done`

- `conclusion` is non-empty.
- Every required verification check is complete.
- `outcome_confirmed` is true.
- `review_status` is `passed`.
- `blockers` is empty.
- `next_action` is empty.

Returning from `verify` or `review` to `implement`, `discriminate`, `explore`,
or `model` automatically clears completed verification and outcome status.
Required checks remain, so they must be run again after rework.

## Commands

Initialize:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py init \
  --mode analysis \
  --goal "Find the root cause" \
  --target-experience "Stable globe behavior"
```

Patch nested fields:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py patch \
  --json '{"unknowns":["Which stage first diverges?"],"next_action":"Map the first divergent stage"}'
```

`phase`, `status`, identity, schema, and timestamp fields are protected. Use
`transition`, `pause`, `resume`, `block`, or `cancel` instead of patching
lifecycle fields.

When the user changes an analysis task into an implementation request, preserve
the evidence and change mode explicitly:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py set-mode \
  --mode implementation
```

If the user explicitly abandons or replaces the current goal in the same thread:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py cancel \
  --reason "Superseded by the user's new request"
```

Cancelled and completed state is archived automatically when the next
orchestrated task initializes.

Append one item:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py append \
  --field hypotheses \
  --json '{"id":"H1","claim":"...","evidence_for":[],"evidence_against":[],"falsifier":"...","confidence":0.5,"status":"active"}'
```

Validate:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py validate
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py validate --completion
```

Inspect compact recovery context:

```bash
python3 .codex/skills/earth-task-orchestrator/scripts/task_state.py context
```
