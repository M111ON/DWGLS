"""
Codec Extract — แกะ codec จาก geometric transform

เก็บแค่ parameters, ไม่ต้องพกโครงสร้าง
"""
import numpy as np
import json

# ============ Codec Definition ============
class GeometricCodec:
    """
    Codec extracted from geometric transform
    
    เก็บแค่ parameters:
    - mapping function (stored as formula)
    - rotation angles
    - dimension
    """
    
    def __init__(self, dim=4):
        self.dim = dim
        self.angles = []  # rotation angles
        self.axes = []    # rotation axes
        
    def add_rotation(self, angle, axis1, axis2):
        """Add a rotation parameter"""
        self.angles.append(float(angle))
        self.axes.append((int(axis1), int(axis2)))
    
    def encode_char(self, c):
        """Encode character to vector"""
        idx = ord(c) - ord('a')
        theta = (idx * 2 * np.pi) / 26
        phi = (idx * np.pi) / 26
        r = 1.0
        
        point = np.zeros(self.dim)
        point[0] = r * np.cos(theta) * np.sin(phi)
        point[1] = r * np.sin(theta) * np.sin(phi)
        point[2] = r * np.cos(phi)
        point[3] = idx / 26.0
        
        return point
    
    def encode(self, text):
        """Encode text to vectors"""
        return np.array([self.encode_char(c) for c in text])
    
    def rotate(self, point, angle, axis1, axis2):
        """Apply rotation"""
        p = point.copy()
        c, s = np.cos(angle), np.sin(angle)
        p[axis1] = c * point[axis1] - s * point[axis2]
        p[axis2] = s * point[axis1] + c * point[axis2]
        return p
    
    def transform(self, vectors):
        """Apply all rotations"""
        result = vectors.copy()
        for angle, (a1, a2) in zip(self.angles, self.axes):
            for i in range(len(result)):
                result[i] = self.rotate(result[i], angle, a1, a2)
        return result
    
    def decode_vector(self, vec):
        """Decode vector back to character"""
        # Simple: use x coordinate to estimate
        # (in real codec, need inverse function)
        x = vec[0]
        # Map x back to index
        idx = int(round((x + 1) * 13))  # rough mapping
        idx = max(0, min(25, idx))
        return chr(ord('a') + idx)
    
    def decode(self, vectors):
        """Decode vectors back to text"""
        return ''.join([self.decode_vector(v) for v in vectors])
    
    def save(self, filepath):
        """Save codec parameters"""
        params = {
            'dim': self.dim,
            'angles': self.angles,
            'axes': self.axes
        }
        with open(filepath, 'w') as f:
            json.dump(params, f, indent=2)
        print(f"Codec saved to {filepath}")
        print(f"Parameters: dim={self.dim}, rotations={len(self.angles)}")
        print(f"Size: {len(json.dumps(params))} bytes")
    
    @classmethod
    def load(cls, filepath):
        """Load codec parameters"""
        with open(filepath, 'r') as f:
            params = json.load(f)
        
        codec = cls(dim=params['dim'])
        codec.angles = params['angles']
        codec.axes = [tuple(a) for a in params['axes']]
        
        print(f"Codec loaded from {filepath}")
        return codec


# ============ Demo ============
def demo_codec_extraction():
    """Demo: extract codec from geometric transform"""
    print("=" * 60)
    print("CODEC EXTRACTION DEMO")
    print("=" * 60)
    
    # 1. Create codec
    print("\n1. Creating codec...")
    codec = GeometricCodec(dim=4)
    codec.add_rotation(np.pi/4, 0, 3)      # rotate in XW plane
    codec.add_rotation(np.pi/4 * 0.7, 1, 2)  # rotate in YZ plane
    
    # 2. Encode text
    print("\n2. Encoding 'hello'...")
    text = "hello"
    vectors = codec.encode(text)
    print(f"   Original: {text}")
    print(f"   Encoded: {vectors[0][:2]}...")
    
    # 3. Transform
    print("\n3. Transforming...")
    transformed = codec.transform(vectors)
    print(f"   Transformed: {transformed[0][:2]}...")
    
    # 4. Save codec
    print("\n4. Saving codec...")
    codec.save("codec.json")
    
    # 5. Load codec
    print("\n5. Loading codec...")
    codec2 = GeometricCodec.load("codec.json")
    
    # 6. Verify
    print("\n6. Verifying...")
    transformed2 = codec2.transform(codec2.encode(text))
    match = np.allclose(transformed, transformed2)
    print(f"   Match: {match}")
    
    # 7. Show parameters
    print("\n7. Codec parameters:")
    print(f"   Dimension: {codec.dim}")
    print(f"   Rotations: {len(codec.angles)}")
    for i, (angle, axes) in enumerate(zip(codec.angles, codec.axes)):
        print(f"     {i+1}. angle={angle:.4f} ({angle*180/np.pi:.1f}°), axes={axes}")
    
    return codec


# ============ Main ============
if __name__ == "__main__":
    codec = demo_codec_extraction()
