#!/usr/bin/env python3
"""Validate both directions of the TCP runtime benchmark matrix.

This is a fixed-work protocol and accounting smoke test. It deliberately does
not compare timing values or publish a performance ranking.
"""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import time
from typing import Any, TextIO


SCHEMA = "elio.tcp-loopback.v1"
SUMMARY_SCHEMA = "elio.tcp-loopback-conformance-summary.v1"
HOST = "127.0.0.1"
RUNTIMES = ("elio", "libuv", "asio")
MESSAGE_SIZES = (64, 1024, 4096, 65536)
RECORDS = 32
WARMUP_RECORDS = 8
CREDIT_WINDOW = 4
BULK_BYTES = 64 * 1024
CHUNK_BYTES = 4 * 1024
TIMEOUT_SECONDS = 30
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

DIAGNOSTIC_WARNING = (
    "performance_eligible=false: timings from this hosted/shared runner are "
    "diagnostic observations only; they must not be used for rankings, winner "
    "claims, ratios, or percentage-level performance conclusions."
)


class ConformanceError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate TCP benchmark protocol/accounting in both directions"
    )
    parser.add_argument("--reference-server", required=True, type=Path)
    parser.add_argument("--reference-client", required=True, type=Path)
    for runtime in RUNTIMES:
        parser.add_argument(f"--{runtime}-client", required=True, type=Path)
        parser.add_argument(f"--{runtime}-server", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args()


def executable(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ConformanceError(f"{label} is not an executable file: {path}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tested_revision() -> str | None:
    completed = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "HEAD"],
        text=True, capture_output=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else None


def git_dirty_state() -> bool | None:
    inside = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "--is-inside-work-tree"],
        text=True, capture_output=True, check=False,
    )
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        return None
    status = subprocess.run(
        # Build and artifact directories may intentionally live below the
        # checkout. Only tracked-source changes make the tested revision dirty.
        ["git", "-C", str(REPOSITORY_ROOT), "status", "--porcelain",
         "--untracked-files=no"],
        text=True, capture_output=True, check=False,
    )
    if status.returncode != 0:
        return None
    return bool(status.stdout)


def validate_manifest(manifest: dict[str, Any]) -> None:
    dirty = manifest.get("git_dirty")
    if dirty is not None and type(dirty) is not bool:
        raise ConformanceError("manifest git_dirty must be boolean or null")
    if manifest.get("performance_eligible") is not False:
        raise ConformanceError(
            "conformance manifest must declare performance_eligible=false"
        )
    executables = manifest.get("executables")
    if not isinstance(executables, dict) or not executables:
        raise ConformanceError("manifest must identify the tested executables")


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((HOST, 0))
        return int(probe.getsockname()[1])


def wait_ready(server: subprocess.Popen[str], port: int, label: str) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise ConformanceError(
                f"{label} exited before readiness (status {server.returncode})"
            )
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise ConformanceError(f"{label} did not accept loopback connections")


def stop_server(server: subprocess.Popen[str]) -> None:
    if server.poll() is not None:
        return
    server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=3)


def one_json(stdout: str, label: str) -> dict[str, Any]:
    objects: list[dict[str, Any]] = []
    for line_number, line in enumerate(stdout.splitlines(), 1):
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line, parse_constant=_reject_nonfinite_json)
        except (json.JSONDecodeError, ValueError) as error:
            raise ConformanceError(
                f"{label}:{line_number} contains malformed JSON: {error}"
            ) from error
        if isinstance(value, dict):
            objects.append(value)
    if len(objects) != 1:
        raise ConformanceError(
            f"{label} must emit one JSON result, emitted {len(objects)}"
        )
    return objects[0]


def enrich_result_lines(text: str, comparison: str) -> str:
    rewritten: list[str] = []
    for line in text.splitlines():
        if line.lstrip().startswith("{"):
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                value = None
            if isinstance(value, dict) and value.get("kind") != "server_connection":
                value["comparison"] = comparison
                value["performance_eligible"] = False
                rewritten.append(json.dumps(value, sort_keys=True))
                continue
        rewritten.append(line)
    return "\n".join(rewritten) + ("\n" if rewritten else "")


def integer(mapping: dict[str, Any], field: str, label: str) -> int:
    value = mapping.get(field)
    if type(value) is not int or value < 0:
        raise ConformanceError(f"{label}.{field} must be a non-negative integer")
    return value


def validate_counters(
    counters: dict[str, Any], count: int, size: int, credit: int, label: str
) -> None:
    record_fields = (
        "write_submissions", "write_completions", "sent_records",
        "received_records", "verified_records",
    )
    values = [integer(counters, field, label) for field in record_fields]
    if any(value != count for value in values):
        raise ConformanceError(f"{label} record counts differ: {values}")
    byte_fields = ("sent_bytes", "received_bytes", "verified_bytes")
    byte_values = [integer(counters, field, label) for field in byte_fields]
    if any(value != count * size for value in byte_values):
        raise ConformanceError(f"{label} byte counts differ: {byte_values}")
    writes = integer(counters, "max_write_operations_in_flight", label)
    if writes > 1 or (count and writes != 1):
        raise ConformanceError(f"{label} violates the single-writer contract")
    if integer(counters, "max_unacknowledged_records", label) > credit:
        raise ConformanceError(f"{label} exceeded the credit window")
    if integer(counters, "current_write_operations_in_flight", label) != 0:
        raise ConformanceError(f"{label} retained an active write")
    for field in (
        "write_size_errors", "records_per_write_errors",
        "write_completion_errors", "sequence_errors", "payload_errors",
        "transport_errors", "phase_errors",
    ):
        if integer(counters, field, label) != 0:
            raise ConformanceError(f"{label} reported {field}")
    if integer(counters, "error_count", label) != 0:
        raise ConformanceError(f"{label} reported an integrity/accounting error")


