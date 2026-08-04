"""
Voronoi Subdivision → Root Efficiency (Better Demo)

Key insight:
- More Voronoi seeds = more regions
- More regions = more path options  
- More options = shorter paths
- Shorter paths = less memory

Better demo: Access MULTIPLE targets in same region
"""
import numpy as np

PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2
R0 = 1.0

def ruler_tick(sign, n):
    return sign * R0 * (1 + ALPHA) ** n

def voxel_size(n):
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


class SmartVoronoi:
    """
    Voronoi that subdivides when dense → more path options
    """
    
    def __init__(self, n_seeds=9):
        self.seeds = self._phi_seeds(n_seeds)
        self.regions = {i: [] for i in range(n_seeds)}
        self.subdivision_count = 0
        
    def _phi_seeds(self, n):
        seeds = []
        for i in range(n):
            theta = 2 * np.pi * i / PHI
            phi = np.arccos(1 - 2 * (i / n))
            r = np.sqrt(i / n)
            seeds.append([r*np.sin(phi)*np.cos(theta),
                         r*np.sin(phi)*np.sin(theta),
                         r*np.cos(phi)])
        return np.array(seeds)
    
    def assign_and_maybe_subdivide(self, point, threshold=10):
        distances = np.linalg.norm(self.seeds - point, axis=1)
        region_id = np.argmin(distances)
        min_dist = distances[region_id]
        
        self.regions[region_id].append(point)
        
        new_seed = None
        if len(self.regions[region_id]) > threshold:
            centroid = np.mean(self.regions[region_id], axis=0)
            self.seeds = np.vstack([self.seeds, centroid])
            self.regions[len(self.seeds)-1] = []
            self.subdivision_count += 1
            new_seed = centroid
            self.regions[region_id] = []
        
        return region_id, min_dist, new_seed


class RootPathOptimizer:
    """
    Find shortest root path using Voronoi regions
    """
    
    def __init__(self, voronoi):
        self.voronoi = voronoi
        
    def find_shortest_path(self, target, max_steps=50):
        current = np.array([0.0, 0.0, 0.0])
        path = [current.copy()]
        memory_used = 1
        visited = set()
        
        for step in range(max_steps):
            # Find nearest unvisited seed that gets us closer to target
            distances_to_target = np.linalg.norm(self.voronoi.seeds - target, axis=1)
            distances_to_current = np.linalg.norm(self.voronoi.seeds - current, axis=1)
            
            # Score: closer to target AND reachable from current
            score = distances_to_target + distances_to_current * 0.3
            
            # Mark visited seeds
            for i in range(len(score)):
                if i in visited:
                    score[i] = float('inf')
            
            # Pick best
            best_idx = np.argmin(score)
            visited.add(best_idx)
            
            seed = self.voronoi.seeds[best_idx]
            new_dist_to_target = np.linalg.norm(seed - target)
            current_dist_to_target = np.linalg.norm(current - target)
            
            # Only move if closer to target
            if new_dist_to_target < current_dist_to_target:
                current = seed
                path.append(current.copy())
                memory_used += 1
            
            # Check if reached
            if np.linalg.norm(current - target) < 1.0:
                break
        
        return path, memory_used
    
    def find_multi_target_path(self, targets, max_steps=50):
        """
        Find path that visits ALL targets (TSP-like)
        
        Key: more seeds = better options = shorter total path
        """
        current = np.array([0.0, 0.0, 0.0])
        path = [current.copy()]
        memory_used = 1
        visited_targets = set()
        
        for step in range(max_steps):
            # Find nearest unvisited target
            best_target_idx = None
            best_target_dist = float('inf')
            
            for i, target in enumerate(targets):
                if i not in visited_targets:
                    dist = np.linalg.norm(current - target)
                    if dist < best_target_dist:
                        best_target_dist = dist
                        best_target_idx = i
            
            if best_target_idx is None:
                break
            
            target = targets[best_target_idx]
            
            # Find path to this target
            distances_to_target = np.linalg.norm(self.voronoi.seeds - target, axis=1)
            distances_to_current = np.linalg.norm(self.voronoi.seeds - current, axis=1)
            
            score = distances_to_target + distances_to_current * 0.3
            best_idx = np.argmin(score)
            
            seed = self.voronoi.seeds[best_idx]
            new_dist_to_target = np.linalg.norm(seed - target)
            current_dist_to_target = np.linalg.norm(current - target)
            
            if new_dist_to_target < current_dist_to_target:
                current = seed
                path.append(current.copy())
                memory_used += 1
            
            # Check if reached target
            if np.linalg.norm(current - target) < 1.0:
                visited_targets.add(best_target_idx)
        
        return path, memory_used


