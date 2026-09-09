#!/usr/bin/env python3
"""Verified HTTP/1 loopback diagnostics; requires h11==0.16.0, not a ranking.

Each coordinate owns a sender subprocess and one TCP/TLS connection. TLS uses
a freshly generated, explicitly trusted localhost certificate. No verification
is disabled. Prepared expectations, startup, handshake, warmup and final reuse
are outside timing. All process logs and parsed evidence survive failure.
"""

import argparse
import hashlib
import json
import math
import platform
import re
import socket
import ssl
import subprocess
import sys
import time
import uuid
from pathlib import Path

import h11


H11_VERSION = "0.16.0"
BODY_BYTES = 4 * 1024 * 1024
BLOCK_BYTES = 64 * 1024
MEASURED_COUNT = 16
COORDINATES = tuple((transport, mode) for transport in ("tcp", "tls")
                    for mode in ("complete", "known_stream", "chunked"))
SEQUENCES = [("warmup", 0)] + [("measure", n) for n in range(MEASURED_COUNT)] + [("probe", 0)]
TRIAL_PATTERN = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
MAX_LINE = 131072


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def exact_integer(value, expected):
    return integer(value) and value == expected


def strict_json(text):
    def object_pairs(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, f"duplicate JSON key: {key}")
            value[key] = item
        return value

    def invalid_constant(value):
        raise ValueError(f"non-finite JSON constant: {value}")

    return json.loads(text, object_pairs_hook=object_pairs, parse_constant=invalid_constant)


