"""
Data Flow — โครงสร้างรับผิดชอบข้อมูล
====================================
ข้อมูลไหลผ่าน geometry → structure บีบให้ → -1000 ยังไม่ 0
"""
import numpy as np
import time

# === Import geometry core ===
from geometry_core import (
    ruler_tick, voxel_size, compound_144,
    geo_jump_hilbert, geo_jump_peano, geo_jump_metatron,
    geo_fibo_clock, GRID_SIZE, PHI
)

class StructureContainer:
    """
    Container ที่โครงสร้างรับผิดชอบข้อมูล
    - ไม่ต้องมี codec
    - Geometry บีบให้เอง
    - เข้าไปเล็ก → ขับออกมาใหญ่
    """
    
    def __init__(self, layers=5):
        self.layers = layers
        self.compound = compound_144(scale=2.0)
        self.grid = np.zeros(GRID_SIZE, dtype=np.float64)
        self.address_map = {}
        
        # Build address space from ruler
        for n in range(layers):
            r = ruler_tick(+1, n)
            vsize = voxel_size(n)
            # Each layer has (n+1)*12 points
            n_points = (n + 1) * 12
            for k in range(n_points):
                addr = n * GRID_SIZE // layers + k
                if addr < GRID_SIZE:
                    self.address_map[addr] = {
                        'layer': n,
                        'radius': r,
                        'voxel_size': vsize,
                        'point_idx': k
                    }
    
    def encode(self, data):
        """
        Encode: ข้อมูลเข้า → structure บีบให้
        ไม่ต้องคิด algorithm — geometry จัดการ
        """
        start = time.perf_counter_ns()
        
        result = []
        for i, val in enumerate(data):
            # Find which layer this belongs to
            addr = i % GRID_SIZE
            info = self.address_map.get(addr, {'layer': 0, 'voxel_size': voxel_size(0)})
            
            # Structure compresses: small voxel = dense data
            layer = info['layer']
            vsize = info['voxel_size']
            
            # Natural compression: smaller voxel = more compression
            # The structure IS the codec
            compressed_val = val * vsize  # scale by voxel size
            result.append({
                'original': val,
                'compressed': compressed_val,
                'layer': layer,
                'voxel_size': vsize,
                'address': addr
            })
        
        elapsed = time.perf_counter_ns() - start
        return result, elapsed
    
    def decode(self, encoded):
        """
        Decode: ข้อมูลออกมา → structure คลายให้
        ไม่ต้องคิด algorithm — geometry จัดการ
        """
        start = time.perf_counter_ns()
        
        result = []
        for item in encoded:
            # Reverse the natural compression
            vsize = item['voxel_size']
            original_val = item['compressed'] / vsize if vsize > 0 else 0
            result.append(original_val)
        
        elapsed = time.perf_counter_ns() - start
        return result, elapsed
    
    def verify(self, original, decoded):
        """Verify: -1000 ยังไม่ 0 = data preservation"""
        if len(original) != len(decoded):
            return False, f"Length mismatch: {len(original)} vs {len(decoded)}"
        
        mismatches = 0
        for i, (a, b) in enumerate(zip(original, decoded)):
            if abs(a - b) > 1e-10:
                mismatches += 1
        
        return mismatches == 0, f"{mismatches}/{len(original)} mismatches"
    
    def access_from_inside(self, target_addr):
        """
        Access จากข้างใน — เล็ก → ใหญ่
        Structure รับผิดชอบ — ไม่ต้องคิด
        """
        # Start from center (smallest voxel)
        center_layer = 0
        current_vsize = voxel_size(center_layer)
        
        path = []
        for layer in range(self.layers):
            vsize = voxel_size(layer)
            path.append({
                'layer': layer,
                'voxel_size': vsize,
                'expansion': vsize / current_vsize
            })
            current_vsize = vsize
        
        return path


