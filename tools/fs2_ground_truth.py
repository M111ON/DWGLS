#!/usr/bin/env python3
"""fs2_ground_truth.py — FastSpeech2-LJSpeech ground truth generator
for the catching-mesh audio codec.

WHY THIS MODEL (user direction):
  - deterministic: same phonemes + pace → IDENTICAL mel every time
    (lossless roundtrip verification possible — unlike edge-tts/whisper)
  - durations[]: exact phoneme boundaries = the TIME AXIS we were missing
    ("เราไม่มีแกนกลางที่เป็นตัวกำหนดเวลา") — replaces char-count
    proportional alignment that misled v12-v21
  - pitch/energy per phoneme: the "แรงดึงก่อน ค่อยคลาย" envelope, directly
  - single speaker (LJSpeech) — no speaker-variance confound
  - pace/pitch_rate/energy_rate controls: test magnet-pull hypothesis
    (คำวิ่งเร็วๆ threshold กางออก) by slowing/speeding the SAME text

USAGE:
  python tools/fs2_ground_truth.py \
      --text "the quick brown fox jumps over the lazy dog" \
      --pace 1.0 --out tts_data/fs2_quickfox

  python tools/fs2_ground_truth.py --phrases-file tts_fs2_phrases.txt --out tts_data/

OUTPUTS (per phrase):
  <out>.mel.npy    (1, 80, T)   mel spectrogram, sr=22050, hop=256
  <out>.dur.npy    (1, n_ph)    durations per phoneme (frames)
  <out>.pitch.npy  (1, n_ph, 1)
  <out>.energy.npy (1, n_ph, 1)
  <out>.phn.txt    phoneme sequence (space separated)
  <out>.wav        waveform via HiFi-GAN vocoder (if --wav)

PHONEME SOURCE:
  --text: needs a --lexicon file (CMU-style) to map words→phonemes,
          else falls back to a built-in mini lexicon.
  --phrases-file: lines of "text<TAB>ph1 ph2 ph3 ..." (pre-phonemized)
          — most reliable, no g2p dependency.
"""
import argparse, os, sys, types, warnings

# ── stub optional speechbrain integrations (k2/g2p-wordemb) ──────────
def _mk(modname, is_pkg=False):
    m = types.ModuleType(modname)
    if is_pkg:
        m.__path__ = []
    sys.modules.setdefault(modname, m)
    return m

_mk("speechbrain.integrations.huggingface", is_pkg=True)
_mk("speechbrain.integrations.huggingface.wordemb", is_pkg=True)
_mk("speechbrain.integrations.huggingface.wordemb.util").expand_to_chars = \
    lambda emb, seq, seq_len, word_separator: emb
_mk("speechbrain.wordemb", is_pkg=True)
tr = _mk("speechbrain.wordemb.transformer")


class _TWEStub:
    def __init__(self, *a, **k): pass
    def to(self, *a, **k): return self
    def embeddings(self, txt): return txt


tr.TransformerWordEmbeddings = _TWEStub
_mk("speechbrain.integrations.k2_fsa")

# ── mini CMU-style lexicon (ARPA phonemes, matches FS2 vocab) ────────
LEXICON = {
    "the": "DH AH", "quick": "K W IH K", "brown": "B R AW N",
    "fox": "F AA K S", "jumps": "JH AH M P S", "over": "OW V ER",
    "lazy": "L EY Z IY", "dog": "D AO G", "cat": "K AE T",
    "sat": "S AE T", "on": "AA N", "mat": "M AE T",
    "and": "AE N D", "watched": "W AA CH T", "birds": "B ER D Z",
    "fly": "F L AY", "across": "AH K R AO S", "blue": "B L UW",
    "sky": "S K AY", "a": "AH", "an": "AE N", "in": "IH N",
    "of": "AH V", "to": "T UW", "is": "IH Z", "was": "W AA Z",
    "it": "IH T", "that": "DH AE T", "with": "W IH DH",
    "from": "F R AH M", "by": "B AY", "at": "AE T",
}


def text_to_phonemes(text, lexicon):
    words = text.lower().split()
    out = []
    for w in words:
        w2 = w.strip(".,!?;:'\"-()")
        if w2 in lexicon:
            out.extend(lexicon[w2].split())
        else:
            print(f"  [lexicon miss: '{w2}' → spn]", file=sys.stderr)
            out.append("spn")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default=None, help="sentence to synthesize")
    ap.add_argument("--phrases-file", default=None,
                    help="file: 'text<TAB>ph1 ph2 ...' lines")
    ap.add_argument("--out", default="tts_data/fs2_out", help="output stem")
    ap.add_argument("--pace", type=float, default=1.0)
    ap.add_argument("--pitch-rate", type=float, default=1.0)
    ap.add_argument("--energy-rate", type=float, default=1.0)
    ap.add_argument("--wav", action="store_true", help="also render wav (HiFi-GAN)")
    args = ap.parse_args()

    if args.phrases_file:
        phrases = []
        with open(args.phrases_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line.strip():
                    continue
                if "\t" in line:
                    text, ph = line.split("\t", 1)
                    phrases.append((text.strip(), ph.split()))
                else:
                    phrases.append((line.strip(), text_to_phonemes(line, LEXICON)))
    elif args.text:
        phrases = [(args.text, text_to_phonemes(args.text, LEXICON))]
    else:
        ap.error("need --text or --phrases-file")

    from speechbrain.inference.TTS import FastSpeech2
    fs2 = FastSpeech2.from_hparams(
        source="speechbrain/tts-fastspeech2-ljspeech",
        savedir="models/tts-fastspeech2-ljspeech",
    )
    vocoder = None
    if args.wav:
        from speechbrain.inference.vocoders import HIFIGAN
        vocoder = HIFIGAN.from_hparams(
            source="speechbrain/tts-hifigan-ljspeech",
            savedir="models/tts-hifigan-ljspeech",
        )

    import numpy as np

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    for i, (text, phn) in enumerate(phrases):
        stem = args.out if len(phrases) == 1 else f"{args.out}_{i:02d}"
        mel, dur, pitch, energy = fs2.encode_phoneme(
            [phn], pace=args.pace,
            pitch_rate=args.pitch_rate, energy_rate=args.energy_rate,
        )
        np.save(stem + ".mel.npy", mel.numpy())
        np.save(stem + ".dur.npy", dur.numpy())
        np.save(stem + ".pitch.npy", pitch.numpy())
        np.save(stem + ".energy.npy", energy.numpy())
        with open(stem + ".phn.txt", "w") as f:
            f.write(" ".join(phn) + "\n")
        with open(stem + ".text.txt", "w") as f:
            f.write(text + "\n")
        print(f"[{i}] {text}")
        print(f"    mel {tuple(mel.shape)} | dur {tuple(dur.shape)} "
              f"sum={dur.sum().item():.1f} frames | phn={len(phn)}")
        print(f"    saved → {stem}.{{mel.npy,dur.npy,pitch.npy,energy.npy,phn.txt}}")
        if vocoder is not None:
            wav = vocoder.decode_batch(mel)
            import torchaudio
            torchaudio.save(stem + ".wav", wav.squeeze(1), 22050)
            print(f"    wav  → {stem}.wav (22050 Hz)")


if __name__ == "__main__":
    warnings.filterwarnings("ignore")
    main()
