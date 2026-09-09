#!/usr/bin/env python3
"""Pure validator/protocol tests; no real sender build or TLS benchmark required."""

import copy
import hashlib
import json
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import http_streaming_metrics as metrics


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.digest = metrics.body_digest()
        self.planned = [{"transport": transport, "mode": mode, "trial": f"trial_{index}"}
                        for index, (transport, mode) in enumerate(metrics.COORDINATES)]
        self.clients, self.servers = [], []
        for plan in self.planned:
            encrypted = plan["transport"] == "tls"
            server = {**plan, "event": "result", "success": True,
                      "measured_count": 16, "confirmed_body_bytes": 16 * metrics.BODY_BYTES,
                      "server_cpu_ns": 16000, "body_bytes": metrics.BODY_BYTES,
                      "stream_block_bytes": metrics.BLOCK_BYTES, "warmup_count": 1,
                      "probe_success": True, "backend": "epoll", "workers": 1,
                      "build_type": "Release", "compiler": "test-compiler", "build_head": "a" * 40,
                      "build_dirty": True,
                      "openssl_version": "test-openssl", "tls_version": "TLSv1.3" if encrypted else "none",
                      "tls_cipher": "TLS_AES_256_GCM_SHA384" if encrypted else "none", "responses": []}
            client = {**plan, "success": True, "measured_count": 16,
                      "verified_body_bytes": 16 * metrics.BODY_BYTES, "elapsed_ns": 1_000_000_000,
                      "server_returncode": 0,
                      "client_cpu_ns": 123000, "ready": {"backend": "epoll", "workers": 1},
                      "tls_version": server["tls_version"], "tls_cipher": server["tls_cipher"], "responses": []}
            for phase, sequence in metrics.SEQUENCES:
                size = 2 if phase == "probe" else metrics.BODY_BYTES
                server["responses"].append({"phase": phase, "sequence": sequence, "success": True,
                    "reusable": True, "confirmed_body_bytes": size, "error": 0, "transport_error": 0,
                    "server_cpu_ns": 1000 if phase == "measure" else 0})
                client["responses"].append({"trial": plan["trial"], "phase": phase, "sequence": sequence,
                    "body_bytes": size, "sha256": hashlib.sha256(b"ok").hexdigest() if phase == "probe" else self.digest,
                    "framing": "chunked" if plan["mode"] == "chunked" and phase != "probe" else "content_length",
                    "status": 200})
            self.servers.append(server)
            self.clients.append(client)

    def summary(self):
        return metrics.build_summary(self.planned, self.clients, self.servers, self.digest)

    def test_exact_six_coordinates_pass_with_diagnostic_metrics(self):
        summary = self.summary()
        self.assertTrue(summary["success"])
        self.assertFalse(summary["performance_eligible"])
        self.assertEqual(summary["covered_coordinates"], 6)
        self.assertEqual(summary["rows"][0]["pipeline_mib_per_second"], 64)
        self.assertIn("6/6", metrics.render_summary(summary))

    def test_server_order_does_not_determine_attribution(self):
        self.servers.reverse()
        self.assertTrue(self.summary()["success"])

    def test_duplicate_server_invalidates_coordinate(self):
        self.servers.append(copy.deepcopy(self.servers[0]))
        summary = self.summary()
        self.assertFalse(summary["success"])
        self.assertEqual(summary["covered_coordinates"], 5)
        self.assertNotIn("pipeline_mib_per_second", summary["rows"][0])

    def test_duplicate_client_invalidates_coordinate(self):
        self.clients.append(copy.deepcopy(self.clients[0]))
        self.assertEqual(self.summary()["covered_coordinates"], 5)

    def test_missing_server_is_incomplete(self):
        self.servers.pop()
        self.assertEqual(self.summary()["covered_coordinates"], 5)

    def test_wrong_trial_is_not_attributable(self):
        self.servers[0]["trial"] = "foreign"
        summary = self.summary()
        self.assertFalse(summary["success"])
        self.assertEqual(summary["covered_coordinates"], 5)
        self.assertTrue(summary["errors"])

    def test_wrong_coordinate_with_matching_trial_rejected(self):
        self.servers[0]["transport"] = "tls"
        self.assertEqual(self.summary()["covered_coordinates"], 5)

    def test_extra_foreign_evidence_fails_even_with_six_valid_rows(self):
        self.servers.append({"trial": "unexpected"})
        self.assertFalse(self.summary()["success"])

    def test_invalid_manifest_rejected(self):
        self.planned[-1] = self.planned[0].copy()
        self.assertFalse(self.summary()["success"])
        self.assertEqual(self.summary()["covered_coordinates"], 0)

    def test_non_object_server_evidence_rejected(self):
        self.servers.append(None)
        self.assertFalse(self.summary()["success"])

    def test_clock_failures_rejected(self):
        for name in ("elapsed_ns", "client_cpu_ns"):
            for value in (0, -1, True, float("nan"), float("inf"), "100"):
                with self.subTest(name=name, value=value):
                    original = self.clients[0][name]
                    self.clients[0][name] = value
                    self.assertFalse(self.summary()["success"])
                    self.clients[0][name] = original

    def test_response_contract_corruption_rejected(self):
        mutations = [
            ("success", False), ("reusable", False), ("confirmed_body_bytes", 42),
            ("sequence", 9), ("error", 1), ("transport_error", 32),
            ("server_cpu_ns", -1), ("server_cpu_ns", True),
        ]
        for key, value in mutations:
            with self.subTest(key=key):
                original = self.servers[0]["responses"][1][key]
                self.servers[0]["responses"][1][key] = value
                self.assertFalse(self.summary()["success"])
                self.servers[0]["responses"][1][key] = original

    def test_missing_and_duplicate_per_response_rows_rejected(self):
        self.servers[0]["responses"].pop()
        self.servers[1]["responses"].append(copy.deepcopy(self.servers[1]["responses"][0]))
        self.assertEqual(self.summary()["covered_coordinates"], 4)

    def test_client_digest_framing_and_probe_rejected(self):
        for key, value in (("sha256", "bad"), ("framing", "close_delimited"),
                           ("body_bytes", 3), ("trial", "wrong")):
            with self.subTest(key=key):
                original = self.clients[0]["responses"][-1][key]
                self.clients[0]["responses"][-1][key] = value
                self.assertFalse(self.summary()["success"])
                self.clients[0]["responses"][-1][key] = original

    def test_aggregate_cpu_must_equal_measured_response_sum(self):
        self.servers[0]["server_cpu_ns"] += 1
        self.assertFalse(self.summary()["success"])

    def test_tls_metadata_must_match_observed_session(self):
        self.clients[3]["tls_cipher"] = "wrong-cipher"
        self.assertFalse(self.summary()["success"])

    def test_debug_build_not_reported_as_release_measurement(self):
        self.servers[0]["build_type"] = "Debug"
        self.assertFalse(self.summary()["success"])

    def test_fractional_counts_and_boolean_status_are_rejected(self):
        self.servers[0]["measured_count"] = 16.0
        self.clients[1]["server_returncode"] = False
        self.assertEqual(self.summary()["covered_coordinates"], 4)

    def test_malformed_ready_evidence_does_not_crash_validator(self):
        self.clients[0]["ready"] = []
        self.assertEqual(self.summary()["covered_coordinates"], 5)


