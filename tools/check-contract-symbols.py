#!/usr/bin/env python3
"""Pilot: contract-row symbol drift check (governance feedback, proposal 2).

Hypothesis under test: the first column of wiki/API-Contracts.md already names
symbols in a machine-extractable form, so a drift check (does the symbol still
exist in the headers?) needs NO new anchor syntax. Scope: one pilot section.

Resolution rules:
- Text is normalized first: single-pass comment/string stripping, then
  preprocessor handling (the first branch of every #if/#ifdef chain is kept,
  later branches dropped, so brace accounting survives the duplicated-brace
  pattern this repo uses in #ifdef constructors), then all directive lines
  removed.
- A namespace-level row (`sync::mutex`) requires a DEFINITION of the leaf
  (class/struct/enum body, or using-alias) at DIRECT scope of the public
  namespace path elio::<subdir> (namespace-relative brace depth 1).
  Forward/friend declarations, template parameters, elaborated type
  specifiers/return types, nested-namespace types, and nested-class members
  do not count.
- A member row (`sync::condition_variable::wait`) additionally requires
  `leaf(` in declaration context (not preceded by `.`, `->`, or `:` — the
  last excludes mem-initializers) at direct member scope (brace-depth 1) of
  the owner class body.
- Function-style rows (e.g. `runtime::current_worker_id()`) are not validated
  by this pilot; future work.

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


def strip_comments_and_strings(text: str) -> str:
    """Single left-to-right pass: comments and string/char literals become
    spaces (a `//` inside a string literal must not start a comment).
    Newlines are preserved."""
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            if j == -1:
                break
            out.append("\n")
            i = j + 1
        elif two == "/*":
            j = text.find("*/", i + 2)
            seg = text[i:(j + 2 if j != -1 else n)]
            out.append("\n" * seg.count("\n"))
            i = j + 2 if j != -1 else n
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    break
                j += 1
            j = min(j + 1, n)
            out.append('""' if quote == '"' else "' '")
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def select_pp_branch(text: str) -> str:
    """Keep the first branch of every #if/#ifdef/#ifndef chain and drop
    #elif/#else branches, so brace accounting survives duplicated-brace
    conditional patterns. All directive lines are removed afterwards."""
    out = []
    stack: list[bool] = []
    active = True
    for line in text.splitlines():
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", line)
        if m:
            d = m.group(1)
            if d in ("if", "ifdef", "ifndef"):
                stack.append(active)          # take the first branch
            elif d in ("elif", "else"):
                active = stack[-1] and False if stack else False
            elif d == "endif":
                active = stack.pop() if stack else True
            out.append("")
        elif line.lstrip().startswith("#"):
            out.append("")                    # drop all other directives
        else:
            out.append(line if active else "")
    return "\n".join(out)


def strip_header(text: str) -> str:
    return select_pp_branch(strip_comments_and_strings(text))


def load_headers(subdir: str) -> dict[str, str]:
    root = INCLUDE / subdir
    if not root.is_dir():
        return {}
    return {str(p.relative_to(root)): strip_header(p.read_text(errors="replace"))
            for p in root.rglob("*.hpp")}


NS_TOKEN = re.compile(r"\bnamespace\s+([\w]+(?:::[\w]+)*)\s*\{|\bnamespace\s*\{|[{}]")


def scope_at(text: str, pos: int) -> tuple[tuple, int]:
    """(cumulative namespace path, brace depth within the innermost
    namespace) at `pos`. Handles `namespace a::b {`, nested forms, and
    anonymous namespaces."""
    path: list[str] = []
    stack: list[tuple | None] = []
    depth = 0
    for t in NS_TOKEN.finditer(text, 0, max(0, pos)):
        tok = t.group(0)
        if tok == "{":
            stack.append(None)
            depth += 1
        elif tok == "}":
            entry = stack.pop() if stack else None
            if entry:
                parts, _ = entry
                del path[-len(parts):]
            depth -= 1
        else:
            parts = tuple(t.group(1).split("::")) if t.group(1) else ("<anon>",)
            path.extend(parts)
            stack.append((parts, depth))
            depth += 1
    ns_entry_depths = [d for e in stack if e for _, d in [e]]
    rel = depth - ns_entry_depths[-1] if ns_entry_depths else depth
    return tuple(path), rel


def defn_candidates(text: str, name: str):
    """Candidate type-definition/using-alias matches for `name`.
    `[^;{]*{` excludes forward/friend declarations; candidates reached from
    `<`, `,`, `(` (template parameters, elaborated specifiers) are rejected;
    a `(` between name and `{` (elaborated return type with inline body)
    is also rejected."""
    pat = re.compile(
        rf"\b(?:class|struct|enum(?:\s+class)?)\s+{re.escape(name)}\b([^;{{]*)\{{")
    for m in pat.finditer(text):
        k = m.start() - 1
        while k >= 0 and text[k] in " \t\n":
            k -= 1
        if k >= 0 and text[k] in "<,(":
            continue
        if "(" in m.group(1) or ")" in m.group(1):
            continue
        yield m
    for m in re.finditer(rf"\busing\s+{re.escape(name)}\s*=", text):
        yield m


def has_definition(cache: dict[str, str], name: str, want_ns: tuple) -> bool:
    for text in cache.values():
        for m in defn_candidates(text, name):
            path, rel = scope_at(text, m.start())
            if path == want_ns and rel == 1:
                return True
    return False


def class_bodies(cache: dict[str, str], owner: str, want_ns: tuple) -> list[str]:
    """Owner class/struct bodies at direct scope of the public namespace
    (brace-matched on normalized text)."""
    bodies = []
    pat = re.compile(rf"\b(?:class|struct)\s+{re.escape(owner)}\b([^;{{]*)\{{")
    for text in cache.values():
        for m in pat.finditer(text):
            k = m.start() - 1
            while k >= 0 and text[k] in " \t\n":
                k -= 1
            if k >= 0 and text[k] in "<,(":
                continue
            if "(" in m.group(1) or ")" in m.group(1):
                continue
            path, rel = scope_at(text, m.start())
            if path != want_ns or rel != 1:
                continue
            i = text.index("{", m.end() - 1)
            depth = 0
            for j in range(i, len(text)):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        bodies.append(text[i + 1:j])
                        break
    return bodies


def has_member(bodies: list[str], leaf: str) -> bool:
    """`leaf(` in declaration context at direct member scope (brace-depth 1
    relative to the owner body). Candidates whose last non-space predecessor
    is `.`, `->`, `:`, or `,` (member call, mem-initializer at any position)
    do not count."""
    leaf_pat = re.compile(rf"\b{re.escape(leaf)}\s*\(")
    for body in bodies:
        depths = [1] * (len(body) + 1)
        d = 1
        for idx, ch in enumerate(body):
            if ch == "{":
                d += 1
            elif ch == "}":
                d -= 1
            depths[idx + 1] = d
        for m in leaf_pat.finditer(body):
            k = m.start() - 1
            while k >= 0 and body[k] in " \t\n":
                k -= 1
            if k >= 0 and body[k] in ".,>:":
                continue
            if depths[m.start()] == 1:
                return True
    return False


def symbol_exists(sym: str, cache_by_dir: dict[str, dict[str, str]]) -> bool:
    segs = sym.split("::")
    subdir = segs[0] if len(segs) > 1 else ""
    if subdir not in cache_by_dir:
        cache_by_dir[subdir] = load_headers(subdir)
    cache = cache_by_dir[subdir]
    if not cache:
        return False
    want_ns = ("elio", subdir)
    if len(segs) <= 2:
        return has_definition(cache, segs[-1], want_ns)
    owner, leaf = segs[-2], segs[-1]
    bodies = class_bodies(cache, owner, want_ns)
    if not bodies:
        return False
    return has_member(bodies, leaf)


def split_symbols(cell: str) -> list[str]:
    """First-column cell -> individual symbol paths from EVERY backtick span.
    Signatures and template args are stripped BEFORE splitting, so
    `send(value, token)` cannot leak `token)` as a fake symbol."""
    out = []
    for span in re.findall(r"`([^`]+)`", cell):
        span = re.sub(r"\([^()]*\)", "", span)
        span = re.sub(r"<[^<>]*>", "", span)
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
        fake = {"fake.hpp": strip_header(
            "namespace elio::fake {\n"
            "class Fwd;\n"
            "class Real { public: void wait(int); };\n"
            "class Nested { public: class Inner { public: void wait(int); }; };\n"
            "struct HasBody { void m(); };\n"
            "using Alias = Real;\n"
            "void take(class Elab s) { (void)s; }\n"
            "template<class TpParam> struct helper { TpParam s; };\n"
            "class ElabRet make_elab() { return {}; }\n"
            "struct Wrap { class Wrapped { public: void lock(); }; };\n"
            "struct W2 { using AliasInClass = int; };\n"
            "class WithCtor { public: int wait; WithCtor() : wait(0) {} };\n"
            "class WithCtor2 { public: int s_; int wait; WithCtor2() : s_(0), wait(0) {} };\n"
            "#ifdef HOOKS\n"
            "class PpCtor { public: PpCtor() : a_{0} {\n"
            "#else\n"
            "class PpCtor { public: PpCtor() : b_{0} {\n"
            "#endif\n"
            "  } };\n"
            "class AfterIfdef { public: void wait(int); };\n"
            "}\n"
            "namespace elio::fake::detail {\n"
            "class Hidden { public: void wait(int); };\n"
            "}\n"
            "const char* url = \"https://example.com/x\";  // string with // inside\n"
            "namespace elio::fake { class AfterString { public: void m(); }; }\n"
        )}
        cache = {"fake": fake}
        cases = [
            ("fake::Fwd", False, "forward declaration must not count"),
            ("fake::Real", True, "class definition must count"),
            ("fake::HasBody", True, "struct definition must count"),
            ("fake::Alias", True, "using alias must count"),
            ("fake::Elab", False, "elaborated type specifier must not count"),
            ("fake::TpParam", False, "template parameter must not count"),
            ("fake::ElabRet", False, "elaborated return type must not count"),
            ("fake::Wrapped", False, "nested-class member must not count"),
            ("fake::AliasInClass", False, "member-scope alias must not count"),
            ("fake::Hidden", False, "nested-namespace type must not count"),
            ("fake::AfterString", True, "string containing // must not corrupt parsing"),
            ("fake::AfterIfdef", True, "duplicated-brace #ifdef ctor must not corrupt accounting"),
            ("fake::AfterIfdef::wait", True, "member after #ifdef block must still resolve"),
            ("fake::Real::wait", True, "member declaration in owner body must count"),
            ("fake::Nested::wait", False, "member of nested class must not count"),
            ("fake::Hidden::wait", False, "member of detail type must not count"),
            ("fake::WithCtor::wait", False, "mem-initializer and data member must not count"),
            ("fake::WithCtor2::wait", False, "non-first mem-initializer must not count"),
        ]
        for sym, want, why in cases:
            got = symbol_exists(sym, {"fake": dict(fake)})
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
