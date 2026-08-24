#!/usr/bin/env python3

import copy
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


sys.dont_write_bytecode = True

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "tools" / "run-tcp-benchmark-conformance.py"
RUNTIMES = ("elio", "libuv", "asio")
WORKLOAD_CASES = (
    *(("latency", size) for size in (64, 1024, 4096, 65536)),
    *(("message", size) for size in (64, 1024, 4096, 65536)),
    ("bulk", 4096),
)
EXPECTED_COORDINATES = (
    *(("client-reference", runtime, "posix-reference", workload, size)
      for runtime in RUNTIMES for workload, size in WORKLOAD_CASES),
    *(("server-reference", runtime, "posix-reference-client", workload, size)
      for runtime in RUNTIMES for workload, size in WORKLOAD_CASES),
    *(("cross-runtime", client, server, "message", 1024)
      for server in RUNTIMES for client in RUNTIMES),
)
TRIAL_BY_COORDINATE = {
    coordinate: index for index, coordinate in enumerate(EXPECTED_COORDINATES, 1)
}


def load_runner():
    spec = importlib.util.spec_from_file_location("elio_tcp_conformance", RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUNNER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def counters(count, size, credit):
    return {
        "write_submissions": count,
        "write_completions": count,
        "sent_records": count,
        "received_records": count,
        "verified_records": count,
        "sent_bytes": count * size,
        "received_bytes": count * size,
        "verified_bytes": count * size,
        "max_write_operations_in_flight": 1,
        "max_unacknowledged_records": min(count, credit),
        "current_write_operations_in_flight": 0,
        "write_size_errors": 0,
        "records_per_write_errors": 0,
        "write_completion_errors": 0,
        "sequence_errors": 0,
        "payload_errors": 0,
        "transport_errors": 0,
        "phase_errors": 0,
        "error_count": 0,
    }


def result(comparison, implementation, peer, workload, size, ordinal, trial=None):
    measured = 16 if workload == "bulk" else 32
    credit = 1 if workload == "latency" else 4
    metrics = {"mib_per_second": 300.5 + ordinal}
    if workload != "bulk":
        metrics["records_per_second"] = 2000.25 + ordinal
    if workload == "latency":
        metrics["latency_ns"] = {
            "p50": 1000 + ordinal,
            "p95": 1100 + ordinal,
            "p99": 1200 + ordinal,
        }
    return {
        "schema_version": "elio.tcp-loopback.v1",
        "comparison": comparison,
        "implementation": implementation,
        "role": "server" if comparison == "server-reference" else "client",
        "peer": peer,
        "workload": workload,
        "trial": ordinal if trial is None else trial,
        "counter_scope": "driver" if comparison == "server-reference" else "adapter",
        "performance_eligible": False,
        "valid": True,
        "warmup_drained": True,
        "invariant_failures": [],
        "elapsed_ns": 1_000_000 + ordinal,
        "process_cpu_elapsed_ns": 500_000 + ordinal,
        "latency_sample_count": 32 if workload == "latency" else 0,
        "configuration": {
            "write_size_bytes": size,
            "credit_window": credit,
            "warmup_records": 8,
            "measured_records": measured,
        },
        "counters": {
            "warmup": counters(8, size, credit),
            "measured": counters(measured, size, credit),
        },
        "metrics": metrics,
    }


def server_evidence(comparison, client, server, workload, size, trial):
    measured = 16 if workload == "bulk" else 32
    records = 8 + measured
    return {
        "schema_version": "elio.tcp-loopback.v1",
        "kind": "server_connection",
        "implementation": server,
        "case_label": (
            f"{comparison}-"
            f"{server if comparison == 'server-reference' else client}-"
            f"{server}-{workload}-{size}"
        ),
        "comparison": comparison,
        "client_implementation": client,
        "server_implementation": server,
        "workload": workload,
        "trial": trial,
        "valid": True,
        "warmup_records": 8,
        "measured_records": measured,
        "write_size_bytes": size,
        "warmup_bytes": 8 * size,
        "measured_bytes": measured * size,
        "received_records": records,
        "verified_records": records,
        "write_submissions": records,
        "write_completions": records,
        "received_bytes": records * size,
        "verified_bytes": records * size,
        "submitted_bytes": records * size,
        "completed_bytes": records * size,
        "current_write_operations_in_flight": 0,
        "max_write_operations_in_flight": 1,
        "integrity_errors": 0,
        "transport_errors": 0,
        "accounting_errors": 0,
    }


def complete_fixture():
    results = []
    evidence = []
    ordinal = 1
    for runtime in RUNTIMES:
        for workload, size in WORKLOAD_CASES:
            coordinate = (
                "client-reference", runtime, "posix-reference", workload, size
            )
            trial = TRIAL_BY_COORDINATE[coordinate]
            results.append(result(
                "client-reference", runtime, "posix-reference",
                workload, size, ordinal, trial,
            ))
            evidence.append(server_evidence(
                "client-reference", runtime, "posix-reference", workload, size,
                trial,
            ))
            ordinal += 1
    for server in RUNTIMES:
        for workload, size in WORKLOAD_CASES:
            coordinate = (
                "server-reference", server, "posix-reference-client",
                workload, size,
            )
            trial = TRIAL_BY_COORDINATE[coordinate]
            results.append(result(
                "server-reference", server, "posix-reference-client",
                workload, size, ordinal, trial,
            ))
            evidence.append(server_evidence(
                "server-reference", "posix-reference-client", server,
                workload, size, trial,
            ))
            ordinal += 1
        for client in RUNTIMES:
            coordinate = ("cross-runtime", client, server, "message", 1024)
            trial = TRIAL_BY_COORDINATE[coordinate]
            results.append(result(
                "cross-runtime", client, server, "message", 1024, ordinal, trial,
            ))
            evidence.append(server_evidence(
                "cross-runtime", client, server, "message", 1024, trial,
            ))
            ordinal += 1
    manifest = {
        "schema_version": "elio.tcp-loopback.v1",
        "purpose": "conformance-only; timings are not compared",
        "performance_eligible": False,
        "tested_revision": "a" * 40,
        "git_dirty": False,
    }
    return results, evidence, manifest


class TcpBenchmarkConformanceSummaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def build(self, results=None, evidence=None, manifest=None, **kwargs):
        complete_results, complete_evidence, complete_manifest = complete_fixture()
        return self.runner.build_conformance_summary(
            complete_results if results is None else results,
            complete_evidence if evidence is None else evidence,
            complete_manifest if manifest is None else manifest,
            **kwargs,
        )

    def test_complete_summary_has_exact_counts_and_axes(self):
        summary = self.build(status="passed")

        self.assertEqual(summary["status"], "passed")
        self.assertFalse(summary["performance_eligible"])
        self.assertIsNone(summary["error"])
        self.assertEqual(summary["totals"], {
            "expected_cases": 63,
            "completed_cases": 63,
            "valid_cases": 63,
            "performance_eligible_cases": 0,
            "expected_server_evidence": 63,
            "server_evidence_cases": 63,
            "valid_server_evidence_cases": 63,
        })
        self.assertEqual(summary["fixed_work"]["warmup_records"], 8)
        self.assertEqual(summary["fixed_work"]["latency"]["measured_records"], 32)
        self.assertEqual(summary["fixed_work"]["message"]["credit_window"], 4)
        self.assertEqual(summary["fixed_work"]["bulk"]["total_bytes"], 65536)
        for axis, expected in (
            ("client-reference", 27),
            ("server-reference", 27),
            ("cross-runtime", 9),
        ):
            with self.subTest(axis=axis):
                self.assertEqual(summary["axes"][axis]["expected"], expected)
                self.assertEqual(summary["axes"][axis]["completed"], expected)
                self.assertEqual(summary["axes"][axis]["valid"], expected)

        diagnostics = summary["diagnostics"]
        workload_counts = {name: 0 for name in ("latency", "message", "bulk")}
        for axis in ("client-reference", "server-reference", "cross-runtime"):
            for row in diagnostics[axis]:
                workload_counts[row["workload"]] += 1
        self.assertEqual(workload_counts, {"latency": 24, "message": 33, "bulk": 6})

    def test_reference_and_cross_runtime_coordinates_are_complete(self):
        summary = self.build(status="passed")
        expected_reference = {
            (runtime, workload, size)
            for runtime in RUNTIMES for workload, size in WORKLOAD_CASES
        }
        for axis in ("client-reference", "server-reference"):
            actual = {
                (row["implementation"], row["workload"], row["write_size_bytes"])
                for row in summary["diagnostics"][axis]
            }
            self.assertEqual(actual, expected_reference)

        matrix = summary["cross_runtime_matrix"]
        self.assertEqual(set(matrix), set(RUNTIMES))
        for client in RUNTIMES:
            self.assertEqual(set(matrix[client]), set(RUNTIMES))
            for server in RUNTIMES:
                self.assertEqual(matrix[client][server], {
                    "expected": 1, "completed": 1, "valid": 1,
                })

    def test_passed_summary_rejects_missing_duplicate_invalid_or_eligible_rows(self):
        results, evidence, manifest = complete_fixture()
        malformed_sets = []

        duplicate = copy.deepcopy(results)
        duplicate[-1] = copy.deepcopy(duplicate[0])
        malformed_sets.append(duplicate)
        invalid = copy.deepcopy(results)
        invalid[0]["valid"] = False
        malformed_sets.append(invalid)
        eligible = copy.deepcopy(results)
        eligible[0]["performance_eligible"] = True
        malformed_sets.append(eligible)
        eligibility_missing = copy.deepcopy(results)
        del eligibility_missing[0]["performance_eligible"]
        malformed_sets.append(eligibility_missing)
        wrong_trial = copy.deepcopy(results)
        wrong_trial[0]["trial"] = 63
        malformed_sets.append(wrong_trial)

        for index, malformed in enumerate(malformed_sets):
            with self.subTest(kind=index):
                with self.assertRaises(self.runner.ConformanceError):
                    self.runner.build_conformance_summary(
                        malformed, evidence, manifest, status="passed"
                    )

    def test_passed_summary_rejects_bad_server_evidence_coordinates(self):
        results, evidence, manifest = complete_fixture()
        malformed_sets = []

        malformed_sets.append(copy.deepcopy(evidence[:-1]))
        duplicate = copy.deepcopy(evidence)
        duplicate[-1] = copy.deepcopy(duplicate[0])
        malformed_sets.append(duplicate)
        unexpected = copy.deepcopy(evidence)
        unexpected[-1].update({
            "implementation": "unexpected",
            "server_implementation": "unexpected",
            "case_label": "cross-runtime-asio-unexpected-message-1024",
        })
        malformed_sets.append(unexpected)
        for field, value in (
            ("schema_version", "wrong"),
            ("kind", "wrong"),
            ("implementation", "wrong"),
            ("workload", "wrong"),
            ("write_size_bytes", 2048),
            ("trial", 63),
        ):
            invalid = copy.deepcopy(evidence)
            invalid[0][field] = value
            malformed_sets.append(invalid)

        for index, malformed in enumerate(malformed_sets):
            with self.subTest(kind=index):
                with self.assertRaises(self.runner.ConformanceError):
                    self.runner.build_conformance_summary(
                        results, malformed, manifest, status="passed"
                    )

    def test_passed_summary_rejects_non_fixed_work(self):
        results, evidence, manifest = complete_fixture()
        for field, value in (
            ("warmup_records", 9),
            ("measured_records", 31),
            ("credit_window", 2),
        ):
            malformed = copy.deepcopy(results)
            malformed[0]["configuration"][field] = value
            with self.subTest(field=field):
                with self.assertRaises(self.runner.ConformanceError):
                    self.runner.build_conformance_summary(
                        malformed, evidence, manifest, status="passed"
                    )

    def test_passed_summary_rejects_nonfinite_or_unordered_metrics(self):
        results, evidence, manifest = complete_fixture()
        mutations = (
            ("records_per_second", float("nan")),
            ("mib_per_second", float("inf")),
            ("latency_order", None),
        )
        for field, value in mutations:
            malformed = copy.deepcopy(results)
            if field == "latency_order":
                malformed[0]["metrics"]["latency_ns"].update({
                    "p50": 2000, "p95": 1500, "p99": 1000,
                })
            else:
                malformed[0]["metrics"][field] = value
            with self.subTest(field=field):
                with self.assertRaises(self.runner.ConformanceError):
                    self.runner.build_conformance_summary(
                        malformed, evidence, manifest, status="passed"
                    )

    def test_failed_writer_uses_strict_json_and_only_existing_artifacts(self):
        results, _, manifest = complete_fixture()
        partial = copy.deepcopy(results[:1])
        partial[0]["metrics"]["latency_ns"]["p50"] = float("nan")
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            summary = self.runner.write_conformance_summary(
                output, partial, [], manifest,
                status="failed", error="invalid metric",
            )
            serialized = (output / "summary.json").read_text(encoding="utf-8")
            self.assertNotIn("NaN", serialized)
            self.assertIsNone(
                summary["diagnostics"]["client-reference"][0]
                ["latency_ns"]["p50"]
            )
            self.assertNotIn("results", summary["artifacts"])
            self.assertNotIn("server_evidence", summary["artifacts"])
            markdown = (output / "summary.md").read_text(encoding="utf-8")
            self.assertIn("No raw evidence files were produced", markdown)
            self.assertNotIn("Available raw evidence", markdown)

    def test_jsonl_reader_retains_parse_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "server-evidence.jsonl"
            path.write_text(
                '{"valid": true}\n{broken\n[1, 2]\n{"metric": NaN}\n',
                encoding="utf-8",
            )
            diagnostics = []
            objects = self.runner.read_jsonl_objects(path, diagnostics)
        self.assertEqual(objects, [{"valid": True}])
        self.assertEqual([value["line"] for value in diagnostics], [2, 3, 4])
        self.assertTrue(all(
            value["file"] == "server-evidence.jsonl" for value in diagnostics
        ))

    def test_case_artifact_parse_diagnostics_keep_file_and_line(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            (output / "case.jsonl").write_text(
                '{"valid": true}\n{broken\n', encoding="utf-8"
            )
            (output / "case.stdout.log").write_text(
                'diagnostic text\n{broken\n', encoding="utf-8"
            )
            diagnostics = self.runner.collect_case_parse_diagnostics(output)
        self.assertEqual(
            [(value["file"], value["line"]) for value in diagnostics],
            [("case.jsonl", 2), ("case.stdout.log", 2)],
        )
        with self.assertRaisesRegex(
            self.runner.ConformanceError,
            r"case\.stdout\.log:2 contains malformed JSON",
        ):
            self.runner.one_json("diagnostic text\n{broken\n", "case.stdout.log")

    def test_mid_batch_failure_persists_completed_server_evidence(self):
        first_case = (
            pathlib.Path("client"), "client-reference", "elio", "latency", 64,
        )
        second_case = (
            pathlib.Path("client"), "client-reference", "elio", "latency", 1024,
        )
        cases = [first_case, second_case]
        raw_summaries = []
        for trial, (_, comparison, implementation, workload, size) in enumerate(
            cases, 1
        ):
            value = server_evidence(
                comparison, implementation, "posix-reference", workload, size,
                trial,
            )
            for field in (
                "case_label", "comparison", "client_implementation",
                "server_implementation", "workload",
            ):
                value.pop(field)
            raw_summaries.append(value)

        holder = {}

        class FakeServer:
            def __init__(self, stream):
                self.stream = stream
                self.returncode = None

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = -15

            def wait(self, timeout=None):
                return self.returncode

            def kill(self):
                self.returncode = -9

        def popen(*args, **kwargs):
            holder["server"] = FakeServer(kwargs["stdout"])
            return holder["server"]

        calls = 0

        def run_case(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                # Connection summaries may complete in a different order from
                # client starts; the wire trial, not position, proves identity.
                for summary in reversed(raw_summaries):
                    holder["server"].stream.write(json.dumps(summary) + "\n")
                holder["server"].stream.flush()
                raise self.runner.ConformanceError("second case failed")
            return result(
                "client-reference", "elio", "posix-reference",
                "latency", 64, 1,
            )

        with tempfile.TemporaryDirectory() as directory:
            client_evidence = io.StringIO()
            persisted_server_evidence = io.StringIO()
            with mock.patch.object(
                self.runner.subprocess, "Popen", side_effect=popen
            ), mock.patch.object(
                self.runner, "wait_ready", return_value=None
            ), mock.patch.object(
                self.runner, "run_case", side_effect=run_case
            ):
                with self.assertRaisesRegex(
                    self.runner.ConformanceError, "second case failed"
                ):
                    self.runner.run_against_server(
                        pathlib.Path("server"), "posix-reference", cases,
                        pathlib.Path(directory), client_evidence,
                        persisted_server_evidence,
                    )
            persisted = [
                json.loads(line)
                for line in persisted_server_evidence.getvalue().splitlines()
            ]
        self.assertEqual(len(persisted), 2)
        self.assertEqual(
            [value["write_size_bytes"] for value in persisted], [1024, 64]
        )
        self.assertEqual(
            [value["case_label"] for value in persisted],
            [
                "client-reference-elio-posix-reference-latency-1024",
                "client-reference-elio-posix-reference-latency-64",
            ],
        )
        self.assertTrue(all(
            value["comparison"] == "client-reference" for value in persisted
        ))

    def test_failed_partial_summary_is_honest_and_deterministic(self):
        results, evidence, manifest = complete_fixture()
        results = results[:17]
        evidence = evidence[:17]
        first = self.runner.build_conformance_summary(
            results, evidence, manifest, status="failed", error="server timed out"
        )
        second = self.runner.build_conformance_summary(
            list(reversed(results)), list(reversed(evidence)), manifest,
            status="failed", error="server timed out",
        )

        first_without_timestamp = copy.deepcopy(first)
        second_without_timestamp = copy.deepcopy(second)
        first_without_timestamp.pop("generated_at_utc", None)
        second_without_timestamp.pop("generated_at_utc", None)
        self.assertEqual(first_without_timestamp, second_without_timestamp)
        self.assertEqual(first["status"], "failed")
        self.assertEqual(first["error"], "server timed out")
        self.assertEqual(first["totals"]["expected_cases"], 63)
        self.assertEqual(first["totals"]["completed_cases"], 17)
        self.assertEqual(first["totals"]["valid_cases"], 17)
        self.assertEqual(first["totals"]["performance_eligible_cases"], 0)
        self.assertEqual(first["totals"]["expected_server_evidence"], 63)
        self.assertEqual(first["totals"]["server_evidence_cases"], 17)

    def test_markdown_shows_separate_diagnostics_without_ranking_claims(self):
        summary = self.build(status="passed")
        latency_row = summary["diagnostics"]["client-reference"][0]
        self.assertEqual(latency_row["records_per_second"], 2001.25)
        self.assertEqual(latency_row["mib_per_second"], 301.5)
        markdown = self.runner.render_conformance_summary_markdown(summary)
        lowered = markdown.lower()
        ungrouped_numbers = markdown.replace(",", "")

        self.assertIn("performance_eligible=false", lowered)
        self.assertTrue(
            "no ranking" in lowered or "must not be used for ranking" in lowered
        )
        self.assertIn("### client axis", lowered)
        self.assertIn("### server axis", lowered)
        self.assertIn("cross-runtime", lowered)
        self.assertIn("latency", lowered)
        self.assertIn("workload contract", lowered)
        self.assertIn("records/s", lowered)
        self.assertIn("mib/s", lowered)
        self.assertIn(
            "| Runtime | Peer | Size (B) | p50 (ns) | p95 (ns) | p99 (ns) | "
            "Records/s | MiB/s |",
            markdown,
        )
        self.assertIn("1001", ungrouped_numbers)
        self.assertIn("2005.25", ungrouped_numbers)
        self.assertIn("309.5", ungrouped_numbers)
        for claim in (
            "| winner |", " is faster", " is slower", "speedup:", "ratio:",
        ):
            self.assertNotIn(claim, lowered)


if __name__ == "__main__":
    unittest.main()
