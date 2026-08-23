#!/usr/bin/env python3
"""Run a test command while preserving complete failure evidence.

The tool mirrors combined stdout/stderr to the terminal and to a log file,
writes a small metadata JSON file, and returns the child process exit code.
It intentionally records only explicit environment overrides so that a local
user's full environment is not copied into evidence artifacts.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import TextIO


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def parse_env_assignment(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            f"environment override must be NAME=VALUE, got {value!r}"
        )
    name, raw = value.split("=", 1)
    if not name:
        raise argparse.ArgumentTypeError("environment override name is empty")
    return name, raw


def sanitize_label(label: str) -> str:
    allowed = []
    for ch in label:
        if ch.isalnum() or ch in ("-", "_", "."):
            allowed.append(ch)
        else:
            allowed.append("-")
    cleaned = "".join(allowed).strip(".-")
    return cleaned or "test"


def write_json(path: Path, payload: dict[str, object]) -> None:
    with path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")


def current_git_head() -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def stream_child(command: list[str], env: dict[str, str], log: TextIO) -> int:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
        env=env,
    )
    assert process.stdout is not None
    for line in process.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        log.write(line)
        log.flush()
    return process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a test command and preserve complete output metadata."
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="directory that will receive the combined log and metadata JSON",
    )
    parser.add_argument(
        "--label",
        default="test",
        help="basename for the generated log and metadata files",
    )
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        type=parse_env_assignment,
        metavar="NAME=VALUE",
        help=(
            "environment override for the child process; VALUE may contain "
            "{output_dir}, which expands to the absolute evidence directory"
        ),
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="test command to run, normally preceded by --",
    )

    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing command to run")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    label = sanitize_label(args.label)
    log_path = output_dir / f"{label}.log"
    metadata_path = output_dir / f"{label}.json"

    env = os.environ.copy()
    explicit_env: dict[str, str] = {}
    for name, raw in args.env:
        value = raw.replace("{output_dir}", str(output_dir))
        explicit_env[name] = value
        env[name] = value

    start_time = utc_now()
    metadata: dict[str, object] = {
        "command": command,
        "cwd": os.getcwd(),
        "end_time_utc": None,
        "environment_overrides": explicit_env,
        "exit_code": None,
        "git_head": current_git_head(),
        "log_path": str(log_path),
        "start_time_utc": start_time,
    }
    write_json(metadata_path, metadata)

    print(f"[capture-test-evidence] writing combined output to {log_path}")
    print(f"[capture-test-evidence] writing metadata to {metadata_path}")

    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"[capture-test-evidence] start_time_utc={start_time}\n")
        log.write("[capture-test-evidence] command=")
        log.write(json.dumps(command))
        log.write("\n")
        if explicit_env:
            log.write("[capture-test-evidence] environment_overrides=")
            log.write(json.dumps(explicit_env, sort_keys=True))
            log.write("\n")
        log.flush()
        exit_code = stream_child(command, env, log)
        end_time = utc_now()
        log.write(f"[capture-test-evidence] end_time_utc={end_time}\n")
        log.write(f"[capture-test-evidence] exit_code={exit_code}\n")

    metadata["end_time_utc"] = end_time
    metadata["exit_code"] = exit_code
    write_json(metadata_path, metadata)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
