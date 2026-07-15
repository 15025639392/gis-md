#!/usr/bin/env python3
"""Persistent state manager for the gis-md task orchestrator."""

from __future__ import annotations

import argparse
import contextlib
import copy
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Callable, Iterator

try:
    import fcntl
except ImportError:  # pragma: no cover - this project currently runs on macOS.
    fcntl = None


SCHEMA_VERSION = 1
PHASES = (
    "intake",
    "model",
    "explore",
    "discriminate",
    "implement",
    "verify",
    "review",
    "done",
)
STATUSES = ("active", "paused", "blocked", "cancelled", "done")
HYPOTHESIS_STATUSES = ("active", "supported", "weakened", "rejected")
PROTECTED_PATCH_FIELDS = {
    "schema_version",
    "thread_id",
    "task_id",
    "task_mode",
    "status",
    "phase",
    "created_at",
    "updated_at",
}
TRANSITIONS = {
    "intake": {"model"},
    "model": {"intake", "explore"},
    "explore": {"model", "discriminate"},
    "discriminate": {"model", "explore", "implement", "review"},
    "implement": {"discriminate", "verify"},
    "verify": {"discriminate", "implement", "review"},
    "review": {"discriminate", "implement", "verify", "done"},
    "done": set(),
}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()


def repo_root(cwd: str | Path | None = None) -> Path:
    override = os.environ.get("EARTH_ORCHESTRATOR_ROOT")
    if override:
        return Path(override).expanduser().resolve()

    working_dir = Path(cwd or os.getcwd()).resolve()
    try:
        output = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=working_dir,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        if output:
            return Path(output).resolve()
    except (OSError, subprocess.CalledProcessError):
        pass
    return working_dir


def resolve_thread_id(explicit: str | None = None) -> str:
    raw = explicit or os.environ.get("CODEX_THREAD_ID")
    if not raw:
        raise SystemExit(
            "A task thread id is required; set CODEX_THREAD_ID or pass --thread-id"
        )
    return re.sub(r"[^A-Za-z0-9_.-]", "_", raw)


def state_directory(root: Path) -> Path:
    return root / ".codex" / "task-state"


def state_path(root: Path, thread_id: str) -> Path:
    return state_directory(root) / f"{resolve_thread_id(thread_id)}.json"


def archive_completed_state(
    root: Path,
    thread_id: str,
    state: dict[str, Any],
) -> Path:
    archive_directory = state_directory(root) / "archive"
    archive_directory.mkdir(parents=True, exist_ok=True)
    timestamp = re.sub(r"[^0-9]", "", str(state.get("updated_at", utc_now())))
    destination = archive_directory / (
        f"{resolve_thread_id(thread_id)}-{timestamp or 'completed'}.json"
    )
    counter = 1
    while destination.exists():
        destination = archive_directory / (
            f"{resolve_thread_id(thread_id)}-{timestamp or 'completed'}-{counter}.json"
        )
        counter += 1
    os.replace(state_path(root, thread_id), destination)
    return destination


def default_state(
    thread_id: str,
    mode: str,
    goal: str,
    target_experience: str,
    title: str,
) -> dict[str, Any]:
    now = utc_now()
    return {
        "schema_version": SCHEMA_VERSION,
        "thread_id": thread_id,
        "task_id": thread_id,
        "title": title or goal[:100],
        "task_mode": mode,
        "status": "active",
        "phase": "intake",
        "goal": goal,
        "target_experience": target_experience,
        "known_facts": [],
        "unknowns": [],
        "hypotheses": [],
        "dependency_map": [],
        "reference_evidence": [],
        "rejected_paths": [],
        "decisions": [],
        "changed_files": [],
        "risk": {
            "performance_sensitive": False,
            "visual_or_interaction": False,
            "android": False,
        },
        "verification_plan": [],
        "verification": {
            "required": [],
            "completed": [],
            "outcome_confirmed": False,
            "review_status": "pending",
            "notes": [],
        },
        "conclusion": "",
        "next_action": "",
        "blockers": [],
        "events": [],
        "created_at": now,
        "updated_at": now,
    }


