#!/usr/bin/env python3
"""Independent bounded CONNECT wire checks, not a performance measurement.

Outer and inner TLS certificates are independently verified. Inner TLS uses
SSLObject/MemoryBIO so it also runs over an already encrypted proxy connection.
"""
import argparse
import json
import pathlib
import signal
import socket
import ssl
import subprocess
import threading
import time
import uuid

try:
    import h11
except ImportError:
    h11 = None


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def alarm_handler(_signal, _frame):
    raise TimeoutError("absolute coordinate deadline exceeded")


class Upstream:
    def __init__(self, trailer, certificate=None, key=None):
        self.trailer = trailer
        self.security = None
        if certificate is not None:
            self.security = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.security.minimum_version = self.security.maximum_version = ssl.TLSVersion.TLSv1_3
            self.security.load_cert_chain(str(certificate), str(key))
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.active = None
        self.errors = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    peer, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.active = peer
                try:
                    peer.settimeout(5)
                    inner_tls = peer.recv(1, socket.MSG_PEEK) == b"\x16"
                    if inner_tls:
                        require(self.security is not None, "unexpected TLS input to plaintext upstream")
                        peer = self.security.wrap_socket(peer, server_side=True)
                        self.active = peer
                    while not self.stop.is_set():
                        data = peer.recv(65536)
                        if not data:
                            if inner_tls:
                                peer.unwrap().close()
                            elif self.trailer:
                                peer.sendall(self.trailer)
                            break
                        peer.sendall(data)
                except OSError as error:
                    if not self.stop.is_set() and not (isinstance(error, ConnectionResetError) and not self.trailer):
                        self.errors.append(str(error))
                finally:
                    peer.close()
                    self.active = None
        except OSError as error:
            if not self.stop.is_set() and not (isinstance(error, ConnectionResetError) and not self.trailer):
                self.errors.append(str(error))

    def close(self):
        self.stop.set()
        peer = self.active
        if peer is not None:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        try:
            self.listener.close()
        finally:
            self.thread.join(6)
        require(not self.thread.is_alive(), "upstream thread failed to join")


class InnerTLS:
    """One SSLObject owner; ciphertext is carried by the existing tunnel."""
    def __init__(self, peer, certificate):
        self.peer = peer
        self.incoming, self.outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
        context = ssl.create_default_context(cafile=str(certificate))
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        self.ssl = context.wrap_bio(self.incoming, self.outgoing, server_hostname="localhost")

    def flush(self):
        while self.outgoing.pending:
            self.peer.sendall(self.outgoing.read())

    def operation(self, function, *args):
        while True:
            try:
                result = function(*args)
                self.flush()
                return result
            except ssl.SSLWantWriteError:
                self.flush()
            except ssl.SSLWantReadError:
                self.flush()
                data = self.peer.recv(65536)
                require(data, "outer EOF before inner TLS completion")
                self.incoming.write(data)

    def client_hello(self):
        try:
            self.ssl.do_handshake()
        except ssl.SSLWantReadError:
            return self.outgoing.read()
        raise RuntimeError("inner TLS unexpectedly completed without peer input")

    def echo(self, payload):
        self.operation(self.ssl.do_handshake)
        require(self.ssl.version() == "TLSv1.3", "wrong inner TLS version")
        offset = 0
        while offset < len(payload):
            offset += self.operation(self.ssl.write, payload[offset:])
        received = bytearray()
        while len(received) < len(payload):
            chunk = self.operation(self.ssl.read, len(payload) - len(received))
            require(chunk, "inner TLS EOF before echo")
            received.extend(chunk)
        require(bytes(received) == payload, "inner TLS plaintext mismatch")
        self.operation(self.ssl.unwrap)


def exact(peer, length, prefix=b""):
    data = bytearray(prefix)
    while len(data) < length:
        part = peer.recv(length - len(data))
        require(part, "premature tunnel EOF")
        data.extend(part)
    require(len(data) == length, "unexpected excess tunnel bytes")
    return bytes(data)


