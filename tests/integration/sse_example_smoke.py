#!/usr/bin/env python3
"""Run the actual SSE demo; decode an event and verify cooperative stop."""

import argparse
import http.client
import json
import signal
import socket
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    process = None
    connection = None
    scenarios = []
    first_event = bytearray()
    after_cancel = bytearray()
    failure = None
    cleanup_error = None

    def expired(_signal, _frame):
        raise TimeoutError("SSE demo smoke exceeded its 45-second absolute deadline")

    signal.signal(signal.SIGALRM, expired)
    signal.alarm(45)
    try:
        with (args.output_dir / "stdout.log").open("wb") as stdout, \
                (args.output_dir / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen([str(args.server.resolve()), str(port)],
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise RuntimeError("SSE demo exited before startup")
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                try:
                    connection.connect()
                    break
                except ConnectionRefusedError:
                    connection.close()
                    if time.monotonic() >= deadline:
                        raise TimeoutError("SSE demo startup timed out")
                    time.sleep(0.01)  # Startup retry only; not a correctness barrier.

            connection.request("GET", "/events")
            response = connection.getresponse()
            headers = response.getheaders()
            transfer = [value.lower() for name, value in headers
                        if name.lower() == "transfer-encoding"]
            if (response.status != 200 or transfer != ["chunked"] or
                    response.getheader("Content-Length") is not None or
                    response.getheader("Content-Type", "").split(";", 1)[0] != "text/event-stream"):
                raise AssertionError("SSE demo did not start an unambiguous chunked event stream")

            # HTTPResponse decodes chunk sizes and CRLF boundaries before SSE
            # parsing. One decoded byte at a time avoids waiting for a later
            # event merely to fill a read buffer on this deliberately live body.
            while not first_event.endswith(b"\n\n"):
                part = response.read(1)
                if not part:
                    raise AssertionError("SSE demo completed before its first event")
                first_event.extend(part)
                if len(first_event) > 65536:
                    raise AssertionError("SSE first event exceeded the smoke byte limit")
            fields = {}
            for line in first_event.decode("utf-8").splitlines():
                if not line:
                    continue
                name, separator, value = line.partition(":")
                if not separator or name in fields:
                    raise AssertionError("unexpected or repeated first-event field")
                fields[name] = value.removeprefix(" ")
            if (set(fields) != {"id", "event", "retry", "data"} or
                    fields["id"] != "1" or fields["event"] != "heartbeat" or
                    fields["retry"] != "3000"):
                raise AssertionError(f"wrong first SSE event fields: {fields}")
            data = json.loads(fields["data"])
            if (set(data) != {"alive", "timestamp"} or data["alive"] is not True or
                    type(data["timestamp"]) is not int or data["timestamp"] <= 0):
                raise AssertionError("wrong heartbeat JSON data")
            scenarios.append({"scenario": "chunk-decoded first event has id/event/retry/data",
                              "passed": True})

            # The first complete event is the barrier: the real producer has
            # started. Keep the socket open while SIGTERM requests cancellation.
            process.send_signal(signal.SIGTERM)
            try:
                while True:
                    part = response.read(1)
                    if not part:
                        raise AssertionError("cancelled SSE demo emitted a normal chunk terminator")
                    after_cancel.extend(part)
                    if len(after_cancel) > 65536:
                        raise AssertionError("SSE demo exceeded the post-cancel byte limit")
            except http.client.IncompleteRead as error:
                after_cancel.extend(error.partial)
            scenarios.append({"scenario": "SIGTERM truncates chunk framing without normal terminator",
                              "passed": True})
            process.wait(timeout=10)
            if process.returncode != 0:
                raise AssertionError(f"SSE demo did not drain cleanly: {process.returncode}")
            scenarios.append({"scenario": "SIGTERM joins listener and drains producer before exit",
                              "passed": True})
    except Exception as error:
        failure = f"{type(error).__name__}: {error}"
    finally:
        # Cleanup gets its own bounded grace period even after the alarm fired.
        signal.alarm(0)
        if connection is not None:
            connection.close()
        if process is not None and process.poll() is None:
            try:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            except Exception as error:
                cleanup_error = f"{type(error).__name__}: {error}"

    report = {"success": failure is None and cleanup_error is None,
              "scenarios": scenarios, "error": failure, "cleanup_error": cleanup_error,
              "returncode": process.returncode if process is not None else None,
              "first_event": first_event.decode("utf-8", errors="replace"),
              "decoded_bytes_after_cancel": len(after_cancel)}
    (args.output_dir / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
