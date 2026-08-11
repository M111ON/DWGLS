#!/usr/bin/env python3
"""
Whisper Mel Spectrogram Extractor (v2)
Computes the exact mel spectrogram that Whisper feeds to the transformer.
Matches whisper.cpp implementation exactly:
  - Power spectrum (magnitude squared)
  - Reflective padding at start
  - Dynamic clamping: max - 8.0
  - Hann window
  - Pre-emphasis: 0.97
"""

import numpy as np
import scipy.io.wavfile as wavfile
import json
import sys
import os

def hz_to_mel(f):
    """Convert frequency in Hz to mel scale."""
    return 2595.0 * np.log10(1.0 + f / 700.0)

def mel_to_hz(m):
    """Convert mel scale to frequency in Hz."""
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

def create_mel_filterbank(n_mels=80, n_fft=400, sample_rate=16000):
    """
    Create triangular mel filterbank exactly like Whisper.
    
    Whisper uses 201 frequency bins (n_fft/2 + 1).
    Filterbank is n_mels x n_fft.
    """
    n_freqs = n_fft // 2 + 1  # 201 frequency bins
    
    # Frequency points for FFT bins
    freq_points = np.linspace(0, sample_rate / 2, n_freqs)
    
    # Mel scale boundaries
    mel_low = hz_to_mel(0)  # 0 Hz
    mel_high = hz_to_mel(sample_rate / 2)  # 8000 Hz
    
    # Create equally spaced mel points (n_mels + 2 points for triangular filters)
    mel_points = np.linspace(mel_low, mel_high, n_mels + 2)
    hz_points = mel_to_hz(mel_points)
    
    # Convert Hz points to FFT bin indices
    bin_indices = np.floor((n_fft + 1) * hz_points / sample_rate).astype(int)
    
    # Create triangular filterbank
    filterbank = np.zeros((n_mels, n_freqs))
    
    for i in range(n_mels):
        left_bin = bin_indices[i]
        center_bin = bin_indices[i + 1]
        right_bin = bin_indices[i + 2]
        
        # Rising slope
        if center_bin != left_bin:
            filterbank[i, left_bin:center_bin] = np.arange(left_bin, center_bin) - left_bin
            filterbank[i, left_bin:center_bin] /= (center_bin - left_bin)
        
        # Falling slope
        if right_bin != center_bin:
            filterbank[i, center_bin:right_bin] = right_bin - np.arange(center_bin, right_bin)
            filterbank[i, center_bin:right_bin] /= (right_bin - center_bin)
    
    return filterbank

def compute_mel_spectrogram(audio, sample_rate=16000, n_fft=400, hop_length=160, n_mels=80):
    """
    Compute mel spectrogram exactly like Whisper (whisper.cpp).
    
    Steps:
    1. Pre-emphasis filter (0.97)
    2. Reflective padding at start (n_fft/2 samples)
    3. Zero padding at end (30 seconds)
    4. Apply Hann window
    5. Compute FFT
    6. Compute POWER spectrum (magnitude squared)
    7. Apply mel filterbank
    8. Log10 + dynamic clamping (max - 8.0)
    9. Normalize: (x+4)/4
    """
    # Pre-emphasis filter (Whisper applies this)
    audio = np.append(audio[0], audio[1:] - 0.97 * audio[:-1])
    
    # Reflective padding at start (200 samples = n_fft/2)
    pad_start = n_fft // 2
    padded_start = audio[1:pad_start+1][::-1]  # Reverse of first pad_start samples
    audio_padded = np.concatenate([padded_start, audio])
    
    # Zero padding at end (30 seconds = 480000 samples)
    pad_end = sample_rate * 30
    audio_padded = np.concatenate([audio_padded, np.zeros(pad_end)])
    
    # Create Hann window
    hann_window = 0.5 * (1 - np.cos(2 * np.pi * np.arange(n_fft) / n_fft))
    
    # Create mel filterbank
    mel_filterbank = create_mel_filterbank(n_mels, n_fft, sample_rate)
    
    # Number of frames
    n_frames = (len(audio_padded) - n_fft) // hop_length
    
    # Compute mel spectrogram
    mel_spectrogram = np.zeros((n_frames, n_mels))
    
    for i in range(n_frames):
        # Extract frame
        start = i * hop_length
        frame = audio_padded[start:start + n_fft] * hann_window
        
        # Compute FFT
        fft_result = np.fft.rfft(frame)
        
        # Compute POWER spectrum (magnitude squared) - THIS IS KEY
        power = np.abs(fft_result) ** 2
        
        # Apply mel filterbank
        mel_spectrogram[i] = np.dot(mel_filterbank, power)
    
    # Apply log10 and clamp
    mel_spectrogram = np.log10(np.maximum(mel_spectrogram, 1e-10))
    
    # Dynamic clamping: find max, subtract 8.0, clamp below that
    mmax = mel_spectrogram.max()
    mel_spectrogram = np.maximum(mel_spectrogram, mmax - 8.0)
    
    # Normalize: (x + 4) / 4
    mel_spectrogram = (mel_spectrogram + 4.0) / 4.0
    
    return mel_spectrogram

