"""
A-Z Geometric Transform — เห็น pattern เปลี่ยนจริง

Test: map a-z ผ่าน geometry จริง
"""
import numpy as np

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2

def rotate_4d(point, angle, axis1, axis2):
    p = point.copy()
    c, s = np.cos(angle), np.sin(angle)
    p[axis1] = c * point[axis1] - s * point[axis2]
    p[axis2] = s * point[axis1] + c * point[axis2]
    return p

def char_to_4d(c, total=26):
    """Map character to 4D point"""
    idx = ord(c) - ord('a')
    theta = (idx * 2 * np.pi) / total
    phi = (idx * np.pi) / total
    r = 1.0
    return np.array([
        r * np.cos(theta) * np.sin(phi),
        r * np.sin(theta) * np.sin(phi),
        r * np.cos(phi),
        idx / total  # w = position in alphabet
    ])

def geometric_transform_char(c, angle=0.785):
    """Transform single character through 4D geometry"""
    point = char_to_4d(c)
    rotated = rotate_4d(point, angle, 0, 3)
    rotated = rotate_4d(rotated, angle * 0.7, 1, 2)
    return rotated

def transform_string(text, angle=0.785):
    """Transform entire string"""
    result = []
    for c in text:
        transformed = geometric_transform_char(c, angle)
        result.append(transformed)
    return np.array(result)


# ============ Test: A-Z ============
def test_az():
    """Test: a-z"""
    print("=" * 60)
    print("A-Z GEOMETRIC TRANSFORM")
    print("=" * 60)
    
    text = "abcdefghijklmnopqrstuvwxyz"
    print(f"\nInput: {text}")
    
    # Transform
    result = transform_string(text)
    
    # Show first few
    print(f"\nFirst 5 characters:")
    for i, c in enumerate(text[:5]):
        print(f"  '{c}' → [{result[i,0]:.3f}, {result[i,1]:.3f}, {result[i,2]:.3f}, {result[i,3]:.3f}]")
    
    # Show pattern
    print(f"\nAll characters (x-coordinate):")
    x_coords = result[:, 0]
    print(f"  {[f'{x:.3f}' for x in x_coords]}")
    
    # Analysis
    print(f"\nAnalysis:")
    print(f"  Mean: {result.mean():.3f}")
    print(f"  Std:  {result.std():.3f}")
    print(f"  Min:  {result.min():.3f}")
    print(f"  Max:  {result.max():.3f}")
    
    # Compare with simple scaling
    simple = np.array([ord(c) - ord('a') for c in text]) / 25.0
    print(f"\nSimple scaling (a=0, z=1):")
    print(f"  {[f'{s:.3f}' for s in simple]}")
    
    print(f"\nGeometric transform:")
    print(f"  {[f'{x:.3f}' for x in x_coords]}")
    
    print(f"\nAre they different? {not np.allclose(simple, x_coords)}")
    
    return result


# ============ Main ============
if __name__ == "__main__":
    result = test_az()
