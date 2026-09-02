#!/usr/bin/env python3
"""Pilot: contract-row symbol drift check (governance feedback, proposal 2).

Hypothesis under test: the first column of wiki/API-Contracts.md already names
symbols in a machine-extractable form, so a drift check (does the symbol still
exist in the headers?) needs NO new anchor syntax. Scope: one pilot section.

Resolution rules:
- A namespace-level row (`sync::mutex`) requires a DEFINITION of the leaf
  (class/struct/enum body, or using-alias) in the mapped include dir, in
  comment-, string-literal-, and `#include`-line-stripped header text.
  Forward declarations (`class X;`) and friend declarations do not count.
- A member row (`sync::condition_variable::wait`) additionally requires the
  member name in declaration context (`name(` not preceded by `.` or `->`)
  inside the owner class BODY — not merely anywhere in the owner's file, and
  never from another type's member (`sync::semaphore::wait` cannot be
  satisfied by `sync::event::wait`).
- Function-style rows (e.g. `runtime::current_worker_id()`) are not validated
  by this pilot (patterns cover types and member functions); future work.

Usage:
  tools/check-contract-symbols.py [--section "Synchronization Primitives"]
  tools/check-contract-symbols.py --self-test

Exit code: 0 if every row's symbol resolves, 1 if any is missing or the section
yields no rows (format drift), 2 on usage error.
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
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "' '", text)
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#include"))


def load_headers(subdir: str) -> dict[str, str]:
    root = INCLUDE / subdir
    if not root.is_dir():
        return {}
    return {str(p.relative_to(root)): strip_header(p.read_text(errors="replace"))
            for p in root.rglob("*.hpp")}


def defn_pattern(name: str) -> re.Pattern:
    """Definition context: a class/struct/enum body, or a using-alias.
    `[^;{]*{` excludes forward declarations (`class X;`) and friend
    declarations (`friend class X;`), which never reach a body."""
    return re.compile(
        rf"(?<!friend )(?<!friend  )\b(?:class|struct|enum(?:\s+class)?)\s+"
        rf"{re.escape(name)}\b[^;{{]*\{{"
        rf"|\busing\s+{re.escape(name)}\s*="
    )


def class_bodies(cache: dict[str, str], owner: str) -> list[str]:
    """Extract owner class/struct bodies by brace matching (comments and
    string literals already stripped, so braces are structural)."""
    pat = re.compile(rf"(?<!friend )\b(?:class|struct)\s+{re.escape(owner)}\b[^;{{]*\{{")
    bodies = []
    for text in cache.values():
        pos = 0
        while (m := pat.search(text, pos)):
            i = text.index("{", m.end() - 1)
            depth = 0
            for j in range(i, len(text)):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        bodies.append(text[i:j])
                        break
            pos = m.end()
    return bodies


def symbol_exists(sym: str, cache_by_dir: dict[str, dict[str, str]]) -> bool:
    segs = sym.split("::")
    subdir = segs[0] if len(segs) > 1 else ""
    if subdir not in cache_by_dir:
        cache_by_dir[subdir] = load_headers(subdir)
    cache = cache_by_dir[subdir]
    if not cache:
        return False
    if len(segs) <= 2:
        pat = defn_pattern(segs[-1])
        return any(pat.search(body) for body in cache.values())
    owner, leaf = segs[-2], segs[-1]
    bodies = class_bodies(cache, owner)
    if not bodies:
        return False
    # Member declaration context inside the owner body: `leaf(` not reached
    # through a call (`x.leaf(`, `x->leaf(`) — incidental uses do not count.
    leaf_pat = re.compile(rf"(?<![.>])\b{re.escape(leaf)}\s*\(")
    return any(leaf_pat.search(b) for b in bodies)


def split_symbols(cell: str) -> list[str]:
    """First-column cell -> individual symbol paths from EVERY backtick span.
    Handles `` `a`, `b` `` lists and `a and b` connectors. Signatures and
    template args are stripped BEFORE splitting, so `send(value, token)`
    cannot leak `token)` as a fake symbol."""
    out = []
    for span in re.findall(r"`([^`]+)`", cell):
        span = re.sub(r"\([^()]*\)", "", span)   # drop signature
        span = re.sub(r"<[^<>]*>", "", span)     # drop template args
        for p in re.split(r"\s+and\s+|,\s*", span):
            p = p.strip()
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
        if i + 1 >= len(args) or args[i + 1].startswith("--"):
            print("usage error: --section requires a value", file=sys.stderr)
            return 2
        section = args[i + 1]

    text = CONTRACTS.read_text()
    m = re.search(rf"^## {re.escape(section)}\s*$(.*?)(?=^## |\Z)", text, flags=re.S | re.M)
    if not m:
        print(f"section not found: {section}")
        return 1

    cells = list(iter_rows(m.group(1)))
    if not cells:
        print(f"error: no contract rows parsed in section '{section}' — table format drift?")
        return 1

    if self_test:
        # fixture-level pins (deterministic, independent of repo contents)
        fake = {
            "a.hpp": ("class Fwd;\n"
                      "class Real { public: void wait(int); };\n"
                      "class Other { public: void other(); };\n"
                      "struct HasBody { void m(); };\n"),
            "b.hpp": "friend class Friendly;\nusing Alias = Real;\n",
        }
        cache = {"fake": fake}
        cases = [
            ("fake::Fwd", False, "forward declaration must not count"),
            ("fake::Friendly", False, "friend declaration must not count"),
            ("fake::Real", True, "class definition must count"),
            ("fake::HasBody", True, "struct definition must count"),
            ("fake::Alias", True, "using alias must count"),
            ("fake::Other::wait", False, "member must bind to its own class body"),
            ("fake::Real::wait", True, "member declaration in owner body must count"),
        ]
        for sym, want, why in cases:
            got = symbol_exists(sym, {k: dict(v) for k, v in cache.items()})
            assert got == want, f"self-test case failed: {sym} -> {got}, want {want} ({why})"
        # integration fakes against the real section
        cells.append("`sync::no_such_widget`")
        cells.append("`sync::semaphore::wait`")   # qualifier-confusion regression
        fixture = list(iter_rows("| `sync::event`, `sync::no_such_widget_2` and `sync::mutex` | x | y |"))
        got = [s for c in fixture for s in split_symbols(c)]
        assert got == ["sync::event", "sync::no_such_widget_2", "sync::mutex"], got
        cells.append(fixture[0])
        sig = split_symbols("`sync::channel::send(value, token)`")
        assert sig == ["sync::channel::send"], sig  # no `token)` leak

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
        ok = missing == 3  # exactly the three injected integration fakes must fail
        print(f"self-test: {'PASS (all fakes detected)' if ok else 'FAIL'}")
        return 0 if ok else 1
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
