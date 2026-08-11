"""Build geometric word dictionary and decode audio."""
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

layout = np.arange(N)
cosine = lambda a,b: np.dot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b)+1e-10)

# Build dictionary from known sentences
sentences = []
with open('I:/DWGLS/tools/tts_sentences_50.txt') as f:
    sentences = [l.strip() for l in f if l.strip()]

print('Building dictionary...')
dict_fps = {}  # word -> list of fingerprints

for i in range(1, 41):
    wav = f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav): continue
    samples, sr = read_wav_mono(wav)
    mels = compute_mel_frames(samples)
    words = detect_words(mels)
    text = sentences[i-1].split() if i-1 < len(sentences) else []
    
    # Align: assume words in order
    for j, (s, e) in enumerate(words):
        if j < len(text):
            word = text[j].lower().strip('.,!?')
            wm = mels[s:e]
            fp = word_fp(wm, layout)
            if word not in dict_fps:
                dict_fps[word] = []
            dict_fps[word].append(fp)

print(f'Dictionary: {len(dict_fps)} unique words')
for w, fps in sorted(dict_fps.items(), key=lambda x: -len(x[1]))[:10]:
    print(f'  "{w}": {len(fps)} samples')

# Save dictionary
np.save('geo_dict_fps.npy', {k: np.array(v) for k, v in dict_fps.items()})
print(f'\nSaved dictionary')
