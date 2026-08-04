"""
Spherical Container Test — Golden-Ratio Voxels
===============================================
Test: radial depth as natural LOD + self-indexing container

Concept:
- Surface layer: big voxels = index (fast scan)
- Center layer: tiny voxels = data (high detail)
- Access from outside in: drill-down naturally
"""

import numpy as np
import time
import json

# ============================================================
# Part 1: Golden-Ratio Ruler (unchanged from voxel_world.py)
# ============================================================
alpha = 0.381966  # 1/phi^2
R0 = 1.0

def ruler_tick(sign, n):
    return sign * R0 * (1 + alpha) ** n

def voxel_size(n):
    """voxel gap at layer n — bigger n = bigger voxel"""
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


# ============================================================
# Part 2: Spherical Coordinate System
# ============================================================
def spherical_address(r, theta, phi):
    """Convert spherical (r, theta, phi) to Cartesian (x, y, z)"""
    x = r * np.sin(theta) * np.cos(phi)
    y = r * np.sin(theta) * np.sin(phi)
    z = r * np.cos(theta)
    return (x, y, z)

def golden_sphere_layers(N=5):
    """
    Build sphere with N radial layers.
    Each layer has golden-ratio radius = ruler_tick(layer).
    Outer layers = big voxels (index), inner = small (data).
    """
    layers = []
    for n in range(N):
        r = ruler_tick(+1, n)
        vsize = voxel_size(n)
        
        # Theta/phi resolution scales with radius
        # Outer = fewer points (big voxels), inner = more (small voxels)
        # But we cap it to keep visualization clean
        n_theta = max(4, int(8 * (n + 1)))
        n_phi = max(4, int(8 * (n + 1)))
        
        points = []
        for i_theta in range(n_theta):
            theta = np.pi * (i_theta + 0.5) / n_theta
            for i_phi in range(n_phi):
                phi = 2 * np.pi * i_phi / n_phi
                x, y, z = spherical_address(r, theta, phi)
                points.append({
                    'x': x, 'y': y, 'z': z,
                    'r': r,
                    'theta': theta, 'phi': phi,
                    'layer': n,
                    'voxel_size': vsize,
                    'volume': vsize ** 3
                })
        
        layers.append({
            'n': n,
            'radius': r,
            'voxel_size': vsize,
            'volume': vsize ** 3,
            'n_points': len(points),
            'points': points
        })
    
    return layers


