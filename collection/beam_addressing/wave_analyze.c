/*
 * wave_analyze.c — Waveform analysis of real Q8_0 weights
 * ═══════════════════════════════════════════════════════════════════
 *
 * ปล่อย weights ให้ PUSH เส้นบน angular map (360°)
 * วัดรัศมีวนรอบแกน → เกิด waveform
 * แล้ววัดว่า waveform มี pattern พอไหมสำหรับ compression
 *
 * Build: gcc -O2 -std=c11 -D_GNU_SOURCE beam_addressing/wave_analyze.c -o beam_addressing/wave_analyze.exe -lm
 * Usage: wave_analyze.exe model.gguf [n_blocks]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "gguf_reader.h"

#define ANGLES 360

/* ═══════════════════════════════════════════════════════════════════ */
/*  Waveform: radius[360] — weight values mapped to angles            */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * 32 weights → 360 angles
 * Each weight occupies a sector: 360/32 = 11.25° per weight
 * Radius at angle θ = weight value at that sector
 */
static void weights_to_waveform(float radius[ANGLES], const int8_t w[32]) {
    memset(radius, 0, sizeof(float) * ANGLES);
    for (int i = 0; i < 32; i++) {
        int start = (int)(i * ANGLES / 32.0);
        int end   = (int)((i + 1) * ANGLES / 32.0);
        float val = (float)w[i];
        for (int a = start; a < end && a < ANGLES; a++) {
            radius[a] = val;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Peak detection — count local maxima on the waveform               */
/* ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int angle;
    float value;
} Peak;

static int find_peaks(const float r[ANGLES], Peak peaks[64]) {
    int n = 0;
    for (int i = 0; i < ANGLES && n < 64; i++) {
        int prev = (i - 1 + ANGLES) % ANGLES;
        int next = (i + 1) % ANGLES;
        if (r[i] > r[prev] && r[i] > r[next]) {
            peaks[n].angle = i;
            peaks[n].value = r[i];
            n++;
        }
    }
    return n;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Fourier descriptor — compress waveform shape                       */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * DFT of radius[360] → complex coefficients[181]
 * Keep only first K coefficients → reconstruct approximate waveform
 * This is the "waveform signature" — K complex numbers vs 32 int8 weights
 */
static void dft_radius(const float r[ANGLES], float *re_out, float *im_out, int K) {
    for (int k = 0; k < K; k++) {
        float re = 0, im = 0;
        for (int n = 0; n < ANGLES; n++) {
            float angle = 2.0f * M_PI * k * n / ANGLES;
            re += r[n] * cosf(angle);
            im -= r[n] * sinf(angle);
        }
        re_out[k] = re / ANGLES;
        im_out[k] = im / ANGLES;
    }
}

static void idft_radius(float r[ANGLES], const float *re_in, const float *im_in, int K) {
    for (int n = 0; n < ANGLES; n++) {
        float val = re_in[0]; /* DC component */
        for (int k = 1; k < K; k++) {
            float angle = 2.0f * M_PI * k * n / ANGLES;
            val += 2.0f * (re_in[k] * cosf(angle) - im_in[k] * sinf(angle));
        }
        r[n] = val;
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  ASCII waveform printer                                            */
/* ═══════════════════════════════════════════════════════════════════ */

static void print_waveform(const float r[ANGLES], const char *label) {
    int width = 72; /* characters wide */
    float max_r = 0;
    for (int i = 0; i < ANGLES; i++) {
        float a = fabsf(r[i]);
        if (a > max_r) max_r = a;
    }
    if (max_r == 0) max_r = 1;

    printf("  %s (max=%.0f):\n", label, max_r);
    printf("  ");
    for (int i = 0; i < width; i++) {
        int angle = (int)((double)i / width * ANGLES);
        float norm = r[angle] / max_r;
        int h = (int)((norm + 1.0f) / 2.0f * 16); /* map -1..1 → 0..16 */
        if (h < 0) h = 0; if (h > 15) h = 15;
        printf("%c", " .:-=+*#%@"[h]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Main                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "model/smolVLM-256M-Instruct-text.Q8_0.gguf";
    int max_blocks = argc > 2 ? atoi(argv[2]) : 20;

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open %s\n", path); return 1; }

    /* Find first Q8_0 tensor */
    int idx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { idx = (int)i; break; }
    }
    if (idx < 0) { printf("No Q8_0 tensor\n"); gguf_close(gf); return 1; }

    GGUF_Tensor *t = &gf->tensors[idx];
    printf("Tensor: %s\n", t->name);

    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Waveform Analysis: weights → angular map → radius pattern\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* Statistics */
    int total_peaks = 0;
    int total_blocks = 0;

    /* Fourier analysis: how many coefficients needed? */
    float err_sum[33]; /* error for K=1..32 coefficients */
    memset(err_sum, 0, sizeof(err_sum));

    for (int b = 0; b < max_blocks; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        /* Create waveform */
        float radius[ANGLES];
        weights_to_waveform(radius, w);

        /* Find peaks */
        Peak peaks[64];
        int n_peaks = find_peaks(radius, peaks);

        printf("─── Block %d (scale=0x%04x) ───\n", b, scale);
        printf("  weights: ");
        for (int i = 0; i < 32; i++) printf("%4d", w[i]);
        printf("\n");

        /* Print waveform */
        print_waveform(radius, "Raw waveform");

        printf("  peaks: %d (on 360° circle)\n", n_peaks);
        total_peaks += n_peaks;
        total_blocks++;

        /* Fourier reconstruction error for different K */
        float orig[ANGLES];
        memcpy(orig, radius, sizeof(float) * ANGLES);

        for (int K = 1; K <= 32; K++) {
            float re[33] = {0}, im[33] = {0};
            float recon[ANGLES] = {0};
            dft_radius(orig, re, im, K);
            idft_radius(recon, re, im, K);

            float err = 0;
            for (int a = 0; a < ANGLES; a++) {
                float diff = orig[a] - recon[a];
                err += diff * diff;
            }
            err_sum[K] += err / ANGLES; /* MSE */
        }

        printf("\n");
    }

    /* ═══ Summary ═══ */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  SUMMARY: %d blocks analyzed\n", total_blocks);
    printf("  Average peaks per block: %.1f / 360 angles\n",
        (float)total_peaks / total_blocks);

    printf("\n  Fourier Reconstruction Error (MSE) vs Coefficients:\n");
    printf("  K coeff → bits/weight → MSE → quality\n");
    for (int K = 1; K <= 32; K++) {
        float mse = err_sum[K] / total_blocks;
        /* K complex coefficients = 2K floats = 2K × 16 bits for fp16 */
        float bits_per_weight = (2.0f * K * 16.0f) / 32.0f;
        const char *quality;
        if (mse < 1.0f) quality = "★★★ excellent";
        else if (mse < 10.0f) quality = "★★☆ good";
        else if (mse < 100.0f) quality = "★☆☆ fair";
        else quality = "☆☆☆ poor";

        printf("  K=%2d  →  %5.1f bits/w  →  MSE=%8.1f  →  %s\n",
            K, bits_per_weight, mse, quality);
    }

    /* Show reconstruction for K=8 (1.0 bits/weight) */
    printf("\n  ═══ Reconstruction demo (K=8 coefficients = 1.0 bits/weight) ═══\n");
    fseek(gf->fp, (long)data_start, SEEK_SET);
    for (int b = 0; b < 3 && b < max_blocks; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;

        float radius[ANGLES];
        weights_to_waveform(radius, w);

        float re[33] = {0}, im[33] = {0};
        float recon[ANGLES] = {0};
        dft_radius(radius, re, im, 8);
        idft_radius(recon, re, im, 8);

        printf("\n  Block %d:\n", b);
        print_waveform(radius, "Original");
        print_waveform(recon, "Reconstructed (K=8)");

        /* Compare weights */
        printf("  Original vs reconstructed weights:\n");
        printf("    orig: ");
        for (int i = 0; i < 32; i++) printf("%4d", w[i]);
        printf("\n    reco: ");
        for (int i = 0; i < 32; i++) {
            int sector_start = (int)(i * ANGLES / 32.0);
            int sector_end   = (int)((i + 1) * ANGLES / 32.0);
            float avg = 0;
            for (int a = sector_start; a < sector_end; a++) avg += recon[a];
            avg /= (sector_end - sector_start);
            printf("%4d", (int)roundf(avg));
        }
        printf("\n");
    }

    gguf_close(gf);
    return 0;
}
