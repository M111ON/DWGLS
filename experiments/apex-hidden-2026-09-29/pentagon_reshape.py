"""Can the data be reshaped by the pentagon step 72 (and 72x5 = 360)?

Field     20736 = 2^8 * 3^4   -> 20736 / 72 = 288 exact, 20736 / 360 = 57.6 NOT exact
Tensor    q_proj 896x896 = 2^14 * 7^2 -> no factor 3, no factor 5

So: field-level reshape by 72 is exact; tensor-level is not. This measures whether the
(288,72) pentagon view of a real field shows any adjacency structure, against the other
candidate reshapes.
"""
import struct, sys, math

PATH = r"qproj_L0.f32"
N = 20736

raw = open(PATH, "rb").read()
vals = struct.unpack("<%df" % (len(raw) // 4), raw)
print("tensor cells %d  = 2^%d * %d" % (len(vals), (len(vals) & -len(vals)).bit_length() - 1,
                                        len(vals) // (len(vals) & -len(vals))))


def pearson(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    va = n * saa - sa * sa; vb = n * sbb - sb * sb
    if va <= 0 or vb <= 0:
        return 0.0
    return (n * sab - sa * sb) / math.sqrt(va * vb)


def probe(block, cols, label):
    rows = len(block) // cols
    if rows * cols != len(block):
        return None
    # vertical adjacency: same column, next row
    vx = []; vy = []
    for r in range(rows - 1):
        for c in range(cols):
            vx.append(block[r * cols + c])
            vy.append(block[(r + 1) * cols + c])
    # horizontal adjacency: same row, next column
    hx = []; hy = []
    for r in range(rows):
        base = r * cols
        for c in range(cols - 1):
            hx.append(block[base + c])
            hy.append(block[base + c + 1])
    return (label, rows, cols, pearson(vx, vy), pearson(hx, hy))


print("\n20736 = 72 x 288 = 128 x 162 = 32 x 648 = 144 x 144 = 360 x 57.6")
print("\n%-22s %6s %6s %12s %12s" % ("reshape of field 0", "rows", "cols", "vert adj r", "horiz adj r"))
for cols, label in ((32, "648x32  (Q8_0 block)"), (64, "324x64"), (72, "288x72  PENTAGON"),
                    (128, "162x128 (quantum)"), (144, "144x144"), (162, "128x162"),
                    (288, "72x288")):
    r = probe(vals[:N], cols, label)
    if r:
        print("%-22s %6d %6d %+12.6f %+12.6f" % (r[0], r[1], r[2], r[3], r[4]))

print("\n-- control: shuffle the same field, same probes --")
import random
random.seed(7)
sh = list(vals[:N]); random.shuffle(sh)
print("%-22s %6s %6s %12s %12s" % ("reshape of SHUFFLED field 0", "rows", "cols", "vert adj r", "horiz adj r"))
for cols, label in ((72, "288x72  PENTAGON"), (128, "162x128 (quantum)"), (32, "648x32")):
    r = probe(sh, cols, label)
    print("%-22s %6d %6d %+12.6f %+12.6f" % (r[0], r[1], r[2], r[3], r[4]))

print("\n-- tensor-level divisibility --")
for name, rows, cols in (("q_proj/o_proj", 896, 896), ("v_proj", 128, 896),
                         ("up_proj", 4864, 896), ("down_proj", 896, 4864),
                         ("embed_tokens", 151936, 896)):
    n = rows * cols
    f = lambda k: "YES" if n % k == 0 else "no"
    print("%-14s %10d   /72 %-4s /360 %-4s /9 %-4s /5 %-4s" %
          (name, n, f(72), f(360), f(9), f(5)))

print("\n-- 5 fields = 103680 --")
print("103680 / 360 =", 103680 / 360, " 103680 / 72 =", 103680 / 72)
print("so the pentagon closes over 5 fields: 288 full turns x 360 degrees")
