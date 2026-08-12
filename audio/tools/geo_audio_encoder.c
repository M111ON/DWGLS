// Geometric Audio Encoder
// Maps audio to 20736 address space via mel spectrogram

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define WHISPER_SAMPLE_RATE 16000
#define WHISPER_N_FFT 400
#define WHISPER_HOP_LENGTH 160
#define WHISPER_N_MEL 80
#define GEO_ADDR_SPACE 20736
#define GEO_MEL_BINS 80
#define GEO_FRAMES_PER_BLOCK 259  // 80 × 259 = 20720 ≈ 20736

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
    printf("=== GEOMETRIC AUDIO ENCODER ===\n");
    printf("Input: %s\n", argv[1]);
    printf("Duration: %.2f sec, %d frames\n", (double)n_samples/WHISPER_SAMPLE_RATE, n_frames);
    printf("\n");
    
    // Compute mel spectrogram
    double *mel = malloc(GEO_MEL_BINS * n_frames * sizeof(double));
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
        mel_filter(fft_mag, &mel[frame * GEO_MEL_BINS], WHISPER_N_FFT / 2 + 1, GEO_MEL_BINS);
    }
    
    // Normalize
    double mmax = -1e20;
    for (int i = 0; i < GEO_MEL_BINS * n_frames; i++) {
        if (mel[i] > mmax && mel[i] < 1000) mmax = mel[i];  // skip INF
    }
    mmax -= 8.0;
    for (int i = 0; i < GEO_MEL_BINS * n_frames; i++) {
        if (mel[i] > 1000) mel[i] = mmax;  // fix INF
        if (mel[i] < mmax) mel[i] = mmax;
        mel[i] = (mel[i] + 4.0) / 4.0;
    }
    
    printf("=== MEL SPECTROGRAM ===\n");
    printf("Shape: [%d, %d] (frames × mel_bins)\n", n_frames, GEO_MEL_BINS);
    printf("Total values: %d\n", GEO_MEL_BINS * n_frames);
    printf("\n");
    
    // Map to 20736 address space
    printf("=== GEOMETRIC MAPPING ===\n");
    printf("Address space: %d (12⁴)\n", GEO_ADDR_SPACE);
    printf("Mel bins: %d\n", GEO_MEL_BINS);
    printf("Frames per block: %d\n", GEO_FRAMES_PER_BLOCK);
    printf("Block size: %d × %d = %d\n", GEO_MEL_BINS, GEO_FRAMES_PER_BLOCK, GEO_MEL_BINS * GEO_FRAMES_PER_BLOCK);
    printf("\n");
    
    // Create address space (20736 values)
    double *addr_space = calloc(GEO_ADDR_SPACE, sizeof(double));
    int *addr_count = calloc(GEO_ADDR_SPACE, sizeof(int));
    
    // Map each mel value to address
    for (int frame = 0; frame < n_frames; frame++) {
        for (int mel_bin = 0; mel_bin < GEO_MEL_BINS; mel_bin++) {
            int addr = (frame % GEO_FRAMES_PER_BLOCK) * GEO_MEL_BINS + mel_bin;
            if (addr < GEO_ADDR_SPACE) {
                addr_space[addr] += mel[frame * GEO_MEL_BINS + mel_bin];
                addr_count[addr]++;
            }
        }
    }
    
    // Average
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_count[i] > 0) {
            addr_space[i] /= addr_count[i];
        }
    }
    
    // Show address space statistics
    printf("=== ADDRESS SPACE STATISTICS ===\n");
    double min_val = 1e20, max_val = -1e20, sum = 0;
    int nonzero = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_space[i] < min_val) min_val = addr_space[i];
        if (addr_space[i] > max_val) max_val = addr_space[i];
        sum += addr_space[i];
        if (addr_space[i] > 0.01) nonzero++;
    }
    printf("Min: %.4f\n", min_val);
    printf("Max: %.4f\n", max_val);
    printf("Mean: %.4f\n", sum / GEO_ADDR_SPACE);
    printf("Nonzero addresses: %d / %d (%.1f%%)\n", nonzero, GEO_ADDR_SPACE, nonzero * 100.0 / GEO_ADDR_SPACE);
    printf("\n");
    
    // Show first few addresses
    printf("=== FIRST 20 ADDRESSES ===\n");
    printf("Addr   Value     Mel_Bin  Frame\n");
    for (int i = 0; i < 20; i++) {
        int mel_bin = i % GEO_MEL_BINS;
        int frame = i / GEO_MEL_BINS;
        printf("%5d  %8.4f  %3d      %d\n", i, addr_space[i], mel_bin, frame);
    }
    printf("\n");
    
    // Show 3 frequency bands (low/mid/high)
    printf("=== FREQUENCY BANDS ===\n");
    double low_sum = 0, mid_sum = 0, high_sum = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        int mel_bin = i % GEO_MEL_BINS;
        if (mel_bin < 27) low_sum += addr_space[i];
        else if (mel_bin < 54) mid_sum += addr_space[i];
        else high_sum += addr_space[i];
    }
    printf("Low  (bins 0-26):  %.4f\n", low_sum / 6912);
    printf("Mid  (bins 27-53): %.4f\n", mid_sum / 6912);
    printf("High (bins 54-79): %.4f\n", high_sum / 6912);
    printf("\n");
    
    // Save address space
    FILE *out = fopen("geo_audio_addr.json", "w");
    if (out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"address_space\": %d,\n", GEO_ADDR_SPACE);
        fprintf(out, "  \"mel_bins\": %d,\n", GEO_MEL_BINS);
        fprintf(out, "  \"frames\": %d,\n", n_frames);
        fprintf(out, "  \"duration_sec\": %.2f,\n", (double)n_samples/WHISPER_SAMPLE_RATE);
        fprintf(out, "  \"data\": [");
        for (int i = 0; i < GEO_ADDR_SPACE; i++) {
            if (i > 0) fprintf(out, ",");
            fprintf(out, "%.6f", addr_space[i]);
        }
        fprintf(out, "]\n}\n");
        fclose(out);
        printf("Saved to geo_audio_addr.json\n");
    }
    
    printf("\n=== SUMMARY ===\n");
    printf("Audio → Mel Spectrogram → %d Addresses\n", GEO_ADDR_SPACE);
    printf("Each address = average mel value for that position\n");
    printf("This is the GEOMETRIC AUDIO ENCODING\n");
    printf("Pattern of 20736 values = audio signature\n");
    
    free(mel); free(fft_in); free(fft_out); free(samples);
    free(addr_space); free(addr_count);
    return 0;
}