def demo_multi_target():
    """
    Access MULTIPLE targets in same region
    
    Key: more seeds = better options = shorter total path
    """
    print("=" * 70)
    print("VORONOI SUBDIVISION → ROOT EFFICIENCY (Multi-Target)")
    print("=" * 70)
    
    # Multiple targets in same region
    targets = [
        np.array([10.0, 5.0, 3.0]),
        np.array([12.0, 7.0, 4.0]),
        np.array([11.0, 6.0, 2.0]),
        np.array([9.0, 4.0, 5.0]),
        np.array([13.0, 8.0, 1.0]),
    ]
    
    # Case 1: No subdivision (9 seeds)
    print("\n--- Case 1: No subdivision (9 seeds) ---")
    vor_no_sub = SmartVoronoi(n_seeds=9)
    optimizer_no_sub = RootPathOptimizer(vor_no_sub)
    
    path_no_sub, mem_no_sub = optimizer_no_sub.find_multi_target_path(targets, max_steps=50)
    print(f"  Targets: {len(targets)}")
    print(f"  Path steps: {len(path_no_sub)}")
    print(f"  Memory used: {mem_no_sub}")
    
    # Case 2: With subdivision (9 → grows)
    print("\n--- Case 2: With subdivision (9 → grows) ---")
    vor_with_sub = SmartVoronoi(n_seeds=9)
    
    # Simulate data to trigger subdivisions
    rng = np.random.default_rng(42)
    for _ in range(200):
        point = rng.uniform(-20, 20, 3)
        vor_with_sub.assign_and_maybe_subdivide(point, threshold=10)
    
    print(f"  Seeds after subdivision: {len(vor_with_sub.seeds)}")
    print(f"  Subdivisions: {vor_with_sub.subdivision_count}")
    
    optimizer_with_sub = RootPathOptimizer(vor_with_sub)
    
    path_with_sub, mem_with_sub = optimizer_with_sub.find_multi_target_path(targets, max_steps=50)
    print(f"  Targets: {len(targets)}")
    print(f"  Path steps: {len(path_with_sub)}")
    print(f"  Memory used: {mem_with_sub}")
    
    # Compare
    print("\n--- Comparison ---")
    print(f"  Without subdivision: {mem_no_sub} memory")
    print(f"  With subdivision:    {mem_with_sub} memory")
    savings = (mem_no_sub - mem_with_sub) / mem_no_sub * 100
    print(f"  Savings: {savings:.1f}%")
    
    return vor_no_sub, vor_with_sub


def demo_region_density():
    """
    Show: more seeds = denser coverage = better paths
    """
    print("\n" + "=" * 70)
    print("REGION DENSITY — More Seeds = Denser Coverage")
    print("=" * 70)
    
    targets = [
        np.array([10.0, 5.0, 3.0]),
        np.array([12.0, 7.0, 4.0]),
        np.array([11.0, 6.0, 2.0]),
    ]
    
    for n_seeds in [9, 18, 36]:
        vor = SmartVoronoi(n_seeds=n_seeds)
        
        # Add random points to trigger subdivisions
        rng = np.random.default_rng(42)
        for _ in range(200):
            point = rng.uniform(-20, 20, 3)
            vor.assign_and_maybe_subdivide(point, threshold=10)
        
        optimizer = RootPathOptimizer(vor)
        path, mem = optimizer.find_multi_target_path(targets, max_steps=50)
        
        print(f"\n  Seeds={n_seeds:2d}: final_seeds={len(vor.seeds):3d}, "
              f"path={len(path)} steps, memory={mem}")
        print(f"    Coverage density: {len(vor.seeds)/200:.3f} seeds/unit")
    
    return None


if __name__ == "__main__":
    vor1, vor2 = demo_multi_target()
    demo_region_density()
