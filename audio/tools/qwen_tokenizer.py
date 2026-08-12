import sys, struct, os

def load_vocab(path):
    with open(path, 'rb') as f:
        f.read(16)  # magic + ver + n_tensors + n_kv
        kv = {}
        for _ in range(1000):
            kl = struct.unpack('<Q', f.read(8))[0]
            if kl == 0: break
            key = f.read(kl).decode('utf-8', errors='replace')
            vt = struct.unpack('<I', f.read(4))[0]
            if vt == 8:
                sl = struct.unpack('<Q', f.read(8))[0]
                kv[key] = f.read(sl).decode('utf-8', errors='replace')
            elif vt == 9:
                at = struct.unpack('<I', f.read(4))[0]
                n = struct.unpack('<Q', f.read(8))[0]
                if at == 8:
                    arr = []
                    for _ in range(n):
                        sl = struct.unpack('<Q', f.read(8))[0]
                        arr.append(f.read(sl).decode('utf-8', errors='replace'))
                    kv[key] = arr
                else:
                    szs = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                    f.seek(szs.get(at, 4) * n, 1)
            else:
                szs = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
                f.seek(szs.get(vt, 4), 1)
        return kv

def bpe_encode(text, tokens, merges):
    """BPE tokenize using GGUF merges list."""
    # GPT-2 style: space = Ġ prefix
    words = text.split(' ')
    init = []
    for w in words:
        chars = list(w)
        init.append('Ġ' + chars[0] if init == [] else chars[0])
        for c in chars[1:]:
            init.append(c)
    # simplified: just split by known tokens
    merged = list(text.replace(' ', 'Ġ'))
    merge_set = {}
    for i, m in enumerate(merges):
        a, b = m.split(' ', 1)
        merge_set[(a, b)] = i
    for _ in range(len(merges)):
        best = None
        for i in range(len(merged) - 1):
            pair = (merged[i], merged[i+1])
            if pair in merge_set:
                if best is None or merge_set[pair] < merge_set[best]:
                    best = pair
        if best is None:
            break
        a, b = best
        new = []
        i = 0
        while i < len(merged):
            if i < len(merged) - 1 and merged[i] == a and merged[i+1] == b:
                new.append(a + b)
                i += 2
            else:
                new.append(merged[i])
                i += 1
        merged = new
    # map to IDs
    tok2id = {t: i for i, t in enumerate(tokens)}
    ids = [tok2id.get(t, 0) for t in merged]
    return ids, merged

if __name__ == '__main__':
    path = sys.argv[1]
    kv = load_vocab(path)
    tokens = kv.get('tokenizer.ggml.tokens', [])
    merges = kv.get('tokenizer.ggml.merges', [])
    text = sys.argv[2] if len(sys.argv) > 2 else 'Hello world'
    ids, toks = bpe_encode(text, tokens, merges)
    print(f'input: {text}')
    print(f'tokens: {toks[:10]}...')
    print(f'ids: {ids[:10]}...')
    print(f'count: {len(ids)}')
