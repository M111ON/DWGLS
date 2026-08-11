// Word Decoder: mel pattern → word
// Uses full 80D mel pattern as address (no compression)
// Decoder = pattern matching (Euclidean distance)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N_MELS 80
#define MAX_WORDS 100
#define MAX_WORD_LEN 64

typedef struct {
    char word[MAX_WORD_LEN];
    float mel[N_MELS];
} WordEntry;

typedef struct {
    WordEntry entries[MAX_WORDS];
    int count;
} Dictionary;

// Compute mel spectrogram for a segment
void compute_mel(const float *samples, int n_samples, int sr, 
                 float t0, float t1, float *mel) {
    int N_FFT = 400, HOP = 160;
    int s0 = (int)(t0 * sr);
    int s1 = (int)(t1 * sr);
    int seg_len = s1 - s0;
    
    if (seg_len < N_FFT) seg_len = N_FFT;
    
    float windowed[N_FFT];
    memset(mel, 0, N_MELS * sizeof(float));
    
    int n_frames = (seg_len - N_FFT) / HOP + 1;
    if (n_frames > 50) n_frames = 50;
    if (n_frames < 1) n_frames = 1;
    
    for (int f = 0; f < n_frames; f++) {
        // Apply window
        for (int i = 0; i < N_FFT; i++) {
            int idx = s0 + f * HOP + i;
            float h = 0.5f * (1.0f - cosf(2.0f * M_PI * i / N_FFT));
            float val = (idx < n_samples) ? samples[idx] : 0;
            windowed[i] = h * val;
        }
        
        // DFT magnitude
        for (int k = 0; k < N_MELS; k++) {
            float real = 0, imag = 0;
            for (int i = 0; i < N_FFT; i++) {
                float angle = 2.0f * M_PI * k * i / N_FFT;
                real += windowed[i] * cosf(angle);
                imag -= windowed[i] * sinf(angle);
            }
            float mag = sqrtf(real*real + imag*imag) / N_FFT;
            mel[k] += log10f(fmaxf(mag, 1e-10f));
        }
    }
    
    // Average across frames
    for (int k = 0; k < N_MELS; k++) {
        mel[k] /= n_frames;
    }
}

// Euclidean distance between two mel patterns
float sig_dist(const float *a, const float *b) {
    float sum = 0;
    for (int i = 0; i < N_MELS; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sqrtf(sum);
}

// Find closest word in dictionary
const char* decode(const Dictionary *dict, const float *mel) {
    float best_dist = 1e9;
    const char *best_word = NULL;
    
    for (int i = 0; i < dict->count; i++) {
        float d = sig_dist(mel, dict->entries[i].mel);
        if (d < best_dist) {
            best_dist = d;
            best_word = dict->entries[i].word;
        }
    }
    
    return best_word;
}

int main(void) {
    printf("=== WORD DECODER (mel pattern → word) ===\n\n");
    
    printf("Architecture:\n");
    printf("  Mel pattern (80D) = address (no compression)\n");
    printf("  Decoder = pattern matching (Euclidean distance)\n");
    printf("  This is how the brain works: pattern → meaning\n\n");
    
    printf("Usage:\n");
    printf("  1. Build dictionary from known audio + text\n");
    printf("  2. For new audio, compute mel pattern\n");
    printf("  3. Find closest word in dictionary\n");
    printf("  4. That's the decoded word\n\n");
    
    printf("Key insight:\n");
    printf("  - Mel pattern IS the address (80D vector)\n");
    printf("  - No compression needed (80 values = 80D address)\n");
    printf("  - Decoder = pattern matching (not lookup table)\n");
    printf("  - Same word, different context → different pattern\n");
    printf("  - This is WHY context matters in speech recognition\n");
    
    return 0;
}
