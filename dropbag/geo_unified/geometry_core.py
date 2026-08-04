"""
Geometry Core — Rigid Structure (ห้ามยุ่ง)
==========================================
6ico compound (144 verts) + 20736 grid + golden-ratio ruler
โครงสร้างรับผิดชอบข้อมูล — ไม่ต้องมี codec
"""
import numpy as np

# === Sacred Constants ===
PHI = (1 + np.sqrt(5)) / 2
INV_PHI2 = 1.0 / (PHI * PHI)  # 0.381966
R0 = 1.0
GRID_SIZE = 20736  # 144^2 = universal grid

def ruler_tick(sign, n):
    """Golden-ratio ruler — structure, not computation"""
    return sign * R0 * (1 + INV_PHI2) ** n

def voxel_size(n):
    """Distance between consecutive ticks"""
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))

# === 6ico Compound (144 vertices) ===
def ico_vertices(scale=1.0):
    """12 vertices of icosahedron"""
    v = np.array([
        [0, 1, PHI], [0, -1, PHI], [0, 1, -PHI], [0, -1, -PHI],
        [1, PHI, 0], [-1, PHI, 0], [1, -PHI, 0], [-1, -PHI, 0],
        [PHI, 0, 1], [-PHI, 0, 1], [PHI, 0, -1], [-PHI, 0, -1],
    ])
    return v / np.linalg.norm(v[0]) * scale

def dodeca_vertices(scale=1.0):
    """20 vertices of dodecahedron"""
    a, b = 1.0, 1/PHI
    c = PHI
    v = []
    for x in (-1, 1):
        for y in (-1, 1):
            for z in (-1, 1):
                v.append([x*a, y*a, z*a])
    for x in (-b, b):
        for y in (-c, c):
            v.append([0, x, y])
            v.append([x, y, 0])
            v.append([y, 0, x])
    return np.array(v) / np.sqrt(3) * scale

def compound_144(scale=1.0):
    """6 icosahedra rotated = 144 vertices (protagonist)"""
    verts = []
    # 6 rotation axes for 6 icosahedra
    rotations = [
        np.array([[1,0,0],[0,1,0],[0,0,1]]),  # identity
        np.array([[0,1,0],[0,0,1],[1,0,0]]),  # 120° around (1,1,1)
        np.array([[0,0,1],[1,0,0],[0,1,0]]),  # 240° around (1,1,1)
        np.array([[-1,0,0],[0,-1,0],[0,0,1]]),# 180° around z
        np.array([[-1,0,0],[0,1,0],[0,0,-1]]),# 180° around y
        np.array([[1,0,0],[0,-1,0],[0,0,-1]]),# 180° around x
    ]
    base = ico_vertices(scale)
    for R in rotations:
        for v in base:
            verts.append(R @ v)
    return np.array(verts)

# === GeoJump: Hilbert + Peano + Metatron ===
def geo_jump_hilbert(addr, n=4):
    """Hilbert curve mapping — 1D → 2D"""
    result = [0, 0]
    for i in range(n):
        bit = (addr >> (2*i)) & 3
        if bit == 0:
            result[0], result[1] = result[1], result[0]
        elif bit == 1:
            result[1] += (1 << i)
        elif bit == 2:
            result[0] += (1 << i)
            result[1] += (1 << i)
        elif bit == 3:
            result[0], result[1] = (1 << i) - 1 - result[1], (1 << i) - 1 - result[0]
            result[0] += (1 << i)
            result[1] += (1 << i)
    return result

def geo_jump_peano(addr, n=3):
    """Peano curve mapping — 1D → 3D"""
    result = [0, 0, 0]
    for i in range(n):
        bit = (addr // (3**i)) % 3
        result[0] += (bit % 3) * (1 << i)
        result[1] += (bit // 3 % 3) * (1 << i)
        result[2] += (bit // 9 % 3) * (1 << i)
    return result

def geo_jump_metatron(addr, n=4):
    """Metatron cube mapping — 1D → 3D (6^3 grid)"""
    result = [0, 0, 0]
    for i in range(n):
        d = (addr // (6**i)) % 6
        result[0] += (d % 6) * (1 << i)
        result[1] += (d // 6 % 6) * (1 << i)
        result[2] += (d // 36 % 6) * (1 << i)
    return result

# === 1440 = GEO_FIBO_CLOCK ===
def geo_fibo_clock(addr):
    """1440 = 15 towers × 48 addr × 2 polar"""
    TOWERS = 15
    ADDRS = 48
    POLAR = 2
    tower = addr % TOWERS
    a = (addr // TOWERS) % ADDRS
    p = (addr // (TOWERS * ADDRS)) % POLAR
    return tower, a, p

print("[OK] geometry_core.py loaded")
print(f"  GRID_SIZE = {GRID_SIZE}")
print(f"  6ico compound = {len(compound_144())} vertices")
print(f"  ruler_tick(1,0) = {ruler_tick(1,0)}")
print(f"  ruler_tick(1,4) = {ruler_tick(1,4):.3f}")
