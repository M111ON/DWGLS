"""const_map.py — is this number a *constant with a derivation* or just a *size*?

The problem this solves: DWGLS is built entirely from 2-3-smooth numbers, and there are
only 78 of them below 20736. So "my number appears in the repo" is not evidence — every
candidate lands somewhere. The only useful question is whether the DERIVATION matches.

usage:
    python tools/const_map.py 5184 1728 25 360      # look numbers up
    python tools/const_map.py --audit               # how crowded is the space
    python tools/const_map.py --expr "72*5" "45*8"  # evaluate candidates
"""
import os
import re
import sys
from collections import defaultdict

ROOTS = ["core", "include", "tests"]
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+([^/\n]+?)\s*(?:/\*(.*?)\*/)?\s*$")
SAFE = re.compile(r"^[0-9uUlL+\-*/() xX]+$")


def evaluate(expr):
    e = expr.strip().replace("u", "").replace("U", "").replace("l", "").replace("L", "")
    if not e or not SAFE.match(expr):
        return None
    try:
        v = eval(e, {"__builtins__": {}}, {})  # whitelisted chars only
    except Exception:
        return None
    if isinstance(v, int) and 0 < v < 10 ** 12:
        return v
    return None


def factor(n):
    out, x = [], n
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        k = 0
        while x % p == 0:
            x //= p
            k += 1
        if k:
            out.append((p, k))
    if x > 1:
        out.append((x, 1))
    return out


def smooth23(n):
    x = n
    for p in (2, 3):
        while x % p == 0:
            x //= p
    return x == 1


def fmt_factor(f):
    return " * ".join("%d^%d" % (p, k) if k > 1 else str(p) for p, k in f)


def scan():
    table = defaultdict(list)
    for root in ROOTS:
        for dirpath, _, files in os.walk(root):
            for fn in files:
                if not fn.endswith((".h", ".c")):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
                except OSError:
                    continue
                for i, line in enumerate(lines, 1):
                    m = DEFINE.match(line)
                    if not m:
                        continue
                    name, expr, note = m.group(1), m.group(2), (m.group(3) or "").strip()
                    v = evaluate(expr)
                    if v is None:
                        continue
                    table[v].append((name, expr.strip(), note, "%s:%d" % (path.replace("\\", "/"), i)))
    return table


def lookup(table, n):
    hits = table.get(n)
    print("\n=== %d ===" % n)
    print("  factor      : %s" % fmt_factor(factor(n)))
    print("  2-3-smooth  : %s" % ("YES (so it is guaranteed to appear somewhere)" if smooth23(n)
                                  else "no  (carries a prime outside {2,3} -- it can only appear as a chosen size)"))
    if not hits:
        print("  defines     : none")
        return
    print("  defines     : %d" % len(hits))
    for name, expr, note, loc in hits[:12]:
        print("    %-26s %-28s %s" % (name, expr, note))
        print("    %-26s %s" % ("", loc))
    if len(hits) > 12:
        print("    ... %d more" % (len(hits) - 12))


def audit(table):
    vals = sorted(table)
    s23 = [v for v in vals if smooth23(v)]
    print("distinct #define values : %d" % len(vals))
    print("  2-3-smooth            : %d (%.1f%%)" % (len(s23), 100.0 * len(s23) / len(vals)))
    print("  other                 : %d" % (len(vals) - len(s23)))
    all23 = []
    a = 1
    while a <= 20736:
        b = a
        while b <= 20736:
            all23.append(b)
            b *= 3
        a *= 2
    print("\n2-3-smooth numbers <= 20736 : %d" % len(set(all23)))
    covered = sum(1 for v in set(all23) if v in table)
    print("  of which used by a #define: %d (%.0f%%)" % (covered, 100.0 * covered / len(set(all23))))
    print("\n=> any 2-3-smooth candidate has a ~%.0f%% chance of matching some constant by "
          "accident." % (100.0 * covered / len(set(all23))))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 0
    if argv[1] == "--expr":
        for e in argv[2:]:
            v = evaluate(e)
            print("%-14s = %s" % (e, v))
        return 0
    table = scan()
    if argv[1] == "--audit":
        audit(table)
        return 0
    for a in argv[1:]:
        try:
            lookup(table, int(a))
        except ValueError:
            v = evaluate(a)
            if v:
                lookup(table, v)
            else:
                print("not a number: %s" % a)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
