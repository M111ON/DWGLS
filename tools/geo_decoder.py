"""Decode audio using geometric word dictionary."""
import numpy as np, wave, os, json

GEO=144; N=GEO*GEO; N_MELS=80
TTS_DIR='I:/DWGLS/tools/tts_data'

def read_wav_mono(p):
    w=wave.open(p,'r'); raw=w.readframes(w.getnframes()); sr=w.getframerate(); w.close()
    return np.frombuffer(raw,dtype=np.int16).astype(np.float32)/32768.0, sr

def compute_mel_frames(samples):
    N_FFT=400; HOP=160; nf=(len(samples)-N_FFT)//HOP
    mels=np.zeros((nf,N_MELS),dtype=np.float32)
    win=0.5*(1-np.cos(2*np.pi*np.arange(N_FFT)/N_FFT))
    for f in range(nf):
        s=f*HOP; w=samples[s:s+N_FFT]*win
        for k in range(N_MELS):
            a=2*np.pi*k*np.arange(N_FFT)/N_FFT
            r=np.sum(w*np.cos(a)); im=np.sum(w*(-np.sin(a)))
            mels[f,k]=np.log10(max(np.sqrt(r**2+im**2)/N_FFT,1e-10))
    return mels

def detect_words(mels, min_dur=5):
    energy=np.sum(mels**2,axis=1); thresh=np.percentile(energy,25)
    in_w=False; words=[]
    for i,e in enumerate(energy):
        if e>thresh and not in_w: start=i; in_w=True
        elif e<=thresh and in_w:
            if i-start>=min_dur: words.append((start,i))
            in_w=False
    if in_w and len(energy)-start>=min_dur: words.append((start,len(energy)))
    return words

def mel_to_addrs(mel):
    return [((k%GEO)*GEO+int(max(0,min(1,(mel[k]+10)/10))*(GEO-1))) for k in range(N_MELS)]

def word_fp(word_mels, layout):
    all_a=[layout[a] for m in word_mels[:10] for a in mel_to_addrs(m)]
    return np.bincount(all_a, minlength=N).astype(np.float32)

def cosine(a,b): return np.dot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b)+1e-10)

def decode_word(fp, dict_fps):
    best_word = None; best_sim = -1
    for word, samples in dict_fps.items():
        # Compare with mean of all samples
        mean_fp = np.mean(samples, axis=0)
        sim = cosine(fp, mean_fp)
        if sim > best_sim:
            best_sim = sim; best_word = word
    return best_word, best_sim

# Load dictionary
dict_data = np.load('geo_dict_fps.npy', allow_pickle=True).item()
print(f'Dictionary: {len(dict_data)} words')

# Test: decode s1 (not in dictionary building set?)
layout = np.arange(N)

# Use s1 (first sentence) for testing
wav = f'{TTS_DIR}/s1.wav'
samples, sr = read_wav_mono(wav)
mels = compute_mel_frames(samples)
words = detect_words(mels)

print(f'\nDecoding s1: {len(samples)/sr:.1f}s, {len(words)} words detected')
print(f'True text: "The quick brown fox jumps over the lazy dog"')

# Decode each word
true_text = "The quick brown fox jumps over the lazy dog".split()
decoded = []
for j, (s, e) in enumerate(words):
    wm = mels[s:e]
    fp = word_fp(wm, layout)
    word, sim = decode_word(fp, dict_data)
    decoded.append(word)
    true = true_text[j].lower() if j < len(true_text) else "?"
    match = "✓" if word == true else "✗"
    print(f'  {j:2d}: "{word:12s}" (sim={sim:.4f}) vs "{true:12s}" {match}')

# Accuracy
correct = sum(1 for d, t in zip(decoded, true_text[:len(decoded)]) if d == t.lower())
print(f'\nAccuracy: {correct}/{len(decoded)} = {100*correct/len(decoded):.1f}%')
