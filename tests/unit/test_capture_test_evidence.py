#!/usr/bin/env python3

import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "capture-test-evidence.py"


class CaptureTestEvidenceTests(unittest.TestCase):
    def run_capture(self, output_dir, label, child_command, env_overrides=None):
        command = [
            sys.executable,
            str(SCRIPT),
            "--output-dir",
            str(output_dir),
            "--label",
            label,
        ]
        for override in env_overrides or []:
            command.extend(["--env", override])
        command.extend(["--", *child_command])
        return subprocess.run(
            command,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def read_single_metadata(self, output_dir, label):
        metadata_files = sorted(pathlib.Path(output_dir).glob(f"{label}-*.json"))
        self.assertEqual(len(metadata_files), 1)
        with metadata_files[0].open(encoding="utf-8") as f:
            metadata = json.load(f)
        log_path = pathlib.Path(metadata["log_path"])
        self.assertTrue(log_path.exists())
        return metadata, log_path.read_text(encoding="utf-8")

    def test_success_captures_output_metadata_and_expands_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_capture(
                tmp,
                "success",
                [
                    sys.executable,
                    "-c",
                    "import os; print(os.environ['ELIO_CAPTURE_TEST'])",
                ],
                ["ELIO_CAPTURE_TEST={output_dir}/native"],
            )

            self.assertEqual(result.returncode, 0)
            metadata, log = self.read_single_metadata(tmp, "success")
            expected_env = str(pathlib.Path(tmp).resolve() / "native")
            self.assertEqual(
                metadata["environment_overrides"],
                {"ELIO_CAPTURE_TEST": expected_env},
            )
            self.assertEqual(metadata["exit_code"], 0)
            self.assertEqual(metadata["return_code"], 0)
            self.assertIsNone(metadata["signal"])
            self.assertEqual(metadata["git_head"], self.current_git_head())
            self.assertIn(expected_env, log)

    def test_failure_status_and_identity_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_capture(
                tmp,
                "fail",
                [
                    sys.executable,
                    "-c",
                    "import sys; print('failure identity'); sys.exit(7)",
                ],
            )

            self.assertEqual(result.returncode, 7)
            metadata, log = self.read_single_metadata(tmp, "fail")
            self.assertEqual(metadata["exit_code"], 7)
            self.assertEqual(metadata["return_code"], 7)
            self.assertIsNone(metadata["signal"])
            self.assertIn("failure identity", log)
            self.assertIn("[capture-test-evidence] exit_code=7", log)

    def test_signal_status_uses_shell_compatible_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_capture(
                tmp,
                "signal",
                [
                    sys.executable,
                    "-c",
                    "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
                ],
            )

            expected_status = 128 + signal.SIGTERM
            self.assertEqual(result.returncode, expected_status)
            metadata, log = self.read_single_metadata(tmp, "signal")
            self.assertEqual(metadata["exit_code"], expected_status)
            self.assertEqual(metadata["return_code"], -signal.SIGTERM)
            self.assertEqual(metadata["signal"], signal.SIGTERM)
            self.assertIn(
                f"[capture-test-evidence] signal={signal.SIGTERM}",
                log,
            )

    def test_reused_label_creates_distinct_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = self.run_capture(
                tmp,
                "repeat",
                [sys.executable, "-c", "print('first failure identity')"],
            )
            second = self.run_capture(
                tmp,
                "repeat",
                [sys.executable, "-c", "print('second failure identity')"],
            )

            self.assertEqual(first.returncode, 0)
            self.assertEqual(second.returncode, 0)
            logs = sorted(pathlib.Path(tmp).glob("repeat-*.log"))
            metadata_files = sorted(pathlib.Path(tmp).glob("repeat-*.json"))
            self.assertEqual(len(logs), 2)
            self.assertEqual(len(metadata_files), 2)
            combined = "\n".join(path.read_text(encoding="utf-8") for path in logs)
            self.assertIn("first failure identity", combined)
            self.assertIn("second failure identity", combined)

    def current_git_head(self):
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=True,
        )
        return result.stdout.strip()


if __name__ == "__main__":
    unittest.main()
