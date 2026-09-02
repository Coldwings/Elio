#!/usr/bin/env python3
"""Pilot: contract-row symbol drift check (governance feedback, proposal 2).

Hypothesis under test: the first column of wiki/API-Contracts.md already names
symbols in a machine-extractable form, so a drift check (does the symbol still
exist in the headers?) needs NO new anchor syntax. Scope: one pilot section.

Usage:
  tools/check-contract-symbols.py [--section "Synchronization Primitives"]
  tools/check-contract-symbols.py --self-test

Exit code: 0 if every row's symbol resolves, 1 if any is missing.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONTRACTS = REPO / "wiki" / "API-Contracts.md"
INCLUDE = REPO / "include" / "elio"

SECTION = "Synchronization Primitives"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def load_headers(subdir: str) -> str:
    root = INCLUDE / subdir
    if not root.is_dir():
        return ""
    return "\n".join(strip_comments(p.read_text(errors="replace")) for p in root.glob("*.hpp"))


def split_symbols(cell: str) -> list[str]:
    """First-column cell -> individual symbol paths.
    Handles `a and b`, comma lists, and trailing signatures/templates."""
    parts = re.split(r"\s+and\s+|,\s*", cell)
    out = []
    for p in parts:
        p = p.strip()
        p = re.sub(r"\(.*$", "", p)      # drop signature: wait(sync::mutex&)
        p = re.sub(r"<.*$", "", p)       # drop template args: channel<T>
        p = p.strip()
        if p:
            out.append(p)
    return out


def main() -> int:
    section = SECTION
    args = sys.argv[1:]
    self_test = "--self-test" in args
    if "--section" in args:
        section = args[args.index("--section") + 1]

    text = CONTRACTS.read_text()
    m = re.search(rf"^## {re.escape(section)}\s*$(.*?)(?=^## |\Z)", text, flags=re.S | re.M)
    if not m:
        print(f"section not found: {section}")
        return 1

    rows = re.findall(r"^\| `([^`]+)`", m.group(1), flags=re.M)
    if self_test:
        rows.append("sync::no_such_widget")

    header_cache: dict[str, str] = {}
    missing = 0
    for row in rows:
        for sym in split_symbols(row):
            segs = sym.split("::")
            subdir = segs[0] if len(segs) > 1 else ""
            leaf = segs[-1]
            if subdir not in header_cache:
                header_cache[subdir] = load_headers(subdir)
            body = header_cache[subdir]
            found = bool(body) and bool(re.search(rf"\b{re.escape(leaf)}\b", body))
            mark = "OK     " if found else "MISSING"
            if not found:
                missing += 1
            print(f"  [{mark}] {sym:45s} -> include/elio/{subdir}/")
    print(f"{len(rows)} rows checked in '{section}', {missing} missing")
    if self_test:
        ok = missing == 1  # exactly the injected fake must fail
        print(f"self-test: {'PASS (fake symbol detected)' if ok else 'FAIL'}")
        return 0 if ok else 1
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