def analyze_mel(mel):
    """Analyze mel spectrogram statistics."""
    print(f"\n{'='*60}")
    print("MEL SPECTROGRAM ANALYSIS")
    print(f"{'='*60}")
    
    n_frames, n_mels = mel.shape
    print(f"Shape: {n_frames} frames × {n_mels} mel bins")
    print(f"Duration: {n_frames * 0.01:.2f} seconds (at 10ms hop)")
    
    # Overall statistics
    print(f"\nOverall:")
    print(f"  Min: {mel.min():.6f}")
    print(f"  Max: {mel.max():.6f}")
    print(f"  Mean: {mel.mean():.6f}")
    print(f"  Std: {mel.std():.6f}")
    
    # Unique values (after rounding to understand discretization)
    rounded = np.round(mel, 4)
    unique_vals = np.unique(rounded)
    print(f"  Unique values (rounded to 4 decimals): {len(unique_vals)}")
    
    # Per-bin statistics
    print(f"\nPer-mel-bin statistics (first 10 and last 5):")
    print(f"  {'Bin':>4} {'Min':>8} {'Max':>8} {'Mean':>8} {'Std':>8}")
    for i in list(range(10)) + list(range(75, 80)):
        print(f"  {i:4d} {mel[:,i].min():8.4f} {mel[:,i].max():8.4f} {mel[:,i].mean():8.4f} {mel[:,i].std():8.4f}")
    
    # Distribution analysis
    print(f"\nDistribution across mel bins:")
    print(f"  Active bins (std > 0.01): {np.sum(mel.std(axis=0) > 0.01)}")
    print(f"  Active frames (any bin > min+0.1): {np.sum(np.any(mel > mel.min() + 0.1, axis=1))}")
    
    # Address space coverage
    print(f"\nAddress Space Analysis (20736 = 144×144):")
    print(f"  Total mel values: {n_frames * n_mels}")
    if n_frames * n_mels >= 20736:
        print(f"  ✓ Enough values to fill 20736 addresses")
    else:
        print(f"  ✗ Not enough values ({n_frames * n_mels} < 20736)")
    
    # Quantization potential
    print(f"\nQuantization potential:")
    for bits in [4, 8, 12, 16]:
        levels = 2 ** bits
        quantized = np.round((mel - mel.min()) / (mel.max() - mel.min()) * (levels - 1)).astype(int)
        unique_q = len(np.unique(quantized))
        print(f"  {bits:2d}-bit quantization: {unique_q} unique values out of {levels} possible")
    
    # Value distribution histogram
    print(f"\nValue distribution (rounded to 2 decimals):")
    rounded_2 = np.round(mel, 2)
    unique_2 = np.unique(rounded_2)
    print(f"  Unique values at 2 decimal places: {len(unique_2)}")
    print(f"  Sample values: {unique_2[:10]}...{unique_2[-5:]}")
    
    return {
        'shape': [n_frames, n_mels],
        'min': float(mel.min()),
        'max': float(mel.max()),
        'mean': float(mel.mean()),
        'std': float(mel.std()),
        'unique_rounded_4dec': len(np.unique(np.round(mel, 4))),
        'unique_rounded_2dec': len(np.unique(np.round(mel, 2)))
    }

def main():
    # Default audio path
    audio_path = sys.argv[1] if len(sys.argv) > 1 else "I:/DWGLS/tools/short_audio.wav"
    output_path = sys.argv[2] if len(sys.argv) > 2 else "I:/DWGLS/whisper_mel_real.json"
    
    print(f"Loading audio: {audio_path}")
    
    # Load audio
    sample_rate, audio = wavfile.read(audio_path)
    print(f"Original: {sample_rate} Hz, {len(audio)} samples, {len(audio)/sample_rate:.2f} sec")
    
    # Convert to float32 and normalize
    if audio.dtype == np.int16:
        audio = audio.astype(np.float32) / 32768.0
    elif audio.dtype == np.int32:
        audio = audio.astype(np.float32) / 2147483648.0
    
    # Resample to 16kHz if needed
    if sample_rate != 16000:
        print(f"Resampling from {sample_rate} Hz to 16000 Hz...")
        import scipy.signal
        audio = scipy.signal.resample(audio, int(len(audio) * 16000 / sample_rate))
        sample_rate = 16000
    
    print(f"Resampled: {sample_rate} Hz, {len(audio)} samples, {len(audio)/sample_rate:.2f} sec")
    
    # Compute mel spectrogram
    print(f"\nComputing mel spectrogram...")
    mel = compute_mel_spectrogram(audio, sample_rate)
    print(f"Mel spectrogram shape: {mel.shape}")
    
    # Analyze
    stats = analyze_mel(mel)
    
    # Save to JSON
    print(f"\nSaving to {output_path}...")
    output = {
        'frames': mel.tolist(),
        'shape': list(mel.shape),
        'stats': stats,
        'parameters': {
            'sample_rate': 16000,
            'n_fft': 400,
            'hop_length': 160,
            'n_mels': 80,
            'window': 'hann',
            'mel_scale': 'htk',
            'pre_emphasis': 0.97,
            'reflective_pad': True,
            'power_spectrum': True,
            'log_base': 10,
            'clamp': 'dynamic (max - 8.0)',
            'normalize': '(x+4)/4'
        }
    }
    
    with open(output_path, 'w') as f:
        json.dump(output, f)
    
    file_size = os.path.getsize(output_path) / (1024 * 1024)
    print(f"Saved: {file_size:.2f} MB")
    
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"Mel spectrogram: {mel.shape[0]} frames × {mel.shape[1]} bins")
    print(f"Duration: {mel.shape[0] * 0.01:.2f} seconds")
    print(f"Value range: [{mel.min():.4f}, {mel.max():.4f}]")
    print(f"Address space: {mel.shape[0] * mel.shape[1]} values vs 20736 needed")
    if mel.shape[0] * mel.shape[1] >= 20736:
        print(f"✓ Sufficient for 144×144 grid mapping")
    else:
        print(f"✗ Need {20736 - mel.shape[0] * mel.shape[1]} more values")

if __name__ == "__main__":
    main()
