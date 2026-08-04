"""
Seed Roots Lane — Access Bigger Space Without Exploding Memory

Concept:
- Seed = starting point (ico or dodeca vertex)
- Roots = spike paths growing outward
- Lane = data flows through these paths
- Memory stays bounded: only load what you follow

Key: Structure IS the access pattern
      Geometry bok forns where to go next
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966
R0 = 1.0

def ruler_tick(sign, n):
    """Distance from origin at tick n"""
    return sign * R0 * (1 + ALPHA) ** n

def voxel_size(n):
    """Size of voxel at layer n = distance to next tick"""
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


# ============ Spike Path (Root Growth) ============
class SeedRootsLane:
    """
    Access data by following spike paths from seed.
    
    Seed → Spike 1 → Spike 2 → Spike 3 → ...
    
    Each spike = one data chunk
    Memory = O(chunks_accessed), NOT O(total_data)
    """
    
    def __init__(self, seed_type='ico'):
        """
        seed_type: 'ico' (20 faces) or 'dodeca' (12 faces)
        """
        self.seed_type = seed_type
        self.seed_pos = np.array([0.0, 0.0, 0.0])  # origin
        self.current_pos = self.seed_pos.copy()
        self.root_path = [self.seed_pos.copy()]  # track path
        self.loaded_chunks = {}  # chunk_id → data
        
    def spike_direction(self, depth):
        """
        Compute spike direction based on depth.
        
        Each spike alternates ico ↔ dodeca
        Direction follows golden ratio geometry
        """
        # Alternate between ico and dodeca directions
        if depth % 2 == 0:
            # Ico: 20 faces, spike to face centers
            angle = depth * (2 * np.pi / 20)  # 20 directions
        else:
            # Dodeca: 12 faces, spike to face centers
            angle = depth * (2 * np.pi / 12)  # 12 directions
        
        # Radius = golden ratio growth
        radius = ruler_tick(+1, depth)
        
        # 3D direction (spherical → cartesian)
        theta = angle
        phi = np.arccos(1 - 2 * ((depth * PHI) % 1))  # golden angle
        
        x = radius * np.sin(phi) * np.cos(theta)
        y = radius * np.sin(phi) * np.sin(theta)
        z = radius * np.cos(phi)
        
        return np.array([x, y, z])
    
    def grow_root(self, depth):
        """
        Grow root by one spike.
        
        Returns: position after spike, chunk_id
        """
        direction = self.spike_direction(depth)
        new_pos = self.current_pos + direction
        
        # Chunk ID = depth (each spike = one chunk)
        chunk_id = depth
        
        # Update state
        self.current_pos = new_pos
        self.root_path.append(new_pos.copy())
        
        return new_pos, chunk_id
    
    def load_chunk(self, chunk_id, data):
        """
        Load data into chunk.
        
        Memory: O(1) per chunk loaded
        """
        self.loaded_chunks[chunk_id] = data
        
    def access_chunk(self, chunk_id):
        """
        Access chunk by ID.
        
        If not loaded → load on demand (lazy loading)
        """
        if chunk_id not in self.loaded_chunks:
            # Simulate loading from "disk"
            # In real system: fetch from GGUF/external storage
            self.loaded_chunks[chunk_id] = np.zeros(100)  # placeholder
        
        return self.loaded_chunks[chunk_id]
    
    def get_memory_usage(self):
        """
        Current memory usage.
        
        Returns: number of chunks loaded
        """
        return len(self.loaded_chunks)
    
    def get_path_length(self):
        """
        How far we've grown.
        
        Returns: number of spikes traversed
        """
        return len(self.root_path) - 1


# ============ Demo: Access Pattern ============
def demo_access_pattern():
    """
    Demonstrate: grow roots, access data on demand.
    
    Key: memory grows with ACCESS, not with TOTAL space
    """
    print("=" * 60)
    print("SEED ROOTS LANE — Access Pattern Demo")
    print("=" * 60)
    
    # Create seed
    lane = SeedRootsLane(seed_type='ico')
    
    # Total potential space (huge)
    total_space = 20736  # 12^4
    
    # But we only access what we need
    access_count = 10  # only 10 spikes
    
    print(f"\nTotal space: {total_space:,} positions")
    print(f"Accessing: {access_count} spikes")
    print(f"Memory loaded: {lane.get_memory_usage()} chunks")
    
    # Grow roots and access data
    for depth in range(access_count):
        # Grow one spike
        pos, chunk_id = lane.grow_root(depth)
        
        # Load data on demand
        data = np.random.randn(100)  # simulate data
        lane.load_chunk(chunk_id, data)
        
        # Access it
        accessed = lane.access_chunk(chunk_id)
        
        print(f"  Spike {depth:2d}: pos=({pos[0]:7.2f}, {pos[1]:7.2f}, {pos[2]:7.2f}) "
              f"chunk={chunk_id:3d} mem={lane.get_memory_usage()}")
    
    print(f"\nFinal memory: {lane.get_memory_usage()} / {total_space} = "
          f"{lane.get_memory_usage()/total_space*100:.2f}%")
    
    return lane


# ============ Demo: Memory Efficiency ============
def demo_memory_efficiency():
    """
    Compare: load all vs load on demand.
    
    Key: lazy loading saves memory
    """
    print("\n" + "=" * 60)
    print("MEMORY EFFICIENCY — Lazy vs Eager")
    print("=" * 60)
    
    total_chunks = 1000
    access_count = 10
    
    # Eager: load everything
    eager_memory = total_chunks  # 1000 chunks
    
    # Lazy: load only what we access
    lazy_memory = access_count  # 10 chunks
    
    print(f"\nTotal chunks: {total_chunks}")
    print(f"Accessed: {access_count}")
    print(f"\nEager loading: {eager_memory} chunks")
    print(f"Lazy loading:  {lazy_memory} chunks")
    print(f"Memory saved:  {eager_memory - lazy_memory} chunks ({(eager_memory-lazy_memory)/eager_memory*100:.0f}%)")
    
    return eager_memory, lazy_memory


# ============ Demo: Spike Path Geometry ============
def demo_spike_geometry():
    """
    Visualize spike path (root growth).
    
    Shows how geometry defines access pattern
    """
    print("\n" + "=" * 60)
    print("SPIKE PATH GEOMETRY")
    print("=" * 60)
    
    lane = SeedRootsLane()
    
    # Grow 20 spikes
    for depth in range(20):
        pos, chunk_id = lane.grow_root(depth)
        
        # Show distance from origin
        dist = np.linalg.norm(pos)
        voxel_sz = voxel_size(depth)
        
        print(f"  Spike {depth:2d}: dist={dist:8.3f}  voxel_size={voxel_sz:8.6f}  "
              f"ratio={dist/voxel_sz:.1f}x")
    
    return lane


# ============ Main ============
if __name__ == "__main__":
    lane = demo_access_pattern()
    eager, lazy = demo_memory_efficiency()
    lane2 = demo_spike_geometry()
