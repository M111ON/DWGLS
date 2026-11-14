"""walk_locality.py — is x*k a usable address walk, or only a global bijection?

Compares multiplicative walks (next = k*s mod N) against additive strides
(next = s+k mod N) on the metrics that decide whether a walk is cache- and
locality-friendly. Run from anywhere.

    python walk_locality.py
"""
from collections import Counter

N = 20736
LINE = 16          # 4-byte slots in a 64-byte cache line
QUAD = N // 4      # 5184


def orbits(step):
    """step(s) -> next address. Returns the list of orbit sizes."""
    seen = [False] * N
    out = []
    for s in range(N):
        if seen[s]:
            continue
        o, x = 0, s
        while not seen[x]:
            seen[x] = True
            o += 1
            x = step(x)
        out.append(o)
    return out


def mul_stats(k):
    """next = k*s mod N over every start point."""
    same_line = same_quad = near = 0
    tot = 0
    for s in range(N):
        n = k * s % N
        d = abs(n - s)
        tot += d
        if s // LINE == n // LINE:
            same_line += 1
        if s // QUAD == n // QUAD:
            same_quad += 1
        if d <= LINE:
            near += 1
    return tot / N, 100.0 * same_line / N, 100.0 * same_quad / N, 100.0 * near / N


def add_stats(k):
    """next = s+k mod N — constant jump except at the wrap."""
    same_line = same_quad = near = 0
    tot = 0
    for s in range(N):
        n = (s + k) % N
        d = abs(n - s)
        tot += d
        if s // LINE == n // LINE:
            same_line += 1
        if s // QUAD == n // QUAD:
            same_quad += 1
        if d <= LINE:
            near += 1
    return tot / N, 100.0 * same_line / N, 100.0 * same_quad / N, 100.0 * near / N


def order(k):
    x, m = k % N, 1
    while x != 1:
        x = x * k % N
        m += 1
        if m > N:
            return -1
    return m


print("N = %d   cache line = %d slots   quadrant = %d" % (N, LINE, QUAD))
print()
print("=== MULTIPLICATIVE walks  next = k*s mod N (gcd(k,N)=1) ===")
print("%-6s %-7s %-6s %-10s %-8s %9s %9s %9s %9s" %
      ("k", "ord", "orbits", "orb size", "max", "mean|d|", "sameLine%", "sameQuad%", "|d|<=16%"))
for k in (5, 7, 11, 13, 17, 37, 41, 137):
    if N % k == 0 or k % 3 == 0:
        continue
    o = orbits(lambda s: k * s % N)
    c = Counter(o)
    mean, sl, sq, nr = mul_stats(k)
    print("%-6d %-7d %-6d %-10s %-8d %9.1f %9.2f %9.2f %9.2f" %
          (k, order(k), len(o), "%d sizes" % len(c), max(o), mean, sl, sq, nr))

print()
print("=== ADDITIVE strides  next = s+k mod N (the existing GJ / spine walks) ===")
print("%-6s %-7s %-6s %-10s %-8s %9s %9s %9s %9s" %
      ("k", "ord", "orbits", "orb size", "max", "mean|d|", "sameLine%", "sameQuad%", "|d|<=16%"))
for k in (1, 12, 37, 144, 1728):
    o = orbits(lambda s: (s + k) % N)
    c = Counter(o)
    mean, sl, sq, nr = add_stats(k)
    print("%-6d %-7d %-6d %-10s %-8d %9.1f %9.2f %9.2f %9.2f" %
          (k, max(o), len(o), "%d sizes" % len(c), max(o), mean, sl, sq, nr))

print()
print("=== verdict inputs ===")
print("additive stride 37 : every step moves exactly 37 slots -> predictable, sequential-ish")
print("multiplicative x5  : step size varies with s -> mean jump is a large fraction of N")