# ============================================================
# Part 3: Self-Indexing Access Pattern
# ============================================================
class SphericalContainer:
    """
    Self-indexing container:
    - Surface = sparse index (few big voxels)
    - Center = dense data (many small voxels)
    - Access: surface → drill down → center
    """
    
    def __init__(self, N_layers=5, data_per_center_voxel=16):
        self.layers = golden_sphere_layers(N_layers)
        self.N_layers = N_layers
        self.data_per_voxel = data_per_center_voxel
        
        # Pre-build index: which layer has what sector
        self.index = self._build_index()
        
        # Store data in center voxels
        self.data_store = {}
        self._fill_data()
    
    def _build_index(self):
        """Build sparse index from outer layers"""
        index = {}
        for layer in self.layers:
            n = layer['n']
            # Each point in this layer = one index entry
            # Maps (theta_sector, phi_sector) → next layer
            for pt in layer['points']:
                theta_sec = int(pt['theta'] / np.pi * 4)  # 4 theta sectors
                phi_sec = int(pt['phi'] / (2 * np.pi) * 8)  # 8 phi sectors
                key = (n, theta_sec, phi_sec)
                index[key] = {
                    'layer': n,
                    'point': pt,
                    'next_layer': n + 1 if n < self.N_layers - 1 else None
                }
        return index
    
    def _fill_data(self):
        """Fill center layer with fake data"""
        center_layer = self.layers[-1]
        for i, pt in enumerate(center_layer['points']):
            # Generate some data for each center voxel
            self.data_store[pt['theta'], pt['phi']] = {
                'value': np.random.randint(0, 256, size=self.data_per_voxel).tolist(),
                'voxel_size': pt['voxel_size'],
                'access_depth': pt['layer']
            }
    
    def surface_scan(self, target_theta, target_phi):
        """
        Step 1: Scan surface (outer layer) to find which sector
        This is FAST because surface voxels are big = few to check
        """
        start = time.perf_counter_ns()
        
        surface_layer = self.layers[0]
        best_match = None
        best_dist = float('inf')
        
        for pt in surface_layer['points']:
            d_theta = abs(pt['theta'] - target_theta)
            d_phi = abs(pt['phi'] - target_phi)
            dist = d_theta**2 + d_phi**2
            if dist < best_dist:
                best_dist = dist
                best_match = pt
        
        elapsed = time.perf_counter_ns() - start
        
        return {
            'matched_point': best_match,
            'distance': best_dist,
            'scans': len(surface_layer['points']),
            'time_ns': elapsed
        }
    
    def drill_down(self, target_theta, target_phi):
        """
        Step 2: Drill from surface to center
        Each layer narrows the search area
        """
        start = time.perf_counter_ns()
        
        path = []
        current_theta = target_theta
        current_phi = target_phi
        
        for n in range(self.N_layers):
            layer = self.layers[n]
            
            # Find closest point in this layer
            best_pt = None
            best_dist = float('inf')
            for pt in layer['points']:
                d = (pt['theta'] - current_theta)**2 + (pt['phi'] - current_phi)**2
                if d < best_dist:
                    best_dist = d
                    best_pt = pt
            
            path.append({
                'layer': n,
                'point': best_pt,
                'distance': best_dist,
                'voxel_size': layer['voxel_size']
            })
            
            # Narrow search for next layer (use matched point's coords)
            current_theta = best_pt['theta']
            current_phi = best_pt['phi']
        
        elapsed = time.perf_counter_ns() - start
        
        # Final data at center
        center_data = self.data_store.get(
            (path[-1]['point']['theta'], path[-1]['point']['phi']),
            None
        )
        
        return {
            'path': path,
            'total_steps': len(path),
            'final_voxel_size': path[-1]['voxel_size'],
            'data': center_data,
            'time_ns': elapsed
        }
    
    def flat_access(self, target_theta, target_phi):
        """
        Compare: flat access — scan ALL points in all layers
        This is the "no index" approach
        """
        start = time.perf_counter_ns()
        
        all_points = []
        for layer in self.layers:
            all_points.extend(layer['points'])
        
        best_pt = None
        best_dist = float('inf')
        for pt in all_points:
            d = (pt['theta'] - target_theta)**2 + (pt['phi'] - target_phi)**2
            if d < best_dist:
                best_dist = d
                best_pt = pt
        
        elapsed = time.perf_counter_ns() - start
        
        return {
            'point': best_pt,
            'scans': len(all_points),
            'time_ns': elapsed
        }


