#!/usr/bin/env python3
"""Exercise the real fixture's failure JSON; no failed transfer earns metrics."""
import argparse
import json
from pathlib import Path
import subprocess
import time
import uuid

import http_streaming_metrics as metrics


def check_failure(server, directory, scenario, certificate, key):
    directory.mkdir()
    transport = "tls" if scenario == "tls_truncation" else "tcp"
    command = [str(server), "--transport", transport, "--mode", "complete", "--trial", scenario]
    if transport == "tls":
        command += ["--cert", str(certificate), "--key", str(key)]
    metrics.write_json(directory / "invocation.json", command)
    row = {"scenario": scenario, "passed": False, "error": ""}
    peer = process = None
    try:
        with (directory / "stdout.jsonl").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
        ready = metrics.wait_ready(process, directory / "stdout.jsonl", time.monotonic() + 15)
        peer = metrics.Peer(ready["port"], transport, certificate, time.monotonic() + 15)
        if scenario == "tls_truncation":
            # Client handshake completion alone does not prove the server has
            # finished its handshake. Verify one response before disconnecting.
            peer.request(scenario, "warmup", 0, "complete", metrics.body_digest(), time.monotonic() + 15)
        elif scenario == "partial_request_eof":
            peer.socket.sendall(b"GET /")
        if scenario != "phase_timeout":
            peer.close()
            peer = None
        # The existing server phase budget is ten seconds, unchanged by this
        # test. Keep the peer open without a request for the supervision case.
        process.wait(timeout=25)
        row["returncode"] = process.returncode
        events = metrics.read_events(directory / "stdout.jsonl")
        metrics.write_json(directory / "events.json", events)
        metrics.require(process.returncode == 1, "fixture did not report failure")
        metrics.require([item["event"] for item in events] == ["ready", "result"], "wrong failure lifecycle")
        result = events[1]
        metrics.require(result["success"] is False and result["probe_success"] is False,
                        "failed exchange was marked successful")
        metrics.require(result["measured_count"] == 0 and result["confirmed_body_bytes"] == 0,
                        "failure contributed measured output")
        failure = result["failure"]
        metrics.require(failure["stage"] == "request_read", "wrong active stage")
        phase = "measure" if scenario == "tls_truncation" else "warmup"
        metrics.require(failure["request_phase"] == phase and failure["request_sequence"] == 0,
                        "wrong request attribution")
        metrics.require(failure["request_complete"] is False, "partial request marked complete")
        metrics.require(failure["request_bytes"] == (5 if scenario == "partial_request_eof" else 0),
                        "wrong request progress")
        metrics.require(metrics.integer(failure["request_elapsed_ns"]), "missing request elapsed time")
        metrics.require(failure["error_type"] == "std::exception" and failure["error"] == "request read failed",
                        "missing server exception")
        read = failure["read_result"]
        metrics.require(isinstance(read, dict) and type(read["result"]) is int and metrics.integer(read["flags"]),
                        "missing exact read result/flags")
        metrics.require(read["result"] == 0 if scenario == "partial_request_eof" else read["result"] < 0,
                        "wrong EOF/error classification")
        cause = "phase_deadline" if scenario == "phase_timeout" else "none"
        metrics.require(failure["supervision_cause"] == cause, "wrong supervision attribution")
        row["passed"] = True
    except Exception as error:
        row["error"] = f"{type(error).__name__}: {error}"
    finally:
        def cleanup(action):
            try:
                action()
            except Exception as error:
                row["passed"] = False
                row["error"] += f"; cleanup: {type(error).__name__}: {error}"
        if peer is not None:
            cleanup(peer.close)
        if process is not None and process.poll() is None:
            cleanup(process.terminate)
            cleanup(lambda: process.wait(timeout=5))
            if process.poll() is None:
                cleanup(process.kill)
                cleanup(lambda: process.wait(timeout=5))
        metrics.write_json(directory / "result.json", row)
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    output = args.output_root.resolve() / uuid.uuid4().hex
    output.mkdir(parents=True)
    expected = ("partial_request_eof", "tls_truncation", "phase_timeout")
    rows = []
    try:
        metrics.require(metrics.h11.__version__ == metrics.H11_VERSION, "requires h11==0.16.0")
        certificate, key = metrics.generate_certificate(output)
        rows = [check_failure(args.server.resolve(), output / scenario, scenario, certificate, key)
                for scenario in expected]
    except Exception as error:
        rows += [{"scenario": name, "passed": False, "error": str(error)} for name in expected[len(rows):]]
    summary = {"success": len(rows) == len(expected) and all(row["passed"] for row in rows),
               "scenarios": rows, "performance_eligible": False}
    metrics.write_json(output / "summary.json", summary)
    print(json.dumps({**summary, "output_dir": str(output)}))
    return 0 if summary["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
