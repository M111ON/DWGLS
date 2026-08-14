"""Generate binary index for LFM2.5-VL-450M safetensors."""
import struct, json, os

st_path = "I:/model/LFM2.5-VL-450M/model.safetensors"
idx_path = "I:/FGLS_new/beam_addressing/lfm25vl.idx"

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

print(f"Index: {os.path.getsize(idx_path)} bytes")