def validate_result(
    result: dict[str, Any], implementation: str, role: str, peer: str,
    workload: str, size: int, trial: int,
) -> None:
    label = f"{role}/{implementation}/{workload}/{size}"
    expected_text = {
        "schema_version": SCHEMA,
        "implementation": implementation,
        "role": role,
        "peer": peer,
        "workload": workload,
        "counter_scope": "driver" if role == "server" else "adapter",
    }
    for field, expected in expected_text.items():
        if result.get(field) != expected:
            raise ConformanceError(
                f"{label} {field}={result.get(field)!r}, expected {expected!r}"
            )
    if integer(result, "trial", label) != trial:
        raise ConformanceError(f"{label}.trial does not match scheduled trial {trial}")
    if result.get("valid") is not True or result.get("warmup_drained") is not True:
        raise ConformanceError(f"{label} did not report a valid drained run")
    if result.get("invariant_failures") != []:
        raise ConformanceError(f"{label} reported invariant failures")
    if integer(result, "elapsed_ns", label) == 0:
        raise ConformanceError(f"{label} has a zero measured interval")
    expected_samples = RECORDS if workload == "latency" else 0
    if integer(result, "latency_sample_count", label) != expected_samples:
        raise ConformanceError(f"{label} has an invalid latency sample count")

    configuration = result.get("configuration")
    counters = result.get("counters")
    if not isinstance(configuration, dict) or not isinstance(counters, dict):
        raise ConformanceError(f"{label} lacks structured configuration/counters")
    credit = 1 if workload == "latency" else CREDIT_WINDOW
    measured = BULK_BYTES // CHUNK_BYTES if workload == "bulk" else RECORDS
    expected_configuration = {
        "write_size_bytes": size,
        "credit_window": credit,
        "warmup_records": WARMUP_RECORDS,
        "measured_records": measured,
    }
    for field, expected in expected_configuration.items():
        if integer(configuration, field, f"{label}.configuration") != expected:
            raise ConformanceError(
                f"{label}.configuration.{field} does not equal {expected}"
            )
    for phase, count in (("warmup", WARMUP_RECORDS), ("measured", measured)):
        phase_counters = counters.get(phase)
        if not isinstance(phase_counters, dict):
            raise ConformanceError(f"{label} lacks {phase} counters")
        validate_counters(
            phase_counters, count, size, credit, f"{label}.counters.{phase}"
        )
    metrics = result.get("metrics")
    if not isinstance(metrics, dict):
        raise ConformanceError(f"{label} lacks metrics")
    if workload == "latency" and not isinstance(metrics.get("latency_ns"), dict):
        raise ConformanceError(f"{label} lacks latency percentiles")


def workload_cases() -> list[tuple[str, int]]:
    return (
        [("latency", size) for size in MESSAGE_SIZES]
        + [("message", size) for size in MESSAGE_SIZES]
        + [("bulk", CHUNK_BYTES)]
    )


def client_command(
    client: Path, port: int, workload: str, size: int, json_path: Path,
    trial: int, peer_implementation: str | None = None,
) -> list[str]:
    command = [
        str(client), "--host", HOST, "--port", str(port),
        "--mode", workload, "--message-size", str(size),
        "--records", str(RECORDS), "--warmup-records", str(WARMUP_RECORDS),
        "--credit-window", str(1 if workload == "latency" else CREDIT_WINDOW),
        "--bulk-bytes", str(BULK_BYTES), "--chunk-bytes", str(CHUNK_BYTES),
        "--trial", str(trial), "--json", str(json_path),
    ]
    if peer_implementation is not None:
        command.extend(["--peer-implementation", peer_implementation])
    return command


