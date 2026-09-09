#!/usr/bin/env python3
"""Bounded smoke of the actual restricted CONNECT example executable."""
import argparse
import json
import pathlib
import signal
import socket
import subprocess
import time

from http_connect_peer import Upstream, alarm_handler, exact, h11, request_head, require


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        parser.error("output directory must be empty; refusing to overwrite evidence")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    result = {"success": False, "performance_eligible": False, "checks": [], "errors": []}
    process = peer = upstream = None
    signal.signal(signal.SIGALRM, alarm_handler)
    signal.alarm(40)
    try:
        require(h11 is not None and h11.__version__ == "0.16.0", "requires h11==0.16.0")
        upstream = Upstream(b"AFTER-FIN")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        command = [str(args.server.resolve()), "--port", str(port), "--upstream-port", str(upstream.port)]
        (args.output_dir / "invocation.json").write_text(json.dumps(command, indent=2))
        with (args.output_dir / "stdout.log").open("wb") as stdout, (args.output_dir / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
        deadline = time.monotonic() + 6
        while True:
            require(process.poll() is None, "example exited before startup")
            try:
                peer = socket.create_connection(("127.0.0.1", port), 1)
                peer.settimeout(5)
                break
            except ConnectionRefusedError:
                require(time.monotonic() < deadline, "example startup timed out")
                time.sleep(0.01)
        denied, _ = request_head(peer, "denied.example:443")
        require(denied.status_code == 403, "example did not restrict destination")
        result["checks"].append("denied-destination")
        peer.close()
        peer = socket.create_connection(("127.0.0.1", port), 5)
        peer.settimeout(5)
        payload = b"\x00\xffcoalesced" + bytes(range(256)) * 17
        authority = f"127.0.0.1:{upstream.port}"
        accepted, prefix = request_head(peer, authority, payload)
        require(accepted.status_code == 200, "example CONNECT rejected")
        headers = dict(accepted.headers)
        require(b"content-length" not in headers and b"transfer-encoding" not in headers, "tunnel body framing")
        require(exact(peer, len(payload), prefix) == payload, "example binary echo mismatch")
        result["checks"].append("real-example-binary-tunnel")
        peer.shutdown(socket.SHUT_WR)
        require(exact(peer, 9) == b"AFTER-FIN", "example discarded reverse bytes after FIN")
        require(peer.recv(1) == b"", "example did not relay upstream EOF")
        result["checks"].append("FIN-preserves-reverse")
        peer.close()
        peer = socket.create_connection(("127.0.0.1", port), 5)
        peer.settimeout(5)
        accepted, prefix = request_head(peer, authority, b"park")
        require(accepted.status_code == 200 and exact(peer, 4, prefix) == b"park", "active tunnel setup failed")
        require(not upstream.errors, f"upstream errors: {upstream.errors}")
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=8)
        require(process.returncode == 0, "SIGTERM did not cleanly drain example")
        require(peer.recv(1) == b"", "active tunnel survived example shutdown")
        result["checks"].append("SIGTERM-drains-active-tunnel")
        result["success"] = True
    except Exception as error:
        result["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        signal.alarm(0)
        def cleanup(action):
            try:
                action()
            except Exception as error:
                result["success"] = False
                result["errors"].append(f"cleanup: {type(error).__name__}: {error}")
        if peer is not None:
            cleanup(peer.close)
        if process is not None:
            if process.poll() is None:
                cleanup(process.terminate)
                cleanup(lambda: process.wait(timeout=8))
                if process.poll() is None:
                    cleanup(process.kill)
                    cleanup(lambda: process.wait(timeout=3))
            result["returncode"] = process.returncode
        if upstream is not None:
            cleanup(upstream.close)
        (args.output_dir / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        lines = ["# CONNECT example smoke", "", "PASS" if result["success"] else "FAIL", ""]
        lines += [f"- {check}" for check in result["checks"]]
        lines += ["", *result["errors"], "", "Correctness only; performance_eligible=false."]
        (args.output_dir / "summary.md").write_text("\n".join(lines) + "\n")
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
