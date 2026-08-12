import numpy as np, struct, wave, subprocess, os, sys

GGUF_PATH = 'I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf'
EMB_DIM = 896
BLOCK = 32
BLOCK_B = 34
TTS_DIR = 'I:/DWGLS/tools/tts_data'
N_MELS = 80

def load_vocab(path):
    from gguf_vocab import load_gguf_vocab
    kv, _ = load_gguf_vocab(path)
    return kv.get('tokenizer.ggml.tokens', []), kv.get('tokenizer.ggml.merges', [])

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
        i = best[0]
        a, b = merged[i], merged[i+1]
        new, j = [], 0
        while j < len(merged):
            if j < len(merged)-1 and merged[j]==a and merged[j+1]==b:
                new.append(a+b); j += 2
            else: new.append(merged[j]); j += 1
        merged = new
    return merged

def load_emb_matrix(token_ids):
    """Load dequantized embeddings for specific token IDs."""
    from gguf import GGUFReader
    reader = GGUFReader(GGUF_PATH)
    emb = None
    for t in reader.tensors:
        if t.name == 'token_embd.weight':
            emb = t.data; break
    result = {}
    for tid in set(token_ids):
        row = emb[tid].astype(np.uint8)
        out = np.zeros(EMB_DIM, dtype=np.float32)
        for b in range(len(row) // BLOCK_B):
            off = b * BLOCK_B
            scale = struct.unpack('<e', row[off:off+2].tobytes())[0]
            q8 = row[off+2:off+BLOCK_B].astype(np.float32)
            out[b*BLOCK:(b+1)*BLOCK] = scale * (q8 - 128.0) / 127.0
        result[tid] = out
    return result

def compute_mel_frame(samples, sr, frame_idx):
    """Compute 80-dim log-mel for one frame."""
    N_FFT = 400; HOP = 160
    start = frame_idx * HOP
    windowed = np.zeros(N_FFT, dtype=np.float32)
    end = min(start + N_FFT, len(samples))
    n = end - start
    windowed[:n] = samples[start:end]
    win = 0.5 * (1 - np.cos(2*np.pi*np.arange(N_FFT)/N_FFT))
    windowed *= win
    mel = np.zeros(N_MELS, dtype=np.float32)
    for k in range(N_MELS):
        angle = 2*np.pi*k*np.arange(N_FFT)/N_FFT
        real = np.sum(windowed * np.cos(angle))
        imag = np.sum(windowed * -np.sin(angle))
        mel[k] = np.log10(max(np.sqrt(real**2+imag**2)/N_FFT, 1e-10))
    return mel

def geo_mel_to_addresses(mel):
    """mel 80 → 80 addresses in 144×144 grid."""
    GEO = 144
    addrs = []
    for k in range(N_MELS):
        norm = (mel[k] + 10) / 10
        norm = max(0, min(1, norm))
        radius = int(norm * (GEO-1))
        angle = k % GEO
        addrs.append(angle * GEO + radius)
    return addrs

def mel_to_geometric_vec(mel):
    """mel 80 → 896-dim geometric vector (golden angle)."""
    GEO = 144; HIDDEN = 896
    GOLDEN = 137.508 * np.pi / 180
    peak = np.argmax(mel)
    norm_c = (np.sum(np.abs(mel)*np.arange(N_MELS)) / max(np.sum(np.abs(mel)),1e-10)) / N_MELS
    angle = peak % GEO; radius = int(norm_c * (GEO-1))
    vec = np.zeros(HIDDEN, dtype=np.float32)
    for i in range(HIDDEN):
        theta = GOLDEN * i
        r = np.sqrt(i / HIDDEN) * (GEO-1)
        x = (int(r * np.cos(theta) + GEO) + angle) % GEO
        y = (int(r * np.sin(theta) + GEO) + radius) % GEO
        vec[i] = (x * GEO + y) / (GEO * GEO)
    return vec

def word_boundaries_from_mel(samples, sr):
    """Detect word boundaries using mel energy + 20th percentile threshold."""
    N_FFT = 400; HOP = 160
    n_frames = (len(samples) - N_FFT) // HOP
    energy = []
    for f in range(n_frames):
        start = f * HOP
        frame = samples[start:start+N_FFT].astype(np.float32)
        win = 0.5*(1-np.cos(2*np.pi*np.arange(N_FFT)/N_FFT))
        frame *= win
        energy.append(np.sum(frame**2))
    energy = np.array(energy)
    if len(energy) == 0: return []
    thresh = np.percentile(energy, 20)
    in_word = False
    boundaries = []
    for i, e in enumerate(energy):
        if e > thresh and not in_word:
            start_f = i; in_word = True
        elif e <= thresh and in_word:
            boundaries.append((start_f, i))
            in_word = False
    if in_word:
        boundaries.append((start_f, len(energy)))
    return boundaries

def read_wav_mono(path):
    w = wave.open(path, 'r')
    raw = w.readframes(w.getnframes())
    sr = w.getframerate(); w.close()
    return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0, sr

def main():
    tokens, merges = load_vocab(GGUF_PATH)
    tok2id = {t: i for i, t in enumerate(tokens)}
    print(f'vocab: {len(tokens)} tokens, {len(merges)} merges')

    # Read sentences
    with open('I:/DWGLS/tools/tts_sentences.txt') as f:
        sentences = [l.strip() for l in f if l.strip()]
    print(f'sentences: {len(sentences)}')

    # Extract geometric vectors (per file) and target embeddings (per word)
    X_list = []  # geometric vectors
    Y_list = []  # LLM embeddings
    word_labels = []

    for i, sent in enumerate(sentences):
        wav_path = f'{TTS_DIR}/s{i+1}.wav'
        if not os.path.exists(wav_path): continue
        samples, sr = read_wav_mono(wav_path)
        print(f'  s{i+1}: {len(samples)/sr:.1f}s, sr={sr}')

        # Geometric vector per file (time-averaged)
        geo_vec = mel_to_geometric_vec(compute_mel_frame(samples, sr, 0))

        # Target: average of token embeddings for sentence
        subtoks = bpe_tokenize(sent, merges)
        ids = [tok2id.get(t, 0) for t in subtoks]
        print(f'    tokens: {len(ids)}')
        X_list.append(geo_vec)
        word_labels.append(sent[:50])
    
    print(f'\nloaded {len(X_list)} geometric vectors')

    # Load target embeddings (batch)
    all_ids = []
    for sent in sentences:
        subtoks = bpe_tokenize(sent, merges)
        all_ids.extend([tok2id.get(t, 0) for t in subtoks])
    print(f'loading {len(all_ids)} unique token embeddings...')
    emb_dict = load_emb_matrix(list(set(all_ids)))
    print(f'loaded {len(emb_dict)} embeddings')

    Y_list = []
    for sent in sentences:
        subtoks = bpe_tokenize(sent, merges)
        ids = [tok2id.get(t, 0) for t in subtoks]
        vecs = [emb_dict[tid] for tid in ids if tid in emb_dict]
        Y_list.append(np.mean(vecs, axis=0))

    X = np.array(X_list, dtype=np.float32)
    Y = np.array(Y_list, dtype=np.float32)
    print(f'\ndataset: X={X.shape} Y={Y.shape}')

    # Ridge regression: X @ W ≈ Y
    lam = 1.0
    W = np.linalg.solve(X.T @ X + lam * np.eye(X.shape[1]), X.T @ Y)
    print(f'adapter W: shape={W.shape}')

    # Evaluate: cosine similarity
    Y_pred = X @ W
    cosines = []
    for i in range(len(X)):
        c = np.dot(Y_pred[i], Y[i]) / (np.linalg.norm(Y_pred[i]) * np.linalg.norm(Y[i]) + 1e-10)
        cosines.append(c)
    print(f'\ncosine similarity: mean={np.mean(cosines):.4f}, min={np.min(cosines):.4f}, max={np.max(cosines):.4f}')
    for i, (s, c) in enumerate(zip(word_labels, cosines)):
        print(f'  {s:50s} cos={c:.4f}')

    # Save adapter
    np.save('adapter_W.npy', W)
    print(f'\nsaved adapter W: {W.shape} ({W.nbytes/1024:.1f} KB)')

if __name__ == '__main__':
    main()
