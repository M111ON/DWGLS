import numpy as np
from scipy.spatial import Voronoi, voronoi_plot_2d
import matplotlib.pyplot as plt
from collections import defaultdict

# ============ ค่าคงที่จากเอกสารวิจัย ============
PHI    = (1 + 5**0.5) / 2
ALPHA  = 1 / PHI**2
R0     = 1.0
TOTAL_GRID = 20736  # 144²

def ruler_tick(sign, n): return sign * R0 * (1 + ALPHA)**n
def voxel_size(n): return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))

# ============ 1. Seed Roots Lane (จาก prototype เดิม) ============
class SeedRootsLane:
    def __init__(self):
        self.roots = {}
        self.chunks = {}
    
    def grow_root(self, root_id):
        self.roots[root_id] = {'layer': 0, 'value': None}
        return root_id
    
    def load_chunk(self, root_id, data):
        self.chunks[root_id] = data
    
    def get_path_length(self): return len(self.roots)
    def get_memory_usage(self): return len(self.chunks)

# ============ 2. Voronoi Partitioning ============
def build_voronoi_from_roots(root_positions, layer=0):
    """
    สร้าง Voronoi cells รอบๆ seed roots
    root_positions: list of 3D coordinates
    """
    if len(root_positions) < 4:
        # Voronoi ต้องการอย่างน้อย 4 จุดใน 3D
        # เพิ่มจุด boundary
        boundary = np.array([
            [10, 10, 10], [-10, 10, 10], [10, -10, 10], [10, 10, -10],
            [-10, -10, 10], [-10, 10, -10], [10, -10, -10], [-10, -10, -10]
        ])
        points = np.vstack([root_positions, boundary])
    else:
        points = root_positions
    
    vor = Voronoi(points)
    return vor

def count_voronoi_cells(vor):
    """นับจำนวน Voronoi cells ที่ไม่ใช่ boundary"""
    # cells ที่เป็น finite regions
    finite_cells = 0
    for region in vor.regions:
        if -1 not in region and len(region) > 0:
            finite_cells += 1
    return finite_cells

# ============ 3. Lane Subdivision ============
class AdaptiveLane:
    """
    Lane ที่ subdivide ตามความสำคัญของข้อมูล
    แทนที่จะใช้ 144 channels ตายตัว
    """
    def __init__(self, base_channels=144):
        self.base_channels = base_channels
        self.active_channels = {}
        self.subdivisions = {}
    
    def subdivide(self, channel_id, depth=1):
        """
        แบ่ง channel ย่อยตาม depth
        depth=1: แบ่ง 2 ส่วน
        depth=2: แบ่ง 4 ส่วน
        depth=3: แบ่ง 8 ส่วน
        """
        if channel_id not in self.active_channels:
            self.active_channels[channel_id] = True
        
        if channel_id not in self.subdivisions:
            self.subdivisions[channel_id] = 0
        
        self.subdivisions[channel_id] = depth
        return 2 ** depth
    
    def get_active_count(self):
        """นับจำนวน sub-channels ที่ใช้งานจริง"""
        total = 0
        for ch_id, depth in self.subdivisions.items():
            total += 2 ** depth
        return total
    
    def get_memory_reduction(self):
        """คำนวณ % ที่ลดได้จากการไม่ใช้ channel ที่ไม่จำเป็น"""
        if not self.active_channels:
            return 0.0
        
        full_capacity = self.base_channels * 8  # ถ้า subdivide เต็มที่ depth=3
        actual_usage = self.get_active_count()
        reduction = (full_capacity - actual_usage) / full_capacity * 100
        return reduction

