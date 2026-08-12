"""Build geometric word dictionary with forced alignment + silence trimming."""
import numpy as np, wave, os

GEO = 144; N = GEO * GEO; N_MELS = 80
TTS_DIR = 'I:/DWGLS/tools/tts_data'

def read_wav_mono(p):
    w = wave.open(p, 'r')
    raw = w.readframes(w.getnframes()); sr = w.getframerate(); w.close()
    return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0, sr

def compute_mel_frames(samples):
    N_FFT = 400; HOP = 160; nf = (len(samples) - N_FFT) // HOP
    mels = np.zeros((nf, N_MELS), dtype=np.float32)
    win = 0.5 * (1 - np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT))
    for f in range(nf):
        s = f * HOP
        w = samples[s:s+N_FFT] * win
        for k in range(N_MELS):
            a = 2 * np.pi * k * np.arange(N_FFT) / N_FFT
            r = np.sum(w * np.cos(a)); im = np.sum(w * (-np.sin(a)))
            mels[f, k] = np.log10(max(np.sqrt(r**2 + im**2) / N_FFT, 1e-10))
    return mels

def trim_silence(mels, thresh_percentile=15):
    """Trim silence from mel spectrogram (return trimmed mels and frame offset)."""
    energy = np.sum(mels**2, axis=1)
    thresh = np.percentile(energy, thresh_percentile)
    speech = np.where(energy > thresh)[0]
    if len(speech) == 0:
        return mels, 0
    start = speech[0]
    end = speech[-1] + 1
    return mels[start:end], start

def mel_to_addrs(mel):
    return [((k % GEO) * GEO + int(max(0, min(1, (mel[k] + 10) / 10)) * (GEO - 1)))
            for k in range(N_MELS)]

def word_fp(word_mels, layout):
    all_a = [layout[a] for m in word_mels[:10] for a in mel_to_addrs(m)]
    return np.bincount(all_a, minlength=N).astype(np.float32)

def forced_align(n_frames, n_words):
    """Force-align: split frames evenly into n_words segments."""
    words = []
    per_word = n_frames // n_words
    for i in range(n_words):
        start = i * per_word
        end = min((i + 1) * per_word, n_frames)
        if end - start >= 3:
            words.append((start, end))
    return words

def cosine(a, b):
    return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-10)

# Load sentences
with open('I:/DWGLS/tools/tts_sentences_50.txt') as f:
    sentences = [l.strip() for l in f if l.strip()]

layout = np.arange(N)

# Build dictionary with forced alignment + silence trimming
print('Building dictionary with forced alignment + silence trimming...')
dict_fps = {}

for i in range(1, 41):  # Train on s1-s40
    wav = f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav):
        continue
    samples, sr = read_wav_mono(wav)
    mels = compute_mel_frames(samples)
    
    # Trim silence
    mels_trimmed, offset = trim_silence(mels)
    
    text = sentences[i - 1] if i - 1 < len(sentences) else ''
    words = text.split()
    
    # Force-align on trimmed mels
    word_segments = forced_align(len(mels_trimmed), len(words))
    
    for j, (s, e) in enumerate(word_segments):
        if j < len(words):
            word = words[j].lower().strip('.,!?')
            wm = mels_trimmed[s:e]
            fp = word_fp(wm, layout)
            if word not in dict_fps:
                dict_fps[word] = []
            dict_fps[word].append(fp)

print(f'Dictionary: {len(dict_fps)} unique words')
for w, fps in sorted(dict_fps.items(), key=lambda x: -len(x[1]))[:10]:
    fps_arr = np.array(fps)
    mean_fp = np.mean(fps_arr, axis=0)
    sims = [cosine(fp, mean_fp) for fp in fps]
    print(f'  "{w}": {len(fps)} samples, mean_sim={np.mean(sims):.3f}')

# Save
dict_save = {k: np.array(v) for k, v in dict_fps.items()}
np.save('geo_dict_fps_v2.npy', dict_save)

# Test on held-out (s41-s50)
print('\n=== Testing on held-out sentences (s41-s50) ===')
test_correct = 0; test_total = 0
for i in range(41, 51):
    wav = f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav):
        continue
    samples, sr = read_wav_mono(wav)
    mels = compute_mel_frames(samples)
    mels_trimmed, _ = trim_silence(mels)
    
    text = sentences[i - 1] if i - 1 < len(sentences) else ''
    true_words = text.split()
    
    word_segments = forced_align(len(mels_trimmed), len(true_words))
    
    decoded = []
    for j, (s, e) in enumerate(word_segments):
        if j < len(true_words):
            wm = mels_trimmed[s:e]
            fp = word_fp(wm, layout)
            best_word = None; best_sim = -1
            for w2, fps_arr in dict_save.items():
                mean_fp = np.mean(fps_arr, axis=0)
                sim = cosine(fp, mean_fp)
                if sim > best_sim:
                    best_sim = sim; best_word = w2
            decoded.append(best_word)
    
    correct = sum(1 for d, t in zip(decoded, true_words[:len(decoded)]) if d == t.lower().strip('.,!?'))
    test_correct += correct
    test_total += len(true_words)
    print(f's{i}: {correct}/{len(true_words)} = {100*correct/len(true_words):.1f}% | {" ".join(decoded)}')

print(f'\nTotal: {test_correct}/{test_total} = {100*test_correct/test_total:.1f}%')
