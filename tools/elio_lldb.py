#!/usr/bin/env python3
"""LLDB import entrypoint that loads the hyphen-named implementation.

Use this file with:
  command script import /path/to/tools/elio_lldb.py
"""

import importlib.util
import pathlib


def _load_impl():
    script_path = pathlib.Path(__file__).with_name("elio-lldb.py")
    spec = importlib.util.spec_from_file_location("elio_lldb_impl", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


_impl = _load_impl()


def __getattr__(name):
    return getattr(_impl, name)


def __lldb_init_module(debugger, internal_dict):
    return _impl.__lldb_init_module(debugger, internal_dict)
