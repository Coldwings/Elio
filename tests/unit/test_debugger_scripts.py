#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import pathlib
import sys
import types
import unittest


sys.dont_write_bytecode = True

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(module)
    return module


class FakeGdbCommand:
    def __init__(self, *_args, **_kwargs):
        pass


class FakeGdbField:
    def __init__(self, name, bitpos=None):
        self.name = name
        if bitpos is not None:
            self.bitpos = bitpos


class FakeGdbType:
    def __init__(self, fields):
        self._fields = fields

    def strip_typedefs(self):
        return self

    def fields(self):
        return self._fields


class FakeLldbField:
    def __init__(self, name, offset):
        self._name = name
        self._offset = offset

    def GetName(self):
        return self._name

    def GetOffsetInBytes(self):
        return self._offset


class FakeLldbType:
    def __init__(self, fields=None):
        self._fields = fields

    def IsValid(self):
        return self._fields is not None

    def GetNumberOfFields(self):
        return len(self._fields)

    def GetFieldAtIndex(self, index):
        return self._fields[index]


class FakeLldbTarget:
    def __init__(self, promise_type):
        self._promise_type = promise_type

    def FindFirstType(self, _name):
        return self._promise_type


class FakeLldbProcess:
    def __init__(self, promise_type):
        self._target = FakeLldbTarget(promise_type)

    def GetTarget(self):
        return self._target


class DebuggerScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fake_gdb = types.ModuleType("gdb")
        fake_gdb.Command = FakeGdbCommand
        fake_gdb.COMMAND_USER = 0
        fake_gdb.COMPLETE_COMMAND = 0
        fake_gdb.lookup_type = lambda _name: (_ for _ in ()).throw(
            RuntimeError("no debug symbols"))
        sys.modules["gdb"] = fake_gdb

        fake_lldb = types.ModuleType("lldb")
        sys.modules["lldb"] = fake_lldb

        cls.gdb_module = load_module(
            "elio_gdb_test", REPO_ROOT / "tools" / "elio-gdb.py")
        cls.lldb_module = load_module(
            "elio_lldb_test", REPO_ROOT / "tools" / "elio-lldb.py")
        cls.fake_gdb = fake_gdb

    def test_unknown_metadata_is_not_fabricated(self):
        unknown = {"id": None, "state": None}
        known = {"id": 42, "state": "suspended"}

        self.assertEqual(
            self.gdb_module.frame_display_identity(unknown),
            ("?", "unknown"))
        self.assertEqual(
            self.lldb_module.frame_display_identity(unknown),
            ("?", "unknown"))
        self.assertEqual(
            self.gdb_module.frame_display_identity(known),
            ("42", "suspended"))
        self.assertEqual(
            self.lldb_module.frame_display_identity(known),
            ("42", "suspended"))

    def test_gdb_layout_uses_symbols_and_skips_static_fields(self):
        promise_type = FakeGdbType([
            FakeGdbField("FRAME_MAGIC"),
            FakeGdbField("frame_magic_", 0),
            FakeGdbField("parent_", 64),
            FakeGdbField("debug_location_", 192),
            FakeGdbField("debug_state_", 384),
            FakeGdbField("debug_worker_id_", 416),
            FakeGdbField("debug_id_", 448),
        ])
        self.fake_gdb.lookup_type = lambda _name: promise_type

        layout = self.gdb_module.get_promise_layout(8)

        self.assertEqual(layout["frame_magic_"], 0)
        self.assertEqual(layout["parent_"], 8)
        self.assertEqual(layout["debug_id_"], 56)
        self.assertTrue(layout["has_debug_metadata"])

    def test_gdb_layout_fallback_is_prefix_only(self):
        self.fake_gdb.lookup_type = lambda _name: (_ for _ in ()).throw(
            RuntimeError("no debug symbols"))

        layout = self.gdb_module.get_promise_layout(8)

        self.assertEqual(layout, {
            "frame_magic_": 0,
            "parent_": 8,
            "has_debug_metadata": False,
        })

    def test_lldb_layout_discovery_and_fallback(self):
        promise_type = FakeLldbType([
            FakeLldbField("frame_magic_", 0),
            FakeLldbField("parent_", 8),
            FakeLldbField("debug_location_", 24),
            FakeLldbField("debug_state_", 48),
            FakeLldbField("debug_worker_id_", 52),
            FakeLldbField("debug_id_", 56),
        ])

        layout = self.lldb_module.get_promise_layout(
            FakeLldbProcess(promise_type), 8)
        fallback = self.lldb_module.get_promise_layout(
            FakeLldbProcess(FakeLldbType()), 8)

        self.assertEqual(layout["debug_id_"], 56)
        self.assertTrue(layout["has_debug_metadata"])
        self.assertEqual(fallback, {
            "frame_magic_": 0,
            "parent_": 8,
            "has_debug_metadata": False,
        })


if __name__ == "__main__":
    unittest.main()
