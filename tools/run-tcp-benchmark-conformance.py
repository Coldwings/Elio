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
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import time
from typing import Any, TextIO


SCHEMA = "elio.tcp-loopback.v1"
HOST = "127.0.0.1"
RUNTIMES = ("elio", "libuv", "asio")
MESSAGE_SIZES = (64, 1024, 4096, 65536)
RECORDS = 32
WARMUP_RECORDS = 8
CREDIT_WINDOW = 4
BULK_BYTES = 64 * 1024
CHUNK_BYTES = 4 * 1024
TIMEOUT_SECONDS = 30


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
        ["git", "rev-parse", "HEAD"], text=True, capture_output=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else None


def git_dirty_state() -> bool | None:
    inside = subprocess.run(
        ["git", "rev-parse", "--is-inside-work-tree"],
        text=True, capture_output=True, check=False,
    )
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        return None
    status = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
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
    for line in stdout.splitlines():
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ConformanceError(f"{label} emitted malformed JSON") from error
        if isinstance(value, dict):
            objects.append(value)
    if len(objects) != 1:
        raise ConformanceError(
            f"{label} must emit one JSON result, emitted {len(objects)}"
        )
    return objects[0]


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
    workload: str, size: int,
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
    peer_implementation: str | None = None,
) -> list[str]:
    command = [
        str(client), "--host", HOST, "--port", str(port),
        "--mode", workload, "--message-size", str(size),
        "--records", str(RECORDS), "--warmup-records", str(WARMUP_RECORDS),
        "--credit-window", str(1 if workload == "latency" else CREDIT_WINDOW),
        "--bulk-bytes", str(BULK_BYTES), "--chunk-bytes", str(CHUNK_BYTES),
        "--json", str(json_path),
    ]
    if peer_implementation is not None:
        command.extend(["--peer-implementation", peer_implementation])
    return command


def run_case(
    command: list[str], label: str, log_dir: Path, implementation: str,
    role: str, peer: str, workload: str, size: int,
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
    (log_dir / f"{label}.stdout.log").write_text(completed.stdout, encoding="utf-8")
    (log_dir / f"{label}.stderr.log").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise ConformanceError(f"{label} exited with status {completed.returncode}")
    result = one_json(completed.stdout, label)
    validate_result(result, implementation, role, peer, workload, size)
    return result


Case = tuple[Path, str, str, str, int]


def server_evidence_objects(path: Path) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    if not path.exists():
        return objects
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ConformanceError(f"{path.name} contains malformed JSON") from error
        if isinstance(value, dict) and value.get("kind") == "server_connection":
            objects.append(value)
    return objects


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


def run_against_server(
    server_path: Path, server_name: str, cases: list[Case],
    output_dir: Path, evidence: TextIO, server_evidence: TextIO,
) -> None:
    port = reserve_port()
    server_log_path = output_dir / f"server-{server_name}.log"
    with server_log_path.open("w", encoding="utf-8") as server_log:
        server = subprocess.Popen(
            [str(server_path), "--port", str(port),
             "--chunk-bytes", str(CHUNK_BYTES)],
            text=True, stdout=server_log, stderr=subprocess.STDOUT,
        )
        try:
            wait_ready(server, port, server_name)
            for client, comparison, implementation, workload, size in cases:
                label = f"{comparison}-{implementation}-{server_name}-{workload}-{size}"
                result = run_case(
                    client_command(
                        client, port, workload, size,
                        output_dir / f"{label}.jsonl",
                        server_name if comparison != "client-reference" else None,
                    ),
                    label, output_dir, implementation,
                    "server" if comparison == "server-reference" else "client",
                    "posix-reference-client" if comparison == "server-reference"
                    else ("posix-reference" if comparison == "client-reference"
                          else server_name),
                    workload, size,
                )
                result["comparison"] = comparison
                # Public conformance runs preserve elapsed values for debugging,
                # but no row produced here is a publishable performance sample.
                result["performance_eligible"] = False
                evidence.write(json.dumps(result, sort_keys=True) + "\n")
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
        finally:
            stop_server(server)

    summaries = server_evidence_objects(server_log_path)
    if len(summaries) != len(cases):
        raise ConformanceError(
            f"{server_name} emitted {len(summaries)} server summaries; "
            f"expected {len(cases)}"
        )
    for index, summary in enumerate(summaries):
        validate_server_evidence(summary, server_name,
                                 f"{server_name}.connection[{index}]")
        server_evidence.write(json.dumps(summary, sort_keys=True) + "\n")
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
    server_evidence.flush()


def main() -> int:
    args = parse_args()
    try:
        source_was_dirty = git_dirty_state()
        output_dir: Path = args.output_dir
        output_dir.mkdir(parents=True, exist_ok=False)
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

        print(
            "TCP benchmark conformance passed in both attribution directions; "
            "cross-runtime pairs were validated and no timings were compared."
        )
        return 0
    except (ConformanceError, OSError, subprocess.TimeoutExpired) as error:
        print(f"TCP benchmark conformance failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
