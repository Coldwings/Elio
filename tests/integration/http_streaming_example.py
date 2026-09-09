#!/usr/bin/env python3
"""Bounded smoke test of the canonical HTTP streaming example."""

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
    fixture = args.output_dir / "source.bin"
    payload = bytes(range(256)) * 513
    fixture.write_bytes(payload)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    scenarios = []
    process = None
    connection = None
    failure = None
    def expired(_signal, _frame):
        raise TimeoutError("example smoke exceeded its absolute deadline")
    signal.signal(signal.SIGALRM, expired)
    signal.alarm(45)
    try:
        with (args.output_dir / "stdout.log").open("wb") as stdout, \
                (args.output_dir / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen([str(args.server.resolve()), "--port", str(port),
                                        "--file", str(fixture.resolve())],
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
            try:
                deadline = time.monotonic() + 10
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("example exited before startup")
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                    try:
                        connection.connect()
                        break
                    except ConnectionRefusedError:
                        connection.close()
                        if time.monotonic() >= deadline:
                            raise TimeoutError("example startup timed out")
                        time.sleep(0.01)

                def request(method, path, expected, length):
                    connection.request(method, path)
                    response = connection.getresponse()
                    if response.status != 200 or response.getheader("Content-Length") != str(length):
                        raise AssertionError(f"wrong status/length for {method} {path}")
                    if response.getheader("Transfer-Encoding") is not None or response.read() != expected:
                        raise AssertionError(f"wrong framing/body for {method} {path}")
                    scenarios.append({"scenario": f"{method} {path}", "passed": True})

                request("GET", "/empty", b"", 0)
                request("GET", "/file-invocations", b"0", 1)
                request("HEAD", "/file", b"", len(payload))
                request("GET", "/file-invocations", b"0", 1)
                request("GET", "/file", payload, len(payload))
                request("GET", "/file-invocations", b"1", 1)
                connection.request("GET", "/failure")
                response = connection.getresponse()
                if response.status != 200 or response.getheader("Transfer-Encoding") != "chunked":
                    raise AssertionError("failure example did not start a chunked response")
                try:
                    response.read()
                    raise AssertionError("producer failure emitted a complete response")
                except http.client.IncompleteRead as error:
                    if error.partial != b"prefix":
                        raise AssertionError("producer failure lost/duplicated prefix") from error
                scenarios.append({"scenario": "producer failure truncates", "passed": True})
                connection.close()
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                connection.request("GET", "/cancel")
                response = connection.getresponse()
                if (response.status != 200 or response.getheader("Transfer-Encoding") != "chunked" or
                        response.getheader("Content-Length") is not None or response.read(5) != b"tick\n"):
                    raise AssertionError("cancellable producer did not deliver first write")
                process.send_signal(signal.SIGTERM)
                try:
                    response.read()
                    raise AssertionError("cancelled producer emitted normal completion")
                except http.client.IncompleteRead:
                    pass
                process.wait(timeout=10)
                if process.returncode != 0:
                    raise AssertionError(f"example did not drain cleanly: {process.returncode}")
                scenarios.append({"scenario": "stop cancels and drains producer", "passed": True})
            finally:
                signal.alarm(0)
                if connection is not None:
                    connection.close()
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    except Exception as error:
        failure = str(error)
    finally:
        signal.alarm(0)
    report = {"success": failure is None, "scenarios": scenarios, "error": failure}
    (args.output_dir / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
