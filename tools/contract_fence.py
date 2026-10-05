#!/usr/bin/env python3.11
"""
contract_fence.py — make declared contracts self-verifying.

A header comment that says "No malloc" is a CLAIM, not a constraint. Nothing
compiles it. This reads each header's own prose for the constraints it declares,
strips comments from the code, then greps what remains.

It is a fence, not a gate: violations are reported loudly (WARN by default,
hard FAIL behind --strict), never silently fixed.

Usage:
  python3.11 tools/contract_fence.py                      # scan core/*.h
  python3.11 tools/contract_fence.py --dir core --strict  # CI / pre-commit
  python3.11 tools/contract_fence.py --json                # machine-readable
"""

import argparse
import glob
import json
import os
import re
import sys

# ── Constraint registry ──────────────────────────────────────────────
# claim    — how the prose declares the constraint (searched in raw text)
# violation— what breaking it looks like in code (searched in stripped text)
# scoped   — qualifiers that narrow a claim below what a grep can decide
RULES = [
    {
        "id": "no-malloc",
        "claim": re.compile(r"\b[Nn][Oo]\s+(?:malloc|heap|alloc)\b|\bNO[_ ]?MALLOC\b"),
        "violation": re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\("),
        "scoped": re.compile(
            r"\b(?:hot path|steady state|per entry|beyond|except|only)\b", re.I
        ),
    },
    {
        "id": "no-float",
        "claim": re.compile(r"\b[Nn][Oo]\s+float\b|\bNO[_ ]?FLOAT\b|\bfree of all float\b"),
        "violation": re.compile(r"\b(?:float|double)\b"),
        "scoped": re.compile(r"\b(?:division|except|only|unless|outside)\b", re.I),
    },
    {
        "id": "no-io",
        "claim": re.compile(r"\b[Nn]o\s+(?:I/O|stdio|printf)\b"),
        "violation": re.compile(r"\b(?:printf|fprintf|puts|fopen|fwrite|fread)\s*\("),
        "scoped": re.compile(r"\b(?:except|only|path|tool|bench)\b", re.I),
    },
]

_BLOCK = re.compile(r"/\*.*?\*/", re.S)
_LINE = re.compile(r"//[^\n]*")
_CODE = re.compile(r"\S")
# whole directives, not just the leading '#': `#include <stdint.h>` must go as
# one unit or its angle bracket reads as the first token of real code
_PREPROC = re.compile(
    r"^[ \t]*#(?:include|define|ifndef|ifdef|if|else|elif|endif"
    r"|pragma|undef|line|error|warning)\b[^\n]*$",
    re.M,
)


def _blank(m: re.Match) -> str:
    """Erase a comment, keeping length and newlines so indices still line up."""
    return re.sub(r"[^\n]", " ", m.group(0))


def blank_comments(src: str) -> str:
    return _LINE.sub(_blank, _BLOCK.sub(_blank, src))


def file_scope(raw: str) -> str:
    """The leading block: everything before the first real code character.

    A claim counts as a contract only if it describes the whole file. A passing
    note mid-code ("/* static, no heap alloc */") is a remark about a local —
    flagging it is noise, and noise teaches people to ignore the fence.

    Preprocessor lines sit before code but declare nothing, so skip whole
    directives — `#include <stdint.h>` has to go as one unit, otherwise its
    angle bracket reads as the first token of real code.

    Indices must be taken from `hidden`, not from a shortened string: dropping
    a preprocessor line would shift every later offset. So blank the directive
    in place — length preserved, offsets stay honest — then slice `raw`.
    """
    blanked = blank_comments(raw)
    hidden = _PREPROC.sub(_blank, blanked)
    code = _CODE.search(hidden)
    return raw if code is None else raw[: code.start()]


def analyse(path: str):
    """Return (violations, scoped) for one header."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        raw = fh.read()
    code = blank_comments(raw)
    scope = file_scope(raw)
    scope_lines = scope.splitlines()

    violations, scoped = [], []
    for rule in RULES:
        claims = list(rule["claim"].finditer(scope))
        if not claims:
            continue
        breaks = list(rule["violation"].finditer(code))
        if not breaks:
            continue
        rec = {
            "file": path.replace("\\", "/"),
            "rule": rule["id"],
            "count": len(breaks),
            "hits": sorted({m.group(0) for m in breaks})[:3],
        }
        # a qualifier can sit right next to the claim ("No malloc in hot path")
        # so read the whole line, not just the matched span
        at = {raw.count("\n", 0, m.start()) for m in claims}
        qualified = any(
            rule["scoped"].search(scope_lines[i]) for i in at if i < len(scope_lines)
        )
        (scoped if qualified else violations).append(rec)
    return violations, scoped


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="core")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.h")))
    violations, scoped = [], []
    for f in files:
        v, s = analyse(f)
        violations += v
        scoped += s

    if args.json:
        print(json.dumps(
            {"scanned": len(files), "violations": violations, "scoped": scoped},
            indent=2,
        ))
        return 1 if (args.strict and violations) else 0

    print("════════ CONTRACT FENCE ════════")
    print(f"scanned {len(files)} headers")
    for label, group, tag in (
        ("violated", violations, "⚠"),
        ("scoped claim — qualifier narrows it below what grep can decide", scoped, "·"),
    ):
        if not group:
            continue
        print(f"\n{tag} {len(group)} {label}")
        for v in group:
            print(f"    {v['file']} · {v['rule']} · {v['count']}× ({', '.join(v['hits'])})")
    if not violations and not scoped:
        print("✅ no declared contract violated")

    if args.strict and violations:
        print("\n❌ strict: treating as failure")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
