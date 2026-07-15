#!/usr/bin/env python3
"""Codex lifecycle hooks for the earth task orchestrator."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import sys
from typing import Any


EDIT_TOOLS = {"apply_patch", "Edit", "Write"}
SHELL_TOOLS = {"Bash"}
MUTATING_SHELL = re.compile(
    r"""
    (
      \bsed\b[^\n]*(?:\s-i(?:\s|$)|--in-place)
      |\bperl\b[^\n]*\s-pi
      |\bgit\s+(?:apply|checkout|restore|reset|clean)\b
      |\bpatch\b[^\n]*(?:<|--input)
      |\b(?:clang-format|prettier)\b[^\n]*(?:\s-i(?:\s|$)|--write)
      |\b(?:python|python3)\b[^\n]*(?:write_text|write_bytes|open\s*\([^\n]*(?:['"]w|['"]a|['"]x))
      |\b(?:tee|cp|mv|rm|touch|rsync)\b[^\n]*(?:scaffold/|docs/|\.codex/|AGENTS\.md|\.gitignore)
      |\b(?:tee|cp|mv|rm|touch|rsync)\b[^\n]*\.(?:c|cc|cpp|cxx|h|hpp|java|kt|kts|gradle|toml|json|ya?ml|md|py|sh|cmake)\b
      |(?:>|>>)\s*[^\s;&|]*(?:scaffold/|docs/|\.codex/|AGENTS\.md|\.gitignore)
      |(?:>|>>)\s*[^\s;&|]*\.(?:c|cc|cpp|cxx|h|hpp|java|kt|kts|gradle|toml|json|ya?ml|md|py|sh|cmake|txt)\b
    )
    """,
    re.IGNORECASE | re.VERBOSE,
)


def read_payload() -> dict[str, Any]:
    try:
        value = json.load(sys.stdin)
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


def root_from_payload(payload: dict[str, Any]) -> Path:
    cwd = payload.get("cwd") or os.getcwd()
    current = Path(cwd).resolve()
    for candidate in (current, *current.parents):
        if (candidate / ".codex" / "skills" / "earth-task-orchestrator").exists():
            return candidate
    return current


def load_state_module(root: Path) -> Any:
    module_path = (
        root
        / ".codex"
        / "skills"
        / "earth-task-orchestrator"
        / "scripts"
        / "task_state.py"
    )
    spec = importlib.util.spec_from_file_location("earth_task_state", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load task state module from {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def emit(value: dict[str, Any]) -> None:
    print(json.dumps(value, ensure_ascii=False))


def tool_name(payload: dict[str, Any]) -> str:
    return str(payload.get("tool_name") or payload.get("tool") or "")


def tool_input(payload: dict[str, Any]) -> Any:
    return payload.get("tool_input", payload.get("input", {}))


def shell_command(value: Any) -> str:
    if isinstance(value, dict):
        command = value.get("cmd") or value.get("command")
        return str(command or "")
    return str(value or "")


def is_probable_shell_write(value: Any) -> bool:
    command = shell_command(value)
    if not command:
        return False
    if "earth-task-orchestrator/scripts/task_state.py" in command:
        return False
    return bool(MUTATING_SHELL.search(command))


def is_runtime_state_edit(value: Any) -> bool:
    serialized = json.dumps(value, ensure_ascii=False)
    paths = re.findall(r"\*\*\* (?:Add|Update|Delete) File: ([^\n]+)", serialized)
    return bool(paths) and all("/.codex/task-state/" in path for path in paths)


def summarize_tool(payload: dict[str, Any]) -> str:
    name = tool_name(payload)
    value = tool_input(payload)
    if name in EDIT_TOOLS:
        serialized = json.dumps(value, ensure_ascii=False)
        paths = re.findall(r"\*\*\* (?:Add|Update|Delete) File: ([^\n]+)", serialized)
        return f"{name}: " + (", ".join(paths[:8]) if paths else "edit")
    if isinstance(value, dict):
        command = value.get("cmd") or value.get("command")
        if command:
            return f"{name}: {str(command)[:400]}"
    return f"{name}: completed"


def main() -> int:
    payload = read_payload()
    event = str(payload.get("hook_event_name") or "")
    root = root_from_payload(payload)
    state_module = load_state_module(root)
    thread_id = state_module.resolve_thread_id(
        payload.get("session_id") or os.environ.get("CODEX_THREAD_ID")
    )
    state = state_module.load_state(root, thread_id, required=False)

    if state is None:
        return 0

    if event == "SessionStart":
        if state.get("status") in {"done", "cancelled"}:
            return 0
        emit(
            {
                "hookSpecificOutput": {
                    "hookEventName": "SessionStart",
                    "additionalContext": state_module.compact_context(state),
                }
            }
        )
        return 0

    if event == "PreToolUse":
        name = tool_name(payload)
        current_status = state.get("status")
        phase = state.get("phase")
        input_value = tool_input(payload)
        edit_attempt = name in EDIT_TOOLS and not is_runtime_state_edit(input_value)
        shell_write_attempt = (
            name in SHELL_TOOLS and is_probable_shell_write(input_value)
        )
        if edit_attempt or shell_write_attempt:
            if current_status in {"paused", "blocked"}:
                emit(
                    {
                        "hookSpecificOutput": {
                            "hookEventName": "PreToolUse",
                            "permissionDecision": "deny",
                            "permissionDecisionReason": (
                                "The earth task orchestrator is "
                                f"{current_status}; resume or cancel it before project edits."
                            ),
                        }
                    }
                )
            elif (
                current_status == "active"
                and (state.get("task_mode") == "analysis" or phase != "implement")
            ):
                emit(
                    {
                        "hookSpecificOutput": {
                            "hookEventName": "PreToolUse",
                            "permissionDecision": "deny",
                            "permissionDecisionReason": (
                                "The earth task orchestrator permits project edits only "
                                f"during implement. Current mode={state.get('task_mode')} "
                                f"phase={phase}. Update evidence and transition first."
                            ),
                        }
                    }
                )
        return 0

    if event == "PostToolUse":
        if state.get("status") != "active":
            return 0
        state_module.update_state(
            root,
            thread_id,
            lambda current: state_module.append_event(
                current,
                "tool",
                summarize_tool(payload),
            ),
        )
        return 0

    if event == "PreCompact":
        if state.get("status") != "active":
            return 0

        def record_compaction(current: dict[str, Any]) -> None:
            state_module.append_event(current, "compact", "Context compaction requested")

        state = state_module.update_state(root, thread_id, record_compaction)
        errors = state_module.validate_state(state)
        warnings = list(errors)
        if state.get("status") == "active" and not str(
            state.get("next_action", "")
        ).strip():
            warnings.append("next_action is empty")
        if warnings:
            emit(
                {
                    "systemMessage": (
                        "Earth task state needs attention before relying on compaction: "
                        + "; ".join(warnings[:6])
                    )
                }
            )
        return 0

    if event == "Stop":
        if payload.get("stop_hook_active"):
            return 0
        if state.get("status") != "active" or state.get("phase") == "done":
            return 0
        next_action = str(state.get("next_action", "")).strip()
        reason = (
            "The orchestrated task is still active "
            f"(phase={state.get('phase')}). "
        )
        if next_action:
            reason += f"Continue with: {next_action}"
        else:
            reason += (
                "Update persistent state with the current evidence and next_action, "
                "then continue or explicitly pause/block the task."
            )
        emit({"decision": "block", "reason": reason})
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