def body_digest():
    digest = hashlib.sha256()
    block = b"x" * BLOCK_BYTES
    for _ in range(BODY_BYTES // BLOCK_BYTES):
        digest.update(block)
    return digest.hexdigest()


def remaining(deadline):
    value = deadline - time.monotonic()
    require(value > 0, "absolute deadline exceeded")
    return value


def unique_headers(headers):
    values = {}
    for name, value in headers:
        require(name not in values, f"duplicate response header: {name!r}")
        values[name] = value
    return values


class Peer:
    def __init__(self, port, transport, certificate, deadline):
        self.socket = socket.create_connection(("127.0.0.1", port), remaining(deadline))
        try:
            if transport == "tls":
                context = ssl.create_default_context(cafile=str(certificate))
                self.socket.settimeout(remaining(deadline))
                self.socket = context.wrap_socket(self.socket, server_hostname="localhost")
            self.protocol = h11.Connection(h11.CLIENT, max_incomplete_event_size=16384)
            self.tls_version = self.socket.version() if transport == "tls" else "none"
            self.tls_cipher = self.socket.cipher()[0] if transport == "tls" else "none"
        except BaseException:
            self.socket.close()
            raise

    def close(self):
        self.socket.close()

    def request(self, trial, phase, sequence, mode, expected_digest, deadline):
        expected_bytes = 2 if phase == "probe" else BODY_BYTES
        expected_framing = "chunked" if mode == "chunked" and phase != "probe" else "content_length"
        event = h11.Request(method=b"GET", target=b"/", headers=[
            (b"Host", b"localhost"), (b"X-Trial-ID", trial.encode("ascii")),
            (b"X-Phase", phase.encode("ascii")), (b"X-Sequence", str(sequence).encode("ascii"))])
        for data in (self.protocol.send(event), self.protocol.send(h11.EndOfMessage())):
            self.socket.settimeout(remaining(deadline))
            self.socket.sendall(data)
        received = 0
        digest = hashlib.sha256()
        response = None
        eof = False
        while True:
            remaining(deadline)
            event = self.protocol.next_event()
            if event is h11.NEED_DATA:
                require(not eof, "decoder requested input after EOF")
                self.socket.settimeout(remaining(deadline))
                data = self.socket.recv(BLOCK_BYTES)
                eof = not data
                self.protocol.receive_data(data)
            elif isinstance(event, h11.Response):
                require(response is None, "multiple final responses")
                response = event
                require(event.status_code == 200 and event.http_version == b"1.1", "unexpected status/version")
                headers = unique_headers(event.headers)
                for name, value in ((b"x-trial-id", trial), (b"x-phase", phase),
                                    (b"x-sequence", str(sequence))):
                    require(headers.get(name) == value.encode("ascii"), f"wrong echoed {name!r}")
                require(b"close" not in [v.strip().lower() for v in headers.get(b"connection", b"").split(b",")],
                        "response disabled connection reuse")
                if expected_framing == "chunked":
                    require(headers.get(b"transfer-encoding") == b"chunked" and b"content-length" not in headers,
                            "wrong chunked framing")
                else:
                    require(headers.get(b"content-length") == str(expected_bytes).encode("ascii") and
                            b"transfer-encoding" not in headers, "wrong Content-Length framing")
            elif isinstance(event, h11.Data):
                require(response is not None, "body before final headers")
                received += len(event.data)
                require(received <= expected_bytes, "body exceeds fixed workload")
                digest.update(event.data)
            elif isinstance(event, h11.EndOfMessage):
                require(response is not None, "completion without response")
                require(not event.headers, "unexpected response trailers")
                require(received == expected_bytes, "body length mismatch")
                actual_digest = digest.hexdigest()
                require(actual_digest == expected_digest, "body digest mismatch")
                require(not self.protocol.trailing_data[0], "unsolicited bytes after response")
                remaining(deadline)
                self.protocol.start_next_cycle()
                return {"trial": trial, "phase": phase, "sequence": sequence,
                        "body_bytes": received, "sha256": actual_digest,
                        "framing": expected_framing, "status": 200}
            else:
                raise ValueError(f"unexpected HTTP event: {event!r}")


def read_events(path):
    """Parse bounded records, while the original unmodified file stays on disk."""
    events = []
    if not path.exists():
        return events
    with path.open("rb") as source:
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                break
            require(len(line) <= MAX_LINE and line.endswith(b"\n"), "invalid/oversized JSONL record")
            value = strict_json(line)
            require(isinstance(value, dict), "JSONL event must be an object")
            events.append(value)
            require(len(events) <= 64, "too many server events")
    return events


def wait_ready(process, path, deadline):
    with path.open("rb") as source:
        pending = bytearray()
        while True:
            remaining(deadline)
            pending.extend(source.read(MAX_LINE + 1 - len(pending)))
            require(len(pending) <= MAX_LINE, "oversized startup record")
            if b"\n" in pending:
                line = pending.split(b"\n", 1)[0]
                event = strict_json(line)
                require(isinstance(event, dict) and event.get("event") == "ready", "missing initial ready event")
                require(integer(event.get("port"), 1) and event["port"] <= 65535, "invalid bound port")
                require(event.get("backend") in ("epoll", "io_uring") and
                        integer(event.get("workers"), 1) and event["workers"] == 1,
                        "invalid ready backend/worker metadata")
                return event
            require(process.poll() is None, "server exited before readiness")
            time.sleep(min(0.01, remaining(deadline)))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def run_coordinate(server, output, planned, certificate, key, args, expected_digest):
    trial = planned["trial"]
    directory = output / trial
    directory.mkdir()
    stdout_path = directory / "server.stdout.jsonl"
    client = {**planned, "success": False, "responses": [], "error": ""}
    events = []
    process = None
    peer = None
    deadline = time.monotonic() + args.trial_timeout
    command = [str(server), "--transport", planned["transport"], "--mode", planned["mode"], "--trial", trial]
    if planned["transport"] == "tls":
        command += ["--cert", str(certificate), "--key", str(key)]
    write_json(directory / "invocation.json", {"argv": command, "trial_timeout_seconds": args.trial_timeout,
               "response_timeout_seconds": args.response_timeout, "disconnect_cleanup_grace_seconds": 5,
               "terminate_grace_seconds": 5, "kill_join_seconds": 5})
    with stdout_path.open("wb") as stdout, (directory / "server.stderr.log").open("wb") as stderr:
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
            ready = wait_ready(process, stdout_path, min(deadline, time.monotonic() + args.startup_timeout))
            client["ready"] = ready
            peer = Peer(ready["port"], planned["transport"], certificate,
                        min(deadline, time.monotonic() + args.startup_timeout))
            client["tls_version"] = peer.tls_version
            client["tls_cipher"] = peer.tls_cipher
            client["responses"].append(peer.request(trial, "warmup", 0, planned["mode"], expected_digest,
                min(deadline, time.monotonic() + args.response_timeout)))
            started_cpu = time.process_time_ns()
            started_wall = time.monotonic_ns()
            for sequence in range(MEASURED_COUNT):
                client["responses"].append(peer.request(trial, "measure", sequence, planned["mode"], expected_digest,
                    min(deadline, time.monotonic() + args.response_timeout)))
            elapsed = time.monotonic_ns() - started_wall
            cpu = time.process_time_ns() - started_cpu
            require(elapsed > 0 and cpu > 0, "invalid client clock interval")
            client.update(elapsed_ns=elapsed, client_cpu_ns=cpu, measured_count=MEASURED_COUNT,
                          verified_body_bytes=BODY_BYTES * MEASURED_COUNT)
            client["responses"].append(peer.request(trial, "probe", 0, planned["mode"],
                hashlib.sha256(b"ok").hexdigest(), min(deadline, time.monotonic() + args.response_timeout)))
            peer.close()
            peer = None
            process.wait(timeout=remaining(deadline))
            require(process.returncode == 0, f"server exit code {process.returncode}")
            client["success"] = True
        except Exception as error:
            client["error"] = str(error)
        finally:
            if peer is not None:
                peer.close()
            if process is not None:
                if process.poll() is None:
                    # Closing the peer first lets the server observe EOF and
                    # cooperatively join its own I/O before process escalation.
                    client["cleanup_action"] = "joined_after_disconnect"
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client["cleanup_action"] = "terminated_after_disconnect_grace"
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            client["cleanup_action"] = "killed_after_terminate_grace"
                            process.kill()
                            process.wait(timeout=5)
                else:
                    client["cleanup_action"] = "already_exited"
                client["server_returncode"] = process.returncode
    try:
        events = read_events(stdout_path)
        write_json(directory / "server-events.json", events)
        require([event.get("event") for event in events] == ["ready", "result", "stopped"],
                "server lifecycle must contain exactly ready/result/stopped")
    except Exception as error:
        client["success"] = False
        client["error"] = "; ".join(filter(None, [client["error"], str(error)]))
    write_json(directory / "client.json", client)
    return client, [event for event in events if event.get("event") == "result"]


def validate_pair(planned, client, server, expected_digest):
    for evidence in (client, server):
        require(all(evidence.get(key) == planned[key] for key in ("trial", "transport", "mode")),
                "wrong trial or coordinate attribution")
        require(evidence.get("success") is True, "unsuccessful evidence")
        require(exact_integer(evidence.get("measured_count"), MEASURED_COUNT), "wrong measured count")
    require(server.get("event") == "result", "not server result evidence")
    require(exact_integer(client.get("server_returncode"), 0), "server did not exit successfully")
    require(exact_integer(server.get("confirmed_body_bytes"), BODY_BYTES * MEASURED_COUNT) and
            exact_integer(client.get("verified_body_bytes"), BODY_BYTES * MEASURED_COUNT), "wrong aggregate bytes")
    require(exact_integer(server.get("body_bytes"), BODY_BYTES) and
            exact_integer(server.get("stream_block_bytes"), BLOCK_BYTES) and
            integer(server.get("warmup_count"), 1) and server["warmup_count"] == 1 and
            server.get("probe_success") is True, "wrong workload/probe")
    require(integer(server.get("workers"), 1) and server["workers"] == 1 and
            server.get("backend") in ("epoll", "io_uring"), "invalid server backend/workers")
    ready = client.get("ready", {})
    require(isinstance(ready, dict) and ready.get("backend") == server["backend"] and
            exact_integer(ready.get("workers"), server["workers"]),
            "ready/result metadata disagree")
    for name in ("compiler", "build_type", "openssl_version", "tls_version", "tls_cipher"):
        require(isinstance(server.get(name), str) and server[name], f"missing server {name}")
    require(server["build_type"].lower() == "release", "metrics require a Release sender")
    require(isinstance(server.get("build_head"), str) and
            re.fullmatch(r"[0-9a-fA-F]{40}", server["build_head"]), "missing configure-time build HEAD")
    require(type(server.get("build_dirty")) is bool, "missing configure-time dirty-state metadata")
    if planned["transport"] == "tls":
        require(server["tls_version"] == client.get("tls_version") and
                server["tls_cipher"] == client.get("tls_cipher") and server["tls_version"] != "none",
                "TLS metadata mismatch")
    else:
        require(all(evidence.get(name) == "none"
                    for evidence in (client, server)
                    for name in ("tls_version", "tls_cipher")),
                "plain TCP evidence must report TLS version and cipher as none")
    for value, name in ((client.get("elapsed_ns"), "elapsed"), (client.get("client_cpu_ns"), "client CPU"),
                        (server.get("server_cpu_ns"), "server CPU")):
        require(integer(value, 1), f"invalid {name} clock interval")
    server_responses = server.get("responses")
    client_responses = client.get("responses")
    require(isinstance(server_responses, list) and isinstance(client_responses, list) and
            len(server_responses) == len(SEQUENCES) and len(client_responses) == len(SEQUENCES),
            "missing or duplicate per-response evidence")
    measured_cpu = 0
    for expected, sent, received in zip(SEQUENCES, server_responses, client_responses):
        phase, sequence = expected
        size = 2 if phase == "probe" else BODY_BYTES
        digest = hashlib.sha256(b"ok").hexdigest() if phase == "probe" else expected_digest
        framing = "chunked" if planned["mode"] == "chunked" and phase != "probe" else "content_length"
        require(isinstance(sent, dict) and isinstance(received, dict), "response evidence must be objects")
        for evidence in (sent, received):
            require(integer(evidence.get("sequence")) and
                    (evidence.get("phase"), evidence.get("sequence")) == expected, "response order/sequence mismatch")
        require(sent.get("success") is True and sent.get("reusable") is True and
                exact_integer(sent.get("confirmed_body_bytes"), size) and integer(sent.get("error")) and
                sent["error"] == 0 and integer(sent.get("transport_error")) and
                sent["transport_error"] == 0, "invalid sender completion")
        require(integer(sent.get("server_cpu_ns")), "invalid per-response CPU")
        if phase == "measure":
            measured_cpu += sent["server_cpu_ns"]
        require(received.get("trial") == planned["trial"] and received.get("status") == 200 and
                exact_integer(received.get("body_bytes"), size) and received.get("sha256") == digest and
                received.get("framing") == framing, "invalid client integrity/framing evidence")
    require(measured_cpu == server["server_cpu_ns"], "server CPU aggregate mismatch")


def build_summary(planned, clients, servers, expected_digest):
    """Count exact attributable coordinates, never successful evidence volume."""
    errors = []
    rows = []
    plan_valid = (len(planned) == len(COORDINATES) and
                  {(p.get("transport"), p.get("mode")) for p in planned} == set(COORDINATES) and
                  len({p.get("trial") for p in planned}) == len(COORDINATES) and
                  all(isinstance(p.get("trial"), str) and TRIAL_PATTERN.fullmatch(p["trial"]) for p in planned))
    if not plan_valid:
        errors.append("invalid expected coordinate/trial manifest")
    known_trials = {p.get("trial") for p in planned}
    for label, evidence in (("client", clients), ("server", servers)):
        if any(not isinstance(item, dict) or item.get("trial") not in known_trials for item in evidence):
            errors.append(f"unexpected/unattributable {label} evidence")
    for item in planned:
        row = {**item, "status": "incomplete", "errors": []}
        matched_clients = [c for c in clients if isinstance(c, dict) and c.get("trial") == item.get("trial")]
        matched_servers = [s for s in servers if isinstance(s, dict) and s.get("trial") == item.get("trial")]
        if len(matched_clients) != 1 or len(matched_servers) != 1:
            row["errors"].append("expected exactly one client and one server row")
            if len(matched_clients) == 1:
                client = matched_clients[0]
                if (all(client.get(key) == item.get(key)
                        for key in ("trial", "transport", "mode")) and
                        isinstance(client.get("error"), str) and client["error"]):
                    row["errors"].append(f"client: {client['error']}")
        elif plan_valid:
            try:
                client, server = matched_clients[0], matched_servers[0]
                validate_pair(item, client, server, expected_digest)
                row.update(status="passed", verified_responses=MEASURED_COUNT,
                           verified_body_bytes=BODY_BYTES * MEASURED_COUNT, elapsed_ns=client["elapsed_ns"],
                           client_cpu_ns=client["client_cpu_ns"], server_cpu_ns=server["server_cpu_ns"],
                           pipeline_mib_per_second=(BODY_BYTES * MEASURED_COUNT / (1024 * 1024)) /
                               (client["elapsed_ns"] / 1_000_000_000),
                           backend=server["backend"], workers=server["workers"],
                           submission_shape="one complete body" if item["mode"] == "complete" else "borrowed 64 KiB slices",
                           server_metadata={k: server[k] for k in
                               ("build_head", "build_dirty", "build_type", "compiler", "openssl_version", "tls_version", "tls_cipher")})
            except (ValueError, TypeError, KeyError, OverflowError) as error:
                row["errors"].append(str(error))
        rows.append(row)
    covered = sum(row["status"] == "passed" for row in rows)
    return {"schema_version": 1, "performance_eligible": False,
            "success": plan_valid and not errors and covered == len(COORDINATES),
            "expected_coordinates": len(COORDINATES), "covered_coordinates": covered,
            "errors": errors, "rows": rows,
            "measurement_scope": "Verified loopback pipeline: requests, TCP/TLS, h11 parsing and SHA-256 verification; not isolated server capacity.",
            "cpu_scope": "Server process CPU summed around measured send_response calls; client process CPU across 16 measured request/verification cycles. These are separate processes.",
            "excluded": "Startup, certificate generation, connect/handshake, warmup and final reuse probe. Allocation/copy evidence is separate.",
            "build_head_scope": "Server build_head is configure-time git HEAD, not proof of clean sources or exact binary provenance.",
            "client_metadata": {"python": sys.version, "h11": h11.__version__, "openssl": ssl.OPENSSL_VERSION,
                                "platform": platform.platform(), "read_buffer_bytes": BLOCK_BYTES,
                                "header_limit_bytes": 16384, "tls_verification": "explicitly trusted generated localhost certificate"}}


def render_summary(summary):
    lines = ["## Verified HTTP loopback diagnostics", "", summary["measurement_scope"], "",
             "Shared-runner diagnostics: performance_eligible=false. No rankings or cross-row ratios.", "",
             f"Coverage: {summary['covered_coordinates']}/{summary['expected_coordinates']} attributable coordinates.", "",
             "| Transport | Mode | Verified responses / bytes | Elapsed s | Pipeline MiB/s | Server CPU s | Client CPU s | Result |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    for row in summary["rows"]:
        if row["status"] == "passed":
            values = (f"{row['verified_responses']} / {row['verified_body_bytes']}", f"{row['elapsed_ns']/1e9:.6f}",
                      f"{row['pipeline_mib_per_second']:.3f}", f"{row['server_cpu_ns']/1e9:.6f}",
                      f"{row['client_cpu_ns']/1e9:.6f}")
        else:
            values = ("—",) * 5
        lines.append("| " + " | ".join((row["transport"], row["mode"], *values, row["status"])) + " |")
    lines += ["", summary["cpu_scope"], "", "Excluded: " + summary["excluded"], "",
              "Submission shape: complete uses one prepared body; streams submit borrowed 64 KiB slices.", ""]
    for error in summary["errors"]:
        lines.append(f"- {error}")
    for row in summary["rows"]:
        for error in row["errors"]:
            lines.append(f"- {row['transport']}/{row['mode']}: {error}")
    return "\n".join(lines) + "\n"


def generate_certificate(output):
    certificate, key = output / "localhost.crt", output / "localhost.key"
    with (output / "certificate.stdout.log").open("wb") as stdout, (output / "certificate.stderr.log").open("wb") as stderr:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(certificate)],
                       check=True, timeout=20, stdout=stdout, stderr=stderr)
    key.chmod(0o600)
    return certificate, key