# ============================================================
# Part 4: Run Tests
# ============================================================
def run_tests():
    print("=" * 60)
    print("SPHERICAL CONTAINER TEST — Golden-Ratio Voxels")
    print("=" * 60)
    
    # --- Test 1: Layer properties ---
    print("\n[1] LAYER PROPERTIES (radial depth = golden-ratio spacing)")
    print("-" * 50)
    layers = golden_sphere_layers(5)
    for layer in layers:
        print(f"  Layer {layer['n']}: r={layer['radius']:.3f}, "
              f"voxel_size={layer['voxel_size']:.4f}, "
              f"volume={layer['volume']:.6f}, "
              f"points={layer['n_points']}")
    
    # --- Test 2: Self-indexing container ---
    print("\n[2] SELF-INDEXING CONTAINER")
    print("-" * 50)
    container = SphericalContainer(N_layers=5, data_per_center_voxel=16)
    print(f"  Total index entries: {len(container.index)}")
    print(f"  Data store entries: {len(container.data_store)}")
    print(f"  Center voxel size: {container.layers[-1]['voxel_size']:.6f}")
    print(f"  Surface voxel size: {container.layers[0]['voxel_size']:.4f}")
    print(f"  Size ratio (surface/center): "
          f"{container.layers[0]['voxel_size']/container.layers[-1]['voxel_size']:.1f}x")
    
    # --- Test 3: Access patterns ---
    print("\n[3] ACCESS PATTERN COMPARISON")
    print("-" * 50)
    
    test_targets = [
        (np.pi/4, np.pi/2),
        (np.pi/3, np.pi),
        (np.pi/6, 3*np.pi/2),
    ]
    
    surface_times = []
    drill_times = []
    flat_times = []
    
    for theta, phi in test_targets:
        print(f"\n  Target: θ={theta:.3f}, φ={phi:.3f}")
        
        # Surface scan
        result = container.surface_scan(theta, phi)
        surface_times.append(result['time_ns'])
        print(f"    Surface scan: {result['scans']} checks, {result['time_ns']} ns")
        
        # Drill down
        result = container.drill_down(theta, phi)
        drill_times.append(result['time_ns'])
        print(f"    Drill down: {result['total_steps']} layers, {result['time_ns']} ns")
        
        # Flat access
        result = container.flat_access(theta, phi)
        flat_times.append(result['time_ns'])
        print(f"    Flat scan: {result['scans']} checks, {result['time_ns']} ns")
    
    # --- Test 4: Benchmark summary ---
    print("\n[4] BENCHMARK SUMMARY")
    print("-" * 50)
    
    avg_surface = np.mean(surface_times)
    avg_drill = np.mean(drill_times)
    avg_flat = np.mean(flat_times)
    
    print(f"  Avg surface scan:  {avg_surface:.0f} ns ({avg_surface/1000:.1f} µs)")
    print(f"  Avg drill-down:    {avg_drill:.0f} ns ({avg_drill/1000:.1f} µs)")
    print(f"  Avg flat scan:     {avg_flat:.0f} ns ({avg_flat/1000:.1f} µs)")
    print(f"  Speedup (surface vs flat): {avg_flat/avg_surface:.1f}x")
    print(f"  Speedup (drill vs flat):   {avg_flat/avg_drill:.1f}x")
    
    # --- Test 5: Data capacity ---
    print("\n[5] DATA CAPACITY ANALYSIS")
    print("-" * 50)
    
    total_voxels = sum(l['n_points'] for l in layers)
    center_voxels = layers[-1]['n_points']
    data_per_voxel = container.data_per_voxel
    
    print(f"  Total voxels: {total_voxels}")
    print(f"  Center voxels (data): {center_voxels}")
    print(f"  Data per center voxel: {data_per_voxel} values")
    print(f"  Total data capacity: {center_voxels * data_per_voxel} values")
    print(f"  Surface voxels (index): {layers[0]['n_points']}")
    print(f"  Index compression ratio: {layers[0]['n_points']/total_voxels*100:.1f}% of total")
    
    # --- Test 6: Visualize structure ---
    print("\n[6] STRUCTURE VISUALIZATION")
    print("-" * 50)
    print("  Layer 0 (Surface):", "█" * layers[0]['n_points'], f"← {layers[0]['n_points']} voxels")
    print("  Layer 1:", "▓" * layers[1]['n_points'], f"← {layers[1]['n_points']} voxels")
    print("  Layer 2:", "▒" * layers[2]['n_points'], f"← {layers[2]['n_points']} voxels")
    print("  Layer 3:", "░" * layers[3]['n_points'], f"← {layers[3]['n_points']} voxels")
    print("  Layer 4 (Center):", "·" * min(layers[4]['n_points'], 50), f"← {layers[4]['n_points']} voxels")
    
    return {
        'layers': layers,
        'surface_times': surface_times,
        'drill_times': drill_times,
        'flat_times': flat_times,
        'container': container
    }


# ============================================================
# Part 5: Visualization (Plotly)
# ============================================================
def visualize(results):
    """Generate interactive 3D visualization"""
    import plotly.graph_objects as go
    
    layers = results['layers']
    
    fig = go.Figure()
    
    # Color palette for layers
    colors = ['#00c8ff', '#00a8d4', '#0088aa', '#006880', '#ff6b35']
    names = ['Surface (Index)', 'Layer 1', 'Layer 2', 'Layer 3', 'Center (Data)']
    
    for i, layer in enumerate(layers):
        fig.add_trace(go.Scatter3d(
            x=[p['x'] for p in layer['points']],
            y=[p['y'] for p in layer['points']],
            z=[p['z'] for p in layer['points']],
            mode='markers',
            name=names[i],
            marker=dict(
                size=layer['voxel_size'] * 8,
                color=colors[i],
                opacity=0.7,
                line=dict(width=0.3, color='black')
            )
        ))
    
    fig.update_layout(
        title='Spherical Container — Golden-Ratio Voxels<br>'
              'Surface=Index (big) → Center=Data (small)',
        scene=dict(
            xaxis_title='X', yaxis_title='Y', zaxis_title='Z',
            aspectmode='data'
        ),
        legend=dict(x=0, y=1)
    )
    
    fig.write_html('spherical_container.html')
    print("\n[OK] Saved: spherical_container.html")


# ============================================================
# Main
# ============================================================
if __name__ == "__main__":
    results = run_tests()
    
    try:
        visualize(results)
    except ImportError:
        print("\n[SKIP] plotly not installed, skipping visualization")
    
    print("\n" + "=" * 60)
    print("DONE")
    print("=" * 60)
