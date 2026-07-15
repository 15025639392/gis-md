#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import task_state


def load_hook_module():
    hook_path = (
        Path(__file__).resolve().parents[3]
        / "hooks"
        / "orchestrator_hook.py"
    )
    spec = importlib.util.spec_from_file_location("orchestrator_hook_test", hook_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TaskStateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        (self.root / ".codex").mkdir()
        self.thread_id = "test-thread"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_implementation_flow(self) -> None:
        state = task_state.default_state(
            self.thread_id,
            "implementation",
            "Fix tile instability",
            "Stable visible tile refinement",
            "",
        )
        state["known_facts"] = [{"claim": "The issue reproduces", "evidence": "log"}]

        self.assertEqual(task_state.transition_errors(state, "model"), [])
        state["phase"] = "model"
        self.assertEqual(task_state.transition_errors(state, "explore"), [])

        state["phase"] = "explore"
        state["hypotheses"] = [
            {
                "id": "H1",
                "claim": "Traversal state is stale",
                "evidence_for": ["trace"],
                "evidence_against": [],
                "falsifier": "Refresh state before traversal",
                "confidence": 0.7,
                "status": "supported",
            },
            {
                "id": "H2",
                "claim": "Provider latency causes the symptom",
                "evidence_for": [],
                "evidence_against": ["The issue reproduces with cached data"],
                "falsifier": "Run with an in-memory provider",
                "confidence": 0.2,
                "status": "rejected",
            },
        ]
        self.assertEqual(task_state.transition_errors(state, "discriminate"), [])

        state["phase"] = "discriminate"
        self.assertEqual(task_state.transition_errors(state, "implement"), [])
        state["phase"] = "implement"
        state["changed_files"] = ["scaffold/src/earth_engine/tiling/example.cpp"]
        state["verification"]["required"] = ["focused-tests", "diff-review"]
        self.assertEqual(task_state.transition_errors(state, "verify"), [])

        state["phase"] = "verify"
        state["verification"]["completed"] = ["focused-tests", "diff-review"]
        state["verification"]["outcome_confirmed"] = True
        self.assertEqual(task_state.transition_errors(state, "review"), [])

        state["phase"] = "review"
        state["verification"]["review_status"] = "passed"
        state["conclusion"] = "Traversal now uses current frame state."
        self.assertEqual(task_state.transition_errors(state, "done"), [])

    def test_edit_gate_prerequisites_are_enforced(self) -> None:
        state = task_state.default_state(
            self.thread_id,
            "implementation",
            "Fix a bug",
            "Correct behavior",
            "",
        )
        state["phase"] = "discriminate"
        errors = task_state.transition_errors(state, "implement")
        self.assertTrue(any("two competing hypotheses" in item for item in errors))
        self.assertTrue(any("supported" in item for item in errors))

    def test_analysis_completion_requires_verification_plan(self) -> None:
        state = task_state.default_state(
            self.thread_id,
            "analysis",
            "Explain the regression",
            "A falsifiable root-cause model",
            "",
        )
        state["phase"] = "discriminate"
        state["known_facts"] = [{"claim": "Observed", "evidence": "test"}]
        state["hypotheses"] = [
            {
                "id": "H1",
                "claim": "Cause one",
                "evidence_for": [],
                "evidence_against": [],
                "falsifier": "Experiment one",
                "confidence": 0.5,
                "status": "supported",
            },
            {
                "id": "H2",
                "claim": "Cause two",
                "evidence_for": [],
                "evidence_against": ["Counterexample"],
                "falsifier": "Experiment two",
                "confidence": 0.3,
                "status": "weakened",
            },
        ]
        state["conclusion"] = "Cause one is currently best supported."
        errors = task_state.transition_errors(state, "review")
        self.assertIn("analysis requires a verification_plan", errors)

    def test_patch_cannot_bypass_lifecycle(self) -> None:
        errors = task_state.patch_errors(
            {
                "phase": "implement",
                "status": "done",
                "task_mode": "implementation",
                "next_action": "Allowed field",
            }
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("phase", errors[0])
        self.assertIn("status", errors[0])
        self.assertIn("task_mode", errors[0])

    def test_invalid_verification_entries_are_rejected(self) -> None:
        state = task_state.default_state(
            self.thread_id,
            "analysis",
            "Analyze behavior",
            "Falsifiable conclusion",
            "",
        )
        state["verification"]["required"] = [{"name": "not-a-string"}]
        errors = task_state.validate_state(state)
        self.assertIn("verification.required entries must be strings", errors)

    def test_verification_is_invalidated_before_rework(self) -> None:
        state = task_state.default_state(
            self.thread_id,
            "implementation",
            "Repair behavior",
            "Verified result",
            "",
        )
        state["verification"]["required"] = ["focused-tests"]
        state["verification"]["completed"] = ["focused-tests"]
        state["verification"]["outcome_confirmed"] = True
        state["verification"]["review_status"] = "failed"

        task_state.invalidate_verification(state, "phase verify -> implement")

        self.assertEqual(state["verification"]["completed"], [])
        self.assertFalse(state["verification"]["outcome_confirmed"])
        self.assertEqual(state["verification"]["review_status"], "pending")
        self.assertIn(
            "Previous verification invalidated",
            state["verification"]["notes"][-1],
        )

    def test_thread_id_has_no_shared_default(self) -> None:
        with mock.patch.dict(os.environ, {"CODEX_THREAD_ID": ""}):
            with self.assertRaises(SystemExit):
                task_state.resolve_thread_id("")

    def test_shell_source_writes_are_detected(self) -> None:
        hook = load_hook_module()
        self.assertTrue(
            hook.is_probable_shell_write(
                {"command": "sed -i '' 's/a/b/' scaffold/src/example.cpp"}
            )
        )
        self.assertTrue(
            hook.is_probable_shell_write(
                {"command": "printf x > .codex/config.toml"}
            )
        )
        self.assertTrue(
            hook.is_probable_shell_write(
                {"command": "cp /tmp/generated.cpp example.cpp"}
            )
        )
        self.assertFalse(
            hook.is_probable_shell_write(
                {"command": "cd scaffold && ./test_native.sh test_tiles"}
            )
        )
        self.assertFalse(
            hook.is_probable_shell_write(
                {
                    "command": (
                        "python3 .codex/skills/earth-task-orchestrator/"
                        "scripts/task_state.py context"
                    )
                }
            )
        )


if __name__ == "__main__":
    unittest.main()