def connect_socket(port, transport, certificate):
    peer = socket.create_connection(("127.0.0.1", port), 5)
    peer.settimeout(5)
    if transport != "tcp":
        context = ssl.create_default_context(cafile=str(certificate))
        version = ssl.TLSVersion.TLSv1_2 if transport == "tls12" else ssl.TLSVersion.TLSv1_3
        context.minimum_version = context.maximum_version = version
        try:
            peer = context.wrap_socket(peer, server_hostname="localhost")
        except BaseException:
            peer.close()
            raise
        require(peer.version() == ("TLSv1.2" if transport == "tls12" else "TLSv1.3"), "wrong outer TLS version")
    return peer


def request_head(peer, authority, prefix=b""):
    protocol = h11.Connection(h11.CLIENT)
    request = h11.Request(method=b"CONNECT", target=authority.encode(), headers=[
        (b"Host", b"wrong.example:9"), (b"Connection", b"Upgrade"),
        (b"Upgrade", b"websocket"), (b"Sec-WebSocket-Version", b"13"),
        (b"Sec-WebSocket-Key", b"dGhlIHNhbXBsZSBub25jZQ==")])
    peer.sendall(protocol.send(request) + protocol.send(h11.EndOfMessage()) + prefix)
    while True:
        event = protocol.next_event()
        if event is h11.NEED_DATA:
            data = peer.recv(65536)
            require(data, "EOF before CONNECT response")
            protocol.receive_data(data)
        elif isinstance(event, h11.Response):
            return event, protocol.trailing_data[0]
        else:
            raise RuntimeError(f"unexpected CONNECT response event {event!r}")


