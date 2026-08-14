"""Generate a simple binary index for safetensors file.
Output: tensor_count(4B) + for each tensor: name_len(2B) + name + dtype_len(1B) + dtype + ndim(1B) + shape(8*int64) + offset_start(8B) + offset_end(8B)
"""
import struct, json, sys

st_path = "I:/model/smolVLM-256M-Instruct/model.safetensors"
idx_path = "I:/FGLS_new/beam_addressing/smolvlm.idx"

with open(st_path, 'rb') as f:
    hdr_len = struct.unpack('<Q', f.read(8))[0]
    hdr = json.loads(f.read(hdr_len))

tensors = [(k, v) for k, v in hdr.items() if k != '__metadata__']
print(f"Writing {len(tensors)} tensors to {idx_path}")

with open(idx_path, 'wb') as f:
    f.write(struct.pack('<I', len(tensors)))
    for name, info in tensors:
        dtype = info['dtype']
        shape = info['shape']
        offsets = info['data_offsets']
        ndim = len(shape)
        
        name_b = name.encode('utf-8')
        dtype_b = dtype.encode('utf-8')
        
        f.write(struct.pack('<H', len(name_b)))
        f.write(name_b)
        f.write(struct.pack('<B', len(dtype_b)))
        f.write(dtype_b)
        f.write(struct.pack('<B', ndim))
        for s in shape:
            f.write(struct.pack('<q', s))
        f.write(struct.pack('<Q', offsets[0]))
        f.write(struct.pack('<Q', offsets[1]))

import os
print(f"Index size: {os.path.getsize(idx_path)} bytes")
print(f"First 5 tensors:")
for name, info in tensors[:5]:
    print(f"  {name}: {info['dtype']} {info['shape']} [{info['data_offsets'][0]},{info['data_offsets'][1]}]")