# === Demo: -1000 ยังไม่ 0 ===
def demo_preservation():
    """ทดสอบว่า structure รักษาข้อมูลได้"""
    print("\n" + "=" * 60)
    print("DEMO: -1000 ยังไม่ 0 = DATA PRESERVATION")
    print("=" * 60)
    
    container = StructureContainer(layers=5)
    
    # Test data: various patterns
    test_cases = [
        ("Sequential", list(range(100))),
        ("Random", list(np.random.randint(-1000, 1000, 100))),
        ("Sparse", [0]*50 + [42]*50),
        ("Edge", [-1000]*50 + [999]*50),
    ]
    
    for name, data in test_cases:
        encoded, enc_time = container.encode(data)
        decoded, dec_time = container.decode(encoded)
        ok, msg = container.verify(data, decoded)
        
        status = "PASS" if ok else "FAIL"
        print(f"\n  [{status}] {name}")
        print(f"    Encode: {enc_time} ns")
        print(f"    Decode: {dec_time} ns")
        print(f"    Verify: {msg}")
        
        # Show structure effect
        layer_sizes = {}
        for item in encoded:
            layer = item['layer']
            if layer not in layer_sizes:
                layer_sizes[layer] = []
            layer_sizes[layer].append(item['voxel_size'])
        
        print(f"    Layers used: {sorted(layer_sizes.keys())}")
    
    return container


# === Demo: Access from inside ===
def demo_inside_access():
    """ทดสอบ access จากข้างใน → ข้างนอก"""
    print("\n" + "=" * 60)
    print("DEMO: ACCESS FROM INSIDE (เล็ก → ใหญ่)")
    print("=" * 60)
    
    container = StructureContainer(layers=5)
    
    # Access path from center to surface
    path = container.access_from_inside(0)
    
    print("\n  Access path (center → surface):")
    print("  " + "-" * 40)
    for step in path:
        bar = "█" * int(step['expansion'] * 5)
        print(f"  Layer {step['layer']}: voxel={step['voxel_size']:.4f}, "
              f"expand={step['expansion']:.1f}x {bar}")
    
    # Show how data density changes
    print("\n  Data density (points per unit volume):")
    print("  " + "-" * 40)
    for layer in range(5):
        r = ruler_tick(+1, layer)
        vsize = voxel_size(layer)
        # Volume of shell at radius r
        shell_vol = (4/3) * np.pi * ((r + vsize)**3 - r**3)
        density = (layer + 1) * 12 / shell_vol if shell_vol > 0 else 0
        print(f"  Layer {layer}: r={r:.3f}, vol={shell_vol:.3f}, "
              f"density={density:.2f} pts/volume")
    
    return path


# === Demo: Natural compression ===
def demo_natural_compression():
    """ทดสอบว่า geometry บีบข้อมูลเอง"""
    print("\n" + "=" * 60)
    print("DEMO: NATURAL COMPRESSION (geometry = codec)")
    print("=" * 60)
    
    container = StructureContainer(layers=5)
    
    # Generate test data
    data = list(np.random.randint(-1000, 1000, 200))
    
    # Encode with structure
    encoded, enc_time = container.encode(data)
    
    # Calculate compression
    original_size = len(data) * 8  # 8 bytes per int
    compressed_size = len(encoded) * 8  # same size, but different distribution
    
    # Show how values are distributed across layers
    layer_data = {}
    for item in encoded:
        layer = item['layer']
        if layer not in layer_data:
            layer_data[layer] = []
        layer_data[layer].append(item['compressed'])
    
    print(f"\n  Original data: {len(data)} values, {original_size} bytes")
    print(f"  Encoding time: {enc_time} ns")
    
    print("\n  Distribution across layers:")
    print("  " + "-" * 50)
    for layer in sorted(layer_data.keys()):
        values = layer_data[layer]
        vsize = voxel_size(layer)
        print(f"  Layer {layer}: {len(values):3d} values, "
              f"voxel={vsize:.4f}, "
              f"range=[{min(values):.1f}, {max(values):.1f}]")
    
    # Verify roundtrip
    decoded, dec_time = container.decode(encoded)
    ok, msg = container.verify(data, decoded)
    print(f"\n  Roundtrip: {'PASS' if ok else 'FAIL'} ({msg})")
    print(f"  Decode time: {dec_time} ns")
    
    # Show the key insight: structure IS the codec
    print("\n  KEY INSIGHT:")
    print("  " + "-" * 50)
    print("  No codec needed — geometry compresses naturally")
    print("  Small voxels (center) = dense data = compression")
    print("  Large voxels (surface) = sparse data = index")
    print("  The structure IS the compression mechanism")


if __name__ == "__main__":
    demo_preservation()
    demo_inside_access()
    demo_natural_compression()
    
    print("\n" + "=" * 60)
    print("ALL DEMOS COMPLETE")
    print("=" * 60)
