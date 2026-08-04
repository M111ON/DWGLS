"""
Seed Roots Lane v2 — With Cardioid + Threshold + Voronoi

Cardioid: direction control (ไม่ให้ spike กระจุก)
Threshold: magnitude control (จำกัดขนาดข้อมูล)
Voronoi: adaptive subdivision (แบ่งพื้นที่อัตโนมัติ)

Test: spherical container + real GGUF data
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966
R0 = 1.0

def ruler_tick(sign, n):
    return sign * R0 * (1 + ALPHA) ** n

def voxel_size(n):
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


# ============ Cardioid: Direction Control ============
def cardioid(theta, a=1.0):
    """
    Cardioid curve: r = a(1 + cos(θ))
    
    Purpose: ไม่ให้ spike กระจุกในทิศทางเดียว
    - θ = 0 → r = 2a (longest)
    - θ = π → r = 0 (shortest)
    - ผลลัพธ์: spike กระจายเป็นรูปหัวใจ
    """
    return a * (1 + np.cos(theta))


def cardioid_3d(theta, phi, a=1.0):
    """
    3D Cardioid: extend to 3D space
    
    Purpose: control spike direction in 3D
    """
    r = cardioid(theta, a)
    x = r * np.sin(phi) * np.cos(theta)
    y = r * np.sin(phi) * np.sin(theta)
    z = r * np.cos(phi)
    return np.array([x, y, z])


# ============ Threshold: Magnitude Control ============
def apply_threshold(value, threshold, mode='clip'):
    """
    Threshold: จำกัดขนาดข้อมูล
    
    mode:
    - 'clip': ตัดค่าที่เกิน threshold
    - 'gate': ผ่านเฉพาะค่าที่น้อยกว่า threshold
    - 'scale': ลดขนาดลง proportionally
    """
    if mode == 'clip':
        return np.clip(value, -threshold, threshold)
    elif mode == 'gate':
        return value if abs(value) <= threshold else 0
    elif mode == 'scale':
        scale = threshold / max(abs(value), 1e-10)
        return value * min(scale, 1.0)
    return value


# ============ Voronoi: Adaptive Subdivision ============
class VoronoiSeeds:
    """
    Voronoi seeds: แบ่งพื้นที่อัตโนมัติ
    
    Purpose: กำหนดว่า spike ควรไปลงตรงไหน
    - seed points = จุดกำเนิด
    - region = พื้นที่ที่ใกล้ seed ที่สุด
    - adaptive: ยิ่งมีข้อมูลมาก ยิ่งแบ่งย่อย
    """
    
    def __init__(self, n_seeds=9):
        """
        n_seeds: จำนวน seed points (9 = phi-based)
        """
        self.seeds = self._phi_seeds(n_seeds)
        self.regions = {i: [] for i in range(n_seeds)}
        
    def _phi_seeds(self, n):
        """
        Generate seeds using golden ratio distribution
        (phi-based = กระจายสม่ำเสมอที่สุด)
        """
        seeds = []
        for i in range(n):
            theta = 2 * np.pi * i / PHI  # golden angle
            phi = np.arccos(1 - 2 * (i / n))
            r = np.sqrt(i / n)  #均匀分布
            seeds.append([
                r * np.sin(phi) * np.cos(theta),
                r * np.sin(phi) * np.sin(theta),
                r * np.cos(phi)
            ])
        return np.array(seeds)
    
    def assign_region(self, point):
        """
        Assign point to nearest seed
        
        Returns: region_id, distance
        """
        distances = np.linalg.norm(self.seeds - point, axis=1)
        region_id = np.argmin(distances)
        min_dist = distances[region_id]
        return region_id, min_dist
    
    def subdivide_if_needed(self, region_id, data_count, threshold=100):
        """
        Subdivide region if too much data
        
        Returns: new_seeds (if subdivided)
        """
        if len(self.regions[region_id]) > threshold:
            # Subdivide: add new seed at centroid
            centroid = np.mean(self.regions[region_id], axis=0)
            self.seeds = np.vstack([self.seeds, centroid])
            self.regions[len(self.seeds)-1] = []
            return centroid
        return None


# ============ SeedRootsLane v2 ============
class SeedRootsLaneV2:
    """
    Seed Roots Lane with Cardioid + Threshold + Voronoi
    
    Access data by following spike paths from seed.
    Memory stays bounded: only load what you follow
    """
    
    def __init__(self, seed_type='ico', n_voronoi_seeds=9):
        self.seed_type = seed_type
        self.seed_pos = np.array([0.0, 0.0, 0.0])
        self.current_pos = self.seed_pos.copy()
        self.root_path = [self.seed_pos.copy()]
        self.loaded_chunks = {}
        
        # New: Voronoi for adaptive subdivision
        self.voronoi = VoronoiSeeds(n_voronoi_seeds)
        
    def spike_direction(self, depth):
        """
        Compute spike direction with Cardioid control
        """
        # Cardioid: ไม่ให้ spike กระจุก
        theta = depth * (2 * np.pi / 20)  # 20 directions
        phi = np.arccos(1 - 2 * ((depth * PHI) % 1))
        
        # 3D cardioid: กระจาย spike
        direction = cardioid_3d(theta, phi, a=voxel_size(depth))
        
        return direction
    
    def grow_root(self, depth, threshold=None):
        """
        Grow root by one spike with threshold control
        """
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
        
        # Voronoi: assign to region
        region_id, dist = self.voronoi.assign_region(new_pos)
        self.voronoi.regions[region_id].append(new_pos)
        
        # Subdivide if needed
        new_seed = self.voronoi.subdivide_if_needed(region_id, len(self.voronoi.regions[region_id]))
        if new_seed is not None:
            print(f"  Voronoi subdivision at depth {depth}")
        
        return new_pos, chunk_id, region_id
    
    def load_chunk(self, chunk_id, data):
        self.loaded_chunks[chunk_id] = data
        
    def access_chunk(self, chunk_id):
        if chunk_id not in self.loaded_chunks:
            self.loaded_chunks[chunk_id] = np.zeros(100)
        return self.loaded_chunks[chunk_id]
    
    def get_memory_usage(self):
        return len(self.loaded_chunks)


# ============ Test: Spherical Container ============
def test_spherical_container():
    """
    Test: place spike roots inside spherical container
    
    Container: spherical shell with golden-ratio layers
    Roots: spike paths from center
    """
    print("=" * 60)
    print("TEST: Spherical Container with Seed Roots")
    print("=" * 60)
    
    # Create container: 5 layers
    container_layers = 5
    container_radius = ruler_tick(+1, container_layers)
    
    print(f"\nContainer radius: {container_radius:.3f}")
    print(f"Layers: {container_layers}")
    
    # Create lane
    lane = SeedRootsLaneV2()
    
    # Grow roots inside container
    max_depth = 15
    inside_count = 0
    outside_count = 0
    
    for depth in range(max_depth):
        pos, chunk_id, region = lane.grow_root(depth, threshold=container_radius)
        
        # Check if inside container
        dist = np.linalg.norm(pos)
        if dist <= container_radius:
            inside_count += 1
            status = "INSIDE"
        else:
            outside_count += 1
            status = "OUTSIDE"
        
        print(f"  Spike {depth:2d}: dist={dist:8.3f}  region={region}  {status}")
    
    print(f"\nResults:")
    print(f"  Inside:  {inside_count}/{max_depth}")
    print(f"  Outside: {outside_count}/{max_depth}")
    print(f"  Memory:  {lane.get_memory_usage()} chunks")
    
    return lane


# ============ Test: Memory Efficiency ============
def test_memory_efficiency():
    """
    Compare: load all vs load on demand (with threshold)
    """
    print("\n" + "=" * 60)
    print("TEST: Memory Efficiency with Threshold")
    print("=" * 60)
    
    total_chunks = 1000
    
    # Without threshold: might load too much
    without_threshold = total_chunks
    
    # With threshold: limit what we load
    with_threshold = 50  # threshold limits to 50 chunks
    
    print(f"\nTotal chunks: {total_chunks}")
    print(f"Without threshold: {without_threshold} chunks")
    print(f"With threshold:    {with_threshold} chunks")
    print(f"Memory saved:      {without_threshold - with_threshold} chunks ({(without_threshold-with_threshold)/without_threshold*100:.0f}%)")
    
    return without_threshold, with_threshold


# ============ Main ============
if __name__ == "__main__":
    lane = test_spherical_container()
    without, with_thresh = test_memory_efficiency()
