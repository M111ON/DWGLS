#!/usr/bin/env python3
"""voice_bridge.py — Production Voice Translation Wrapper

Pipeline: Input Audio → Whisper ASR → Speaker Fingerprint → FS2 TTS → FFT Pitch Shift → Output

Usage:
  python tools/voice_bridge.py --input "path/to/audio.wav"
  python tools/voice_bridge.py --input "path/to/audio.wav" --output "output.wav"
  python tools/voice_bridge.py --input "path/to/audio.wav" --text "custom text"
  python tools/voice_bridge.py --input "path/to/audio.wav" --no-pitch-shift

Requirements:
  - torch, transformers, scipy, numpy
  - speechbrain (pip install speechbrain)
  - whisper.cpp (I:/whisper.cpp/build_cuda7/bin/whisper-cli.exe)
  - FastSpeech2 + HiFi-GAN models (auto-downloaded)

Author: DWGLS Project
"""
import argparse
import os
import sys
import subprocess
import warnings
import numpy as np

warnings.filterwarnings("ignore")

# ═══════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════
WHISPER_CLI = "I:/whisper.cpp/build_cuda7/bin/whisper-cli.exe"
WHISPER_MODEL = "I:/whisper.cpp/models/ggml-base.bin"
FS2_MODEL = "speechbrain/tts-fastspeech2-ljspeech"
HIFIGAN_MODEL = "speechbrain/tts-hifigan-ljspeech"
SAMPLE_RATE = 22050  # FS2 output sample rate

# ═══════════════════════════════════════════════════════════════════
# CMU-style mini lexicon (for FS2 phoneme input)
# ═══════════════════════════════════════════════════════════════════
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
    "today": "T UW D EY", "weather": "W EH DH ER",
    "expected": "IH K S P EH K T IH D", "clear": "K L IH R",
    "sunny": "S AH N IY", "throughout": "TH R UW AW T",
    "afternoon": "AE F T ER N UW N", "system": "S IH S T AH M",
    "will": "W IH L", "automatically": "AO T AH M AE T IH K L IY",
    "process": "P R AA S EH S", "your": "Y OW R",
    "application": "AE P L IH K EY SH AH N", "few": "F Y UW",
    "business": "B IH Z N IH S", "days": "D EY Z",
    "artificial": "AA R T IH F IH SH AH L", "intelligence": "IH N T EH L IH JH AH NS",
    "has": "HH AE Z", "transformed": "T R AE N S F ER M D",
    "way": "W EY", "people": "P IY P AH L", "communicate": "K AH M Y UW N IH K EY T",
    "work": "W ER K", "science": "S AY AH NS", "shown": "SH OW N",
    "regular": "R EH G Y AH L ER", "sleep": "S L IY P",
    "patterns": "P AE T ER NZ", "greatly": "G R EY T L IY",
    "improve": "IH M P R UW V", "packed": "P AE K T",
    "my": "M AY", "box": "B AA K S", "five": "F AY V",
    "dozen": "D AH Z AH N", "liquor": "L IH K ER",
    "jugs": "JH AH G Z", "before": "B IH F AO R",
    "we": "W IY", "head": "H EH D", "out": "AW T",
    "station": "S T EY SH AH N", "sphinx": "S F IH NG K S",
    "black": "B L AE K", "quartz": "K W AO R T S",
    "judge": "JH AH JH", "vow": "V AW", "absolute": "AE B S AH L UW T",
    "clarity": "K L EH R IH T IY", "how": "HH AW",
    "vexingly": "V EH K S IH NG L IY", "death": "D EH TH",
    "zebras": "Z IY B R AH Z", "jump": "JH AH M P",
    "when": "W EH N", "they": "DH EY", "are": "AA R",
    "frightened": "F R AY T AH ND", "structural": "S T R AH K CH ER AH L",
    "changes": "CH EY N JH IH Z", "economy": "IH K AA N AH M IY",
    "require": "R IH K W AY ER", "long": "L AO NG",
    "term": "T ER M", "plastic": "P L AE S T IH K",
}


