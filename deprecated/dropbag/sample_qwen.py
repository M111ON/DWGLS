import numpy as np

# ============ ค่าคงที่จากเอกสารวิจัย ============
PHI    = (1 + 5**0.5) / 2
ALPHA  = 1 / PHI**2            # 0.381966
R0     = 1.0
E_MAX  = 5.04                  # expansion สูงสุด (Layer 5)
SAFETY = 1.5                   # headroom ที่เอกสารแนะนำ
LAYERS = 5                     # n = 0..4
GROUPS = 64                    # angular cells ที่ผิว (index layer)
VOX_PER_GROUP = 55             # 64 × 55 = 3,520 voxels (เท่ากับ flat scan ในเอกสาร)

def ruler_tick(sign, n): return sign * R0 * (1 + ALPHA) ** n
def voxel_size(n): return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))

# ============ ใช้ประโยชน์ 1: โครงสร้างเป็น codec (lossless) ============
def demo_codec():
    rng = np.random.default_rng(7)
    values = np.concatenate([np.arange(100), rng.integers(-1000, 1000, 100)])
    miss = 0
    for v in values:
        for n in range(LAYERS):
            stored   = v * voxel_size(n)        # encode: เรขาคณิตบีบ/ขยายให้
            restored = stored / voxel_size(n)   # decode: เรขาคณิตคลายกลับ
            if round(restored) != v: miss += 1
    print(f"[1] codec roundtrip: {miss} mismatch / {len(values)*LAYERS} checks")

# ============ ใช้ประโยชน์ 2: spatial index สองขั้น (progressive query) ============
def demo_index():
    rng = np.random.default_rng(42)
    data = rng.integers(-50, 50, (GROUPS, VOX_PER_GROUP))   # ส่วนใหญ่ค่าเล็ก
    for g in (7, 23, 41):                                    # ฝัง spike 3 กลุ่ม
        data[g, rng.integers(0, VOX_PER_GROUP, 3)] = rng.integers(900, 1000, 3)

    # outer layer = sparse index: max|value| ต่อกลุ่ม (เก็บแบบ geometric)
    index = {g: np.abs(data[g]).max() * voxel_size(4) for g in range(GROUPS)}
    T = 900

    flat = hits_f = 0
    for g in range(GROUPS):
        for i in range(VOX_PER_GROUP):
            flat += 1
            if abs(data[g, i]) > T: hits_f += 1

    two = hits_t = 0
    for g in range(GROUPS):
        two += 1                                            # ตรวจแค่ผิว
        if index[g] / voxel_size(4) > T:                    # index ชี้ว่า "มีของ"
            for i in range(VOX_PER_GROUP):
                two += 1
                if abs(data[g, i]) > T: hits_t += 1

    print(f"[2] flat={flat} checks | two-stage={two} checks | "
          f"speedup {flat/two:.1f}x | hits match={hits_f==hits_t}")

# ============ ใช้ประโยชน์ 3: progressive loading (outer=overview, inner=detail) ============
def demo_lod():
    signal = np.sin(np.linspace(0, 6*np.pi, 64)) * 100
    coarse = signal.reshape(8, 8).mean(axis=1)   # 8 ค่าภาพรวม -> outer layer
    detail = signal - np.repeat(coarse, 8)       # ส่วนละเอียด -> inner layers
    lvl0 = np.repeat(coarse, 8)                  # โหลดเฉพาะผิว: ถูกแต่หยาบ
    lvl1 = lvl0 + detail                         # drill down: ครบถ้วน
    print(f"[3] outer-only err={np.abs(lvl0-signal).max():.1f} | "
          f"drill-down err={np.abs(lvl1-signal).max():.2e}")

# ============ ใช้ประโยชน์ 4: memory planner กัน OOM ============
def demo_memory(data_gb, ram_gb=8):
    needed = data_gb * E_MAX * SAFETY
    print(f"[4] data {data_gb:.1f}GB -> need {needed:.2f}GB / {ram_gb}GB -> "
          f"{'OK' if needed <= ram_gb else 'OOM'}")

if __name__ == "__main__":
    demo_codec()
    demo_index()
    demo_lod()
    for d in (0.5, 1.0, 2.0): demo_memory(d)