def source_metadata():
    """Record inspected checkout state separately from the executable's build."""
    repository = Path(__file__).resolve().parents[2]
    result = {"scope": "Source checkout containing this driver at invocation time; not binary provenance."}
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repository, check=True,
                              capture_output=True, text=True, timeout=5).stdout.strip()
        status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=normal"],
                                cwd=repository, check=True, capture_output=True, text=True, timeout=5).stdout
        result.update(source_head=head, source_dirty=bool(status))
    except (OSError, subprocess.SubprocessError) as error:
        result.update(source_head=None, source_dirty=None, error=str(error))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--response-timeout", type=float, default=15)
    parser.add_argument("--startup-timeout", type=float, default=15)
    parser.add_argument("--trial-timeout", type=float, default=90)
    args = parser.parse_args()
    for name in ("response_timeout", "startup_timeout", "trial_timeout"):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            parser.error(f"{name} must be finite and positive")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    require(not any(output.iterdir()), "output directory must be empty; preserve prior evidence")
    planned = [{"transport": transport, "mode": mode, "trial": uuid.uuid4().hex}
               for transport, mode in COORDINATES]
    write_json(output / "manifest.json", planned)
    source = source_metadata()
    write_json(output / "source-metadata.json", source)
    clients, servers = [], []
    errors = []
    expected_digest = body_digest()
    try:
        require(h11.__version__ == H11_VERSION, f"requires h11=={H11_VERSION}, found {h11.__version__}")
        certificate, key = generate_certificate(output)
        for item in planned:
            client, evidence = run_coordinate(args.server.resolve(), output, item, certificate, key, args, expected_digest)
            clients.append(client)
            servers.extend(evidence)
    except Exception as error:
        errors.append(str(error))
    summary = build_summary(planned, clients, servers, expected_digest)
    summary["source_metadata"] = source
    summary["errors"].extend(errors)
    summary["success"] = summary["success"] and not errors
    write_json(output / "client-evidence.json", clients)
    write_json(output / "server-evidence.json", servers)
    write_json(output / "summary.json", summary)
    (output / "summary.md").write_text(render_summary(summary))
    print(json.dumps({"success": summary["success"], "covered_coordinates": summary["covered_coordinates"],
                      "performance_eligible": False, "output_dir": str(output)}))
    return 0 if summary["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
