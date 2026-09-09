#!/usr/bin/env python3
"""Validate Elio HTTP/1 streaming with an independent pinned h11 peer.

Install the test-only dependency with `pip install h11==0.16.0`.
Run with --server /path/to/http_streaming_peer_server. No performance ranking.
"""

import argparse
import json
import os
import selectors
import socket
import subprocess
import sys
import time

import h11


H11_VERSION = "0.16.0"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Peer:
    def __init__(self, port, timeout):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout)
        self.timeout = timeout
        self.protocol = h11.Connection(h11.CLIENT)

    def close(self):
        self.socket.close()

    def request(self, path, method=b"GET", truncated=False, expected_status=200):
        deadline = time.monotonic() + self.timeout
        self.socket.settimeout(self.timeout)
        request = h11.Request(method=method, target=path.encode("ascii"),
                              headers=[(b"Host", b"localhost")])
        self.socket.sendall(self.protocol.send(request))
        self.socket.sendall(self.protocol.send(h11.EndOfMessage()))
        response = None
        body = bytearray()
        wire = bytearray()
        eof = False
        while True:
            try:
                event = self.protocol.next_event()
            except h11.RemoteProtocolError:
                # Only EOF truncation after valid headers and a delivered
                # prefix is expected. Earlier malformed framing still fails.
                require(truncated and eof and response is not None and body == b"prefix",
                        f"{path}: unexpected protocol error")
                return response, bytes(body), bytes(wire)
            if event is h11.NEED_DATA:
                remaining = deadline - time.monotonic()
                require(remaining > 0, f"{path}: response deadline exceeded")
                self.socket.settimeout(remaining)
                # Deliberately fragment framing and UTF-8 across decoder feeds.
                data = self.socket.recv(7)
                require(not eof, f"{path}: decoder requested input after EOF")
                eof = not data
                wire.extend(data)
                self.protocol.receive_data(data)
            elif isinstance(event, h11.Response):
                require(response is None, f"{path}: duplicate final response")
                response = event
                require(event.status_code == expected_status, f"{path}: status {event.status_code}")
            elif isinstance(event, h11.Data):
                body.extend(event.data)
            elif isinstance(event, h11.EndOfMessage):
                require(not truncated, f"{path}: failed producer emitted a valid end")
                require(response is not None, f"{path}: missing final headers")
                self.protocol.start_next_cycle()
                return response, bytes(body), bytes(wire)
            else:
                raise AssertionError(f"{path}: unexpected event {event!r}")


def wait_ready(process, timeout):
    # Binary os.read avoids text buffering hiding already available lines.
    deadline = time.monotonic() + timeout
    pending = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            require(remaining > 0, "server startup deadline exceeded")
            require(selector.select(remaining), "server did not announce readiness")
            chunk = os.read(process.stdout.fileno(), 4096)
            require(chunk, f"server exited before readiness: {process.poll()}")
            pending.extend(chunk)
            require(len(pending) <= 65536, "unbounded server startup output")
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                if line.startswith(b"READY "):
                    port = int(line.split()[1])
                    require(0 < port <= 65535, "invalid server port")
                    return port


def validate(port, timeout):
    rows = []
    peer = Peer(port, timeout)
    try:
        cases = [
            ("/ordinary", b"GET", b"hello world", b"11"),
            ("/known", b"GET", b"hello world", b"11"),
            ("/chunked", b"GET", b"hello world", None),
            ("/head", b"HEAD", b"", b"11"),
            ("/head-invocations", b"GET", b"0", b"1"),
            ("/sse", b"GET", b"id: 7\nevent: update\ndata: alpha\ndata: beta\ndata: \n\n"
             b"data: \xe4\xbd\xa0\xe5\xa5\xbd\n\n", None),
        ]
        for path, method, expected, length in cases:
            response, body, wire = peer.request(path, method)
            headers = dict(response.headers)
            require(body == expected, f"{path}: decoded body mismatch: {body!r}")
            require(headers.get(b"connection", b"").lower() != b"close",
                    f"{path}: unexpectedly disabled sequential reuse")
            if length is None:
                require(headers.get(b"transfer-encoding") == b"chunked", f"{path}: missing chunked")
                require(b"content-length" not in headers, f"{path}: conflicting length")
                require(wire.endswith(b"0\r\n\r\n"), f"{path}: missing terminal chunk")
            else:
                require(headers.get(b"content-length") == length, f"{path}: wrong length")
                require(b"transfer-encoding" not in headers, f"{path}: unexpected transfer coding")
            if path == "/sse":
                require(headers.get(b"content-type") == b"text/event-stream", "SSE media type")
            rows.append({"scenario": path, "method": method.decode(), "status": "passed",
                         "body_bytes": len(body), "connection": "shared-sequential"})
        for path, status, length in [
            ("/no-content", 204, None),
            ("/reset-content", 205, b"0"),
            ("/not-modified", 304, b"42"),
        ]:
            response, body, wire = peer.request(path, expected_status=status)
            headers = dict(response.headers)
            require(not body, f"{path}: body-forbidden response delivered payload")
            require(wire.endswith(b"\r\n\r\n"), f"{path}: bytes after final headers")
            require(headers.get(b"content-length") == length, f"{path}: length metadata")
            require(b"transfer-encoding" not in headers, f"{path}: unexpected transfer coding")
            require(headers.get(b"connection", b"").lower() != b"close", f"{path}: reuse disabled")
            rows.append({"scenario": path, "method": "GET", "status": "passed",
                         "body_bytes": 0, "connection": "shared-sequential"})
        # A bodyless message ends at headers. A subsequent response exposes
        # delayed illegal payload and proves the final 304 actually allows reuse.
        response, body, _ = peer.request("/ordinary")
        headers = dict(response.headers)
        require(body == b"hello world" and headers.get(b"content-length") == b"11",
                "ordinary response after bodyless statuses failed")
        require(b"transfer-encoding" not in headers, "unexpected framing after bodyless statuses")
        rows.append({"scenario": "/ordinary-after-bodyless", "method": "GET", "status": "passed",
                     "body_bytes": len(body), "connection": "shared-sequential"})
    finally:
        peer.close()
    peer = Peer(port, timeout)
    try:
        response, body, wire = peer.request("/failure", truncated=True)
        require(dict(response.headers).get(b"transfer-encoding") == b"chunked", "failure framing")
        require(not wire.endswith(b"0\r\n\r\n"), "failure emitted normal terminator")
        rows.append({"scenario": "/failure", "status": "passed",
                     "body_bytes": len(body), "outcome": "h11-rejected-EOF-truncation"})
    finally:
        peer.close()
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    require(h11.__version__ == H11_VERSION,
            f"expected h11=={H11_VERSION}, found {h11.__version__}")
    require(args.timeout > 0, "timeout must be positive")
    process = subprocess.Popen([args.server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=None)
    stop_sent = False
    try:
        port = wait_ready(process, args.timeout)
        rows = validate(port, args.timeout)
        stop_sent = True
        stdout, _ = process.communicate(b"stop\n", timeout=args.timeout + 5)
        require(process.returncode == 0, f"server cleanup failed: {process.returncode}")
        require(b"STOPPED" in stdout.splitlines(), "server did not confirm drained shutdown")
        print(json.dumps({"peer": f"h11=={H11_VERSION}", "performance_eligible": False,
                          "passed": len(rows), "scenarios": rows}, indent=2))
        return 0
    finally:
        if process.poll() is None:
            try:
                process.communicate(None if stop_sent else b"stop\n", timeout=args.timeout + 5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(json.dumps({"status": "failed", "error": str(error)}), file=sys.stderr)
        sys.exit(1)
