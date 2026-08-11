import numpy as np, wave, os, struct

GGUF_PATH = 'I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf'
TTS_DIR = 'I:/DWGLS/tools/tts_data'
N_MELS = 80
GEO = 144
HIDDEN = 896
GOLDEN = 137.508 * np.pi / 180
BLOCK_B = 34

def read_wav_mono(path):
    w = wave.open(path, 'r')
    raw = w.readframes(w.getnframes()); sr = w.getframerate(); w.close()
    return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0, sr

def compute_mel_all_frames(samples):
    N_FFT = 400; HOP = 160
    nframes = (len(samples) - N_FFT) // HOP
    mel_all = np.zeros((nframes, N_MELS), dtype=np.float32)
    for f in range(nframes):
        s = f * HOP
        windowed = samples[s:s+N_FFT].astype(np.float32)
        win = 0.5*(1-np.cos(2*np.pi*np.arange(N_FFT)/N_FFT))
        windowed *= win
        for k in range(N_MELS):
            angle = 2*np.pi*k*np.arange(N_FFT)/N_FFT
            r = np.sum(windowed*np.cos(angle))
            im = np.sum(windowed*(-np.sin(angle)))
            mel_all[f,k] = np.log10(max(np.sqrt(r**2+im**2)/N_FFT, 1e-10))
    return mel_all

def mel_to_geometric_vec(mel_all):
    """Compute 896-dim geo vector from full mel spectrogram statistics."""
    # Global features from all frames
    mel_mean = np.mean(mel_all, axis=0)  # (80,)
    mel_std = np.std(mel_all, axis=0)
    mel_max = np.max(mel_all, axis=0)

    # Peak bin (average across time)
    peak_bins = np.argmax(mel_all, axis=1)
    peak_bin_mean = np.mean(peak_bins)

    # Centroid (time-averaged)
    centroids = []
    for f in range(len(mel_all)):
        m = mel_all[f]
        t = np.sum(np.abs(m))
        if t > 1e-8:
            centroids.append(np.sum(np.abs(m)*np.arange(N_MELS))/t)
    centroid_mean = np.mean(centroids) if centroids else 40

    angle0 = int(peak_bin_mean) % GEO
    radius0 = int(centroid_mean / N_MELS * (GEO-1))

    vec = np.zeros(HIDDEN, dtype=np.float32)
    for i in range(HIDDEN):
        theta = GOLDEN * i
        r = np.sqrt(i/HIDDEN)*(GEO-1)
        # Base position from mel global features
        x = (int(r*np.cos(theta)+GEO)+angle0)%GEO
        y = (int(r*np.sin(theta)+GEO)+radius0)%GEO
        base = (x*GEO+y)/(GEO*GEO)
        # Modulate by mel variance (energy variation → address modulation)
        var_mod = mel_std[i % N_MELS] * 0.01
        vec[i] = base + var_mod
    return vec

def bpe_tokenize(text, merges):
    merged = list(text.replace(' ', 'Ġ'))
    ms = {m: i for i, m in enumerate(merges)}
    for _ in range(50):
        best = None
        for i in range(len(merged)-1):
            p = (merged[i], merged[i+1])
            if p in ms and (best is None or ms[p] < ms[best]):
                best = (i, ms[p])
        if best is None: break
        i = best[0]; a, b = merged[i], merged[i+1]
        new, j = [], 0
        while j < len(merged):
            if j<len(merged)-1 and merged[j]==a and merged[j+1]==b:
                new.append(a+b); j+=2
            else: new.append(merged[j]); j+=1
        merged = new
    return merged

def load_emb_batch(token_ids):
    from gguf import GGUFReader
    reader = GGUFReader(GGUF_PATH)
    emb = None
    for t in reader.tensors:
        if t.name == 'token_embd.weight': emb = t.data; break
    result = {}
    for tid in set(token_ids):
        row = emb[tid].astype(np.uint8)
        out = np.zeros(HIDDEN, dtype=np.float32)
        for b in range(len(row)//BLOCK_B):
            off = b*BLOCK_B
            scale = struct.unpack('<e', row[off:off+2].tobytes())[0]
            q8 = row[off+2:off+BLOCK_B].astype(np.float32)
            out[b*32:(b+1)*32] = scale*(q8-128.0)/127.0
        result[tid] = out
    return result

def main():
    from gguf_vocab import load_gguf_vocab
    kv, _ = load_gguf_vocab(GGUF_PATH)
    tokens = kv.get('tokenizer.ggml.tokens', [])
    merges = kv.get('tokenizer.ggml.merges', [])
    tok2id = {t: i for i, t in enumerate(tokens)}
    print(f'vocab: {len(tokens)}')

    with open('I:/DWGLS/tools/tts_sentences.txt') as f:
        sentences = [l.strip() for l in f if l.strip()]

    X_list, Y_list = [], []
    all_ids = set()
    sent_ids_all = []

    for i, sent in enumerate(sentences):
        wav = f'{TTS_DIR}/s{i+1}.wav'
        if not os.path.exists(wav): continue
        samples, sr = read_wav_mono(wav)
        mel_all = compute_mel_all_frames(samples)
        geo_vec = mel_to_geometric_vec(mel_all)

        subtoks = bpe_tokenize(sent, merges)
        ids = [tok2id.get(t, 0) for t in subtoks]
        all_ids.update(ids)
        sent_ids_all.append(ids)
        X_list.append(geo_vec)

    emb_dict = load_emb_batch(list(all_ids))
    print(f'loaded {len(emb_dict)} embeddings')

    for ids in sent_ids_all:
        vecs = [emb_dict[tid] for tid in ids if tid in emb_dict]
        Y_list.append(np.mean(vecs, axis=0) if vecs else np.zeros(HIDDEN))

    X = np.array(X_list, dtype=np.float32)
    Y = np.array(Y_list, dtype=np.float32)

    # Normalize rows (important for cosine)
    X = X / (np.linalg.norm(X, axis=1, keepdims=True)+1e-10)
    Y = Y / (np.linalg.norm(Y, axis=1, keepdims=True)+1e-10)

    print(f'dataset: X={X.shape} Y={Y.shape}')
    print(f'X variance: {X.var():.8f}')
    print(f'Y variance: {Y.var():.8f}')

    # Train/test split: 8 train, 2 test
    perm = np.random.RandomState(42).permutation(len(X))
    train_idx, test_idx = perm[:8], perm[8:]

    Xtr, Ytr = X[train_idx], Y[train_idx]
    Xte, Yte = X[test_idx], Y[test_idx]

    lam = 0.01
    W = np.linalg.solve(Xtr.T @ Xtr + lam*np.eye(HIDDEN), Xtr.T @ Ytr)

    Ytr_pred = Xtr @ W
    Yte_pred = Xte @ W

    train_cos = [np.dot(Ytr_pred[i],Ytr[i]) for i in range(len(Xtr))]
    test_cos = [np.dot(Yte_pred[i],Yte[i]) for i in range(len(Xte))]
    all_cos = [np.dot(X[i],Y[i]) for i in range(len(X))]

    print(f'\nbefore adapter (raw geo→LLM): mean={np.mean(all_cos):.4f}')
    print(f'train cosine: mean={np.mean(train_cos):.4f}')
    print(f'test cosine:  mean={np.mean(test_cos):.4f}')

    np.save('adapter_W.npy', W)
    print(f'adapter saved: {W.shape}')

if __name__ == '__main__':
    main()
