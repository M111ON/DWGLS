# gguf_vocab.py — read tokens + merges + token_embd rows from Qwen GGUF
# Usage: python gguf_vocab.py <gguf> [tokens.txt]
import struct, sys

def load_gguf_vocab(path):
    with open(path, 'rb') as f:
        magic = f.read(4)
        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]

        kv = {}
        for _ in range(n_kv):
            klen = struct.unpack('<Q', f.read(8))[0]
            key = f.read(klen).decode('utf-8', 'replace')
            vtype = struct.unpack('<I', f.read(4))[0]
            if vtype == 8:  # STR
                slen = struct.unpack('<Q', f.read(8))[0]
                kv[key] = f.read(slen).decode('utf-8', 'replace')
            elif vtype == 9:  # ARR
                atype = struct.unpack('<I', f.read(4))[0]
                n = struct.unpack('<Q', f.read(8))[0]
                if atype == 8:  # arr of strings
                    arr = []
                    for _ in range(n):
                        sl = struct.unpack('<Q', f.read(8))[0]
                        arr.append(f.read(sl).decode('utf-8', 'replace'))
                    kv[key] = arr
                else:
                    sizes = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                    sz = sizes.get(atype, 4)
                    f.seek(sz * n, 1)
                    kv[key] = None
            else:
                sizes = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                f.seek(sizes.get(vtype, 4), 1)
        return kv, f.tell()

if __name__ == '__main__':
    path = sys.argv[1]
    kv, pos = load_gguf_vocab(path)
    tokens = kv.get('tokenizer.ggml.tokens', [])
    merges = kv.get('tokenizer.ggml.merges', [])
    print(f'tokens: {len(tokens)}, merges: {len(merges)}')
    print(f'first 5 tokens: {tokens[:5]}')
    print(f'first 5 merges: {merges[:5]}')
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            for t in tokens:
                f.write(t + '\n')
        print(f'wrote {len(tokens)} tokens to {sys.argv[2]}')