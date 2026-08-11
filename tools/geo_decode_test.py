"""Full decode test on multiple sentences with confidence threshold."""
import numpy as np, wave, os

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

def decode_word(fp, dict_fps, threshold=0.5):
    best_word=None; best_sim=-1
    for word,samples in dict_fps.items():
        mean_fp=np.mean(samples,axis=0)
        sim=cosine(fp,mean_fp)
        if sim>best_sim: best_sim=sim; best_word=word
    if best_sim < threshold: return "<unk>", best_sim
    return best_word, best_sim

layout=np.arange(N)
dict_data=np.load('geo_dict_fps.npy',allow_pickle=True).item()

# Test on sentences 1-10 (dictionary built from 1-40, so these are IN dict)
sentences=[]
with open('I:/DWGLS/tools/tts_sentences_50.txt') as f:
    sentences=[l.strip() for l in f if l.strip()]

total_correct=0; total_words=0
for i in range(1,11):
    wav=f'{TTS_DIR}/s{i}.wav'
    if not os.path.exists(wav): continue
    samples,sr=read_wav_mono(wav)
    mels=compute_mel_frames(samples)
    words=detect_words(mels)
    true_text=sentences[i-1].split() if i-1<len(sentences) else []
    
    decoded=[]
    for s,e in words:
        wm=mels[s:e]; fp=word_fp(wm,layout)
        w,sim=decode_word(fp,dict_data,threshold=0.3)
        decoded.append(w)
    
    # Align with true text (skip extra detections)
    correct=0
    for j,true_w in enumerate(true_text):
        if j<len(decoded) and decoded[j]==true_w.lower().strip('.,!?'):
            correct+=1
    total_correct+=correct; total_words+=len(true_text)
    acc=100*correct/len(true_text) if true_text else 0
    print(f's{i:2d}: {correct}/{len(true_text):2d} = {acc:5.1f}% | {" ".join(decoded[:8])}')

print(f'\nTOTAL: {total_correct}/{total_words} = {100*total_correct/total_words:.1f}%')
