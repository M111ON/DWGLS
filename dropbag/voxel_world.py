import numpy as np
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

# ============================================================
# ส่วนที่ 1: โครงสร้างเดิมของคุณ (ไม่แก้แม้แต่บรรทัดเดียว)
# ============================================================
alpha = 0.381966  # 1/phi^2
R0 = 1.0

def ruler_tick(sign: int, n: int) -> float:
    return sign * R0 * (1 + alpha) ** n

def loop_type(n: int) -> str:
    return "icosa(20)" if n % 2 == 0 else "dodeca(12)"

def voxel_size(n: int) -> float:
    """ขนาด voxel = ระยะถึง tick ถัดไป -> ยิ่งไกลยิ่งใหญ่ = LOD ฟรี"""
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))

def address_to_xyz(sx, nx, sy, ny, sz, nz):
    return (ruler_tick(sx, nx), ruler_tick(sy, ny), ruler_tick(sz, nz))


# ============================================================
# ส่วนที่ 2: รูปทรง icosa / dodeca (dual ของกันและกัน)
# ============================================================
PHI = (1 + np.sqrt(5)) / 2

def icosa_vertices(scale=1.0):
    v = np.array([
        [0, 1, PHI], [0, -1, PHI], [0, 1, -PHI], [0, -1, -PHI],
        [1, PHI, 0], [-1, PHI, 0], [1, -PHI, 0], [-1, -PHI, 0],
        [PHI, 0, 1], [-PHI, 0, 1], [PHI, 0, -1], [-PHI, 0, -1],
    ])
    return v / np.linalg.norm(v[0]) * scale

def dodeca_vertices(scale=1.0):
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


# ============================================================
# ส่วนที่ 3: สร้าง voxel world (ทุก octant, n = 0..N)
# ============================================================
def build_world(N=4):
    pts, sizes, types = [], [], []
    for sx in (+1, -1):
        for sy in (+1, -1):
            for sz in (+1, -1):
                for nx in range(N):
                    for ny in range(N):
                        for nz in range(N):
                            x, y, z = address_to_xyz(sx,nx, sy,ny, sz,nz)
                            pts.append((x, y, z))
                            # ปริมาตร voxel = size^3 -> แปลงเป็นขนาดจุด
                            sizes.append(voxel_size(max(nx,ny,nz)) ** 3)
                            types.append((nx+ny+nz) % 2)  # 0=icosa, 1=dodeca
    return np.array(pts), np.array(sizes), np.array(types)


# ============================================================
# ส่วนที่ 4: Render
# ============================================================
def render():
    pts, sizes, types = build_world(N=4)

    fig = plt.figure(figsize=(11, 9))
    ax = fig.add_subplot(111, projection='3d')

    # สเกลขนาดจุดให้มองเห็นชัด
    s_norm = 40 + (sizes / sizes.max()) * 900

    icosa_mask  = types == 0
    dodeca_mask = types == 1

    ax.scatter(*pts[icosa_mask].T,  s=s_norm[icosa_mask],
               c='#00c8ff', alpha=0.75, edgecolors='k', linewidths=0.3,
               label='icosa(20)  [n parity even]')
    ax.scatter(*pts[dodeca_mask].T, s=s_norm[dodeca_mask],
               c='#ff6b35', alpha=0.75, edgecolors='k', linewidths=0.3,
               label='dodeca(12)  [n parity odd]')

    # วาด 6 half-axes (โครงสร้าง Rubik ของคุณ)
    L = ruler_tick(+1, 4)
    for axis in np.eye(3):
        for sign in (+1, -1):
            ax.plot(*np.array([np.zeros(3), sign*axis*L]).T,
                    color='gray', lw=0.8, alpha=0.4)

    # วางรูปทรงจริงที่ tick นอกสุดหนึ่งจุด เพื่อโชว์ dual
    anchor = np.array(address_to_xyz(+1,3, +1,3, +1,3))
    for verts, color in [(icosa_vertices(0.5), '#00c8ff'),
                         (dodeca_vertices(0.5), '#ff6b35')]:
        ax.scatter(*(anchor + verts).T, s=12, c=color, alpha=0.9)

    ax.set_xlabel('X'); ax.set_ylabel('Y'); ax.set_zlabel('Z')
    ax.set_title('Golden-Ratio Voxel World\n'
                 'voxel โตตามระยะทาง (LOD อัตโนมัติ) + icosa/dodeca สลับกัน')
    ax.legend(loc='upper left')
    plt.tight_layout()
    plt.savefig('golden_voxel_world.png', dpi=150)
    plt.show()
    print("บันทึกเป็น golden_voxel_world.png แล้ว")


if __name__ == "__main__":
    render()