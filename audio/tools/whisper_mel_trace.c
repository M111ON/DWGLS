// Whisper Mel Spectrogram Extractor - Raw Values
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define WHISPER_SAMPLE_RATE 16000
#define WHISPER_N_FFT 400
#define WHISPER_HOP_LENGTH 160
#define WHISPER_N_MEL 80

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t size; char wave[4]; } WavHeader;
typedef struct { char fmt[4]; uint32_t size; uint16_t af; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; } FmtChunk;
typedef struct { char data[4]; uint32_t size; } DataHeader;
#pragma pack(pop)

void fft(double *r, double *im, int n) {
    for (int i=1,j=0;i<n;i++) { int b=n>>1; for(;j&b;b>>=1) j^=b; j^=b;
        if(i<j){double t=r[i];r[i]=r[j];r[j]=t;t=im[i];im[i]=im[j];im[j]=t;} }
    for (int len=2;len<=n;len<<=1) {
        double a=-2*M_PI/len,wr=cos(a),wi=sin(a);
        for(int i=0;i<n;i+=len){double cr=1,ci=0;
            for(int j=0;j<len/2;j++){double ur=r[i+j],ui=im[i+j],
                vr=r[i+j+len/2]*cr-im[i+j+len/2]*ci,
                vi=r[i+j+len/2]*ci+im[i+j+len/2]*cr;
                r[i+j]=ur+vr;im[i+j]=ui+vi;
                r[i+j+len/2]=ur-vr;im[i+j+len/2]=ui-vi;
                double t=cr*wr-ci*wi;ci=cr*wi+ci*wr;cr=t;}} }
}

void mel_filter(double *fft_mag, double *mel_out, int n_fft, int n_mel) {
    double mel_high = 2595.0 * log10(1.0 + (WHISPER_SAMPLE_RATE/2) / 700.0);
    for (int m = 0; m < n_mel; m++) {
        double mel_start = mel_high * m / n_mel;
        double mel_end = mel_high * (m + 1) / n_mel;
        double freq_start = 700.0 * (pow(10.0, mel_start / 2595.0) - 1.0);
        double freq_end = 700.0 * (pow(10.0, mel_end / 2595.0) - 1.0);
        int bin_start = (int)(freq_start * n_fft / WHISPER_SAMPLE_RATE);
        int bin_end = (int)(freq_end * n_fft / WHISPER_SAMPLE_RATE);
        double sum = 0;
        for (int k = bin_start; k <= bin_end && k < n_fft; k++) {
            sum += fft_mag[k];
        }
        mel_out[m] = log10(fmax(sum, 1e-10));
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <wav>\n", argv[0]); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("Error: Cannot open %s\n", argv[1]); return 1; }
    WavHeader h; fread(&h, sizeof(h), 1, fp);
    FmtChunk f; while(1) { fread(&f, sizeof(f), 1, fp); if(memcmp(f.fmt,"fmt ",4)==0) break; fseek(fp,f.size,SEEK_CUR); }
    DataHeader d; while(1) { fread(&d, sizeof(d), 1, fp); if(memcmp(d.data,"data",4)==0) break; fseek(fp,d.size,SEEK_CUR); }
    int total_samples = d.size / (f.bps / 8);
    int16_t *raw = malloc(total_samples * sizeof(int16_t));
    fread(raw, sizeof(int16_t), total_samples, fp);
    fclose(fp);
    
    int n_samples = total_samples * WHISPER_SAMPLE_RATE / f.sr;
    int16_t *samples = malloc(n_samples * sizeof(int16_t));
    for (int i = 0; i < n_samples; i++) {
        samples[i] = raw[i * f.sr / WHISPER_SAMPLE_RATE];
    }
    free(raw);
    
    int n_frames = (n_samples - WHISPER_N_FFT) / WHISPER_HOP_LENGTH + 1;
    printf("Audio: %d samples (%.2f sec), %d frames\n", n_samples, (double)n_samples/WHISPER_SAMPLE_RATE, n_frames);
    
    // Find frame with max energy
    int best_frame = 0;
    double best_energy = 0;
    double *all_mel = malloc(WHISPER_N_MEL * n_frames * sizeof(double));
    double *fft_in = calloc(WHISPER_N_FFT * 2, sizeof(double));
    double *fft_out = malloc(WHISPER_N_FFT * 2 * sizeof(double));
    
    for (int frame = 0; frame < n_frames; frame++) {
        int offset = frame * WHISPER_HOP_LENGTH;
        for (int i = 0; i < WHISPER_N_FFT; i++) {
            double hann = 0.5 * (1.0 - cos(2.0 * M_PI * i / WHISPER_N_FFT));
            fft_in[i] = hann * samples[offset + i] / 32768.0;
        }
        fft(fft_in, fft_out, WHISPER_N_FFT);
        double fft_mag[WHISPER_N_FFT / 2 + 1];
        for (int i = 0; i < WHISPER_N_FFT / 2 + 1; i++) {
            fft_mag[i] = sqrt(fft_in[i] * fft_in[i] + fft_out[i] * fft_out[i]);
        }
        mel_filter(fft_mag, &all_mel[frame * WHISPER_N_MEL], WHISPER_N_FFT / 2 + 1, WHISPER_N_MEL);
        
        double energy = 0;
        for (int m = 0; m < WHISPER_N_MEL; m++) {
            energy += all_mel[frame * 80 + m] * all_mel[frame * 80 + m];
        }
        if (energy > best_energy) { best_energy = energy; best_frame = frame; }
    }
    
    printf("Best frame: %d (time=%.3f sec)\n", best_frame, (double)best_frame * WHISPER_HOP_LENGTH / WHISPER_SAMPLE_RATE);
    printf("\n");
    
    // Show 10 frames around best
    printf("=== MEL SPECTROGRAM (10 frames around best) ===\n");
    printf("Each frame = 80 mel frequency bins\n");
    printf("Bin 0 = lowest frequency, Bin 79 = highest\n\n");
    
    for (int f = best_frame - 5; f <= best_frame + 4; f++) {
        if (f < 0 || f >= n_frames) continue;
        printf("Frame %3d (t=%.3f): ", f, (double)f * WHISPER_HOP_LENGTH / WHISPER_SAMPLE_RATE);
        
        // Show as bar chart (compact)
        for (int m = 0; m < 80; m++) {
            double val = all_mel[f * 80 + m];
            // Simple bar: value range roughly -2 to 2
            int bars = (int)((val + 2) * 5);
            if (bars < 0) bars = 0;
            if (bars > 9) bars = 9;
            printf("%d", bars);
        }
        printf("\n");
    }
    
    printf("\n=== RAW VALUES (best frame, first 20 bins) ===\n");
    printf("Bin  Frequency(Hz)  Value\n");
    double mel_high = 2595.0 * log10(1.0 + (WHISPER_SAMPLE_RATE/2) / 700.0);
    for (int m = 0; m < 20; m++) {
        double mel = mel_high * m / WHISPER_N_MEL;
        double freq = 700.0 * (pow(10.0, mel / 2595.0) - 1.0);
        printf("%3d  %8.1f      %.4f\n", m, freq, all_mel[best_frame * 80 + m]);
    }
    
    printf("\n=== WHAT TRANSFORMER SEES ===\n");
    printf("Shape: [%d, 80] float32\n", n_frames);
    printf("Total: %d values (%.1f KB)\n", n_frames * 80, n_frames * 80 * 4.0 / 1024);
    printf("This is the ENCODER input.\n");
    printf("Decoder autoregressively generates tokens from this.\n");
    
    free(all_mel); free(fft_in); free(fft_out); free(samples);
    return 0;
}
