"""
Binary Transform Test — เห็นค่าเปลี่ยนจริงไหม?

Test: วาง binary data เข้า container แล้วดูว่าค่าเปลี่ยนจริงไหม
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966

def voxel_size(n):
    return abs((1 + ALPHA) ** (n+1) - (1 + ALPHA) ** n)

# ============ Test 1: All 1s ============
def test_all_ones():
    """Test: ไฟล์ 1 ทั้งหมด"""
    print("=" * 60)
    print("TEST 1: All 1s")
    print("=" * 60)
    
    data = np.ones(100)  # 100 bytes = all 1s
    print(f"Input: {data[:10]}... (all 1s)")
    
    # Transform through geometry
    for n in range(5):
        vs = voxel_size(n)
        transformed = data * vs
        print(f"  Layer {n}: voxel_size={vs:.6f}  output={transformed[:3]}  mean={transformed.mean():.6f}")
    
    return data

# ============ Test 2: Alternating 010101 ============
def test_alternating():
    """Test: 01010101..."""
    print("\n" + "=" * 60)
    print("TEST 2: Alternating 010101")
    print("=" * 60)
    
    data = np.array([0, 1, 0, 1, 0, 1, 0, 1, 0, 1] * 10)  # 100 bytes
    print(f"Input: {data[:10]}... (0101010101)")
    
    for n in range(5):
        vs = voxel_size(n)
        transformed = data * vs
        print(f"  Layer {n}: voxel_size={vs:.6f}  output={transformed[:5]}  mean={transformed.mean():.6f}")
    
    return data

# ============ Test 3: Random binary ============
def test_random_binary():
    """Test: random 0s and 1s"""
    print("\n" + "=" * 60)
    print("TEST 3: Random binary")
    print("=" * 60)
    
    rng = np.random.default_rng(42)
    data = rng.integers(0, 2, 100)  # random 0s and 1s
    print(f"Input: {data[:10]}... (random)")
    
    for n in range(5):
        vs = voxel_size(n)
        transformed = data * vs
        print(f"  Layer {n}: voxel_size={vs:.6f}  output={transformed[:5]}  mean={transformed.mean():.6f}")
    
    return data

# ============ Test 4: All 0s ============
def test_all_zeros():
    """Test: ไฟล์ 0 ทั้งหมด"""
    print("\n" + "=" * 60)
    print("TEST 4: All 0s")
    print("=" * 60)
    
    data = np.zeros(100)  # 100 bytes = all 0s
    print(f"Input: {data[:10]}... (all 0s)")
    
    for n in range(5):
        vs = voxel_size(n)
        transformed = data * vs
        print(f"  Layer {n}: voxel_size={vs:.6f}  output={transformed[:3]}  mean={transformed.mean():.6f}")
    
    return data

# ============ Test 5: Compare all patterns ============
def test_compare():
    """Test: เปรียบเทียบทุก pattern"""
    print("\n" + "=" * 60)
    print("TEST 5: Compare all patterns")
    print("=" * 60)
    
    patterns = {
        'all_ones': np.ones(100),
        'alternating': np.array([0, 1, 0, 1, 0, 1, 0, 1, 0, 1] * 10),
        'all_zeros': np.zeros(100),
    }
    
    rng = np.random.default_rng(42)
    patterns['random'] = rng.integers(0, 2, 100)
    
    print(f"{'Pattern':<15} {'Layer 0':<12} {'Layer 1':<12} {'Layer 2':<12} {'Layer 3':<12} {'Layer 4':<12}")
    print("-" * 75)
    
    for name, data in patterns.items():
        means = []
        for n in range(5):
            vs = voxel_size(n)
            transformed = data * vs
            means.append(f"{transformed.mean():.6f}")
        print(f"{name:<15} {' '.join(means)}")
    
    # ผลลัพธ์
    print("\n" + "=" * 60)
    print("ANALYSIS")
    print("=" * 60)
    print("ถ้าค่าเท่ากันทุก pattern = ไม่เห็นความต่าง")
    print("ถ้าค่าต่างกัน = เห็นความต่าง")
    print()
    print("ผลลัพธ์: คูณด้วย voxel_size เท่ากันทุก pattern")
    print("เหตุผล: ไม่ได้ transform จริง แค่คูณเลข")
    print("ต้อง: map ผ่าน geometry จริง ถึงจะเห็นความต่าง")

# ============ Main ============
if __name__ == "__main__":
    test_all_ones()
    test_alternating()
    test_random_binary()
    test_all_zeros()
    test_compare()