@contextlib.contextmanager
def state_lock(root: Path, thread_id: str) -> Iterator[None]:
    directory = state_directory(root)
    directory.mkdir(parents=True, exist_ok=True)
    lock_file = directory / f".{resolve_thread_id(thread_id)}.lock"
    with lock_file.open("a+", encoding="utf-8") as handle:
        if fcntl is not None:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if fcntl is not None:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def load_state(
    root: Path,
    thread_id: str,
    *,
    required: bool = True,
) -> dict[str, Any] | None:
    path = state_path(root, thread_id)
    if not path.exists():
        if required:
            raise SystemExit(f"No orchestrator state exists at {path}")
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Cannot read orchestrator state {path}: {exc}") from exc


def atomic_write(path: Path, state: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    state["updated_at"] = utc_now()
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        suffix=".tmp",
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(state, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)


def update_state(
    root: Path,
    thread_id: str,
    updater: Callable[[dict[str, Any]], None],
) -> dict[str, Any]:
    with state_lock(root, thread_id):
        state = load_state(root, thread_id)
        assert state is not None
        updater(state)
        errors = validate_state(state)
        if errors:
            raise SystemExit("Invalid orchestrator state:\n- " + "\n- ".join(errors))
        atomic_write(state_path(root, thread_id), state)
        return state


def deep_merge(target: dict[str, Any], patch: dict[str, Any]) -> None:
    for key, value in patch.items():
        if isinstance(value, dict) and isinstance(target.get(key), dict):
            deep_merge(target[key], value)
        else:
            target[key] = copy.deepcopy(value)


def validate_hypothesis(item: Any, index: int) -> list[str]:
    if not isinstance(item, dict):
        return [f"hypotheses[{index}] must be an object"]
    errors: list[str] = []
    if not item.get("id"):
        errors.append(f"hypotheses[{index}].id is required")
    if not item.get("claim"):
        errors.append(f"hypotheses[{index}].claim is required")
    status = item.get("status", "active")
    if status not in HYPOTHESIS_STATUSES:
        errors.append(f"hypotheses[{index}].status is invalid")
    confidence = item.get("confidence", 0.5)
    if not isinstance(confidence, (int, float)) or not 0 <= confidence <= 1:
        errors.append(f"hypotheses[{index}].confidence must be between 0 and 1")
    for key in ("evidence_for", "evidence_against"):
        if not isinstance(item.get(key, []), list):
            errors.append(f"hypotheses[{index}].{key} must be a list")
    return errors


def validate_state(state: dict[str, Any], completion: bool = False) -> list[str]:
    errors: list[str] = []
    if state.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    if state.get("task_mode") not in ("analysis", "implementation"):
        errors.append("task_mode must be analysis or implementation")
    if state.get("status") not in STATUSES:
        errors.append("status is invalid")
    if state.get("phase") not in PHASES:
        errors.append("phase is invalid")

    list_fields = (
        "known_facts",
        "unknowns",
        "hypotheses",
        "dependency_map",
        "reference_evidence",
        "rejected_paths",
        "decisions",
        "changed_files",
        "verification_plan",
        "blockers",
        "events",
    )
    for field in list_fields:
        if not isinstance(state.get(field), list):
            errors.append(f"{field} must be a list")

    for index, item in enumerate(state.get("hypotheses", [])):
        errors.extend(validate_hypothesis(item, index))

    verification = state.get("verification")
    if not isinstance(verification, dict):
        errors.append("verification must be an object")
    else:
        for field in ("required", "completed", "notes"):
            if not isinstance(verification.get(field), list):
                errors.append(f"verification.{field} must be a list")
            elif field in {"required", "completed"} and not all(
                isinstance(item, str) for item in verification.get(field, [])
            ):
                errors.append(f"verification.{field} entries must be strings")
        if not isinstance(verification.get("outcome_confirmed"), bool):
            errors.append("verification.outcome_confirmed must be a boolean")
        if verification.get("review_status") not in {
            "pending",
            "passed",
            "failed",
            "inconclusive",
        }:
            errors.append("verification.review_status is invalid")

    risk = state.get("risk")
    if not isinstance(risk, dict):
        errors.append("risk must be an object")
    else:
        for field in ("performance_sensitive", "visual_or_interaction", "android"):
            if not isinstance(risk.get(field), bool):
                errors.append(f"risk.{field} must be a boolean")

    if completion or state.get("phase") == "done":
        errors.extend(completion_errors(state))
    return errors


def common_model_errors(state: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if not str(state.get("goal", "")).strip():
        errors.append("goal is required")
    if not str(state.get("target_experience", "")).strip():
        errors.append("target_experience is required")
    return errors


def hypotheses_ready(state: dict[str, Any]) -> list[str]:
    hypotheses = state.get("hypotheses", [])
    errors: list[str] = []
    if len(hypotheses) < 2:
        errors.append("at least two competing hypotheses are required")
    for item in hypotheses:
        if not str(item.get("falsifier", "")).strip():
            errors.append(f"hypothesis {item.get('id', '?')} needs a falsifier")
    return errors


def verification_errors(state: dict[str, Any]) -> list[str]:
    verification = state.get("verification", {})
    required = {str(item) for item in verification.get("required", [])}
    completed = {str(item) for item in verification.get("completed", [])}
    errors: list[str] = []
    missing = sorted(required - completed)
    if missing:
        errors.append("missing required verification: " + ", ".join(missing))
    if not verification.get("outcome_confirmed"):
        errors.append("verification.outcome_confirmed must be true")

    risk = state.get("risk", {})
    lowered = {str(item).lower() for item in completed}
    if risk.get("performance_sensitive") and not any(
        "perf" in item or "frame" in item for item in lowered
    ):
        errors.append("performance-sensitive work needs a completed performance check")
    if risk.get("android") and not any(
        "android" in item or "device" in item for item in lowered
    ):
        errors.append("Android work needs a completed Android/device check")
    if risk.get("visual_or_interaction") and not any(
        token in item
        for item in lowered
        for token in ("visual", "screenshot", "interaction", "device")
    ):
        errors.append("visual or interaction work needs a completed visual check")
    return errors


def completion_errors(state: dict[str, Any]) -> list[str]:
    errors = common_model_errors(state)
    errors.extend(verification_errors(state))
    if not str(state.get("conclusion", "")).strip():
        errors.append("conclusion is required")
    if state.get("verification", {}).get("review_status") != "passed":
        errors.append("verification.review_status must be passed")
    if state.get("blockers"):
        errors.append("blockers must be empty")
    if str(state.get("next_action", "")).strip():
        errors.append("next_action must be empty")
    return errors


def transition_errors(state: dict[str, Any], destination: str) -> list[str]:
    errors: list[str] = []
    if destination not in PHASES:
        return [f"unknown destination phase: {destination}"]
    if destination not in TRANSITIONS.get(state.get("phase"), set()):
        errors.append(f"transition {state.get('phase')} -> {destination} is not allowed")
        return errors

    gated_phases = {
        "model",
        "explore",
        "discriminate",
        "implement",
        "verify",
        "review",
        "done",
    }
    if destination in gated_phases:
        errors.extend(common_model_errors(state))

    if destination == "explore" and not (
        state.get("known_facts") or state.get("unknowns")
    ):
        errors.append("record at least one known fact or unknown before exploration")

    if destination == "discriminate":
        if not (
            state.get("known_facts")
            or state.get("dependency_map")
            or state.get("reference_evidence")
        ):
            errors.append("exploration must produce evidence before discrimination")
        errors.extend(hypotheses_ready(state))

    if destination == "implement":
        if state.get("task_mode") != "implementation":
            errors.append("analysis tasks cannot enter implementation")
        errors.extend(hypotheses_ready(state))
        hypotheses = state.get("hypotheses", [])
        if not any(item.get("status") == "supported" for item in hypotheses):
            errors.append("at least one hypothesis must be supported")
        for item in hypotheses:
            if item.get("status") == "active":
                errors.append(
                    f"hypothesis {item.get('id', '?')} is still active; "
                    "support, weaken, or reject it"
                )
            if item.get("status") in {"weakened", "rejected"} and not item.get(
                "evidence_against"
            ):
                errors.append(
                    f"hypothesis {item.get('id', '?')} needs counterevidence"
                )

    if destination == "verify":
        if not state.get("changed_files"):
            errors.append("changed_files must be recorded before verification")
        if not state.get("verification", {}).get("required"):
            errors.append("verification.required must be non-empty")

    if destination == "review":
        if state.get("task_mode") == "analysis":
            errors.extend(hypotheses_ready(state))
            if not str(state.get("conclusion", "")).strip():
                errors.append("analysis requires a conclusion before review")
            if not state.get("verification_plan"):
                errors.append("analysis requires a verification_plan")
        else:
            errors.extend(verification_errors(state))

    if destination == "done":
        errors.extend(completion_errors(state))
    return errors


def append_event(state: dict[str, Any], kind: str, summary: str) -> None:
    events = state.setdefault("events", [])
    events.append(
        {
            "time": utc_now(),
            "kind": kind,
            "summary": summary[:500],
        }
    )
    del events[:-100]


def invalidate_verification(state: dict[str, Any], reason: str) -> None:
    verification = state.setdefault("verification", {})
    had_result = bool(
        verification.get("completed")
        or verification.get("outcome_confirmed")
        or verification.get("review_status") not in {None, "pending"}
    )
    verification["completed"] = []
    verification["outcome_confirmed"] = False
    verification["review_status"] = "pending"
    verification.setdefault("notes", [])
    if had_result:
        verification["notes"].append(f"Previous verification invalidated: {reason}")


def compact_context(state: dict[str, Any]) -> str:
    hypotheses = []
    for item in state.get("hypotheses", [])[:6]:
        hypotheses.append(
            {
                "id": item.get("id"),
                "claim": item.get("claim"),
                "status": item.get("status"),
                "confidence": item.get("confidence"),
                "falsifier": item.get("falsifier"),
            }
        )

    payload = {
        "task_id": state.get("task_id"),
        "mode": state.get("task_mode"),
        "status": state.get("status"),
        "phase": state.get("phase"),
        "goal": state.get("goal"),
        "target_experience": state.get("target_experience"),
        "known_facts": state.get("known_facts", [])[-8:],
        "unknowns": state.get("unknowns", [])[-8:],
        "hypotheses": hypotheses,
        "recent_decisions": state.get("decisions", [])[-5:],
        "verification": state.get("verification"),
        "conclusion": state.get("conclusion"),
        "next_action": state.get("next_action"),
        "blockers": state.get("blockers"),
    }
    return (
        "[earth-task-orchestrator persistent state]\n"
        + json.dumps(payload, ensure_ascii=False, indent=2)
        + "\nTreat this state as canonical. Continue from next_action, preserve rejected "
        "paths, and update the state before edits, compaction, or completion."
    )


def parse_json_argument(raw: str) -> Any:
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Invalid JSON: {exc}") from exc


def patch_errors(patch: dict[str, Any]) -> list[str]:
    protected = sorted(PROTECTED_PATCH_FIELDS.intersection(patch))
    if not protected:
        return []
    return [
        "protected state fields must use lifecycle commands: "
        + ", ".join(protected)
    ]


def nested_list(state: dict[str, Any], dotted_field: str) -> list[Any]:
    current: Any = state
    parts = dotted_field.split(".")
    for part in parts[:-1]:
        if not isinstance(current, dict):
            raise SystemExit(f"{dotted_field} does not point to an object field")
        current = current.setdefault(part, {})
    if not isinstance(current, dict):
        raise SystemExit(f"{dotted_field} does not point to an object field")
    value = current.setdefault(parts[-1], [])
    if not isinstance(value, list):
        raise SystemExit(f"{dotted_field} is not a list")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--thread-id", help="Override CODEX_THREAD_ID")
    parser.add_argument("--root", help="Override repository root")
    subparsers = parser.add_subparsers(dest="command", required=True)

    init_parser = subparsers.add_parser("init")
    init_parser.add_argument(
        "--mode",
        choices=("analysis", "implementation"),
        required=True,
    )
    init_parser.add_argument("--goal", required=True)
    init_parser.add_argument("--target-experience", required=True)
    init_parser.add_argument("--title", default="")
    init_parser.add_argument("--force", action="store_true")

    patch_parser = subparsers.add_parser("patch")
    patch_parser.add_argument("--json", required=True)

    append_parser = subparsers.add_parser("append")
    append_parser.add_argument("--field", required=True)
    append_parser.add_argument("--json", required=True)

    transition_parser = subparsers.add_parser("transition")
    transition_parser.add_argument("--to", choices=PHASES, required=True)

    mode_parser = subparsers.add_parser("set-mode")
    mode_parser.add_argument(
        "--mode",
        choices=("analysis", "implementation"),
        required=True,
    )

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--completion", action="store_true")

    subparsers.add_parser("show")
    subparsers.add_parser("context")
    subparsers.add_parser("path")

    pause_parser = subparsers.add_parser("pause")
    pause_parser.add_argument("--reason", required=True)
    subparsers.add_parser("resume")

    block_parser = subparsers.add_parser("block")
    block_parser.add_argument("--reason", required=True)

    cancel_parser = subparsers.add_parser("cancel")
    cancel_parser.add_argument("--reason", required=True)

    event_parser = subparsers.add_parser("record-event")
    event_parser.add_argument("--kind", required=True)
    event_parser.add_argument("--summary", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = Path(args.root).resolve() if args.root else repo_root()
    thread_id = resolve_thread_id(args.thread_id)
    path = state_path(root, thread_id)

    if args.command == "init":
        with state_lock(root, thread_id):
            if path.exists() and not args.force:
                existing = load_state(root, thread_id)
                assert existing is not None
                if existing.get("status") in {"done", "cancelled"}:
                    archive_completed_state(root, thread_id, existing)
                else:
                    print(path)
                    return 0
            state = default_state(
                thread_id,
                args.mode,
                args.goal,
                args.target_experience,
                args.title,
            )
            atomic_write(path, state)
        print(path)
        return 0

    if args.command == "path":
        print(path)
        return 0

    if args.command == "show":
        state = load_state(root, thread_id)
        print(json.dumps(state, ensure_ascii=False, indent=2, sort_keys=True))
        return 0

    if args.command == "context":
        state = load_state(root, thread_id)
        assert state is not None
        print(compact_context(state))
        return 0

    if args.command == "validate":
        state = load_state(root, thread_id)
        assert state is not None
        errors = validate_state(state, completion=args.completion)
        if errors:
            print("\n".join(f"- {error}" for error in errors), file=sys.stderr)
            return 1
        print("ok")
        return 0

    if args.command == "patch":
        patch = parse_json_argument(args.json)
        if not isinstance(patch, dict):
            raise SystemExit("Patch JSON must be an object")
        errors = patch_errors(patch)
        if errors:
            raise SystemExit("Patch rejected:\n- " + "\n- ".join(errors))
        update_state(root, thread_id, lambda state: deep_merge(state, patch))
        print(path)
        return 0

    if args.command == "append":
        value = parse_json_argument(args.json)

        def append_value(state: dict[str, Any]) -> None:
            nested_list(state, args.field).append(value)

        update_state(root, thread_id, append_value)
        print(path)
        return 0

    if args.command == "transition":
        def apply_transition(state: dict[str, Any]) -> None:
            errors = transition_errors(state, args.to)
            if errors:
                raise SystemExit("Transition rejected:\n- " + "\n- ".join(errors))
            previous = state["phase"]
            if previous in {"verify", "review"} and args.to in {
                "model",
                "explore",
                "discriminate",
                "implement",
            }:
                invalidate_verification(
                    state,
                    f"phase {previous} -> {args.to}",
                )
            state["phase"] = args.to
            state["status"] = "done" if args.to == "done" else "active"
            state.setdefault("decisions", []).append(
                {
                    "time": utc_now(),
                    "decision": f"phase {previous} -> {args.to}",
                }
            )

        update_state(root, thread_id, apply_transition)
        print(path)
        return 0

    if args.command == "set-mode":
        def set_mode(state: dict[str, Any]) -> None:
            if state.get("phase") in {"implement", "verify", "done"}:
                raise SystemExit(
                    "Mode change rejected after implementation has started"
                )
            previous = state["task_mode"]
            state["task_mode"] = args.mode
            state.setdefault("decisions", []).append(
                {
                    "time": utc_now(),
                    "decision": f"task_mode {previous} -> {args.mode}",
                }
            )

        update_state(root, thread_id, set_mode)
        print(path)
        return 0

    if args.command == "pause":
        def pause_state(state: dict[str, Any]) -> None:
            state["status"] = "paused"
            append_event(state, "pause", args.reason)

        update_state(root, thread_id, pause_state)
        print(path)
        return 0

    if args.command == "resume":
        def resume_state(state: dict[str, Any]) -> None:
            state["status"] = "active"
            append_event(state, "resume", "Task resumed")

        update_state(root, thread_id, resume_state)
        print(path)
        return 0

    if args.command == "block":
        def block_state(state: dict[str, Any]) -> None:
            state["status"] = "blocked"
            state.setdefault("blockers", []).append(args.reason)
            append_event(state, "block", args.reason)

        update_state(root, thread_id, block_state)
        print(path)
        return 0

    if args.command == "cancel":
        def cancel_state(state: dict[str, Any]) -> None:
            state["status"] = "cancelled"
            append_event(state, "cancel", args.reason)

        update_state(root, thread_id, cancel_state)
        print(path)
        return 0

    if args.command == "record-event":
        update_state(
            root,
            thread_id,
            lambda state: append_event(state, args.kind, args.summary),
        )
        print(path)
        return 0

    raise AssertionError(f"Unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
