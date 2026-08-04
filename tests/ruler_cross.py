"""
ruler_cross.py — Verify cube-in-dodecahedron + 3-axis crossing
Source: Research collaborator (Aug 2026)
"""
import numpy as np

alpha = 0.381966  # 1/phi^2
R0 = 1.0

def ruler_tick(sign: int, n: int) -> float:
    """1 ruler = 1 loop. ไม่มี n=0 คือ 0 จริง เพราะ Rn>0 เสมอ (asymptote เท่านั้น)"""
    return sign * R0 * (1 + alpha) ** n

def loop_type(n: int) -> str:
    return "icosa(20)" if n % 2 == 0 else "dodeca(12)"

# ---- 1 ruler (1 axis) ----
print("=== 1 ไม้บรรทัด (1 loop) ===")
for n in range(0, 5):
    for sign in (+1, -1):
        print(f"  sign={sign:+d} n={n}  tick={ruler_tick(sign, n):+.4f}  type={loop_type(n)}")

# ---- cross 3 rulers -> XYZ point ----
def address_to_xyz(sx, nx, sy, ny, sz, nz):
    return (ruler_tick(sx, nx), ruler_tick(sy, ny), ruler_tick(sz, nz))

print("\n=== Cross 3 ไม้บรรทัด = จุดใน 3D ===")
example = address_to_xyz(+1, 2, -1, 1, +1, 3)
print(f"address (sx=+1,nx=2 | sy=-1,ny=1 | sz=+1,nz=3) -> xyz = {example}")

# ---- key structural point: 3 axes x 2 signs = 6 half-axes ----
print("\n=== ทำไมเข้ากับ Rubik cube เดิมพอดี ===")
print("3 แกน (X,Y,Z) x 2 ทิศ (+,-) = 6 half-axis")
print("ตรงกับ DiamondBlock/Rubik ที่มีอยู่แล้ว = 6 face x 64-bit เป๊ะ")
print("แต่ละ half-axis เก็บ n (generation) ได้อิสระ -> ใส่ลง 64-bit ต่อหน้าได้ตรงๆ")

# ---- origin still unreachable in 3D (corner asymptote) ----
print("\n=== จุด (0,0,0) จริง ===")
for n in [10, 100, 1000]:
    p = address_to_xyz(+1, n, +1, n, +1, n)
    dist = np.linalg.norm(p)
    print(f"  n={n:5d} ทุกแกน -> distance from true origin = {dist:.3e}  (ไม่ถึง 0 เลย)")

# ---- compound type per cell: icosa/dodeca combination across 3 axes ----
print("\n=== ประเภทของ cell จาก parity 3 แกน (2^3 = 8 แบบ) ===")
for nx in (0,1):
    for ny in (0,1):
        for nz in (0,1):
            print(f"  (nx%2={nx}, ny%2={ny}, nz%2={nz}) -> "
                  f"({loop_type(nx)[:1]},{loop_type(ny)[:1]},{loop_type(nz)[:1]})")
