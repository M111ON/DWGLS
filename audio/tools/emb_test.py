import numpy as np
import mmap, struct, os

GGUF_PATH = 'I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf'
EMB_DIM = 896
BLOCK = 32

def find_emb_offset(path):
    """Use gguf package to find token_embd.weight data offset."""
    from gguf import GGUFReader
    reader = GGUFReader(path)
    for t in reader.tensors:
        if t.name == 'token_embd.weight':
            # t.data is mmap'd, but shape is (151936, 952) uint8
            return t
    raise ValueError("not found")

def dequant_row(raw_bytes, out=None):
    """Dequantize Q8_0 row: float16 scale × (Q8-128)/127."""
    if out is None:
        out = np.zeros(EMB_DIM, dtype=np.float32)
    nb = len(raw_bytes) // BLOCK
    for b in range(nb):
        off = b * (BLOCK + 2)
        scale = np.frombuffer(raw_bytes[off:off+2], dtype=np.float16)[0]
        q8 = np.frombuffer(raw_bytes[off+2:off+2+BLOCK], dtype=np.uint8).astype(np.float32)
        out[b*BLOCK:(b+1)*BLOCK] = scale * (q8 - 128.0) / 127.0
    return out

if __name__ == '__main__':
    t = find_emb_offset(GGUF_PATH)
    print(f'tensor: {t.name} shape={t.data.shape} dtype={t.data.dtype}')
    emb = t.data  # (151936, 952) uint8

    # test: dequantize token 0
    row0 = emb[0]
    out = dequant_row(row0.tobytes())
    print(f'token 0: norm={np.linalg.norm(out):.3f}, first5={out[:5]}')

    row1 = emb[1]
    out1 = dequant_row(row1.tobytes())
    print(f'token 1: norm={np.linalg.norm(out1):.3f}, first5={out1[:5]}')

    # batch: load first 100 tokens
    batch = np.zeros((100, EMB_DIM), dtype=np.float32)
    for i in range(100):
        dequant_row(emb[i].tobytes(), batch[i])
    print(f'\nbatch 100: mean_norm={np.mean(np.linalg.norm(batch, axis=1)):.3f}')
    print(f'cos(0,1) = {np.dot(batch[0], batch[1]) / (np.linalg.norm(batch[0]) * np.linalg.norm(batch[1])):.4f}')