class MemorySocket:
    def __init__(self, wire):
        self.wire = wire
        self.offset = 0
        self.max_read = 0

    def settimeout(self, _):
        pass

    def sendall(self, _):
        pass

    def recv(self, size):
        self.max_read = max(size, self.max_read)
        data = self.wire[self.offset:self.offset + min(size, 997)]
        self.offset += len(data)
        return data


class ProtocolTests(unittest.TestCase):
    def peer(self, wire):
        peer = metrics.Peer.__new__(metrics.Peer)
        peer.socket = MemorySocket(wire)
        peer.protocol = metrics.h11.Connection(metrics.h11.CLIENT, max_incomplete_event_size=16384)
        return peer

    def response(self, body=b"ok", extra=b"", length=b"2"):
        return (b"HTTP/1.1 200 OK\r\nContent-Length: " + length + b"\r\nX-Trial-ID: trial\r\n"
                b"X-Phase: probe\r\nX-Sequence: 0\r\n" + extra + b"\r\n" + body)

    def request(self, peer, deadline=None):
        return peer.request("trial", "probe", 0, "complete", hashlib.sha256(b"ok").hexdigest(),
                            time.monotonic() + 2 if deadline is None else deadline)

    def test_probe_is_verified_without_body_accumulation(self):
        peer = self.peer(self.response())
        self.assertEqual(self.request(peer)["body_bytes"], 2)
        self.assertLessEqual(peer.socket.max_read, metrics.BLOCK_BYTES)

    def test_bad_digest_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "digest"):
            self.request(self.peer(self.response(b"no")))

    def test_wrong_echo_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "echoed"):
            self.request(self.peer(self.response().replace(b"trial\r\n", b"wrong\r\n")))

    def test_duplicate_echo_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.request(self.peer(self.response(extra=b"X-Trial-ID: trial\r\n")))

    def test_close_policy_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "reuse"):
            self.request(self.peer(self.response(extra=b"Connection: close\r\n")))

    def test_wrong_length_is_rejected_before_body(self):
        with self.assertRaisesRegex(ValueError, "framing"):
            self.request(self.peer(self.response(length=b"3")))

    def test_truncated_response_rejected(self):
        with self.assertRaises(metrics.h11.RemoteProtocolError):
            self.request(self.peer(self.response(body=b"o")))

    def test_unsolicited_trailing_bytes_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsolicited"):
            self.request(self.peer(self.response() + b"bad"))

    def test_absolute_deadline_rejected(self):
        with self.assertRaisesRegex(ValueError, "deadline"):
            self.request(self.peer(self.response()), time.monotonic() - 1)

    def test_streamed_chunked_body_digest_and_completion(self):
        body = b"x" * 8192
        wire = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Trial-ID: trial\r\n"
                b"X-Phase: measure\r\nX-Sequence: 0\r\n\r\n1000\r\n" + body[:4096] +
                b"\r\n1000\r\n" + body[4096:] + b"\r\n0\r\n\r\n")
        with mock.patch.object(metrics, "BODY_BYTES", len(body)):
            peer = self.peer(wire)
            result = peer.request("trial", "measure", 0, "chunked", hashlib.sha256(body).hexdigest(),
                                  time.monotonic() + 2)
            self.assertEqual(result["body_bytes"], len(body))
            self.assertEqual(result["framing"], "chunked")
            self.assertLessEqual(peer.socket.max_read, metrics.BLOCK_BYTES)
            with self.assertRaises(metrics.h11.RemoteProtocolError):
                self.peer(wire[:-5]).request("trial", "measure", 0, "chunked", hashlib.sha256(body).hexdigest(),
                                            time.monotonic() + 2)

    def test_response_deadline_does_not_reset_on_partial_reads(self):
        with mock.patch.object(metrics.time, "monotonic", side_effect=[n / 10 for n in range(100)]):
            with self.assertRaisesRegex(ValueError, "deadline"):
                self.request(self.peer(self.response()), deadline=0.2)

    def test_json_rejects_duplicates_nonfinite_and_partial_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "raw.jsonl"
            for raw in (b'{"event":"ready","event":"result"}\n', b'{"cpu":NaN}\n', b'{"event":"ready"}'):
                with self.subTest(raw=raw):
                    path.write_bytes(raw)
                    with self.assertRaises(ValueError):
                        metrics.read_events(path)
                    self.assertEqual(path.read_bytes(), raw)

    def test_oversized_json_record_rejected_without_rewriting_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "raw.jsonl"
            raw = b"x" * (metrics.MAX_LINE + 1)
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "oversized"):
                metrics.read_events(path)
            self.assertEqual(path.stat().st_size, len(raw))

    def test_failed_startup_retains_stdout_stderr_and_client_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            server = output / "fake-server"
            server.write_text("#!/bin/sh\nprintf 'not json\\n'\nprintf 'startup failed\\n' >&2\nexit 1\n")
            server.chmod(0o700)
            planned = {"trial": "failed", "transport": "tcp", "mode": "complete"}
            args = SimpleNamespace(trial_timeout=2, startup_timeout=1, response_timeout=1)
            client, evidence = metrics.run_coordinate(server, output, planned, None, None, args, "unused")
            self.assertFalse(client["success"])
            self.assertEqual(evidence, [])
            self.assertEqual((output / "failed/server.stdout.jsonl").read_text(), "not json\n")
            self.assertEqual((output / "failed/server.stderr.log").read_text(), "startup failed\n")
            self.assertFalse(json.loads((output / "failed/client.json").read_text())["success"])


if __name__ == "__main__":
    unittest.main()