# ============ 4. รวมระบบ: Voronoi + Seed Roots + Lane Subdivision ============
def test_voronoi_seed_roots():
    print("=" * 60)
    print("VORONOI + SEED ROOTS: Memory Reduction Test")
    print("=" * 60)
    
    # --- Test 1: Seed Roots (จากเดิม) ---
    lane = SeedRootsLane()
    for i in range(5):
        lane.grow_root(i)
        lane.load_chunk(i, np.ones(100) * i)
    
    roots_used = lane.get_memory_usage()
    baseline_efficiency = roots_used / TOTAL_GRID * 100
    print(f"\n[1] Seed Roots (baseline)")
    print(f"    Roots used: {roots_used}")
    print(f"    Total grid: {TOTAL_GRID}")
    print(f"    Efficiency: {baseline_efficiency:.4f}%")
    
    # --- Test 2: Voronoi Partitioning ---
    # สร้างตำแหน่ง roots แบบสุ่ม 3D
    np.random.seed(42)
    root_positions = np.random.rand(5, 3) * 2 - 1  # 5 roots ใน [-1, 1]
    
    vor = build_voronoi_from_roots(root_positions)
    voronoi_cells = count_voronoi_cells(vor)
    
    print(f"\n[2] Voronoi Partitioning")
    print(f"    Seed roots: {len(root_positions)}")
    print(f"    Voronoi cells generated: {voronoi_cells}")
    print(f"    Reduction vs full grid: {(1 - voronoi_cells/TOTAL_GRID)*100:.2f}%")
    
    # --- Test 3: Lane Subdivision ---
    adaptive_lane = AdaptiveLane(base_channels=144)
    
    # สมมติว่าเราต้องการข้อมูลสำคัญแค่ 5 channels
    # และแต่ละ channel ต้องการ subdivide แค่ depth=1 (แบ่ง 2)
    for ch_id in range(5):
        adaptive_lane.subdivide(ch_id, depth=1)
    
    active_sub_channels = adaptive_lane.get_active_count()
    full_capacity = 144 * 8  # ถ้าทุก channel subdivide depth=3
    
    print(f"\n[3] Adaptive Lane Subdivision")
    print(f"    Base channels: 144")
    print(f"    Active channels: {len(adaptive_lane.active_channels)}")
    print(f"    Sub-channels used: {active_sub_channels}")
    print(f"    Full capacity: {full_capacity}")
    print(f"    Reduction: {(1 - active_sub_channels/full_capacity)*100:.2f}%")
    
    # --- Test 4: รวม Voronoi + Lane Subdivision ---
    # คำนวณ memory reduction รวม
    # แทนที่จะใช้ 20736 grid ทั้งหมด
    # เราใช้เฉพาะ Voronoi cells รอบ roots + sub-channels ที่จำเป็น
    
    # สมมติว่าแต่ละ Voronoi cell ใช้ sub-channels เฉพาะที่จำเป็น
    # ถ้ามี 5 roots และแต่ละ root ใช้ sub-channels 2 ตัว (depth=1)
    total_active = voronoi_cells * active_sub_channels
    total_possible = TOTAL_GRID
    
    combined_reduction = (1 - total_active / total_possible) * 100
    
    print(f"\n[4] Combined: Voronoi + Lane Subdivision")
    print(f"    Voronoi cells: {voronoi_cells}")
    print(f"    Active sub-channels per cell: {active_sub_channels}")
    print(f"    Total active: {total_active}")
    print(f"    Total possible: {total_possible}")
    print(f"    Combined reduction: {combined_reduction:.2f}%")
    
    # --- Test 5: Visualization ---
    if len(root_positions) >= 4:
        fig, ax = plt.subplots(1, 1, figsize=(10, 8))
        voronoi_plot_2d(vor, ax=ax, show_vertices=True, line_colors='orange',
                       line_width=1, line_alpha=0.6, point_size=10)
        ax.set_title('Voronoi Partitioning around Seed Roots')
        ax.set_xlabel('X')
        ax.set_ylabel('Y')
        plt.tight_layout()
        plt.savefig('voronoi_seed_roots.png', dpi=150)
        print(f"\n[5] Visualization saved: voronoi_seed_roots.png")
    
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"  Baseline (flat grid):     100.00%")
    print(f"  Seed roots only:          {baseline_efficiency:.4f}%")
    print(f"  Voronoi partitioning:     {(1 - voronoi_cells/TOTAL_GRID)*100:.2f}% reduction")
    print(f"  Lane subdivision:         {(1 - active_sub_channels/full_capacity)*100:.2f}% reduction")
    print(f"  Combined reduction:       {combined_reduction:.2f}%")
    print("=" * 60)
    
    return {
        'baseline_efficiency': baseline_efficiency,
        'voronoi_reduction': (1 - voronoi_cells/TOTAL_GRID)*100,
        'lane_reduction': (1 - active_sub_channels/full_capacity)*100,
        'combined_reduction': combined_reduction
    }

if __name__ == "__main__":
    results = test_voronoi_seed_roots()