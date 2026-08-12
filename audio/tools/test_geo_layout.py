"""Test geo_fingerprint with optimized layout on 40 sentences."""
import numpy as np, wave, os

GEO=144; N=GEO*GEO; N_MELS=80; TTS_DIR='I:/DWGLS/tools/tts_data'

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
    all_a=[]
    for mel in word_mels[:10]:
        all_a.extend([layout[a] for a in mel_to_addrs(mel)])
    return np.bincount(all_a, minlength=N).astype(np.float32)

def cosine(a,b): return np.dot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b)+1e-10)

# Load words
print('Loading...')
words=[]
for i in range(1,41):
    wav=f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav): continue
    samples,sr=read_wav_mono(wav)
    mels=compute_mel_frames(samples)
    for s,e in detect_words(mels):
        wm=mels[s:e]
        words.append((f's{i}_w{len(words)}',wm))

print(f'{len(words)} words loaded')

# Test layouts
np.random.seed(42)
identity=np.arange(N)
layouts={
    'identity': identity,
    'stride79': (identity*79)%N,
    'random': np.random.permutation(N)
}

for name,layout in layouts.items():
    fps=np.array([word_fp(w[1],layout) for w in words])
    # intra (same word repeated) vs inter (different words)
    # Just check random pairs separation
    n=min(200,len(words))
    sims=[]
    for _ in range(n):
        i,j=np.random.choice(len(words),2,replace=False)
        sims.append(cosine(fps[i],fps[j]))
    print(f'{name:12s}: mean_cos={np.mean(sims):.4f} std={np.std(sims):.4f}')