def coordinate(args, frontend, transport, certificate, key):
    directory = args.output_dir / f"{frontend}-{transport}"
    directory.mkdir(parents=True, exist_ok=True)
    row = {"frontend": frontend, "transport": transport, "success": False, "checks": [], "error": ""}
    upstream = None
    process = None
    peer = None
    signal.alarm(40)
    try:
        upstream = Upstream(b"AFTER-FIN" if transport == "tcp" else b"", certificate, key)
        command = [str(args.server), "--frontend", frontend, "--transport", transport,
                   "--upstream-port", str(upstream.port), "--cert", str(certificate), "--key", str(key)]
        (directory / "invocation.json").write_text(json.dumps(command, indent=2))
        with (directory / "stdout.log").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr)
        deadline = time.monotonic() + 6
        port = None
        while time.monotonic() < deadline:
            lines = (directory / "stdout.log").read_text().splitlines()
            if lines and lines[0].startswith("READY "):
                port = int(lines[0].split()[1])
                break
            require(process.poll() is None, "fixture exited before READY")
            time.sleep(0.01)
        require(port is not None, "startup deadline exceeded")
        authority = f"127.0.0.1:0{upstream.port}"
        prefix = b"\x00\xff\r\nGET /not-http HTTP/1.1\r\n\r\n"
        peer = connect_socket(port, transport, certificate)
        event, remainder = request_head(peer, authority, prefix)
        require(event.status_code == 200, "CONNECT did not select tunnel handler")
        headers = dict(event.headers)
        require(headers.get(b"x-authority") == authority.encode(), "authority spelling was changed")
        require(b"content-length" not in headers and b"transfer-encoding" not in headers, "CONNECT was body-framed")
        require(exact(peer, len(prefix), remainder) == prefix, "coalesced binary prefix not preserved exactly once")
        row["checks"].append("authority-not-host; no-HTTP-framing; binary-prefix; no-WebSocket-upgrade")
        payload = bytes(range(256)) * 257
        peer.sendall(payload)
        require(exact(peer, len(payload)) == payload, "duplex echo mismatch")
        row["checks"].append("bidirectional-binary-echo")
        if transport == "tcp":
            peer.shutdown(socket.SHUT_WR)
            require(exact(peer, 9) == b"AFTER-FIN", "reverse data lost after client FIN")
            require(peer.recv(1) == b"", "tunnel did not finish after upstream EOF")
            row["checks"].append("TCP-half-close-preserves-reverse")
        else:
            # An authenticated close exchange, not a claim of TLS1.2 half-close.
            raw = peer.unwrap()
            peer = raw
            row["checks"].append("verified-outer-TLS-and-clean-close")
        peer.close()
        peer = connect_socket(port, transport, certificate)
        inner = InnerTLS(peer, certificate)
        hello = inner.client_hello()
        require(hello, "missing inner ClientHello")
        event, remainder = request_head(peer, authority, hello)
        require(event.status_code == 200, "inner TLS CONNECT rejected")
        if remainder:
            inner.incoming.write(remainder)
        inner.echo(bytes(range(256)) * 31)
        row["checks"].append("verified-inner-TLS13-with-coalesced-ClientHello")
        peer.close()
        peer = connect_socket(port, transport, certificate)
        event, _ = request_head(peer, "denied.example:443")
        require(event.status_code == 403, "unauthorized authority was accepted")
        peer.close()
        peer = None
        row["checks"].append("unauthorized-destination-rejected")
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        process.wait(timeout=8)
        require(process.returncode == 0, "fixture failed drain")
        require((directory / "stdout.log").read_text().splitlines() == [f"READY {port}", "STOPPED"], "missing clean lifecycle")
        require(not upstream.errors, f"upstream errors: {upstream.errors}")
        row["success"] = True
    except Exception as error:
        row["error"] = f"{type(error).__name__}: {error}"
    finally:
        signal.alarm(0)
        def cleanup_error(error):
            row["success"] = False
            row["error"] += f"; cleanup: {type(error).__name__}: {error}"
        if peer is not None:
            try:
                peer.close()
            except Exception as error:
                cleanup_error(error)
        if process is not None:
            try:
                if process.poll() is None:
                    try:
                        process.communicate(b"stop\n", timeout=8)
                    except (subprocess.TimeoutExpired, BrokenPipeError):
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)
            except Exception as error:
                cleanup_error(error)
                try:
                    process.kill()
                    process.wait(timeout=3)
                except Exception as last_error:
                    cleanup_error(last_error)
            try:
                if process.stdin:
                    process.stdin.close()
            except Exception as error:
                cleanup_error(error)
            row["server_returncode"] = process.returncode
        if upstream is not None:
            try:
                upstream.close()
            except Exception as error:
                cleanup_error(error)
        (directory / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=pathlib.Path, required=True)
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument("--output-dir", type=pathlib.Path)
    outputs.add_argument("--output-root", type=pathlib.Path)
    args = parser.parse_args()
    args.server = args.server.resolve()
    if args.output_root is not None:
        args.output_dir = args.output_root / uuid.uuid4().hex
        print(f"CONNECT evidence: {args.output_dir.resolve()}", flush=True)
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        parser.error("output directory must be empty; refusing to overwrite evidence")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    coordinates = [(frontend, transport) for frontend in ("http", "websocket") for transport in ("tcp", "tls12", "tls13")]
    rows = []
    signal.signal(signal.SIGALRM, alarm_handler)
    try:
        require(h11 is not None and h11.__version__ == "0.16.0", "requires pinned h11==0.16.0")
        certificate, key = args.output_dir / "localhost.crt", args.output_dir / "localhost.key"
        generated = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(certificate), "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost"], capture_output=True, timeout=15)
        (args.output_dir / "certificate.stdout").write_bytes(generated.stdout)
        (args.output_dir / "certificate.stderr").write_bytes(generated.stderr)
        require(generated.returncode == 0, "certificate generation failed")
        for frontend, transport in coordinates:
            rows.append(coordinate(args, frontend, transport, certificate, key))
    except Exception as error:
        for frontend, transport in coordinates[len(rows):]:
            rows.append({"frontend": frontend, "transport": transport, "success": False, "checks": [], "error": str(error)})
    summary = {"performance_eligible": False, "expected": 6, "passed": sum(row["success"] for row in rows),
               "not_covered": ["TLS directional reverse-data-after-close", "backpressure stress"], "coordinates": rows}
    (args.output_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    lines = ["# CONNECT interoperability", "", "Correctness checks only; performance_eligible=false.", "",
             "| Frontend | Transport | Result | Error |", "|---|---|---|---|"]
    lines += [f"| {r['frontend']} | {r['transport']} | {'PASS' if r['success'] else 'FAIL'} | {r['error'].replace('|', '/')} |" for r in rows]
    lines += ["", "Inner TLS 1.3 is verified independently over each outer transport.",
              "Not covered: TLS reverse-data-after-close, backpressure stress."]
    (args.output_dir / "summary.md").write_text("\n".join(lines) + "\n")
    return 0 if summary["passed"] == 6 else 1


if __name__ == "__main__":
    raise SystemExit(main())
