"""Fast stride layout comparison — pre-compute mel addresses."""
import numpy as np, wave, os

GEO = 144; N = GEO * GEO; N_MELS = 80
TTS_DIR = 'I:/DWGLS/tools/tts_data'

def read_wav_mono(p):
    w = wave.open(p, 'r'); raw = w.readframes(w.getnframes()); sr = w.getframerate(); w.close()
    return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0, sr

def compute_mel_frames(samples):
    N_FFT = 400; HOP = 160; nf = (len(samples) - N_FFT) // HOP
    mels = np.zeros((nf, N_MELS), dtype=np.float32)
    win = 0.5 * (1 - np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT))
    for f in range(nf):
        s = f * HOP; w = samples[s:s+N_FFT] * win
        for k in range(N_MELS):
            a = 2 * np.pi * k * np.arange(N_FFT) / N_FFT
            r = np.sum(w * np.cos(a)); im = np.sum(w * (-np.sin(a)))
            mels[f, k] = np.log10(max(np.sqrt(r**2 + im**2) / N_FFT, 1e-10))
    return mels

def detect_words(mels, min_dur=5):
    energy = np.sum(mels**2, axis=1); thresh = np.percentile(energy, 25)
    in_w = False; words = []
    for i, e in enumerate(energy):
        if e > thresh and not in_w: start = i; in_w = True
        elif e <= thresh and in_w:
            if i - start >= min_dur: words.append((start, i))
            in_w = False
    if in_w and len(energy) - start >= min_dur: words.append((start, len(energy)))
    return words

def mel_to_raw_addrs(mel):
    """Return raw (mel_idx, amplitude) pairs — layout-independent."""
    return [(k, max(0, min(1, (mel[k] + 10) / 10))) for k in range(N_MELS)]

def raw_to_addr(raw, layout):
    """Convert raw (mel_idx, amp) to layout address."""
    return [(k % GEO) * GEO + int(a * (GEO - 1)) for k, a in raw]

def word_fp(word_mels, layout):
    """Compute fingerprint: bincount of addresses."""
    addrs = []
    for m in word_mels[:10]:
        raw = mel_to_raw_addrs(m)
        mapped = raw_to_addr(raw, layout)
        addrs.extend(mapped)
    return np.bincount(addrs, minlength=N).astype(np.float32)

cosine = lambda a, b: np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-10)

with open('I:/DWGLS/tools/tts_sentences_50.txt') as f:
    sentences = [l.strip() for l in f if l.strip()]

# Pre-load all training audio mel + raw addresses
print('Pre-loading training data...')
train_data = []  # list of (raw_word_fps_per_frame, word_label)
for i in range(1, 41):
    wav = f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav): continue
    samples, sr = read_wav_mono(wav)
    mels = compute_mel_frames(samples)
    words = detect_words(mels)
    text = sentences[i - 1].split() if i - 1 < len(sentences) else []
    
    for j, (s, e) in enumerate(words):
        if j < len(text):
            word = text[j].lower().strip('.,!?')
            wm = mels[s:e]
            # Store raw mel values for each frame (first 10 frames)
            raw_frames = []
            for f in range(min(10, len(wm))):
                raw_frames.append(mel_to_raw_addrs(wm[f]))
            train_data.append((raw_frames, word))

print(f'Loaded {len(train_data)} word samples')

# Generate layouts
layouts = {}
layouts['identity'] = np.arange(N)
np.random.seed(42)
layouts['random'] = np.random.permutation(N)
for s in [37, 79, 127]:
    layouts[f'stride{s}'] = np.arange(N) * s % N

# Pre-load test data
print('Pre-loading test data...')
test_data = []
for i in range(1, 11):
    wav = f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav): continue
    samples, sr = read_wav_mono(wav)
    mels = compute_mel_frames(samples)
    words = detect_words(mels)
    text = sentences[i - 1].split() if i - 1 < len(sentences) else []
    
    for j, (s, e) in enumerate(words):
        if j < len(text):
            wm = mels[s:e]
            raw_frames = []
            for f in range(min(10, len(wm))):
                raw_frames.append(mel_to_raw_addrs(wm[f]))
            test_data.append((raw_frames, text[j].lower().strip('.,!?')))

print(f'Loaded {len(test_data)} test word samples')

# Test each layout
for name, layout in layouts.items():
    # Build dictionary
    dict_fps = {}
    for raw_frames, word in train_data:
        addrs = []
        for raw in raw_frames:
            mapped = raw_to_addr(raw, layout)
            addrs.extend(mapped)
        fp = np.bincount(addrs, minlength=N).astype(np.float32)
        if word not in dict_fps:
            dict_fps[word] = []
        dict_fps[word].append(fp)
    
    dict_avg = {w: np.mean(np.array(fps), axis=0) for w, fps in dict_fps.items()}
    
    # Test
    correct = 0; total = 0; rejected = 0
    for raw_frames, true_word in test_data:
        addrs = []
        for raw in raw_frames:
            mapped = raw_to_addr(raw, layout)
            addrs.extend(mapped)
        fp = np.bincount(addrs, minlength=N).astype(np.float32)
        
        best_word = None; best_sim = -1
        for w2, mean_fp in dict_avg.items():
            sim = cosine(fp, mean_fp)
            if sim > best_sim:
                best_sim = sim; best_word = w2
        
        if best_sim < 0.5:
            rejected += 1
            continue
        total += 1
        if best_word == true_word:
            correct += 1
    
    acc = 100 * correct / total if total > 0 else 0
    print(f'{name:12s}: {correct}/{total} = {acc:.1f}% (rejected {rejected})')
