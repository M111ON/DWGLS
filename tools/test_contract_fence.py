#!/usr/bin/env python3.11
"""
Self-test for contract_fence.py — six cases that pin the claim/violation
distinction. Run: python3.11 tools/test_contract_fence.py
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from contract_fence import analyse  # noqa: E402

CASES = {
    # name: (header source, expected violations, expected scoped)
    "no_claim": (
        "/* helper */\nstatic inline void f(void) { float x = 1.0f; }\n", 0, 0
    ),
    "unqualified_claim_breached": (
        "/* No float. */\nstatic inline float f(void) { return 1.0f; }\n", 1, 0
    ),
    "unqualified_claim_kept": (
        "/* No malloc. No float. */\nstatic inline int f(void) { return 1; }\n", 0, 0
    ),
    "midfile_aside_ignored": (
        "/* header */\nstatic inline int g(void) {\n"
        "  float y = 2.0f; /* no float */\n  return (int)y;\n}\n", 0, 0
    ),
    "scoped_claim": (
        "/* No malloc in hot path. */\n#include <stdlib.h>\n"
        "static inline void *f(void) { return malloc(8); }\n", 0, 1
    ),
    "claim_after_preprocessor": (
        "#include <stdint.h>\n#define X 1\n/* No float. */\n"
        "static inline float f(void) { return 1.0f; }\n", 1, 0
    ),
}


def main() -> int:
    tmp = tempfile.mkdtemp(prefix="cf_test_")
    failed = []
    for name, (src, want_v, want_s) in CASES.items():
        p = os.path.join(tmp, name + ".h")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(src)
        got_v, got_s = analyse(p)
        ok = len(got_v) == want_v and len(got_s) == want_s
        print(f"  {'✅' if ok else '❌'} {name} — v={len(got_v)}/{want_v} s={len(got_s)}/{want_s}")
        if not ok:
            failed.append(name)

    print("───────────────────────────────────────")
    print(f"PASS: {len(CASES) - len(failed)} / {len(CASES)}  FAIL: {len(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
