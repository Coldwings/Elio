#!/usr/bin/env python3
"""Run controlled, paired TCP loopback performance comparisons.

Unlike run-tcp-benchmark-conformance.py, this driver is intended only for an
otherwise-idle controlled host.  It never runs in public CI.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import signal
import shutil
import socket
import statistics
import subprocess
import sys
import time
from typing import Any, Iterable, TextIO


SCHEMA = "elio.tcp-loopback.v1"
RUNNER_SCHEMA = "elio.tcp-performance-comparison.v1"
HOST = "127.0.0.1"
RUNTIMES = ("elio", "libuv", "asio")
DEFAULT_SIZES = (64, 1024, 4096, 65536)
MINIMUM_PUBLISHABLE_MEASURED_MS = 250.0


class ComparisonError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_csv(text: str) -> list[str]:
    values = [value.strip() for value in text.split(",") if value.strip()]
    if not values:
        raise argparse.ArgumentTypeError("the list must not be empty")
    return values


def parse_sizes(text: str) -> tuple[int, ...]:
    try:
        sizes = tuple(int(value) for value in parse_csv(text))
    except ValueError as error:
        raise argparse.ArgumentTypeError("message sizes must be integers") from error
    if any(size < 32 or size > 64 * 1024 * 1024 for size in sizes):
        raise argparse.ArgumentTypeError("message sizes must be in [32, 64 MiB]")
    if len(set(sizes)) != len(sizes):
        raise argparse.ArgumentTypeError("message sizes must not contain duplicates")
    return sizes


def parse_workloads(text: str) -> tuple[str, ...]:
    values = tuple(parse_csv(text))
    unknown = set(values) - {"latency", "message", "bulk"}
    if unknown:
        raise argparse.ArgumentTypeError(
            "unknown workloads: " + ", ".join(sorted(unknown))
        )
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError("workloads must not contain duplicates")
    return values


def parse_cpuset(text: str) -> frozenset[int]:
    cpus: set[int] = set()
    try:
        for part in parse_csv(text):
            if "-" in part:
                first_text, last_text = part.split("-", 1)
                first, last = int(first_text), int(last_text)
                if first < 0 or last < first:
                    raise ValueError
                cpus.update(range(first, last + 1))
            else:
                cpu = int(part)
                if cpu < 0:
                    raise ValueError
                cpus.add(cpu)
    except ValueError as error:
        raise argparse.ArgumentTypeError("invalid CPU set") from error
    if not cpus:
        raise argparse.ArgumentTypeError("CPU set must not be empty")
    return frozenset(cpus)


def positive(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def nonnegative(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run balanced TCP client/server performance comparisons"
    )
    parser.add_argument("--reference-server", required=True, type=Path)
    parser.add_argument("--reference-client", required=True, type=Path)
    for runtime in RUNTIMES:
        parser.add_argument(f"--{runtime}-client", required=True, type=Path)
        parser.add_argument(f"--{runtime}-server", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--blocks", type=positive, default=18)
    parser.add_argument("--seed", type=int, default=1145)
    parser.add_argument(
        "--workloads", type=parse_workloads,
        default=("latency", "message", "bulk"),
    )
    parser.add_argument(
        "--message-sizes", type=parse_sizes, default=DEFAULT_SIZES,
    )
    parser.add_argument("--records", type=positive, default=100000)
    parser.add_argument("--warmup-records", type=nonnegative, default=1000)
    parser.add_argument("--credit-window", type=positive, default=16)
    parser.add_argument("--bulk-bytes", type=positive, default=1 << 30)
    parser.add_argument("--chunk-bytes", type=positive, default=256 << 10)
    parser.add_argument("--bootstrap-resamples", type=positive, default=20000)
    parser.add_argument("--timeout-seconds", type=positive, default=600)
    parser.add_argument(
        "--minimum-measured-ms", type=float,
        default=MINIMUM_PUBLISHABLE_MEASURED_MS,
        help="publishability floor in milliseconds; may be raised but not lowered",
    )
    parser.add_argument("--build-metadata", type=Path)
    parser.add_argument(
        "--reference-peer-max-cpu-percent", type=float, default=90.0,
        help="mark a trial ineligible when its reference peer reaches this fraction of one CPU",
    )
    parser.add_argument("--client-cpus", type=parse_cpuset)
    parser.add_argument("--server-cpus", type=parse_cpuset)
    parser.add_argument(
        "--dedicated-host", action="store_true",
        help="assert that the host is otherwise idle and controlled",
    )
    args = parser.parse_args()
    if args.blocks < 6 or args.blocks % 6 != 0:
        parser.error("--blocks must be a multiple of 6 and at least 6")
    if args.bulk_bytes % args.chunk_bytes != 0:
        parser.error("--bulk-bytes must be a multiple of --chunk-bytes")
    if args.credit_window > 65536:
        parser.error("--credit-window must not exceed 65536")
    if args.bootstrap_resamples < 1000:
        parser.error("--bootstrap-resamples must be at least 1000")
    if not 0.0 < args.reference_peer_max_cpu_percent <= 100.0:
        parser.error("--reference-peer-max-cpu-percent must be in (0, 100]")
    if args.minimum_measured_ms < MINIMUM_PUBLISHABLE_MEASURED_MS:
        parser.error("--minimum-measured-ms must be at least 250")
    if (args.client_cpus is None) != (args.server_cpus is None):
        parser.error("--client-cpus and --server-cpus must be supplied together")
    if args.client_cpus is not None and args.client_cpus & args.server_cpus:
        parser.error("client and server CPU sets must be disjoint")
    return args


def git_output(
    repository_root: Path, arguments: list[str], binary: bool = False,
) -> str | bytes | None:
    completed = subprocess.run(
        ["git", "-C", str(repository_root), *arguments],
        capture_output=True, check=False,
        text=not binary,
    )
    if completed.returncode != 0:
        return None
    return completed.stdout


def capture_repository_state() -> dict[str, Any]:
    """Capture dirty state before an output directory can change the worktree."""
    repository_root = Path(__file__).resolve().parent.parent
    revision = git_output(repository_root, ["rev-parse", "HEAD"])
    status = git_output(
        repository_root, ["status", "--porcelain=v1", "--untracked-files=all"]
    )
    diff = git_output(repository_root, ["diff", "--binary", "HEAD"], binary=True)
    status_text = status.strip() if isinstance(status, str) else None
    state_available = (
        isinstance(revision, str) and isinstance(status, str)
        and isinstance(diff, bytes)
    )
    return {
        "root": str(repository_root),
        "state_available": state_available,
        "revision": revision.strip() if isinstance(revision, str) else None,
        "dirty": not state_available or bool(status_text),
        "status_porcelain": status_text.splitlines() if status_text else [],
        "tracked_diff_sha256": (
            hashlib.sha256(diff).hexdigest() if isinstance(diff, bytes) else None
        ),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_build_metadata(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    resolved = path.resolve()
    try:
        content = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ComparisonError(f"cannot read build metadata {path}: {error}") from error
    if not isinstance(content, dict):
        raise ComparisonError("--build-metadata must contain one JSON object")
    required_strings = (
        "compiler", "compiler_version", "build_type", "cxxflags", "ldflags",
        "source_revision",
    )
    for field in required_strings:
        if not isinstance(content.get(field), str):
            raise ComparisonError(
                f"build metadata field {field!r} must be a string"
            )
    if not all(content[field] for field in (
        "compiler", "compiler_version", "build_type", "source_revision"
    )):
        raise ComparisonError("build metadata identity fields must not be empty")
    if not isinstance(content.get("cmake_options"), (dict, list)):
        raise ComparisonError(
            "build metadata field 'cmake_options' must be an object or array"
        )
    return {
        "path": str(resolved), "sha256": sha256_file(resolved), "content": content,
    }


def executable(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ComparisonError(f"{label} is not executable: {path}")
    return resolved


def cpu_model() -> str | None:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def host_controls(cpus: Iterable[int]) -> dict[str, Any]:
    return {
        "cpu_frequency": {
            str(cpu): {
                name: read_text(
                    Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/{name}")
                )
                for name in (
                    "scaling_driver", "scaling_governor", "scaling_min_freq",
                    "scaling_max_freq", "scaling_cur_freq",
                )
            }
            for cpu in sorted(cpus)
        },
        "socket_settings": {
            name: read_text(Path("/proc/sys") / relative)
            for name, relative in {
                "tcp_congestion_control": "net/ipv4/tcp_congestion_control",
                "tcp_rmem": "net/ipv4/tcp_rmem",
                "tcp_wmem": "net/ipv4/tcp_wmem",
                "rmem_max": "net/core/rmem_max",
                "wmem_max": "net/core/wmem_max",
            }.items()
        },
        "cpu_boost": read_text(Path("/sys/devices/system/cpu/cpufreq/boost")),
        "intel_no_turbo": read_text(Path("/sys/devices/system/cpu/intel_pstate/no_turbo")),
        "loopback_mtu": read_text(Path("/sys/class/net/lo/mtu")),
        "tcp_nodelay": True,
    }


def canonical_cpuset(cpus: frozenset[int] | None) -> str | None:
    return None if cpus is None else ",".join(str(cpu) for cpu in sorted(cpus))


def sysfs_thread_siblings(cpu: int) -> frozenset[int]:
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
    try:
        return parse_cpuset(path.read_text(encoding="utf-8").strip())
    except (OSError, argparse.ArgumentTypeError) as error:
        raise ComparisonError(f"cannot validate SMT siblings for CPU {cpu}: {error}") from error


def validate_affinity(args: argparse.Namespace) -> dict[str, Any]:
    if args.client_cpus is None:
        reasons = ["client/server CPU affinity was not provided; smoke mode only"]
        if not args.dedicated_host:
            reasons.append(
                "--dedicated-host operator assertion was not provided; smoke mode only"
            )
        return {
            "controlled": False,
            "affinity_validated": False,
            "dedicated_host_asserted": args.dedicated_host,
            "performance_eligible": False,
            "reasons": reasons,
            "thread_siblings": None,
        }
    if shutil.which("taskset") is None:
        raise ComparisonError("taskset is required when CPU affinity is requested")
    if not hasattr(os, "sched_getaffinity"):
        raise ComparisonError("CPU affinity validation is unavailable")
    allowed = set(os.sched_getaffinity(0))
    requested = set(args.client_cpus | args.server_cpus)
    if not requested <= allowed:
        raise ComparisonError(
            f"requested CPUs {sorted(requested)} are outside allowed CPUs {sorted(allowed)}"
        )
    siblings = {cpu: sysfs_thread_siblings(cpu) for cpu in sorted(requested)}
    selected = sorted(requested)
    for index, cpu in enumerate(selected):
        for other in selected[index + 1:]:
            if other in siblings[cpu]:
                raise ComparisonError(
                    f"selected CPUs {cpu} and {other} are SMT siblings; use distinct physical cores"
                )
    return {
        "controlled": args.dedicated_host,
        "affinity_validated": True,
        "dedicated_host_asserted": args.dedicated_host,
        "performance_eligible": args.dedicated_host,
        "reasons": (
            [] if args.dedicated_host else
            ["--dedicated-host operator assertion was not provided; smoke mode only"]
        ),
        "thread_siblings": {
            str(cpu): sorted(values) for cpu, values in siblings.items()
        },
    }


def affinitized(command: list[str], cpus: frozenset[int] | None) -> list[str]:
    if cpus is None:
        return command
    return ["taskset", "-c", canonical_cpuset(cpus), *command]


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((HOST, 0))
        return int(probe.getsockname()[1])


def wait_ready(process: subprocess.Popen[str], port: int, label: str) -> None:
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ComparisonError(
                f"{label} exited before readiness ({process.returncode})"
            )
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise ComparisonError(f"{label} did not accept connections")


def stop_server(process: subprocess.Popen[str]) -> dict[str, Any]:
    if process.poll() is not None:
        return {
            "ok": False, "was_alive": False, "forced_kill": False,
            "returncode": process.returncode,
            "error": "server exited before runner-initiated shutdown",
        }
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)
        return {
            "ok": False, "was_alive": True, "forced_kill": True,
            "returncode": process.returncode,
            "error": "server required SIGKILL after shutdown timeout",
        }
    expected = process.returncode in (0, -signal.SIGTERM)
    return {
        "ok": expected, "was_alive": True, "forced_kill": False,
        "returncode": process.returncode,
        "error": None if expected else "server returned unexpected shutdown status",
    }


def balanced_orders(seed: int, blocks: int, domain: str) -> list[tuple[str, ...]]:
    rng = random.Random(f"{seed}:{domain}")
    permutations = list(itertools.permutations(RUNTIMES))
    orders: list[tuple[str, ...]] = []
    for _ in range(blocks // len(permutations)):
        cycle = permutations.copy()
        rng.shuffle(cycle)
        orders.extend(cycle)
    return orders


def workload_cases(args: argparse.Namespace) -> list[tuple[str, int]]:
    cases: list[tuple[str, int]] = []
    for workload in args.workloads:
        if workload == "bulk":
            cases.append((workload, args.chunk_bytes))
        else:
            cases.extend((workload, size) for size in args.message_sizes)
    return cases


def trial_id(seed: int, label: str) -> int:
    value = int.from_bytes(
        hashlib.sha256(f"{seed}:{label}".encode()).digest()[:8], "big"
    )
    return value or 1


def one_json(stdout: str, label: str) -> dict[str, Any]:
    values: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ComparisonError(f"{label} emitted malformed JSON") from error
        if isinstance(value, dict):
            values.append(value)
    if len(values) != 1:
        raise ComparisonError(f"{label} emitted {len(values)} JSON results, expected 1")
    return values[0]


def integer(mapping: dict[str, Any], field: str, label: str) -> int:
    value = mapping.get(field)
    if type(value) is not int or value < 0:
        raise ComparisonError(f"{label}.{field} must be a non-negative integer")
    return value


def validate_result(
    result: dict[str, Any], args: argparse.Namespace, implementation: str,
    role: str, workload: str, size: int, trial: int,
) -> None:
    label = f"{role}/{implementation}/{workload}/{size}"
    expected = {
        "schema_version": SCHEMA,
        "implementation": implementation,
        "role": role,
        "peer": "posix-reference" if role == "client" else "posix-reference-client",
        "counter_scope": "adapter" if role == "client" else "driver",
        "workload": workload,
    }
    for field, wanted in expected.items():
        if result.get(field) != wanted:
            raise ComparisonError(
                f"{label}.{field}={result.get(field)!r}, expected {wanted!r}"
            )
    if integer(result, "trial", label) != trial:
        raise ComparisonError(f"{label}.trial does not match scheduled trial {trial}")
    if result.get("valid") is not True or result.get("invariant_failures") != []:
        raise ComparisonError(f"{label} reported an invalid result")
    if result.get("warmup_drained") is not True or integer(result, "elapsed_ns", label) == 0:
        raise ComparisonError(f"{label} has an invalid phase boundary")
    measured = args.bulk_bytes // args.chunk_bytes if workload == "bulk" else args.records
    configuration = result.get("configuration")
    counters = result.get("counters")
    if not isinstance(configuration, dict) or not isinstance(counters, dict):
        raise ComparisonError(f"{label} lacks configuration/counters")
    config_expected = {
        "write_size_bytes": size,
        "credit_window": 1 if workload == "latency" else args.credit_window,
        "warmup_records": args.warmup_records,
        "measured_records": measured,
    }
    for field, wanted in config_expected.items():
        if integer(configuration, field, label + ".configuration") != wanted:
            raise ComparisonError(f"{label}.configuration.{field} != {wanted}")
    for phase, count in (("warmup", args.warmup_records), ("measured", measured)):
        values = counters.get(phase)
        if not isinstance(values, dict):
            raise ComparisonError(f"{label} lacks {phase} counters")
        for field in (
            "write_submissions", "write_completions", "sent_records",
            "received_records", "verified_records",
        ):
            if integer(values, field, label) != count:
                raise ComparisonError(f"{label}.{phase}.{field} != {count}")
        for field in ("sent_bytes", "received_bytes", "verified_bytes"):
            if integer(values, field, label) != count * size:
                raise ComparisonError(f"{label}.{phase}.{field} has wrong byte count")
        if integer(values, "max_write_operations_in_flight", label) != (1 if count else 0):
            raise ComparisonError(f"{label}.{phase} violates single-writer accounting")
        if integer(values, "current_write_operations_in_flight", label) != 0:
            raise ComparisonError(f"{label}.{phase} retained an active write")
        if integer(values, "max_unacknowledged_records", label) > config_expected["credit_window"]:
            raise ComparisonError(f"{label}.{phase} exceeded the credit window")
        if integer(values, "error_count", label) != 0:
            raise ComparisonError(f"{label}.{phase} reported errors")
    expected_samples = args.records if workload == "latency" else 0
    if integer(result, "latency_sample_count", label) != expected_samples:
        raise ComparisonError(f"{label} has the wrong latency sample count")
    metrics = result.get("metrics")
    if not isinstance(metrics, dict):
        raise ComparisonError(f"{label} lacks metrics")
    primary = (
        metrics.get("latency_ns", {}).get("p50")
        if workload == "latency" else
        metrics.get("records_per_second")
        if workload == "message" else metrics.get("mib_per_second")
    )
    if not isinstance(primary, (int, float)) or primary <= 0 or not math.isfinite(primary):
        raise ComparisonError(f"{label} has an invalid primary metric")


def client_command(
    binary: Path, args: argparse.Namespace, port: int, workload: str, size: int,
    json_path: Path, trial: int, server_implementation: str | None = None,
) -> list[str]:
    command = [
        str(binary), "--host", HOST, "--port", str(port),
        "--mode", workload, "--message-size", str(size),
        "--records", str(args.records),
        "--warmup-records", str(args.warmup_records),
        "--credit-window", str(1 if workload == "latency" else args.credit_window),
        "--bulk-bytes", str(args.bulk_bytes),
        "--chunk-bytes", str(args.chunk_bytes), "--trial", str(trial),
        "--json", str(json_path),
    ]
    if server_implementation is not None:
        command.extend(["--peer-implementation", server_implementation])
    return command


def server_command(binary: Path, args: argparse.Namespace, port: int) -> list[str]:
    maximum = max((*args.message_sizes, args.chunk_bytes))
    return [
        str(binary), "--port", str(port), "--message-size", str(maximum),
        "--bulk-bytes", str(args.bulk_bytes),
        "--chunk-bytes", str(args.chunk_bytes),
    ]


def append_json_line(file: TextIO, value: dict[str, Any]) -> None:
    file.write(json.dumps(value, sort_keys=True) + "\n")
    file.flush()


def start_server(
    command: list[str], cpus: frozenset[int] | None, log_path: Path,
) -> tuple[subprocess.Popen[str], TextIO, list[str], str]:
    effective = affinitized(command, cpus)
    log = log_path.open("w", encoding="utf-8")
    started = utc_now()
    process = subprocess.Popen(
        effective, text=True, stdout=log, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    return process, log, effective, started


def run_trial(
    *, command: list[str], cpus: frozenset[int] | None, args: argparse.Namespace,
    label: str, metadata: dict[str, Any], logs: Path, commands: TextIO,
    implementation: str, role: str, workload: str, size: int, trial: int,
) -> dict[str, Any]:
    effective = affinitized(command, cpus)
    started_at = utc_now()
    started = time.monotonic_ns()
    process = subprocess.Popen(
        effective, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=args.timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    wall_elapsed = time.monotonic_ns() - started
    (logs / f"{label}.stdout.log").write_text(stdout, encoding="utf-8")
    (logs / f"{label}.stderr.log").write_text(stderr, encoding="utf-8")
    command_record = {
        **metadata, "kind": "client", "label": label, "command": effective,
        "cwd": os.getcwd(),
        "started_at_utc": started_at, "finished_at_utc": utc_now(),
        "wall_elapsed_ns": wall_elapsed, "returncode": process.returncode,
        "timed_out": timed_out,
    }
    append_json_line(commands, command_record)
    if timed_out:
        raise ComparisonError(f"{label} timed out after {args.timeout_seconds}s")
    if process.returncode != 0:
        raise ComparisonError(f"{label} exited with {process.returncode}")
    result = one_json(stdout, label)
    validate_result(result, args, implementation, role, workload, size, trial)
    return {
        **metadata, "label": label, "runner_trial_id": label,
        "wire_trial_id": trial, "server_evidence_label": label,
        "command": effective, "result": result,
    }


def apply_headroom(
    record: dict[str, Any], args: argparse.Namespace, control: dict[str, Any],
    process_cpu_ns: int, source: str,
) -> None:
    elapsed_ns = integer(record["result"], "elapsed_ns", record["label"])
    if process_cpu_ns <= 0 or elapsed_ns <= 0:
        raise ComparisonError(f"{record['label']} lacks measured-phase CPU evidence")
    utilization = 100.0 * process_cpu_ns / elapsed_ns
    below_threshold = utilization < args.reference_peer_max_cpu_percent
    long_enough = elapsed_ns >= args.minimum_measured_ms * 1_000_000.0
    reasons: list[str] = []
    if not control["performance_eligible"]:
        reasons.extend(control["reasons"])
    if not below_threshold:
        reasons.append("reference peer reached configured CPU utilization threshold")
    if not long_enough:
        reasons.append("measured phase was shorter than configured minimum")
    eligible = control["performance_eligible"] and below_threshold and long_enough
    record.update({
        "reference_peer_headroom": {
            "source": source,
            "process_cpu_ns": process_cpu_ns,
            "measured_wall_ns": elapsed_ns,
            "cpu_utilization_percent_of_one_cpu": utilization,
            "maximum_cpu_percent": args.reference_peer_max_cpu_percent,
            "below_threshold": below_threshold,
            "minimum_measured_ms": args.minimum_measured_ms,
            "measured_duration_eligible": long_enough,
        },
        "performance_eligible": eligible,
        "performance_ineligibility_reasons": reasons,
    })


def server_summaries(path: Path) -> list[dict[str, Any]]:
    summaries: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.lstrip().startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ComparisonError(f"malformed server evidence in {path}") from error
        if isinstance(value, dict) and value.get("kind") == "server_connection":
            summaries.append(value)
    return summaries


def validate_server_summary(
    value: dict[str, Any], implementation: str, warmup: int, measured: int,
    size: int, trial: int, label: str,
) -> None:
    if value.get("schema_version") != SCHEMA or value.get("implementation") != implementation:
        raise ComparisonError(f"{label} has the wrong server evidence identity")
    if value.get("valid") is not True:
        raise ComparisonError(f"{label} reported invalid server evidence")
    if integer(value, "trial", label) != trial:
        raise ComparisonError(f"{label} server trial ID does not match {trial}")
    expected = warmup + measured
    for field in (
        "received_records", "verified_records", "write_submissions",
        "write_completions",
    ):
        if integer(value, field, label) != expected:
            raise ComparisonError(f"{label}.{field} != {expected}")
    if integer(value, "warmup_records", label) != warmup:
        raise ComparisonError(f"{label} warmup count mismatch")
    if integer(value, "measured_records", label) != measured:
        raise ComparisonError(f"{label} measured count mismatch")
    if integer(value, "write_size_bytes", label) != size:
        raise ComparisonError(f"{label} write size mismatch")
    for field in (
        "received_bytes", "verified_bytes", "submitted_bytes", "completed_bytes",
    ):
        if integer(value, field, label) != expected * size:
            raise ComparisonError(f"{label}.{field} != {expected * size}")
    if integer(value, "warmup_bytes", label) != warmup * size:
        raise ComparisonError(f"{label} warmup byte count mismatch")
    if integer(value, "measured_bytes", label) != measured * size:
        raise ComparisonError(f"{label} measured byte count mismatch")
    if integer(value, "current_write_operations_in_flight", label) != 0:
        raise ComparisonError(f"{label} retained an active server write")
    if integer(value, "connection_process_cpu_ns", label) == 0:
        raise ComparisonError(f"{label} lacks server connection process CPU time")
    if integer(value, "max_write_operations_in_flight", label) != 1:
        raise ComparisonError(f"{label} violates the server single-writer contract")
    for field in ("integrity_errors", "transport_errors", "accounting_errors"):
        if integer(value, field, label) != 0:
            raise ComparisonError(f"{label} reported {field}")


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def describe(values: list[float]) -> dict[str, float | int]:
    median = statistics.median(values)
    q1, q3 = quantile(values, 0.25), quantile(values, 0.75)
    return {
        "samples": len(values), "median": median,
        "mad": statistics.median(abs(value - median) for value in values),
        "q1": q1, "q3": q3, "iqr": q3 - q1,
        "minimum": min(values), "maximum": max(values),
    }


def metric_values(result: dict[str, Any]) -> dict[str, float]:
    metrics = result["metrics"]
    values: dict[str, float] = {"mib_per_second": float(metrics["mib_per_second"])}
    if "records_per_second" in metrics:
        values["records_per_second"] = float(metrics["records_per_second"])
    for name, value in metrics.get("latency_ns", {}).items():
        values[f"latency_{name}_ns"] = float(value)
    return values


def primary_metric(workload: str) -> tuple[str, bool]:
    if workload == "latency":
        return "latency_p50_ns", False
    if workload == "message":
        return "records_per_second", True
    return "mib_per_second", True


def paired_ci(
    left: dict[int, float], right: dict[int, float], seed: int,
    resamples: int,
) -> dict[str, Any]:
    blocks = sorted(set(left) & set(right))
    if len(blocks) < 6:
        raise ComparisonError("paired comparison requires at least six blocks")
    log_ratios = [math.log(right[block] / left[block]) for block in blocks]
    point = math.expm1(statistics.median(log_ratios)) * 100.0
    rng = random.Random(seed)
    bootstrapped: list[float] = []
    for _ in range(resamples):
        sample = [rng.choice(log_ratios) for _ in log_ratios]
        bootstrapped.append(math.expm1(statistics.median(sample)) * 100.0)
    lower, upper = quantile(bootstrapped, 0.025), quantile(bootstrapped, 0.975)
    half_width = (upper - lower) / 2.0
    direction = "right_higher" if lower > 0 else "right_lower" if upper < 0 else "unresolved"
    resolution = "resolved" if half_width <= 2.0 else "inconclusive"
    return {
        "paired_blocks": len(blocks),
        "right_over_left_median_change_percent": point,
        "ci95_percent": [lower, upper],
        "ci_half_width_percentage_points": half_width,
        "direction": direction,
        "two_percent_resolution": resolution,
        "status": (
            "inconclusive_at_2_percent" if resolution == "inconclusive"
            else "direction_unresolved" if direction == "unresolved"
            else "resolved"
        ),
    }


def aggregate(
    records: list[dict[str, Any]], seed: int, resamples: int, blocks: int,
) -> dict[str, Any]:
    grouped: dict[tuple[str, str, str, int], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        result = record["result"]
        key = (
            result["role"], result["implementation"], result["workload"],
            int(result["configuration"]["write_size_bytes"]),
        )
        grouped[key].append(record)
    groups: list[dict[str, Any]] = []
    by_case: dict[
        tuple[str, str, int], dict[str, tuple[dict[int, float], bool, list[str]]]
    ] = defaultdict(dict)
    for (role, implementation, workload, size), samples in sorted(grouped.items()):
        sample_blocks = [int(sample["block"]) for sample in samples]
        if len(samples) != blocks or set(sample_blocks) != set(range(blocks)):
            raise ComparisonError(
                f"{role}/{implementation}/{workload}/{size} does not have "
                f"exactly one sample for each of {blocks} blocks"
            )
        metrics: dict[str, list[float]] = defaultdict(list)
        primary, higher = primary_metric(workload)
        block_values: dict[int, float] = {}
        ineligible = [sample for sample in samples if not sample["performance_eligible"]]
        ineligibility_reasons = sorted({
            reason for sample in ineligible
            for reason in sample["performance_ineligibility_reasons"]
        })
        for sample in samples:
            values = metric_values(sample["result"])
            for name, value in values.items():
                metrics[name].append(value)
            block_values[int(sample["block"])] = values[primary]
        metric_statistics = {
            name: describe(values) for name, values in sorted(metrics.items())
        }
        groups.append({
            "role": role, "implementation": implementation,
            "workload": workload, "write_size_bytes": size,
            "axis": "client-reference" if role == "client" else "server-reference",
            "primary_metric": primary, "higher_is_better": higher,
            "qualified": not ineligible,
            "samples": len(samples),
            "ineligible_samples": len(ineligible),
            "qualification_reasons": ineligibility_reasons,
            "metrics": None if ineligible else metric_statistics,
        })
        by_case[(role, workload, size)][implementation] = (
            block_values, not ineligible, ineligibility_reasons,
        )

    comparisons: list[dict[str, Any]] = []
    for (role, workload, size), implementations in sorted(by_case.items()):
        metric, higher = primary_metric(workload)
        for left, right in itertools.combinations(RUNTIMES, 2):
            if left not in implementations or right not in implementations:
                continue
            left_values, left_qualified, left_reasons = implementations[left]
            right_values, right_qualified, right_reasons = implementations[right]
            base = {
                "axis": "client-reference" if role == "client" else "server-reference",
                "role": role, "workload": workload,
                "write_size_bytes": size, "metric": metric,
                "higher_is_better": higher, "left": left, "right": right,
            }
            if not left_qualified or not right_qualified:
                comparisons.append({
                    **base, "qualified": False, "status": "unqualified",
                    "qualification_reasons": sorted(set(left_reasons + right_reasons)),
                    "right_over_left_median_change_percent": None,
                    "ci95_percent": None,
                    "two_percent_resolution": "inconclusive",
                })
                continue
            digest = hashlib.sha256(
                f"{seed}:{role}:{workload}:{size}:{left}:{right}".encode()
            ).digest()
            bootstrap_seed = int.from_bytes(digest[:8], "big")
            comparison = paired_ci(
                left_values, right_values, bootstrap_seed,
                resamples,
            )
            comparisons.append({
                **base, "qualified": True, "qualification_reasons": [],
                **comparison,
            })
    return {
        "schema_version": RUNNER_SCHEMA,
        "bootstrap_method": "paired block bootstrap of median log ratios",
        "bootstrap_resamples": resamples,
        "axes": {
            "client-reference": "runtime client measured against POSIX reference server",
            "server-reference": "runtime server measured by POSIX reference client",
        },
        "qualified": all(group["qualified"] for group in groups),
        "groups": groups,
        "paired_comparisons": comparisons,
    }


def write_markdown(summary: dict[str, Any], path: Path) -> None:
    lines = [
        "# TCP performance comparison", "",
        "Values marked unqualified are diagnostic only and must not be used for rankings.",
        "`inconclusive_at_2_percent` means the 95% interval is too wide to",
        "resolve a two-percentage-point effect reliably.", "",
        "## Aggregates", "",
        "| Role | Implementation | Workload | Size | Qualified | Primary median | MAD | IQR | N |",
        "|---|---|---|---:|---|---:|---:|---:|---:|",
    ]
    for group in summary["groups"]:
        if not group["qualified"]:
            lines.append(
                f"| {group['role']} | {group['implementation']} | {group['workload']} | "
                f"{group['write_size_bytes']} | false | suppressed | suppressed | "
                f"suppressed | {group['samples']} |"
            )
            continue
        stats = group["metrics"][group["primary_metric"]]
        lines.append(
            f"| {group['role']} | {group['implementation']} | {group['workload']} | "
            f"{group['write_size_bytes']} | {str(group['qualified']).lower()} | "
            f"{stats['median']:.6g} | "
            f"{stats['mad']:.6g} | {stats['iqr']:.6g} | {stats['samples']} |"
        )
    lines.extend([
        "", "## Paired comparisons", "",
        "`change` is right/left minus one; the Better column states the",
        "direction of improvement for that metric.", "",
        "| Role | Workload | Size | Better | Left | Right | Change | 95% CI | 2% status |",
        "|---|---|---:|---|---|---|---:|---|---|",
    ])
    for value in summary["paired_comparisons"]:
        if not value["qualified"]:
            lines.append(
                f"| {value['role']} | {value['workload']} | {value['write_size_bytes']} | "
                f"{'higher' if value['higher_is_better'] else 'lower'} | "
                f"{value['left']} | {value['right']} | suppressed | suppressed | unqualified |"
            )
            continue
        low, high = value["ci95_percent"]
        lines.append(
            f"| {value['role']} | {value['workload']} | {value['write_size_bytes']} | "
            f"{'higher' if value['higher_is_better'] else 'lower'} | "
            f"{value['left']} | {value['right']} | "
            f"{value['right_over_left_median_change_percent']:.3f}% | "
            f"[{low:.3f}%, {high:.3f}%] | {value['status']} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    # This must happen before output_dir is created when output lives in-repo.
    repository = capture_repository_state()
    manifest: dict[str, Any] | None = None
    manifest_path: Path | None = None
    try:
        control = validate_affinity(args)
        if not repository["state_available"]:
            control["performance_eligible"] = False
            control["reasons"].append(
                "repository revision/status/diff could not be captured"
            )
        elif repository["dirty"]:
            control["performance_eligible"] = False
            control["reasons"].append(
                "repository is dirty; performance publication requires a clean revision"
            )
        if args.blocks < 18:
            control["performance_eligible"] = False
            control["reasons"].append(
                "fewer than 18 balanced blocks were requested; smoke mode only"
            )
        build_metadata = load_build_metadata(args.build_metadata)
        if build_metadata is None:
            control["performance_eligible"] = False
            control["reasons"].append(
                "--build-metadata was not provided; build provenance is incomplete"
            )
        elif build_metadata["content"]["source_revision"] != repository["revision"]:
            raise ComparisonError(
                "build metadata source_revision does not match repository HEAD"
            )
        control["controlled"] = control["performance_eligible"]
        binaries = {
            "reference_server": executable(args.reference_server, "reference server"),
            "reference_client": executable(args.reference_client, "reference client"),
            **{
                f"{runtime}_client": executable(
                    getattr(args, f"{runtime}_client"), f"{runtime} client"
                ) for runtime in RUNTIMES
            },
            **{
                f"{runtime}_server": executable(
                    getattr(args, f"{runtime}_server"), f"{runtime} server"
                ) for runtime in RUNTIMES
            },
        }
        client_orders = balanced_orders(args.seed, args.blocks, "client")
        server_orders = balanced_orders(args.seed, args.blocks, "server")
        cases = workload_cases(args)
        case_orders: list[list[tuple[str, int]]] = []
        for block in range(args.blocks):
            ordered = cases.copy()
            random.Random(f"{args.seed}:cases:{block}").shuffle(ordered)
            case_orders.append(ordered)

        output: Path = args.output_dir
        output.mkdir(parents=True, exist_ok=False)
        logs = output / "logs"
        logs.mkdir()
        manifest = {
            "schema_version": RUNNER_SCHEMA, "purpose": "controlled-performance",
            "status": "running", "started_at_utc": utc_now(),
            "repository": repository, "seed": args.seed, "blocks": args.blocks,
            "invocation_cwd": os.getcwd(),
            "invocation_argv": sys.argv,
            "output_dir": str(output.resolve()),
            "client_cpus": canonical_cpuset(args.client_cpus),
            "server_cpus": canonical_cpuset(args.server_cpus),
            "controlled_environment": control,
            "build_metadata": build_metadata,
            "allowed_cpus": (
                sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
            ),
            "platform": platform.platform(), "kernel": platform.release(),
            "machine": platform.machine(), "cpu_model": cpu_model(),
            "python": platform.python_version(),
            "host_controls": host_controls(
                (args.client_cpus or frozenset()) | (args.server_cpus or frozenset())
            ),
            "runner": {
                "path": str(Path(__file__).resolve()),
                "sha256": sha256_file(Path(__file__).resolve()),
            },
            "environment": {
                name: os.environ.get(name)
                for name in (
                    "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                    "CMAKE_BUILD_TYPE", "ELIO_BUILD_TCP_BENCHMARKS",
                )
            },
            "configuration": {
                "workloads": args.workloads, "message_sizes": args.message_sizes,
                "records": args.records, "warmup_records": args.warmup_records,
                "credit_window": args.credit_window,
                "bulk_bytes": args.bulk_bytes, "chunk_bytes": args.chunk_bytes,
                "bootstrap_resamples": args.bootstrap_resamples,
                "reference_peer_max_cpu_percent": (
                    args.reference_peer_max_cpu_percent
                ),
                "minimum_measured_ms": args.minimum_measured_ms,
            },
            "client_orders": client_orders, "server_orders": server_orders,
            "case_orders": case_orders,
            "executables": {
                name: {"path": str(path), "sha256": sha256_file(path)}
                for name, path in binaries.items()
            },
        }
        manifest_path = output / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        records: list[dict[str, Any]] = []
        scheduled_trials: set[int] = set()
        with (output / "commands.jsonl").open("x", encoding="utf-8") as commands, \
             (output / "trials.jsonl").open("x", encoding="utf-8") as raw, \
             (output / "server-evidence.jsonl").open("x", encoding="utf-8") as evidence:
            for block in range(args.blocks):
                # Client attribution: all runtime clients share one reference server.
                port = reserve_port()
                server_log = logs / f"client-reference-block-{block:03d}-server.log"
                process, log, command, started = start_server(
                    server_command(binaries["reference_server"], args, port),
                    args.server_cpus, server_log,
                )
                expected_reference: list[dict[str, Any]] = []
                server_body_failed = False
                try:
                    wait_ready(process, port, "reference server")
                    for workload, size in case_orders[block]:
                        measured = (
                            args.bulk_bytes // args.chunk_bytes
                            if workload == "bulk" else args.records
                        )
                        for order_index, runtime in enumerate(client_orders[block]):
                            label = (
                                f"b{block:03d}-client-{workload}-{size}-"
                                f"o{order_index}-{runtime}"
                            )
                            wire_trial = trial_id(args.seed, label)
                            if wire_trial in scheduled_trials:
                                raise ComparisonError("deterministic wire trial ID collision")
                            scheduled_trials.add(wire_trial)
                            metadata = {
                                "axis": "client-reference", "block": block,
                                "order_index": order_index,
                            }
                            record = run_trial(
                                command=client_command(
                                    binaries[f"{runtime}_client"], args, port,
                                    workload, size, output / f"{label}.jsonl",
                                    wire_trial,
                                ),
                                cpus=args.client_cpus, args=args, label=label,
                                metadata=metadata, logs=logs, commands=commands,
                                implementation=runtime, role="client",
                                workload=workload, size=size, trial=wire_trial,
                            )
                            expected_reference.append({
                                "axis": "client-reference", "block": block,
                                "runtime": runtime, "workload": workload,
                                "write_size_bytes": size, "label": label,
                                "warmup": args.warmup_records,
                                "measured": measured,
                                "trial": wire_trial, "record": record,
                            })
                except BaseException:
                    server_body_failed = True
                    raise
                finally:
                    shutdown = stop_server(process)
                    log.close()
                    append_json_line(commands, {
                        "kind": "server", "axis": "client-reference",
                        "block": block, "command": command, "cwd": os.getcwd(),
                        "started_at_utc": started, "finished_at_utc": utc_now(),
                        "returncode": process.returncode, "shutdown": shutdown,
                    })
                    if not shutdown["ok"] and not server_body_failed:
                        raise ComparisonError(str(shutdown["error"]))
                summaries = server_summaries(server_log)
                if len(summaries) != len(expected_reference):
                    raise ComparisonError(
                        "reference-server evidence count does not match scheduled client trials"
                    )
                for index, (summary, expected) in enumerate(
                    zip(summaries, expected_reference)
                ):
                    validate_server_summary(
                        summary, "posix-reference", expected["warmup"],
                        expected["measured"], expected["write_size_bytes"],
                        expected["trial"],
                        f"reference-server[{index}]",
                    )
                    apply_headroom(
                        expected["record"], args, control,
                        integer(summary, "connection_process_cpu_ns", expected["label"]),
                        "server-evidence.connection_process_cpu_ns",
                    )
                    append_json_line(raw, expected["record"])
                    records.append(expected["record"])
                    append_json_line(evidence, {
                        **{key: value for key, value in expected.items()
                           if key not in ("warmup", "measured", "record")},
                        "server_summary": summary,
                    })

                # Server attribution: one common reference client drives each server.
                for workload, size in case_orders[block]:
                    measured = (
                        args.bulk_bytes // args.chunk_bytes
                        if workload == "bulk" else args.records
                    )
                    for order_index, runtime in enumerate(server_orders[block]):
                        port = reserve_port()
                        label = (
                            f"b{block:03d}-server-{workload}-{size}-"
                            f"o{order_index}-{runtime}"
                        )
                        wire_trial = trial_id(args.seed, label)
                        if wire_trial in scheduled_trials:
                            raise ComparisonError("deterministic wire trial ID collision")
                        scheduled_trials.add(wire_trial)
                        server_log = logs / f"{label}-server.log"
                        process, log, command, started = start_server(
                            server_command(binaries[f"{runtime}_server"], args, port),
                            args.server_cpus, server_log,
                        )
                        server_body_failed = False
                        try:
                            wait_ready(process, port, f"{runtime} server")
                            record = run_trial(
                                command=client_command(
                                    binaries["reference_client"], args, port,
                                    workload, size, output / f"{label}.jsonl",
                                    wire_trial, runtime,
                                ),
                                cpus=args.client_cpus, args=args, label=label,
                                metadata={
                                    "axis": "server-reference", "block": block,
                                    "order_index": order_index,
                                },
                                logs=logs, commands=commands,
                                implementation=runtime, role="server",
                                workload=workload, size=size, trial=wire_trial,
                            )
                        except BaseException:
                            server_body_failed = True
                            raise
                        finally:
                            shutdown = stop_server(process)
                            log.close()
                            append_json_line(commands, {
                                "kind": "server", "axis": "server-reference",
                                "block": block, "implementation": runtime,
                                "workload": workload, "write_size_bytes": size,
                                "command": command, "cwd": os.getcwd(),
                                "started_at_utc": started,
                                "finished_at_utc": utc_now(),
                                "returncode": process.returncode,
                                "shutdown": shutdown,
                            })
                            if not shutdown["ok"] and not server_body_failed:
                                raise ComparisonError(str(shutdown["error"]))
                        summaries = server_summaries(server_log)
                        if len(summaries) != 1:
                            raise ComparisonError(
                                f"{label} emitted {len(summaries)} server summaries"
                            )
                        validate_server_summary(
                            summaries[0], runtime, args.warmup_records, measured,
                            size, wire_trial, label,
                        )
                        apply_headroom(
                            record, args, control,
                            integer(
                                record["result"], "process_cpu_elapsed_ns", label
                            ),
                            "result.process_cpu_elapsed_ns",
                        )
                        append_json_line(raw, record)
                        records.append(record)
                        append_json_line(evidence, {
                            "axis": "server-reference", "block": block,
                            "runtime": runtime, "workload": workload,
                            "write_size_bytes": size, "label": label,
                            "trial": wire_trial,
                            "server_summary": summaries[0],
                        })

        summary = aggregate(
            records, args.seed, args.bootstrap_resamples, args.blocks
        )
        (output / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        write_markdown(summary, output / "summary.md")
        manifest["status"] = "complete"
        manifest["finished_at_utc"] = utc_now()
        manifest["trial_count"] = len(records)
        manifest["performance_qualified"] = summary["qualified"]
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"completed {len(records)} paired trials; see {output / 'summary.md'}")
        return 0
    except KeyboardInterrupt as error:
        if manifest is not None and manifest_path is not None:
            manifest["status"] = "interrupted"
            manifest["finished_at_utc"] = utc_now()
            manifest["error"] = "interrupted by operator"
            try:
                manifest_path.write_text(
                    json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
            except OSError:
                pass
        print("TCP performance comparison interrupted", file=sys.stderr)
        return 130
    except (ComparisonError, OSError, subprocess.TimeoutExpired) as error:
        if manifest is not None and manifest_path is not None:
            manifest["status"] = "failed"
            manifest["finished_at_utc"] = utc_now()
            manifest["error"] = str(error)
            try:
                manifest_path.write_text(
                    json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
            except OSError:
                pass
        print(f"TCP performance comparison failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