def run_case(
    command: list[str], label: str, log_dir: Path, implementation: str,
    role: str, peer: str, workload: str, size: int, comparison: str,
    json_path: Path, trial: int,
) -> dict[str, Any]:
    command_record = log_dir / f"{label}.command.json"
    command_evidence: dict[str, Any] = {
        "label": label,
        "command": command,
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
    }
    command_record.write_text(
        json.dumps(command_evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    started = time.monotonic_ns()
    completed = subprocess.run(
        command, text=True, capture_output=True, timeout=TIMEOUT_SECONDS,
        check=False,
    )
    command_evidence["elapsed_ns"] = time.monotonic_ns() - started
    command_evidence["returncode"] = completed.returncode
    command_evidence["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    command_record.write_text(
        json.dumps(command_evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    stdout_path = log_dir / f"{label}.stdout.log"
    enriched_stdout = enrich_result_lines(completed.stdout, comparison)
    stdout_path.write_text(enriched_stdout, encoding="utf-8")
    (log_dir / f"{label}.stderr.log").write_text(completed.stderr, encoding="utf-8")
    try:
        native_text = json_path.read_text(encoding="utf-8")
    except OSError:
        native_text = ""
    enriched_native = enrich_result_lines(native_text, comparison)
    json_path.write_text(enriched_native, encoding="utf-8")
    if completed.returncode != 0:
        raise ConformanceError(f"{label} exited with status {completed.returncode}")
    result = one_json(enriched_stdout, stdout_path.name)
    native_result = one_json(enriched_native, json_path.name)
    if native_result != result:
        raise ConformanceError(f"{label} native JSONL differs from stdout result")
    validate_result(result, implementation, role, peer, workload, size, trial)
    return result


def validate_artifact_ineligibility(output_dir: Path) -> None:
    checked = 0
    for path in sorted(output_dir.glob("*.jsonl")) + sorted(
        output_dir.glob("*.stdout.log")
    ):
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), 1
        ):
            if not line.lstrip().startswith("{"):
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise ConformanceError(
                    f"{path.name}:{line_number} contains malformed JSON"
                ) from error
            if (not isinstance(value, dict) or value.get("schema_version") != SCHEMA
                    or value.get("kind") == "server_connection"):
                continue
            checked += 1
            if value.get("performance_eligible") is not False:
                raise ConformanceError(
                    f"{path.name}:{line_number} retained a result without "
                    "performance_eligible=false"
                )
    if checked == 0:
        raise ConformanceError("artifact ineligibility self-check found no results")


ResultCoordinate = tuple[str, str, str, str, int]
EvidenceCoordinate = tuple[str, str, str, str, int]


def expected_result_coordinates() -> list[ResultCoordinate]:
    coordinates: list[ResultCoordinate] = []
    for runtime in RUNTIMES:
        for workload, size in workload_cases():
            coordinates.append(
                ("client-reference", runtime, "posix-reference", workload, size)
            )
    for runtime in RUNTIMES:
        for workload, size in workload_cases():
            coordinates.append(
                (
                    "server-reference", runtime, "posix-reference-client",
                    workload, size,
                )
            )
    for server_runtime in RUNTIMES:
        for client_runtime in RUNTIMES:
            coordinates.append(
                ("cross-runtime", client_runtime, server_runtime, "message", 1024)
            )
    return coordinates


def scheduled_trial(coordinate: ResultCoordinate) -> int:
    """Return a stable, unique wire identity for one scheduled matrix cell."""
    try:
        return expected_result_coordinates().index(coordinate) + 1
    except ValueError as error:
        raise ConformanceError(
            f"cannot assign a trial to unscheduled coordinate {coordinate!r}"
        ) from error


def result_coordinate(value: dict[str, Any]) -> ResultCoordinate | None:
    configuration = value.get("configuration")
    if not isinstance(configuration, dict):
        return None
    comparison = value.get("comparison")
    implementation = value.get("implementation")
    peer = value.get("peer")
    workload = value.get("workload")
    size = configuration.get("write_size_bytes")
    if not all(isinstance(field, str) for field in (
        comparison, implementation, peer, workload,
    )) or type(size) is not int:
        return None
    return comparison, implementation, peer, workload, size


def expected_evidence_coordinates() -> list[EvidenceCoordinate]:
    coordinates: list[EvidenceCoordinate] = []
    for comparison, implementation, peer, workload, size in (
        expected_result_coordinates()
    ):
        if comparison == "server-reference":
            coordinates.append((comparison, peer, implementation, workload, size))
        else:
            coordinates.append((comparison, implementation, peer, workload, size))
    return coordinates


def evidence_coordinate(value: dict[str, Any]) -> EvidenceCoordinate | None:
    comparison = value.get("comparison")
    client = value.get("client_implementation")
    server = value.get("server_implementation")
    workload = value.get("workload")
    size = value.get("write_size_bytes")
    fields = (comparison, client, server, workload)
    if (value.get("schema_version") != SCHEMA
            or value.get("kind") != "server_connection"
            or not all(isinstance(field, str) for field in fields)
            or type(size) is not int
            or value.get("implementation") != server):
        return None
    label_implementation = server if comparison == "server-reference" else client
    expected_label = (
        f"{comparison}-{label_implementation}-{server}-{workload}-{size}"
    )
    if value.get("case_label") != expected_label:
        return None
    result_identity: ResultCoordinate
    if comparison == "server-reference":
        result_identity = (comparison, server, client, workload, size)
    else:
        result_identity = (comparison, client, server, workload, size)
    try:
        expected_trial = scheduled_trial(result_identity)
    except ConformanceError:
        return None
    if value.get("trial") != expected_trial:
        return None
    return comparison, client, server, workload, size


def _counter_invariants(value: dict[str, Any]) -> dict[str, bool]:
    configuration = value.get("configuration")
    counters = value.get("counters")
    if not isinstance(configuration, dict) or not isinstance(counters, dict):
        return {
            "warmup_drained": False, "exact_result_accounting": False,
            "single_writer": False, "credit_window": False,
            "result_error_free": False,
        }

    exact = True
    single_writer = True
    credit_ok = True
    error_free = True
    credit = configuration.get("credit_window")
    size = configuration.get("write_size_bytes")
    for phase, configured_field in (
        ("warmup", "warmup_records"), ("measured", "measured_records")
    ):
        phase_counters = counters.get(phase)
        count = configuration.get(configured_field)
        if (not isinstance(phase_counters, dict) or type(count) is not int
                or type(size) is not int or type(credit) is not int):
            exact = single_writer = credit_ok = error_free = False
            continue
        record_values = [phase_counters.get(field) for field in (
            "write_submissions", "write_completions", "sent_records",
            "received_records", "verified_records",
        )]
        byte_values = [phase_counters.get(field) for field in (
            "sent_bytes", "received_bytes", "verified_bytes",
        )]
        exact = exact and all(value == count for value in record_values)
        exact = exact and all(value == count * size for value in byte_values)
        single_writer = single_writer and (
            phase_counters.get("max_write_operations_in_flight") == (1 if count else 0)
            and phase_counters.get("current_write_operations_in_flight") == 0
        )
        maximum_unacknowledged = phase_counters.get("max_unacknowledged_records")
        credit_ok = credit_ok and type(maximum_unacknowledged) is int
        if type(maximum_unacknowledged) is int:
            credit_ok = credit_ok and 0 <= maximum_unacknowledged <= credit
        error_free = error_free and all(phase_counters.get(field) == 0 for field in (
            "write_size_errors", "records_per_write_errors",
            "write_completion_errors", "sequence_errors", "payload_errors",
            "transport_errors", "phase_errors", "error_count",
        ))
    return {
        "warmup_drained": value.get("warmup_drained") is True,
        "exact_result_accounting": exact,
        "single_writer": single_writer,
        "credit_window": credit_ok,
        "result_error_free": error_free,
    }


def _server_invariants(value: dict[str, Any]) -> dict[str, bool]:
    warmup = value.get("warmup_records")
    measured = value.get("measured_records")
    size = value.get("write_size_bytes")
    records = value.get("received_records")
    record_fields = (
        "received_records", "verified_records", "write_submissions",
        "write_completions",
    )
    byte_fields = (
        "received_bytes", "verified_bytes", "submitted_bytes", "completed_bytes",
    )
    exact = all(type(value.get(field)) is int for field in record_fields + byte_fields)
    if not all(type(item) is int for item in (warmup, measured, size, records)):
        exact = False
    if exact:
        exact = len({value[field] for field in record_fields}) == 1
        exact = exact and len({value[field] for field in byte_fields}) == 1
        exact = exact and warmup + measured == records
        exact = exact and value.get("warmup_bytes") == warmup * size
        exact = exact and value.get("measured_bytes") == measured * size
        exact = exact and value.get("received_bytes") == records * size
    return {
        "exact_server_accounting": exact,
        "server_single_writer": (
            value.get("max_write_operations_in_flight") == 1
            and value.get("current_write_operations_in_flight") == 0
        ),
        "server_error_free": all(value.get(field) == 0 for field in (
            "integrity_errors", "transport_errors", "accounting_errors",
        )),
    }


def _diagnostic_row(value: dict[str, Any]) -> dict[str, Any]:
    coordinate = result_coordinate(value)
    assert coordinate is not None
    comparison, implementation, peer, workload, size = coordinate
    metrics = value.get("metrics")
    metrics = metrics if isinstance(metrics, dict) else {}

    def diagnostic_number(field: Any) -> int | float | None:
        if type(field) in (int, float) and math.isfinite(field):
            return field
        return None

    row: dict[str, Any] = {
        "runtime": implementation,
        "implementation": implementation,
        "peer": peer,
        "workload": workload,
        "write_size_bytes": size,
        "performance_eligible": False,
    }
    if comparison == "cross-runtime":
        row["client_runtime"] = implementation
        row["server_runtime"] = peer
    if workload == "latency":
        latency = metrics.get("latency_ns")
        latency = latency if isinstance(latency, dict) else {}
        row["latency_ns"] = {
            name: diagnostic_number(latency.get(name))
            for name in ("p50", "p95", "p99")
        }
        row["records_per_second"] = diagnostic_number(
            metrics.get("records_per_second")
        )
        row["mib_per_second"] = diagnostic_number(metrics.get("mib_per_second"))
    elif workload == "message":
        row["records_per_second"] = diagnostic_number(
            metrics.get("records_per_second")
        )
        row["mib_per_second"] = diagnostic_number(metrics.get("mib_per_second"))
    else:
        row["mib_per_second"] = diagnostic_number(metrics.get("mib_per_second"))
    return row


def _result_identity_and_metrics_valid(value: dict[str, Any]) -> bool:
    coordinate = result_coordinate(value)
    if coordinate is None:
        return False
    comparison, _, _, workload, _ = coordinate
    expected_role = "server" if comparison == "server-reference" else "client"
    expected_scope = "driver" if expected_role == "server" else "adapter"
    if (value.get("schema_version") != SCHEMA
            or value.get("role") != expected_role
            or value.get("counter_scope") != expected_scope):
        return False
    if value.get("trial") != scheduled_trial(coordinate):
        return False
    metrics = value.get("metrics")
    if not isinstance(metrics, dict):
        return False

    def numeric(field: Any) -> bool:
        return (
            type(field) in (int, float)
            and math.isfinite(field)
            and field >= 0
        )

    if not numeric(metrics.get("mib_per_second")):
        return False
    if workload != "bulk" and not numeric(metrics.get("records_per_second")):
        return False
    if workload == "latency":
        latency = metrics.get("latency_ns")
        if not isinstance(latency, dict) or not all(
            numeric(latency.get(name)) for name in ("p50", "p95", "p99")
        ):
            return False
        if not latency["p50"] <= latency["p95"] <= latency["p99"]:
            return False
    return True


def _result_fixed_work_valid(value: dict[str, Any]) -> bool:
    coordinate = result_coordinate(value)
    configuration = value.get("configuration")
    if coordinate is None or not isinstance(configuration, dict):
        return False
    _, _, _, workload, size = coordinate
    measured = BULK_BYTES // CHUNK_BYTES if workload == "bulk" else RECORDS
    expected = {
        "write_size_bytes": size,
        "credit_window": 1 if workload == "latency" else CREDIT_WINDOW,
        "warmup_records": WARMUP_RECORDS,
        "measured_records": measured,
    }
    if any(configuration.get(field) != wanted for field, wanted in expected.items()):
        return False
    expected_samples = RECORDS if workload == "latency" else 0
    return value.get("latency_sample_count") == expected_samples


def build_conformance_summary(
    results: list[dict[str, Any]], server_evidence: list[dict[str, Any]],
    manifest: dict[str, Any], *, status: str, error: str | None = None,
) -> dict[str, Any]:
    """Build a deterministic, presentation-friendly conformance report."""
    if status not in ("passed", "failed"):
        raise ConformanceError("summary status must be passed or failed")
    if status == "passed" and error is not None:
        raise ConformanceError("a passed summary cannot contain an error")

    expected = expected_result_coordinates()
    expected_set = set(expected)
    expected_evidence = expected_evidence_coordinates()
    expected_evidence_set = set(expected_evidence)
    indexed: dict[ResultCoordinate, list[dict[str, Any]]] = {}
    unparseable = 0
    for value in results:
        coordinate = result_coordinate(value)
        if coordinate is None:
            unparseable += 1
            continue
        indexed.setdefault(coordinate, []).append(value)
    missing = [coordinate for coordinate in expected if coordinate not in indexed]
    duplicates = [coordinate for coordinate in expected if len(indexed.get(coordinate, [])) > 1]
    unexpected = sorted(coordinate for coordinate in indexed if coordinate not in expected_set)
    evidence_indexed: dict[EvidenceCoordinate, list[dict[str, Any]]] = {}
    evidence_unparseable = 0
    for value in server_evidence:
        coordinate = evidence_coordinate(value)
        if coordinate is None:
            evidence_unparseable += 1
            continue
        evidence_indexed.setdefault(coordinate, []).append(value)
    evidence_missing = [
        coordinate for coordinate in expected_evidence
        if coordinate not in evidence_indexed
    ]
    evidence_duplicates = [
        coordinate for coordinate in expected_evidence
        if len(evidence_indexed.get(coordinate, [])) > 1
    ]
    evidence_unexpected = sorted(
        coordinate for coordinate in evidence_indexed
        if coordinate not in expected_evidence_set
    )
    if status == "passed" and (missing or duplicates or unexpected or unparseable):
        raise ConformanceError(
            "passed summary requires exactly one result for every scheduled coordinate "
            f"(missing={len(missing)}, duplicate={len(duplicates)}, "
            f"unexpected={len(unexpected)}, unparseable={unparseable})"
        )
    if status == "passed" and any(
        value.get("valid") is not True
        or value.get("warmup_drained") is not True
        or value.get("invariant_failures") != []
        or value.get("performance_eligible") is not False
        for value in results
    ):
        raise ConformanceError(
            "passed summary requires every result to be valid and "
            "performance_eligible=false"
        )
    if status == "passed" and any(
        not _result_identity_and_metrics_valid(value) for value in results
    ):
        raise ConformanceError(
            "passed summary requires complete result identity and diagnostic metrics"
        )
    if status == "passed" and any(
        not _result_fixed_work_valid(value) for value in results
    ):
        raise ConformanceError(
            "passed summary requires the exact scheduled fixed-work configuration"
        )
    if status == "passed" and manifest.get("performance_eligible") is not False:
        raise ConformanceError(
            "passed summary requires a performance_eligible=false manifest"
        )
    if status == "passed" and (
        evidence_missing or evidence_duplicates or evidence_unexpected
        or evidence_unparseable
    ):
        raise ConformanceError(
            "passed summary requires exactly one server-evidence record for every "
            "scheduled coordinate "
            f"(missing={len(evidence_missing)}, "
            f"duplicate={len(evidence_duplicates)}, "
            f"unexpected={len(evidence_unexpected)}, "
            f"unparseable={evidence_unparseable})"
        )
    if status == "passed" and any(
        value.get("valid") is not True for value in server_evidence
    ):
        raise ConformanceError(
            "passed summary requires every server-evidence record to be valid"
        )
    if status == "passed" and any(
        not all(_counter_invariants(value).values()) for value in results
    ):
        raise ConformanceError(
            "passed summary requires every result invariant to hold"
        )
    if status == "passed" and any(
        not all(_server_invariants(value).values()) for value in server_evidence
    ):
        raise ConformanceError(
            "passed summary requires every server-evidence invariant to hold"
        )

    def coordinate_counts(coordinates: list[ResultCoordinate]) -> dict[str, int]:
        completed = sum(1 for coordinate in coordinates if len(indexed.get(coordinate, [])) == 1)
        valid = sum(
            1 for coordinate in coordinates
            if len(indexed.get(coordinate, [])) == 1
            and indexed[coordinate][0].get("valid") is True
        )
        return {"expected": len(coordinates), "completed": completed, "valid": valid}

    axes: dict[str, Any] = {}
    for axis in ("client-reference", "server-reference", "cross-runtime"):
        coordinates = [coordinate for coordinate in expected if coordinate[0] == axis]
        axes[axis] = coordinate_counts(coordinates)
        if axis == "cross-runtime":
            continue
        runtime_rows: dict[str, Any] = {}
        for runtime in RUNTIMES:
            workloads: dict[str, Any] = {}
            for workload in ("latency", "message", "bulk"):
                cells = [
                    coordinate for coordinate in coordinates
                    if coordinate[1] == runtime and coordinate[3] == workload
                ]
                workloads[workload] = coordinate_counts(cells)
            runtime_rows[runtime] = {
                **coordinate_counts([
                    coordinate for coordinate in coordinates if coordinate[1] == runtime
                ]),
                "workloads": workloads,
            }
        axes[axis]["runtimes"] = runtime_rows

    cross_runtime_matrix: dict[str, dict[str, dict[str, int]]] = {}
    for client_runtime in RUNTIMES:
        cross_runtime_matrix[client_runtime] = {}
        for server_runtime in RUNTIMES:
            cell = [
                coordinate for coordinate in expected
                if coordinate[0] == "cross-runtime"
                and coordinate[1] == client_runtime and coordinate[2] == server_runtime
            ]
            cross_runtime_matrix[client_runtime][server_runtime] = coordinate_counts(cell)

    result_invariant_names = (
        "warmup_drained", "exact_result_accounting", "single_writer",
        "credit_window", "result_error_free",
    )
    server_invariant_names = (
        "exact_server_accounting", "server_single_writer", "server_error_free",
    )
    result_checks = [_counter_invariants(value) for value in results]
    server_checks = [_server_invariants(value) for value in server_evidence]
    invariant_totals = {
        name: {
            "passed": sum(check[name] for check in result_checks),
            "total": len(result_checks),
        }
        for name in result_invariant_names
    }
    invariant_totals.update({
        name: {
            "passed": sum(check[name] for check in server_checks),
            "total": len(server_checks),
        }
        for name in server_invariant_names
    })

    diagnostics: dict[str, Any] = {"warning": DIAGNOSTIC_WARNING}
    for axis in ("client-reference", "server-reference", "cross-runtime"):
        rows: list[dict[str, Any]] = []
        for coordinate in expected:
            if coordinate[0] != axis or len(indexed.get(coordinate, [])) != 1:
                continue
            rows.append(_diagnostic_row(indexed[coordinate][0]))
        diagnostics[axis] = rows

    summary: dict[str, Any] = {
        "schema_version": SUMMARY_SCHEMA,
        "status": status,
        "purpose": "conformance-only with unscored diagnostic timings",
        "performance_eligible": False,
        "error": error,
        "generated_at_utc": manifest.get("generated_at_utc"),
        "tested_revision": manifest.get("tested_revision"),
        "fixed_work": {
            "warmup_records": WARMUP_RECORDS,
            "latency": {
                "measured_records": RECORDS,
                "credit_window": 1,
                "message_sizes_bytes": list(MESSAGE_SIZES),
            },
            "message": {
                "measured_records": RECORDS,
                "credit_window": CREDIT_WINDOW,
                "message_sizes_bytes": list(MESSAGE_SIZES),
            },
            "bulk": {
                "measured_records": BULK_BYTES // CHUNK_BYTES,
                "total_bytes": BULK_BYTES,
                "chunk_bytes": CHUNK_BYTES,
                "credit_window": CREDIT_WINDOW,
            },
        },
        "totals": {
            "expected_cases": len(expected),
            "completed_cases": sum(
                1 for coordinate in expected if len(indexed.get(coordinate, [])) == 1
            ),
            "valid_cases": sum(
                1 for coordinate in expected
                if len(indexed.get(coordinate, [])) == 1
                and indexed[coordinate][0].get("valid") is True
            ),
            "performance_eligible_cases": sum(
                value.get("performance_eligible") is True for value in results
            ),
            "expected_server_evidence": len(expected),
            "server_evidence_cases": sum(
                1 for coordinate in expected_evidence
                if len(evidence_indexed.get(coordinate, [])) == 1
            ),
            "valid_server_evidence_cases": sum(
                1 for coordinate in expected_evidence
                if len(evidence_indexed.get(coordinate, [])) == 1
                and evidence_indexed[coordinate][0].get("valid") is True
            ),
        },
        "axes": axes,
        "cross_runtime_matrix": cross_runtime_matrix,
        "invariant_totals": invariant_totals,
        "coverage_issues": {
            "missing": [list(value) for value in missing],
            "duplicates": [list(value) for value in duplicates],
            "unexpected": [list(value) for value in unexpected],
            "unparseable_results": unparseable,
            "server_evidence_missing": [list(value) for value in evidence_missing],
            "server_evidence_duplicates": [
                list(value) for value in evidence_duplicates
            ],
            "server_evidence_unexpected": [
                list(value) for value in evidence_unexpected
            ],
            "unparseable_server_evidence": evidence_unparseable,
        },
        "diagnostics": diagnostics,
        "parse_diagnostics": [],
        "artifacts": {
            **({"results": "results.jsonl"} if results else {}),
            **(
                {"server_evidence": "server-evidence.jsonl"}
                if server_evidence else {}
            ),
            **(
                {"manifest": "manifest.json"}
                if manifest.get("schema_version") == SCHEMA else {}
            ),
        },
    }
    return summary


def _format_number(value: Any) -> str:
    if type(value) not in (int, float):
        return "n/a"
    if isinstance(value, float):
        return f"{value:.6g}"
    return f"{value:,}"


def render_conformance_summary_markdown(summary: dict[str, Any]) -> str:
    """Render a GitHub-friendly report without relative-performance claims."""
    totals = summary["totals"]
    status = str(summary["status"]).upper()
    lines = [
        "# TCP loopback conformance report", "",
        f"**{summary['diagnostics']['warning']}**", "",
        f"Status: **{status}**", "",
    ]
    if summary.get("error"):
        lines.extend([f"Failure: `{summary['error']}`", ""])
    lines.extend([
        "## Overview", "",
        "| Check | Valid or completed | Expected |",
        "|---|---:|---:|",
        f"| Result cases | {totals['completed_cases']} | {totals['expected_cases']} |",
        f"| Valid result cases | {totals['valid_cases']} | {totals['expected_cases']} |",
        "| Server-observed evidence | "
        f"{totals['valid_server_evidence_cases']} | "
        f"{totals['expected_server_evidence']} |",
    ])
    for axis, label in (
        ("client-reference", "Runtime clients / POSIX reference server"),
        ("server-reference", "POSIX reference client / runtime servers"),
        ("cross-runtime", "Cross-runtime interoperability"),
    ):
        value = summary["axes"][axis]
        lines.append(f"| {label} | {value['valid']} | {value['expected']} |")
    lines.extend([
        "| Performance-eligible cases (expected zero) | "
        f"{totals['performance_eligible_cases']} | 0 |",
        "", "## Workload contract", "",
        "| Workload | Sizes / chunk | Warmup records | Measured work | Credit window |",
        "|---|---|---:|---:|---:|",
    ])
    fixed_work = summary["fixed_work"]
    for workload in ("latency", "message"):
        value = fixed_work[workload]
        sizes = ", ".join(str(size) for size in value["message_sizes_bytes"])
        lines.append(
            f"| {workload} | {sizes} B | {fixed_work['warmup_records']} | "
            f"{value['measured_records']} records | {value['credit_window']} |"
        )
    bulk = fixed_work["bulk"]
    lines.extend([
        f"| bulk | {bulk['chunk_bytes']} B chunk | {fixed_work['warmup_records']} | "
        f"{bulk['measured_records']} records / {bulk['total_bytes']} B | "
        f"{bulk['credit_window']} |",
        "", "## Fixed-work coverage", "",
    ])
    for axis, title in (
        ("client-reference", "Client implementations against the reference server"),
        ("server-reference", "Server implementations driven by the reference client"),
    ):
        lines.extend([
            f"### {title}", "",
            "| Runtime | Latency sizes | Message sizes | Bulk | Total |",
            "|---|---:|---:|---:|---:|",
        ])
        for runtime in RUNTIMES:
            row = summary["axes"][axis]["runtimes"][runtime]
            workload = row["workloads"]
            lines.append(
                f"| {runtime} | {workload['latency']['valid']}/"
                f"{workload['latency']['expected']} | "
                f"{workload['message']['valid']}/{workload['message']['expected']} | "
                f"{workload['bulk']['valid']}/{workload['bulk']['expected']} | "
                f"{row['valid']}/{row['expected']} |"
            )
        lines.append("")

    lines.extend([
        "### Cross-runtime interoperability", "",
        "| Client \\ Server | elio | libuv | asio |",
        "|---|---:|---:|---:|",
    ])
    matrix = summary["cross_runtime_matrix"]
    for client_runtime in RUNTIMES:
        cells = [
            f"{matrix[client_runtime][server_runtime]['valid']}/"
            f"{matrix[client_runtime][server_runtime]['expected']}"
            for server_runtime in RUNTIMES
        ]
        lines.append(f"| {client_runtime} | {' | '.join(cells)} |")

    lines.extend([
        "", "## Validated invariants", "",
        "| Invariant | Passed | Evaluated |",
        "|---|---:|---:|",
    ])
    invariant_labels = {
        "warmup_drained": "Warmup fully drained",
        "exact_result_accounting": "Client/driver exact record and byte accounting",
        "single_writer": "At most one outstanding write",
        "credit_window": "Credit window respected",
        "result_error_free": "Result integrity and transport error counters are zero",
        "exact_server_accounting": "Server-observed exact record and byte accounting",
        "server_single_writer": "Server-observed single-writer invariant",
        "server_error_free": "Server integrity, transport, and accounting errors are zero",
    }
    for name, label in invariant_labels.items():
        value = summary["invariant_totals"][name]
        lines.append(f"| {label} | {value['passed']} | {value['total']} |")

    lines.extend([
        "", "## Diagnostic timing observations", "",
        f"**{summary['diagnostics']['warning']}**", "",
    ])
    for axis, title in (
        ("client-reference", "Client axis (`client-reference`)"),
        ("server-reference", "Server axis (`server-reference`)"),
        ("cross-runtime", "Cross-runtime interoperability (`cross-runtime`)"),
    ):
        rows = summary["diagnostics"][axis]
        lines.extend([f"### {title}", ""])
        latency_rows = [row for row in rows if row["workload"] == "latency"]
        if latency_rows:
            lines.extend([
                "| Runtime | Peer | Size (B) | p50 (ns) | p95 (ns) | p99 (ns) "
                "| Records/s | MiB/s |",
                "|---|---|---:|---:|---:|---:|---:|---:|",
            ])
            for row in latency_rows:
                latency = row["latency_ns"]
                lines.append(
                    f"| {row['implementation']} | {row['peer']} | {row['write_size_bytes']} | "
                    f"{_format_number(latency['p50'])} | {_format_number(latency['p95'])} | "
                    f"{_format_number(latency['p99'])} | "
                    f"{_format_number(row['records_per_second'])} | "
                    f"{_format_number(row['mib_per_second'])} |"
                )
            lines.append("")
        message_rows = [row for row in rows if row["workload"] == "message"]
        if message_rows:
            lines.extend([
                "| Runtime | Peer | Size (B) | Records/s | MiB/s |",
                "|---|---|---:|---:|---:|",
            ])
            for row in message_rows:
                lines.append(
                    f"| {row['implementation']} | {row['peer']} | {row['write_size_bytes']} | "
                    f"{_format_number(row['records_per_second'])} | "
                    f"{_format_number(row['mib_per_second'])} |"
                )
            lines.append("")
        bulk_rows = [row for row in rows if row["workload"] == "bulk"]
        if bulk_rows:
            lines.extend([
                "| Runtime | Peer | Chunk (B) | MiB/s |",
                "|---|---|---:|---:|",
            ])
            for row in bulk_rows:
                lines.append(
                    f"| {row['implementation']} | {row['peer']} | {row['write_size_bytes']} | "
                    f"{_format_number(row['mib_per_second'])} |"
                )
            lines.append("")
    revision = summary.get("tested_revision") or "unavailable"
    lines.extend(["## Evidence", "", f"Tested revision: `{revision}`", ""])
    artifacts = summary.get("artifacts", {})
    raw_names = [
        artifacts[key] for key in ("results", "server_evidence", "manifest")
        if key in artifacts
    ]
    if raw_names:
        rendered_names = ", ".join(f"`{name}`" for name in raw_names)
        lines.extend([f"Available raw evidence: {rendered_names}.", ""])
    else:
        lines.extend([
            "No raw evidence files were produced before this failure.", "",
        ])
    parse_diagnostics = summary.get("parse_diagnostics", [])
    if parse_diagnostics:
        lines.extend([
            "### JSONL parse diagnostics", "",
            "| File | Line | Reason |",
            "|---|---:|---|",
        ])
        for value in parse_diagnostics:
            reason = str(value["reason"]).replace("|", "\\|")
            lines.append(
                f"| `{value['file']}` | {value['line']} | {reason} |"
            )
        lines.append("")
    return "\n".join(lines)


def write_conformance_summary(
    output_dir: Path, results: list[dict[str, Any]],
    server_evidence: list[dict[str, Any]], manifest: dict[str, Any], *,
    status: str, error: str | None = None,
    parse_diagnostics: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    diagnostics = list(parse_diagnostics or [])
    if status == "passed" and diagnostics:
        raise ConformanceError(
            "passed summary cannot omit malformed JSONL evidence"
        )
    summary = build_conformance_summary(
        results, server_evidence, manifest, status=status, error=error,
    )
    summary["parse_diagnostics"] = diagnostics
    artifact_candidates = {
        "results": "results.jsonl",
        "server_evidence": "server-evidence.jsonl",
        "manifest": "manifest.json",
    }
    summary["artifacts"] = {
        key: name for key, name in artifact_candidates.items()
        if (output_dir / name).is_file()
    }
    summary["artifacts"].update({
        "summary_json": "summary.json", "summary_markdown": "summary.md",
    })
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    (output_dir / "summary.md").write_text(
        render_conformance_summary_markdown(summary), encoding="utf-8",
    )
    return summary


def _reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value}")


def read_jsonl_objects(
    path: Path, diagnostics: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    objects: list[dict[str, Any]] = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if not line.strip():
            continue
        try:
            value = json.loads(line, parse_constant=_reject_nonfinite_json)
        except (json.JSONDecodeError, ValueError) as error:
            if diagnostics is not None:
                diagnostics.append({
                    "file": path.name, "line": line_number,
                    "reason": str(error),
                })
            continue
        if isinstance(value, dict):
            objects.append(value)
        elif diagnostics is not None:
            diagnostics.append({
                "file": path.name, "line": line_number,
                "reason": "JSONL row is not an object",
            })
    return objects


Case = tuple[Path, str, str, str, int]


def server_evidence_objects(
    path: Path, diagnostics: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    if not path.exists():
        return objects
    local_diagnostics: list[dict[str, Any]] = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line, parse_constant=_reject_nonfinite_json)
        except (json.JSONDecodeError, ValueError) as error:
            local_diagnostics.append({
                "file": path.name, "line": line_number, "reason": str(error),
            })
            continue
        if isinstance(value, dict) and value.get("kind") == "server_connection":
            objects.append(value)
    if diagnostics is not None:
        diagnostics.extend(local_diagnostics)
    elif local_diagnostics:
        first = local_diagnostics[0]
        raise ConformanceError(
            f"{first['file']}:{first['line']} contains malformed JSON: "
            f"{first['reason']}"
        )
    return objects


def collect_server_log_parse_diagnostics(output_dir: Path) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    for path in sorted(output_dir.glob("server-*.log")):
        server_evidence_objects(path, diagnostics)
    return diagnostics


def collect_case_parse_diagnostics(output_dir: Path) -> list[dict[str, Any]]:
    """Retain precise malformed-JSON locations from per-case artifacts."""
    diagnostics: list[dict[str, Any]] = []
    aggregate_names = {"results.jsonl", "server-evidence.jsonl"}
    for path in sorted(output_dir.glob("*.jsonl")):
        if path.name not in aggregate_names:
            read_jsonl_objects(path, diagnostics)
    for path in sorted(output_dir.glob("*.stdout.log")):
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), 1
        ):
            if not line.lstrip().startswith("{"):
                continue
            try:
                value = json.loads(line, parse_constant=_reject_nonfinite_json)
            except (json.JSONDecodeError, ValueError) as error:
                diagnostics.append({
                    "file": path.name, "line": line_number,
                    "reason": str(error),
                })
                continue
            if not isinstance(value, dict):
                diagnostics.append({
                    "file": path.name, "line": line_number,
                    "reason": "JSON result row is not an object",
                })
    return diagnostics


def validate_server_evidence(
    value: dict[str, Any], implementation: str, label: str,
) -> None:
    expected = {
        "schema_version": SCHEMA,
        "kind": "server_connection",
        "implementation": implementation,
        "valid": True,
    }
    for field, wanted in expected.items():
        if value.get(field) != wanted:
            raise ConformanceError(
                f"{label} {field}={value.get(field)!r}, expected {wanted!r}"
            )
    count_fields = (
        "received_records", "verified_records", "write_submissions",
        "write_completions",
    )
    counts = [integer(value, field, label) for field in count_fields]
    if counts[0] == 0 or len(set(counts)) != 1:
        raise ConformanceError(f"{label} server record counts differ: {counts}")
    byte_fields = (
        "received_bytes", "verified_bytes", "submitted_bytes", "completed_bytes",
    )
    byte_counts = [integer(value, field, label) for field in byte_fields]
    if byte_counts[0] == 0 or len(set(byte_counts)) != 1:
        raise ConformanceError(f"{label} server byte counts differ: {byte_counts}")
    warmup = integer(value, "warmup_records", label)
    measured = integer(value, "measured_records", label)
    size = integer(value, "write_size_bytes", label)
    if warmup + measured != counts[0]:
        raise ConformanceError(f"{label} phase record counts do not add up")
    if integer(value, "warmup_bytes", label) != warmup * size:
        raise ConformanceError(f"{label} warmup byte count does not match")
    if integer(value, "measured_bytes", label) != measured * size:
        raise ConformanceError(f"{label} measured byte count does not match")
    if integer(value, "current_write_operations_in_flight", label) != 0:
        raise ConformanceError(f"{label} retained an active server write")
    if integer(value, "max_write_operations_in_flight", label) != 1:
        raise ConformanceError(f"{label} violated the server single-writer contract")
    for field in ("integrity_errors", "transport_errors", "accounting_errors"):
        if integer(value, field, label) != 0:
            raise ConformanceError(f"{label} reported {field}")


def attribute_server_evidence(
    summary: dict[str, Any], case: Case, server_name: str,
) -> dict[str, Any]:
    _, comparison, implementation, workload, size = case
    peer = (
        "posix-reference-client" if comparison == "server-reference"
        else ("posix-reference" if comparison == "client-reference" else server_name)
    )
    trial = scheduled_trial((comparison, implementation, peer, workload, size))
    if summary.get("trial") != trial:
        raise ConformanceError(
            f"{server_name} server evidence trial={summary.get('trial')!r}, "
            f"expected {trial} for {comparison}/{implementation}/{workload}/{size}"
        )
    return {
        **summary,
        "case_label": (
            f"{comparison}-{implementation}-{server_name}-{workload}-{size}"
        ),
        "comparison": comparison,
        "client_implementation": (
            "posix-reference-client"
            if comparison == "server-reference" else implementation
        ),
        "server_implementation": server_name,
        "workload": workload,
    }


def run_against_server(
    server_path: Path, server_name: str, cases: list[Case],
    output_dir: Path, evidence: TextIO, server_evidence: TextIO,
) -> None:
    port = reserve_port()
    server_log_path = output_dir / f"server-{server_name}.log"
    started_cases: list[Case] = []
    pending_error: BaseException | None = None
    with server_log_path.open("w", encoding="utf-8") as server_log:
        server = subprocess.Popen(
            [str(server_path), "--port", str(port),
             "--chunk-bytes", str(CHUNK_BYTES)],
            text=True, stdout=server_log, stderr=subprocess.STDOUT,
        )
        try:
            wait_ready(server, port, server_name)
            for case in cases:
                started_cases.append(case)
                client, comparison, implementation, workload, size = case
                label = f"{comparison}-{implementation}-{server_name}-{workload}-{size}"
                case_json_path = output_dir / f"{label}.jsonl"
                peer = (
                    "posix-reference-client" if comparison == "server-reference"
                    else ("posix-reference" if comparison == "client-reference"
                          else server_name)
                )
                trial = scheduled_trial(
                    (comparison, implementation, peer, workload, size)
                )
                result = run_case(
                    client_command(
                        client, port, workload, size,
                        case_json_path, trial,
                        server_name if comparison != "client-reference" else None,
                    ),
                    label, output_dir, implementation,
                    "server" if comparison == "server-reference" else "client",
                    peer,
                    workload, size, comparison, case_json_path,
                    trial,
                )
                evidence.write(
                    json.dumps(result, sort_keys=True, allow_nan=False) + "\n"
                )
                evidence.flush()
                print(f"validated {label}")
            if server.poll() is not None:
                raise ConformanceError(
                    f"{server_name} exited unexpectedly (status {server.returncode})"
                )
            deadline = time.monotonic() + 2.0
            while (len(server_evidence_objects(server_log_path)) < len(cases) and
                   time.monotonic() < deadline):
                time.sleep(0.02)
        except BaseException as error:
            pending_error = error
        finally:
            stop_server(server)

    log_diagnostics: list[dict[str, Any]] = []
    summaries = server_evidence_objects(server_log_path, log_diagnostics)
    cases_by_trial: dict[int, Case] = {}
    for case in started_cases:
        _, comparison, implementation, workload, size = case
        peer = (
            "posix-reference-client" if comparison == "server-reference"
            else ("posix-reference" if comparison == "client-reference"
                  else server_name)
        )
        cases_by_trial[
            scheduled_trial((comparison, implementation, peer, workload, size))
        ] = case

    attributed: list[dict[str, Any]] = []
    seen_trials: set[int] = set()
    for summary in summaries:
        trial = summary.get("trial")
        case = cases_by_trial.get(trial) if type(trial) is int else None
        if case is None or trial in seen_trials:
            attributed.append(summary)
            if pending_error is None:
                reason = "unknown" if case is None else "duplicate"
                pending_error = ConformanceError(
                    f"{server_name} emitted {reason} server evidence trial "
                    f"{trial!r}"
                )
            continue
        seen_trials.add(trial)
        try:
            attributed.append(
                attribute_server_evidence(summary, case, server_name)
            )
        except ConformanceError as error:
            # Preserve the raw observation, but never attach a coordinate that
            # its wire trial does not prove.
            attributed.append(summary)
            if pending_error is None:
                pending_error = error
    for summary in attributed:
        server_evidence.write(
            json.dumps(summary, sort_keys=True, allow_nan=False) + "\n"
        )
    server_evidence.flush()

    if log_diagnostics:
        first = log_diagnostics[0]
        malformed = ConformanceError(
            f"{first['file']}:{first['line']} contains malformed JSON: "
            f"{first['reason']}"
        )
        if pending_error is None:
            pending_error = malformed
    if pending_error is not None:
        raise pending_error

    if len(summaries) != len(cases):
        raise ConformanceError(
            f"{server_name} emitted {len(summaries)} server summaries; "
            f"expected {len(cases)}"
        )
    for index, summary in enumerate(summaries):
        validate_server_evidence(summary, server_name,
                                 f"{server_name}.connection[{index}]")
    expected_work = Counter(
        (
            WARMUP_RECORDS,
            BULK_BYTES // CHUNK_BYTES if workload == "bulk" else RECORDS,
            size,
        )
        for _, _, _, workload, size in cases
    )
    observed_work = Counter(
        (
            integer(summary, "warmup_records", server_name),
            integer(summary, "measured_records", server_name),
            integer(summary, "write_size_bytes", server_name),
        )
        for summary in summaries
    )
    if observed_work != expected_work:
        raise ConformanceError(
            f"{server_name} server evidence does not match scheduled work"
        )


def main() -> int:
    args = parse_args()
    output_dir: Path = args.output_dir
    output_created = False
    manifest: dict[str, Any] | None = None
    try:
        source_was_dirty = git_dirty_state()
        output_dir.mkdir(parents=True, exist_ok=False)
        output_created = True
        reference_server = executable(args.reference_server, "reference server")
        reference_client = executable(args.reference_client, "reference client")
        clients = {
            runtime: executable(getattr(args, f"{runtime}_client"), f"{runtime} client")
            for runtime in RUNTIMES
        }
        servers = {
            runtime: executable(getattr(args, f"{runtime}_server"), f"{runtime} server")
            for runtime in RUNTIMES
        }
        binaries = {
            "reference_server": reference_server,
            "reference_client": reference_client,
            **{f"{name}_client": path for name, path in clients.items()},
            **{f"{name}_server": path for name, path in servers.items()},
        }
        manifest = {
            "schema_version": SCHEMA,
            "purpose": "conformance-only; timings are not compared",
            "performance_eligible": False,
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "tested_revision": tested_revision(),
            "git_dirty": source_was_dirty,
            "platform": platform.platform(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "environment": {
                name: os.environ.get(name)
                for name in ("CC", "CXX", "CFLAGS", "CXXFLAGS")
            },
            "socket_options": {"tcp_nodelay": True, "loopback": HOST},
            "workload": {
                "message_sizes": MESSAGE_SIZES,
                "records": RECORDS,
                "warmup_records": WARMUP_RECORDS,
                "credit_window": CREDIT_WINDOW,
                "bulk_bytes": BULK_BYTES,
                "chunk_bytes": CHUNK_BYTES,
            },
            "executables": {
                name: {"path": str(path), "sha256": sha256_file(path)}
                for name, path in binaries.items()
            },
        }
        validate_manifest(manifest)
        manifest_path = output_dir / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        serialized_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        validate_manifest(serialized_manifest)

        with (output_dir / "results.jsonl").open("x", encoding="utf-8") as evidence, \
             (output_dir / "server-evidence.jsonl").open(
                 "x", encoding="utf-8"
             ) as server_evidence:
            client_cases: list[Case] = [
                (clients[runtime], "client-reference", runtime, workload, size)
                for runtime in RUNTIMES for workload, size in workload_cases()
            ]
            run_against_server(
                reference_server, "posix-reference", client_cases,
                output_dir, evidence, server_evidence,
            )

            for runtime in RUNTIMES:
                server_cases: list[Case] = [
                    (reference_client, "server-reference", runtime, workload, size)
                    for workload, size in workload_cases()
                ]
                # Every runtime pair gets a representative fixed-record
                # interoperability check. These timings are never compared.
                server_cases.extend(
                    (clients[client_runtime], "cross-runtime", client_runtime,
                     "message", 1024)
                    for client_runtime in RUNTIMES
                )
                run_against_server(
                    servers[runtime], runtime, server_cases,
                    output_dir, evidence, server_evidence,
                )

        validate_artifact_ineligibility(output_dir)
        parse_diagnostics: list[dict[str, Any]] = []
        summarized_results = read_jsonl_objects(
            output_dir / "results.jsonl", parse_diagnostics
        )
        summarized_server_evidence = read_jsonl_objects(
            output_dir / "server-evidence.jsonl", parse_diagnostics
        )
        parse_diagnostics.extend(
            collect_server_log_parse_diagnostics(output_dir)
        )
        parse_diagnostics.extend(collect_case_parse_diagnostics(output_dir))
        write_conformance_summary(
            output_dir,
            summarized_results,
            summarized_server_evidence,
            manifest,
            status="passed",
            parse_diagnostics=parse_diagnostics,
        )

        print(
            "TCP benchmark conformance passed in both attribution directions; "
            "cross-runtime pairs were validated; see summary.md for the "
            "conformance report and unscored diagnostic timings."
        )
        return 0
    except (
        ConformanceError, OSError, ValueError, subprocess.TimeoutExpired,
    ) as error:
        if output_created:
            try:
                parse_diagnostics = []
                summarized_results = read_jsonl_objects(
                    output_dir / "results.jsonl", parse_diagnostics
                )
                summarized_server_evidence = read_jsonl_objects(
                    output_dir / "server-evidence.jsonl", parse_diagnostics
                )
                parse_diagnostics.extend(
                    collect_server_log_parse_diagnostics(output_dir)
                )
                parse_diagnostics.extend(collect_case_parse_diagnostics(output_dir))
                write_conformance_summary(
                    output_dir,
                    summarized_results,
                    summarized_server_evidence,
                    manifest or {"tested_revision": tested_revision()},
                    status="failed", error=str(error),
                    parse_diagnostics=parse_diagnostics,
                )
            except (ConformanceError, OSError, ValueError):
                pass
        print(f"TCP benchmark conformance failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
