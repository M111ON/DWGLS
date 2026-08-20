"""
Seed Roots Lane v3 — Integrated with Qwen's AdaptiveLane

Qwen concepts integrated:
- 144 base channels (= 6ico compound 144 vertices)
- AdaptiveLane: subdivide channels by depth (2^depth sub-channels)
- Only activate channels that are needed

Our concepts:
- Cardioid: direction control (spike กระจาย)
- Threshold: magnitude control (จำกัดขนาด)
- Voronoi: spatial subdivision (แบ่งพื้นที่)
- Seed Roots: lazy loading (ไม่ load ทั้งหมด)

Result: memory ประหยัด 99%+ จาก grid ทั้งหมด
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966
R0 = 1.0
TOTAL_GRID = 20736  # 144² = 6ico compound vertices
BASE_CHANNELS = 144  # 6ico = 144 vertices = 144 channels

def ruler_tick(sign, n):
    return sign * R0 * (1 + ALPHA) ** n

def voxel_size(n):
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


# ============ Cardioid: Direction Control ============
def cardioid(theta, a=1.0):
    """Cardioid: r = a(1 + cos(θ)) — spike กระจายไม่กระจุก"""
    return a * (1 + np.cos(theta))

def cardioid_3d(theta, phi, a=1.0):
    """3D Cardioid: control spike direction in 3D"""
    r = cardioid(theta, a)
    x = r * np.sin(phi) * np.cos(theta)
    y = r * np.sin(phi) * np.sin(theta)
    z = r * np.cos(phi)
    return np.array([x, y, z])


# ============ Threshold: Magnitude Control ============
def apply_threshold(value, threshold, mode='clip'):
    """Threshold: จำกัดขนาดข้อมูล"""
    if mode == 'clip':
        return np.clip(value, -threshold, threshold)
    elif mode == 'gate':
        return value if abs(value) <= threshold else 0
    elif mode == 'scale':
        scale = threshold / max(abs(value), 1e-10)
        return value * min(scale, 1.0)
    return value


# ============ AdaptiveLane (from Qwen) ============
class AdaptiveLane:
    """
    Qwen's concept: 144 base channels, subdivide by depth
    
    depth=0: 1 channel (base)
    depth=1: 2 sub-channels
    depth=2: 4 sub-channels
    depth=3: 8 sub-channels
    
    Key: only activate channels that are needed
    """
    
    def __init__(self, base_channels=BASE_CHANNELS):
        self.base_channels = base_channels
        self.active_channels = {}  # channel_id → depth
        self.total_sub_channels = 0
        
    def activate(self, channel_id, depth=0):
        """
        Activate channel with subdivision depth
        
        depth=0: just the base channel (1)
        depth=1: split into 2
        depth=2: split into 4
        depth=3: split into 8
        """
        if channel_id < 0 or channel_id >= self.base_channels:
            return 0
        
        old_depth = self.active_channels.get(channel_id, -1)
        
        if depth > old_depth:
            # Remove old sub-channels
            if old_depth >= 0:
                self.total_sub_channels -= 2 ** old_depth
            
            # Add new sub-channels
            self.active_channels[channel_id] = depth
            self.total_sub_channels += 2 ** depth
        
        return 2 ** depth
    
    def deactivate(self, channel_id):
        """Deactivate channel"""
        if channel_id in self.active_channels:
            depth = self.active_channels.pop(channel_id)
            self.total_sub_channels -= 2 ** depth
    
    def get_memory_usage(self):
        """Memory = active sub-channels (not full grid)"""
        return self.total_sub_channels
    
    def get_memory_reduction(self):
        """% reduction vs full grid"""
        return (1 - self.total_sub_channels / TOTAL_GRID) * 100
    
    def get_summary(self):
        """Summary of active channels"""
        return {
            'base_channels': self.base_channels,
            'active_count': len(self.active_channels),
            'sub_channels': self.total_sub_channels,
            'total_grid': TOTAL_GRID,
            'reduction': self.get_memory_reduction()
        }


# ============ Voronoi Seeds (spatial) ============
class VoronoiSeeds:
    """
    Voronoi: แบ่งพื้นที่อัตโนมัติ
    
    9 seeds (phi-based) = กระจายสม่ำเสมอ
    Subdivide when region too dense
    """
    
    def __init__(self, n_seeds=9):
        self.seeds = self._phi_seeds(n_seeds)
        self.regions = {i: [] for i in range(n_seeds)}
        self.subdivision_count = 0
        
    def _phi_seeds(self, n):
        """Golden ratio distribution"""
        seeds = []
        for i in range(n):
            theta = 2 * np.pi * i / PHI
            phi = np.arccos(1 - 2 * (i / n))
            r = np.sqrt(i / n)
            seeds.append([r*np.sin(phi)*np.cos(theta),
                         r*np.sin(phi)*np.sin(theta),
                         r*np.cos(phi)])
        return np.array(seeds)
    
    def assign_region(self, point):
        """Assign point to nearest seed"""
        distances = np.linalg.norm(self.seeds - point, axis=1)
        region_id = np.argmin(distances)
        min_dist = distances[region_id]
        return region_id, min_dist
    
    def subdivide_if_needed(self, region_id, data_count, threshold=10):
        """Subdivide region if too much data"""
        if len(self.regions[region_id]) > threshold:
            centroid = np.mean(self.regions[region_id], axis=0)
            self.seeds = np.vstack([self.seeds, centroid])
            self.regions[len(self.seeds)-1] = []
            self.subdivision_count += 1
            return centroid
        return None


# ============ Seed Roots Lane v3 (Integrated) ============
class SeedRootsLaneV3:
    """
    Integrated: Cardioid + Threshold + Voronoi + AdaptiveLane
    
    Access data by following spike paths from seed.
    Memory stays bounded: only load what you follow
    """
    
    def __init__(self, seed_type='ico', n_voronoi_seeds=9):
        self.seed_type = seed_type
        self.seed_pos = np.array([0.0, 0.0, 0.0])
        self.current_pos = self.seed_pos.copy()
        self.root_path = [self.seed_pos.copy()]
        self.loaded_chunks = {}
        
        # Spatial subdivision (Voronoi)
        self.voronoi = VoronoiSeeds(n_voronoi_seeds)
        
        # Channel subdivision (AdaptiveLane)
        self.lane = AdaptiveLane(base_channels=BASE_CHANNELS)
        
    def spike_direction(self, depth):
        """Compute spike direction with Cardioid control"""
        theta = depth * (2 * np.pi / 20)  # 20 directions
        phi = np.arccos(1 - 2 * ((depth * PHI) % 1))
        direction = cardioid_3d(theta, phi, a=voxel_size(depth))
        return direction
    
    def grow_root(self, depth, threshold=None):
        """Grow root by one spike with threshold control"""
        direction = self.spike_direction(depth)
        new_pos = self.current_pos + direction
        
        # Threshold: จำกัดขนาด spike
        if threshold is not None:
            new_pos = np.array([
                apply_threshold(x, threshold) for x in new_pos
            ])
        
        chunk_id = depth
        self.current_pos = new_pos
        self.root_path.append(new_pos.copy())
        
        # Voronoi: assign to spatial region
        region_id, dist = self.voronoi.assign_region(new_pos)
        self.voronoi.regions[region_id].append(new_pos)
        
        # Subdivide Voronoi if needed
        self.voronoi.subdivide_if_needed(region_id, len(self.voronoi.regions[region_id]))
        
        # AdaptiveLane: activate channel with depth
        channel_id = region_id % BASE_CHANNELS
        sub_count = self.lane.activate(channel_id, depth=min(depth, 3))
        
        return new_pos, chunk_id, region_id, sub_count
    
    def load_chunk(self, chunk_id, data):
        """Load data into chunk"""
        self.loaded_chunks[chunk_id] = data
        
    def access_chunk(self, chunk_id):
        """Access chunk (lazy load if needed)"""
        if chunk_id not in self.loaded_chunks:
            self.loaded_chunks[chunk_id] = np.zeros(100)
        return self.loaded_chunks[chunk_id]
    
    def get_memory_usage(self):
        """Memory = loaded chunks + active sub-channels"""
        return len(self.loaded_chunks) + self.lane.get_memory_usage()
    
    def get_summary(self):
        """Full summary"""
        lane_summary = self.lane.get_summary()
        return {
            'loaded_chunks': len(self.loaded_chunks),
            'root_path_length': len(self.root_path) - 1,
            'voronoi_seeds': len(self.voronoi.seeds),
            'voronoi_subdivisions': self.voronoi.subdivision_count,
            'lane_summary': lane_summary,
            'total_memory': self.get_memory_usage(),
            'total_grid': TOTAL_GRID,
            'reduction': (1 - self.get_memory_usage() / TOTAL_GRID) * 100
        }


# ============ Demo ============
def demo_integrated():
    """Demo: Integrated system"""
    print("=" * 60)
    print("SEED ROOTS LANE v3 — Integrated with Qwen's AdaptiveLane")
    print("=" * 60)
    
    # Create lane
    lane = SeedRootsLaneV3()
    
    # Grow 20 roots
    for depth in range(20):
        pos, chunk_id, region, sub_count = lane.grow_root(depth, threshold=10.0)
        lane.load_chunk(chunk_id, np.ones(100) * depth)
        
        print(f"  Spike {depth:2d}: region={region:2d}  sub_channels={sub_count:2d}  "
              f"pos=({pos[0]:7.2f}, {pos[1]:7.2f}, {pos[2]:7.2f})")
    
    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    summary = lane.get_summary()
    print(f"  Loaded chunks:       {summary['loaded_chunks']}")
    print(f"  Root path length:    {summary['root_path_length']}")
    print(f"  Voronoi seeds:       {summary['voronoi_seeds']}")
    print(f"  Voronoi subdivisions:{summary['voronoi_subdivisions']}")
    print(f"  Lane active channels:{summary['lane_summary']['active_count']}")
    print(f"  Lane sub-channels:   {summary['lane_summary']['sub_channels']}")
    print(f"  Total memory:        {summary['total_memory']}")
    print(f"  Total grid:          {summary['total_grid']}")
    print(f"  Memory reduction:    {summary['reduction']:.2f}%")
    
    return lane


def demo_comparison():
    """Compare: full grid vs integrated"""
    print("\n" + "=" * 60)
    print("COMPARISON: Full Grid vs Integrated")
    print("=" * 60)
    
    # Full grid
    full_grid = TOTAL_GRID
    
    # Integrated
    lane = SeedRootsLaneV3()
    for depth in range(20):
        lane.grow_root(depth, threshold=10.0)
        lane.load_chunk(depth, np.ones(100) * depth)
    
    integrated_memory = lane.get_memory_usage()
    
    print(f"\n  Full grid:       {full_grid:,}")
    print(f"  Integrated:      {integrated_memory}")
    print(f"  Reduction:       {(1 - integrated_memory/full_grid)*100:.2f}%")
    print(f"  Ratio:           {integrated_memory/full_grid:.6f}x")
    
    return full_grid, integrated_memory


# ============ Main ============
if __name__ == "__main__":
    lane = demo_integrated()
    full, integrated = demo_comparison()