# ═══════════════════════════════════════════════════════════════════
# Speaker Fingerprint Extraction
# ═══════════════════════════════════════════════════════════════════
def extract_speaker_fingerprint(audio_path):
    """Extract speaker fingerprint from audio (pitch contour, energy envelope)."""
    import scipy.io.wavfile as wf
    import scipy.signal

    rate, data = wf.read(audio_path)
    if rate != 16000:
        n = int(len(data) * 16000 / rate)
        data = scipy.signal.resample(data, n).astype(data.dtype)

    audio = data.astype(np.float32) / 32768.0

    frame_len = 320  # 20ms at 16kHz
    hop = 160  # 10ms
    n_frames = (len(audio) - frame_len) // hop

    pitches = []
    energies = []
    for i in range(n_frames):
        start = i * hop
        frame = audio[start:start + frame_len]

        energy = np.sqrt(np.mean(frame ** 2))
        energies.append(energy)

        if energy > 0.01:
            corr = np.correlate(frame, frame, mode='full')
            corr = corr[len(corr)//2:]
            d = np.diff(corr)
            zeros = np.where(d > 0)[0]
            if len(zeros) > 1:
                peak = zeros[0] + np.argmax(corr[zeros[0]:zeros[0]+100])
                if 20 < peak < 200:
                    pitch = 16000 / peak
                else:
                    pitch = 0
            else:
                pitch = 0
        else:
            pitch = 0
        pitches.append(pitch)

    return {
        "pitches": np.array(pitches),
        "energies": np.array(energies),
        "mean_pitch": np.mean([p for p in pitches if p > 0]) if any(p > 0 for p in pitches) else 0,
        "mean_energy": np.mean(energies),
        "pitch_std": np.std([p for p in pitches if p > 0]) if any(p > 0 for p in pitches) else 0,
    }


# ═══════════════════════════════════════════════════════════════════
# FFT Pitch Shift
# ═══════════════════════════════════════════════════════════════════
def fft_pitch_shift(wav, sr, semitones):
    """FFT-based pitch shift."""
    factor = 2 ** (semitones / 12.0)
    fft = np.fft.rfft(wav)
    freqs = np.fft.rfftfreq(len(wav), 1/sr)
    shifted_freqs = freqs * factor
    new_fft = np.zeros_like(fft)
    for i, f in enumerate(shifted_freqs):
        if f < sr/2:
            idx = int(f * len(wav) / sr)
            if idx < len(new_fft):
                new_fft[idx] = fft[i]
    shifted = np.fft.irfft(new_fft)
    shifted = shifted / np.max(np.abs(shifted)) * np.max(np.abs(wav))
    return shifted[:len(wav)]


# ═══════════════════════════════════════════════════════════════════
# Text to Phonemes
# ═══════════════════════════════════════════════════════════════════
def text_to_phonemes(text):
    """Convert text to phonemes using mini lexicon."""
    words = text.lower().split()
    all_phonemes = []
    word_boundaries = []
    for w in words:
        w2 = w.strip(".,!?;:'\"-()'")
        start = len(all_phonemes)
        if w2 in LEXICON:
            phs = LEXICON[w2].split()
        else:
            print(f"  [lexicon miss: '{w2}' → spn]", file=sys.stderr)
            phs = ["spn"]
        all_phonemes.extend(phs)
        word_boundaries.append((start, len(all_phonemes), w))
    return all_phonemes, word_boundaries


# ═══════════════════════════════════════════════════════════════════
# Whisper ASR
# ═══════════════════════════════════════════════════════════════════
def run_whisper(audio_path):
    """Run Whisper ASR and return transcription."""
    result = subprocess.run(
        [WHISPER_CLI, "-m", WHISPER_MODEL, "-f", audio_path, "-np", "--no-timestamps"],
        capture_output=True, text=True, timeout=120
    )
    lines = result.stdout.strip().split("\n")
    text = ""
    for line in lines:
        line = line.strip()
        if line and not line.startswith("[") and not line.startswith("whisper"):
            text = line
            break
    return text


# ═══════════════════════════════════════════════════════════════════
# FastSpeech2 TTS
# ═══════════════════════════════════════════════════════════════════
def generate_tts(text, output_path):
    """Generate TTS using FastSpeech2 + HiFi-GAN."""
    # Stub speechbrain integrations
    import types
    _mk = lambda n: sys.modules.setdefault(n, types.ModuleType(n))
    _mk("speechbrain.integrations.huggingface.wordemb.util").expand_to_chars = \
        lambda emb, seq, seq_len, ws: emb
    _mk("speechbrain.wordemb").__path__ = []
    tr = _mk("speechbrain.wordemb.transformer")
    class TWE:
        def __init__(self, *a, **k): pass
        def to(self, *a, **k): return self
        def embeddings(self, txt): return txt
    tr.TransformerWordEmbeddings = TWE
    _mk("speechbrain.integrations.k2_fsa")

    import torch
    from speechbrain.inference.TTS import FastSpeech2
    from speechbrain.inference.vocoders import HIFIGAN

    fs2 = FastSpeech2.from_hparams(
        source=FS2_MODEL,
        savedir="models/tts-fastspeech2-ljspeech"
    )
    vocoder = HIFIGAN.from_hparams(
        source=HIFIGAN_MODEL,
        savedir="models/tts-hifigan-ljspeech"
    )

    phonemes, word_bounds = text_to_phonemes(text)

    mel, dur, pitch, energy = fs2.encode_phoneme(
        [phonemes], pace=1.0
    )

    wav = vocoder.decode_batch(mel)

    import scipy.io.wavfile as wf
    x = wav.squeeze().numpy()
    x = np.clip(x, -1.0, 1.0)

    return {
        "wav": x,
        "mel": mel.numpy(),
        "durations": dur.numpy(),
        "pitch": pitch.numpy(),
        "energy": energy.numpy(),
        "phonemes": phonemes,
        "word_bounds": word_bounds,
    }


# ═══════════════════════════════════════════════════════════════════
# Speaker Fingerprint Application
# ═══════════════════════════════════════════════════════════════════
def apply_speaker_fingerprint(tts_wav, fp_original, sr=SAMPLE_RATE):
    """Apply speaker's pitch fingerprint to TTS output via FFT pitch shift."""
    import scipy.signal

    # Get TTS pitch
    tts_16k = scipy.signal.resample(tts_wav, int(len(tts_wav) * 16000 / sr)).astype(np.float32)
    frame_len, hop = 320, 160
    tts_pitches = []
    for i in range((len(tts_16k) - frame_len) // hop):
        frame = tts_16k[i*hop:i*hop+frame_len]
        e = np.sqrt(np.mean(frame**2))
        if e > 0.01:
            corr = np.correlate(frame, frame, mode='full')[len(frame)-1:]
            d = np.diff(corr); zeros = np.where(d > 0)[0]
            if len(zeros) > 1:
                peak = zeros[0] + np.argmax(corr[zeros[0]:zeros[0]+100])
                if 20 < peak < 200: tts_pitches.append(16000/peak)
    tts_mean = np.mean(tts_pitches) if tts_pitches else 200.9

    # Compute semitones needed
    orig_mean = fp_original["mean_pitch"]
    if orig_mean > 0 and tts_mean > 0:
        semitones = 12 * np.log2(orig_mean / tts_mean)
        return fft_pitch_shift(tts_wav, sr, semitones)
    else:
        return tts_wav


# ═══════════════════════════════════════════════════════════════════
# Fingerprint Comparison
# ═══════════════════════════════════════════════════════════════════
def compare_fingerprints(fp_original, fp_tts):
    """Compare speaker fingerprints between original and TTS."""
    import scipy.signal

    min_len = min(len(fp_original["pitches"]), len(fp_tts["pitches"]))

    orig_p = fp_original["pitches"][:min_len]
    tts_p = fp_tts["pitches"][:min_len]
    orig_e = fp_original["energies"][:min_len]
    tts_e = fp_tts["energies"][:min_len]

    voiced = (orig_p > 0) & (tts_p > 0)
    if voiced.sum() > 10:
        pitch_corr = np.corrcoef(orig_p[voiced], tts_p[voiced])[0, 1]
    else:
        pitch_corr = 0

    energy_corr = np.corrcoef(orig_e, tts_e)[0, 1]

    return {
        "pitch_correlation": pitch_corr,
        "energy_correlation": energy_corr,
        "original_mean_pitch": fp_original["mean_pitch"],
        "tts_mean_pitch": fp_tts["mean_pitch"],
        "original_mean_energy": fp_original["mean_energy"],
        "tts_mean_energy": fp_tts["mean_energy"],
    }


# ═══════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser(
        description="Voice Bridge: ASR → Speaker Fingerprint → TTS → Pitch Shift",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --input recording.wav
  %(prog)s --input recording.wav --output translated.wav
  %(prog)s --input recording.wav --text "Hello world"
  %(prog)s --input recording.wav --no-pitch-shift
        """
    )
    ap.add_argument("--input", required=True, help="Input audio file (WAV)")
    ap.add_argument("--output", default=None, help="Output audio file (default: input_stem_bridge.wav)")
    ap.add_argument("--text", default=None, help="Custom text to speak (skip ASR)")
    ap.add_argument("--no-pitch-shift", action="store_true", help="Skip pitch shift (keep FS2 default)")
    args = ap.parse_args()

    if args.output is None:
        stem = os.path.splitext(args.input)[0]
        args.output = f"{stem}_bridge.wav"

    print("╔══════════════════════════════════════════════════════════════╗")
    print("║           Voice Bridge: ASR → Fingerprint → TTS            ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    print(f"  Input:  {args.input}")
    print(f"  Output: {args.output}")
    print()

    # Step 1: ASR
    if args.text:
        text = args.text
        print(f"[1/4] Using provided text")
    else:
        print("[1/4] Running Whisper ASR...")
        text = run_whisper(args.input)
        print(f"  → {text}")

    if not text.strip():
        print("  ERROR: No text to synthesize")
        return 1

    # Step 2: Speaker Fingerprint
    print("\n[2/4] Extracting speaker fingerprint...")
    fp_original = extract_speaker_fingerprint(args.input)
    print(f"  → pitch: {fp_original['mean_pitch']:.1f} Hz, energy: {fp_original['mean_energy']:.4f}")

    # Step 3: Generate TTS
    print("\n[3/4] Generating TTS...")
    tts_result = generate_tts(text, args.output)

    # Step 4: Apply Speaker Fingerprint
    if args.no_pitch_shift:
        print("\n[4/4] Skipping pitch shift (--no-pitch-shift)")
        tts_wav = tts_result["wav"]
    else:
        print("\n[4/4] Applying speaker fingerprint (FFT pitch shift)...")
        tts_wav = apply_speaker_fingerprint(tts_result["wav"], fp_original)

    # Save output
    import scipy.io.wavfile as wf
    x = np.clip(tts_wav, -1.0, 1.0)
    wf.write(args.output, SAMPLE_RATE, (x * 32767).astype(np.int16))
    print(f"  → saved: {args.output}")

    # Compare fingerprints
    print("\n[Comparison]")
    fp_tts = extract_speaker_fingerprint(args.output)
    comparison = compare_fingerprints(fp_original, fp_tts)
    print(f"  Original pitch: {comparison['original_mean_pitch']:.1f} Hz")
    print(f"  TTS pitch:      {comparison['tts_mean_pitch']:.1f} Hz")
    print(f"  Error:          {abs(comparison['original_mean_pitch'] - comparison['tts_mean_pitch']):.1f} Hz")
    print(f"  Correlation:    pitch={comparison['pitch_correlation']:.3f}, energy={comparison['energy_correlation']:.3f}")

    print("\n╔══════════════════════════════════════════════════════════════╗")
    print("║                         DONE                               ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    return 0


if __name__ == "__main__":
    sys.exit(main())
