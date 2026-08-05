"""
Container Test — PDF Files into Spherical Container

Test: วาง PDF files เข้า container แล้วดูว่าเล็กลงแค่ไหน
"""
import numpy as np
import os

# ============ Constants ============
PHI = (1 + np.sqrt(5)) / 2
ALPHA = 1 / PHI**2  # 0.381966
R0 = 1.0

def ruler_tick(sign, n):
    return sign * R0 * (1 + ALPHA) ** n

def voxel_size(n):
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))


# ============ Spherical Container ============
class SphericalContainer:
    """
    Spherical Container with golden-ratio voxels
    
    Inside-out: center = dense data, surface = index
    """
    
    def __init__(self, n_layers=5):
        self.n_layers = n_layers
        self.layers = []
        self.data = {}
        
        # Build layers
        for n in range(n_layers):
            radius = ruler_tick(+1, n)
            size = voxel_size(n)
            self.layers.append({
                'n': n,
                'radius': radius,
                'voxel_size': size,
                'capacity': int(4/3 * np.pi * (radius + size)**3 - 4/3 * np.pi * radius**3),
                'used': 0
            })
    
    def get_layer_for_size(self, data_size):
        """Find best layer for data size"""
        for layer in reversed(self.layers):
            if data_size <= layer['voxel_size'] * 1000:  # 1000 voxels max per layer
                return layer
        return self.layers[0]  # smallest layer
    
    def store(self, file_path):
        """Store file in container"""
        # Read file
        with open(file_path, 'rb') as f:
            data = f.read()
        
        file_size = len(data)
        file_name = os.path.basename(file_path)
        
        # Find best layer
        layer = self.get_layer_for_size(file_size)
        
        # Store in layer
        self.data[file_name] = {
            'data': data,
            'size': file_size,
            'layer': layer['n'],
            'voxel_size': layer['voxel_size']
        }
        
        layer['used'] += 1
        
        return {
            'file': file_name,
            'original_size': file_size,
            'layer': layer['n'],
            'voxel_size': layer['voxel_size'],
            'stored_size': int(file_size * layer['voxel_size'])
        }
    
    def get_summary(self):
        """Summary of container"""
        total_original = sum(d['size'] for d in self.data.values())
        total_stored = sum(d['size'] * d['voxel_size'] for d in self.data.values())
        
        return {
            'files': len(self.data),
            'total_original': total_original,
            'total_stored': int(total_stored),
            'ratio': total_stored / total_original if total_original > 0 else 0,
            'layers_used': [d['layer'] for d in self.data.values()]
        }


# ============ Test with PDF files ============
def test_pdf_container():
    """Test: วาง PDF files เข้า container"""
    print("=" * 60)
    print("CONTAINER TEST — PDF Files")
    print("=" * 60)
    
    # PDF files
    pdf_dir = r"I:\DWGLS\dropbag\PDF_sample"
    pdf_files = [f for f in os.listdir(pdf_dir) if f.endswith('.pdf')]
    
    # Create container
    container = SphericalContainer(n_layers=5)
    
    print(f"\nPDF files found: {len(pdf_files)}")
    print(f"Container layers: {container.n_layers}")
    
    # Store each file
    results = []
    for pdf_file in pdf_files:
        file_path = os.path.join(pdf_dir, pdf_file)
        result = container.store(file_path)
        results.append(result)
        
        print(f"\n  File: {pdf_file}")
        print(f"    Original: {result['original_size']:,} bytes ({result['original_size']/1024/1024:.2f} MB)")
        print(f"    Layer: {result['layer']}")
        print(f"    Voxel size: {result['voxel_size']:.6f}")
        print(f"    Stored: {result['stored_size']:,} bytes ({result['stored_size']/1024/1024:.2f} MB)")
        print(f"    Ratio: {result['stored_size']/result['original_size']:.4f}x")
    
    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    summary = container.get_summary()
    print(f"  Files: {summary['files']}")
    print(f"  Total original: {summary['total_original']:,} bytes ({summary['total_original']/1024/1024:.2f} MB)")
    print(f"  Total stored: {summary['total_stored']:,} bytes ({summary['total_stored']/1024/1024:.2f} MB)")
    print(f"  Ratio: {summary['ratio']:.4f}x")
    print(f"  Layers used: {summary['layers_used']}")
    
    return container, results


# ============ Main ============
if __name__ == "__main__":
    container, results = test_pdf_container()
