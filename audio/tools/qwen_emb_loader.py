import numpy as np, struct
from gguf_vocab import load_gguf_vocab

GGUF_PATH = 'I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf'
EMB_DIM = 896
BLOCK = 32
BLOCK_Q8 = BLOCK + 2  # 34 bytes per block (float16 scale + 32 Q8)

def read_emb_row(path, row_idx):
    """Read and dequantize one row from token_embd.weight Q8_0."""
    with open(path, 'rb') as f:
        # Find token_embd.weight tensor offset
        f.read(16)  # header
        # skip kv
        for _ in range(10000):
            kl = struct.unpack('<Q', f.read(8))[0]
            if kl == 0: break
            f.read(kl)
            vt = struct.unpack('<I', f.read(4))[0]
            if vt == 8:
                sl = struct.unpack('<Q', f.read(8))[0]
                f.read(sl)
            elif vt == 9:
                at = struct.unpack('<I', f.read(4))[0]
                n = struct.unpack('<Q', f.read(8))[0]
                szs = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                if at == 8:
                    for _ in range(n):
                        sl = struct.unpack('<Q', f.read(8))[0]
                        f.read(sl)
                else:
                    f.seek(szs.get(at, 4) * n, 1)
            else:
                szs = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                f.seek(szs.get(vt, 4), 1)
        # tensor info
        nt = struct.unpack('<Q', f.read(8))[0]
        for _ in range(nt):
            nl = struct.unpack('<Q', f.read(8))[0]
            name = f.read(nl).decode('utf-8', errors='replace')
            nd = struct.unpack('<I', f.read(4))[0]
            dims = [struct.unpack('<Q', f.read(8))[0] for _ in range(nd)]
            dt = struct.unpack('<I', f.read(4))[0]
            offset = struct.unpack('<Q', f.read(8))[0]
            if name == 'token_embd.weight':
                row_bytes = dims[1]  # 952 bytes per row
                f.seek(offset + row_idx * row_bytes)
                raw = np.frombuffer(f.read(row_bytes), dtype=np.uint8)
                # dequant Q8_0: each block = float16_scale(2 bytes) + 32 Q8
                out = np.zeros(EMB_DIM, dtype=np.float32)
                for b in range(dims[0] // BLOCK):
                    scale_bytes = raw[b*BLOCK_Q8 : b*BLOCK_Q8+2]
                    scale = np.frombuffer(scale_bytes, dtype=np.float16)[0]
                    q8 = raw[b*BLOCK_Q8+2 : b*BLOCK_Q8+BLOCK_Q8].astype(np.float32)
                    out[b*BLOCK:(b+1)*BLOCK] = scale * (q8 - 128.0) / 127.0
                return out
        raise ValueError("token_embd.weight not found")

def get_text_embedding(text, emb_matrix, tokens, merges, tok2id):
    """BPE tokenize text, lookup embeddings, mean-pool → 896-dim vector."""
    merged = list(text.replace(' ', 'Ġ'))
    ms = {}
    for i, m in enumerate(merges):
        a, b = m.split(' ', 1)
        ms[(a, b)] = i
    for _ in range(50):
        best = None
        for i in range(len(merged) - 1):
            p = (merged[i], merged[i+1])
            if p in ms and (best is None or ms[p] < ms[best]):
                best = p
        if best is None: break
        a, b = best
        new, i = [], 0
        while i < len(merged):
            if i < len(merged)-1 and merged[i]==a and merged[i+1]==b:
                new.append(a+b); i += 2
            else: new.append(merged[i]); i += 1
        merged = new
    ids = [tok2id.get(t, 0) for t in merged]
    vecs = [emb_matrix[i] for i in ids if i < len(emb_matrix)]
    return np.mean(vecs, axis=0) if vecs else np.zeros(EMB_DIM)

if __name__ == '__main__':
    kv, _ = load_gguf_vocab(GGUF_PATH)
    tokens = kv.get('tokenizer.ggml.tokens', [])
    merges = kv.get('tokenizer.ggml.merges', [])
    tok2id = {t: i for i, t in enumerate(tokens)}

    test_words = ['Hello', 'world', 'The', 'quick', 'brown', 'fox']
    emb_matrix = np.zeros((len(tokens), EMB_DIM), dtype=np.float32)
    print("loading embeddings for test words...")
    for w in test_words:
        ids = []
        merged = list(w)
        for _ in range(20):
            best = None
            for i in range(len(merged)-1):
                p = (merged[i], merged[i+1])
                mi = next((j for j,m in enumerate(merges) if m.split(' ',1)==p), 999999)
                if best is None or mi < best[1]: best = (i, mi)
            if best is None: break
            i, _ = best
            merged = merged[:i] + [merged[i]+merged[i+1]] + merged[i+2:]
        tid = tok2id.get(w, 0)
        emb = read_emb_row(GGUF_PATH, tid)
        print(f'  "{w}" (id={tid}): norm={np.linalg.norm(emb):.2f}, first3={emb[:3]}')
