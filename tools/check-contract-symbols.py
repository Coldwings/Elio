#!/usr/bin/env python3
"""Pilot: contract-row symbol drift check (governance feedback, proposal 2).

Hypothesis under test: the first column of wiki/API-Contracts.md already names
symbols in a machine-extractable form, so a drift check (does the symbol still
exist in the headers?) needs NO new anchor syntax. Scope: one pilot section.

Resolution rules:
- A namespace-level row (`sync::mutex`) requires a declaration of the leaf in
  the mapped include dir (class/struct/enum/using), in comment-stripped,
  `#include`-line-stripped header text — incidental mentions do not count.
- A member row (`sync::condition_variable::wait`) additionally requires the
  owner type's declaration in the same dir, and the member name must appear in
  a file that declares the owner — so `sync::semaphore::wait` cannot be
  satisfied by `sync::event::wait`.
- Function-style rows (e.g. `runtime::current_worker_id()`) are not validated
  by this pilot (declaration patterns cover types only); treat as future work.

Usage:
  tools/check-contract-symbols.py [--section "Synchronization Primitives"]
  tools/check-contract-symbols.py --self-test

Exit code: 0 if every row's symbol resolves, 1 if any is missing, 2 on usage error.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONTRACTS = REPO / "wiki" / "API-Contracts.md"
INCLUDE = REPO / "include" / "elio"

SECTION = "Synchronization Primitives"


def strip_header(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#include"))


def load_headers(subdir: str) -> dict[str, str]:
    root = INCLUDE / subdir
    if not root.is_dir():
        return {}
    return {p.name: strip_header(p.read_text(errors="replace")) for p in root.glob("*.hpp")}


def decl_pattern(name: str) -> re.Pattern:
    return re.compile(
        rf"\b(?:class|struct|enum(?:\s+class)?)\s+{re.escape(name)}\b"
        rf"|\busing\s+{re.escape(name)}\s*="
    )


def symbol_exists(sym: str, cache_by_dir: dict[str, dict[str, str]]) -> bool:
    segs = sym.split("::")
    subdir = segs[0] if len(segs) > 1 else ""
    if subdir not in cache_by_dir:
        cache_by_dir[subdir] = load_headers(subdir)
    cache = cache_by_dir[subdir]
    if not cache:
        return False
    if len(segs) <= 2:
        pat = decl_pattern(segs[-1])
        return any(pat.search(body) for body in cache.values())
    owner, leaf = segs[-2], segs[-1]
    owner_pat = decl_pattern(owner)
    owner_files = [body for body in cache.values() if owner_pat.search(body)]
    if not owner_files:
        return False
    # Member declaration context: `leaf(` not reached through a call
    # (`x.leaf(`, `x->leaf(`, `a::leaf(`) — incidental uses do not count.
    leaf_pat = re.compile(rf"(?<![.>:])\b{re.escape(leaf)}\s*\(")
    return any(leaf_pat.search(body) for body in owner_files)


def split_symbols(cell: str) -> list[str]:
    """First-column cell -> individual symbol paths from EVERY backtick span.
    Handles `` `a`, `b` `` lists, `a and b` connectors, signatures, templates."""
    out = []
    for span in re.findall(r"`([^`]+)`", cell):
        for p in re.split(r"\s+and\s+|,\s*", span):
            p = re.sub(r"\(.*$", "", p)
            p = re.sub(r"<.*$", "", p).strip()
            if p:
                out.append(p)
    return out


def iter_rows(section_text: str):
    for m in re.finditer(r"^\| ([^|]+) \|", section_text, flags=re.M):
        cell = m.group(1).strip()
        if cell.startswith("`") and "Interface" not in cell and "---" not in cell:
            yield cell


def main() -> int:
    section = SECTION
    args = sys.argv[1:]
    self_test = "--self-test" in args
    if "--section" in args:
        i = args.index("--section")
        if i + 1 >= len(args):
            print("usage error: --section requires a value", file=sys.stderr)
            return 2
        section = args[i + 1]

    text = CONTRACTS.read_text()
    m = re.search(rf"^## {re.escape(section)}\s*$(.*?)(?=^## |\Z)", text, flags=re.S | re.M)
    if not m:
        print(f"section not found: {section}")
        return 1

    cells = list(iter_rows(m.group(1)))
    if self_test:
        cells.append("`sync::no_such_widget`")
        cells.append("`sync::semaphore::wait`")   # qualifier-confusion regression
        # multi-span parsing regression: one cell, three spans, two must resolve
        fixture = list(iter_rows("| `sync::event`, `sync::no_such_widget_2` and `sync::mutex` | x | y |"))
        got = [s for c in fixture for s in split_symbols(c)]
        assert got == ["sync::event", "sync::no_such_widget_2", "sync::mutex"], got
        cells.append(fixture[0])

    cache: dict[str, dict[str, str]] = {}
    missing = 0
    for cell in cells:
        for sym in split_symbols(cell):
            found = symbol_exists(sym, cache)
            mark = "OK     " if found else "MISSING"
            if not found:
                missing += 1
            subdir = sym.split("::")[0]
            print(f"  [{mark}] {sym:45s} -> include/elio/{subdir}/")
    print(f"{len(cells)} cells checked in '{section}', {missing} missing")
    if self_test:
        ok = missing == 3  # exactly the three injected fakes must fail
        print(f"self-test: {'PASS (all fakes detected)' if ok else 'FAIL'}")
        return 0 if ok else 1
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
