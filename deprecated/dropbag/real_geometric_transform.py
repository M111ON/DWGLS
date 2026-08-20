"""
Real Geometric Transform — เห็น pattern เปลี่ยนจริง

Test: map binary data ผ่าน geometry จริง (ไม่ใช่แค่คูณเลข)
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966

def cardioid(theta, a=1.0):
    """Cardioid: r = a(1 + cos(θ))"""
    return a * (1 + np.cos(theta))

def rotate_4d(point, angle, axis1, axis2):
    """
    Rotate point in 4D space
    
    axis1, axis2: which axes to rotate around
    """
    p = point.copy()
    c, s = np.cos(angle), np.sin(angle)
    p[axis1] = c * point[axis1] - s * point[axis2]
    p[axis2] = s * point[axis1] + c * point[axis2]
    return p

def geometric_transform(data, angle=0.785):  # 45 degrees
    """
    Transform binary data through 4D geometry
    
    1. Map each bit to a 4D point
    2. Rotate in 4D
    3. Project back to 1D
    """
    result = np.zeros_like(data, dtype=float)
    
    for i, bit in enumerate(data):
        # Map bit to 4D point
        if bit == 0:
            point = np.array([0.0, 0.0, 0.0, 0.0])
        else:
            # Map to different positions based on index
            theta = (i * 2 * np.pi) / len(data)
            r = 1.0
            point = np.array([
                r * np.cos(theta),
                r * np.sin(theta),
                0.0,
                0.0
            ])
        
        # Rotate in 4D
        rotated = rotate_4d(point, angle, 0, 3)  # rotate in XW plane
        rotated = rotate_4d(rotated, angle * 0.7, 1, 2)  # rotate in YZ plane
        
        # Project back to 1D (magnitude)
        result[i] = np.linalg.norm(rotated)
    
    return result


# ============ Test 1: All 1s ============
def test_all_ones():
    """Test: ไฟล์ 1 ทั้งหมด"""
    print("=" * 60)
    print("TEST 1: All 1s (Real Transform)")
    print("=" * 60)
    
    data = np.ones(20)  # 20 bytes = all 1s
    print(f"Input: {data.astype(int)} (all 1s)")
    
    # Original method (just scale)
    scaled = data * 0.382
    print(f"Scaled: {scaled[:10]} (just multiply)")
    
    # Real geometric transform
    transformed = geometric_transform(data, angle=np.pi/4)
    print(f"Transformed: {transformed[:10]} (real geometry)")
    
    # Compare
    print(f"\nScaled mean: {scaled.mean():.6f}")
    print(f"Transformed mean: {transformed.mean():.6f}")
    print(f"Are they different? {not np.allclose(scaled, transformed)}")
    
    return data, scaled, transformed

# ============ Test 2: Alternating 010101 ============
def test_alternating():
    """Test: 01010101..."""
    print("\n" + "=" * 60)
    print("TEST 2: Alternating 010101 (Real Transform)")
    print("=" * 60)
    
    data = np.array([0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1])
    print(f"Input: {data} (01010101010101010101)")
    
    # Original method (just scale)
    scaled = data * 0.382
    print(f"Scaled: {scaled[:10]} (just multiply)")
    
    # Real geometric transform
    transformed = geometric_transform(data, angle=np.pi/4)
    print(f"Transformed: {transformed[:10]} (real geometry)")
    
    # Compare
    print(f"\nScaled mean: {scaled.mean():.6f}")
    print(f"Transformed mean: {transformed.mean():.6f}")
    print(f"Are they different? {not np.allclose(scaled, transformed)}")
    
    return data, scaled, transformed

# ============ Test 3: Compare patterns ============
def test_compare():
    """Test: เปรียบเทียบทุก pattern"""
    print("\n" + "=" * 60)
    print("TEST 3: Compare all patterns (Real Transform)")
    print("=" * 60)
    
    patterns = {
        'all_ones': np.ones(20),
        'alternating': np.array([0, 1] * 10),
        'all_zeros': np.zeros(20),
        'random': np.random.default_rng(42).integers(0, 2, 20),
    }
    
    print(f"{'Pattern':<15} {'Scaled':<12} {'Transformed':<12} {'Different?':<12}")
    print("-" * 55)
    
    for name, data in patterns.items():
        scaled = (data * 0.382).mean()
        transformed = geometric_transform(data, angle=np.pi/4).mean()
        different = not np.isclose(scaled, transformed)
        print(f"{name:<15} {scaled:<12.6f} {transformed:<12.6f} {str(different):<12}")
    
    # Analysis
    print("\n" + "=" * 60)
    print("ANALYSIS")
    print("=" * 60)
    print("ถ้า Scaled ≠ Transformed = geometry เปลี่ยน pattern จริง")
    print("ถ้า Scaled = Transformed = แค่ scale (ไม่จริง)")

# ============ Main ============
if __name__ == "__main__":
    test_all_ones()
    test_alternating()
    test_compare()
