/* tools/two_gap_fill.c — ทดสอบแนวคิด two-gap fill
 * ═══════════════════════════════════════════════════════════════════════════
 * แนวคิด: deterministic field + log เติม transform gap (ฟรี) + residual เติม
 * detail gap (จ่าย entropy) → อ่านที่ scale ใดก็ lossless
 *
 * การทดสอบ:
 *   วาง block ที่ w₀ (เต็ม 16-bit) → ขยับไป w₁ (coarser = downsample 2×,
 *   deterministic — เทียบเท่า transform ที่ฟรีใน geometry + log 5B)
 *   → predict (upsample ซ้ำ) → residual = orig − predict
 *   → reconstruct = predict + residual — ต้อง lossless เสมอ (by construction)
 *   → วัด fill: (n/2)×H(coarse) + n×H(residual) + log overhead vs n×16 raw
 *
 * สัญญาณ 3 แบบ: sine (smooth — residual ควรเล็ก) / random (residual เต็ม) /
 * wav จริง (ถ้าส่ง path มา — ตัวเลขจริง)
 *
 * BUILD: gcc -O2 -Wall -I. -o build/two_gap_fill tools/two_gap_fill.c -lm
 * RUN:   ./build/two_gap_fill [wav]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BINS 65536
#define LOG_BITS 40.0      /* GhostLogEntry 5B = transform route (from→to) */

static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

/* down/up sample: deterministic transform (avg-pair → repeat) */
static void probe_signal(const char *name, const int16_t *s, uint64_t n) {
    uint64_t nc = n / 2;
    int16_t *coarse = (int16_t *)malloc(nc * 2);
    int16_t *resid  = (int16_t *)malloc(n * 2);
    uint64_t *h_c   = (uint64_t *)calloc(BINS, 8);
    uint64_t *h_r   = (uint64_t *)calloc(BINS, 8);
    uint64_t *h_rd  = (uint64_t *)calloc(BINS, 8);
    uint64_t *h_raw = (uint64_t *)calloc(BINS, 8);

    for (uint64_t i = 0; i < nc; i++)
        coarse[i] = (int16_t)(((int32_t)s[2 * i] + (int32_t)s[2 * i + 1] + 1) >> 1);
    for (uint64_t j = 0; j < n; j++) {
        int16_t pred = coarse[j >> 1];            /* upsample = repeat */
        resid[j] = (int16_t)((int32_t)s[j] - (int32_t)pred);
    }
    /* lossless verify: pred + resid == orig ทุกจุด */
    int ok = 1;
    for (uint64_t j = 0; j < n; j++)
        if ((int32_t)coarse[j >> 1] + (int32_t)resid[j] != (int32_t)s[j]) { ok = 0; break; }

    for (uint64_t i = 0; i < nc; i++) h_c[(uint16_t)coarse[i]]++;
    for (uint64_t j = 0; j < n; j++) h_r[(uint16_t)resid[j]]++;
    for (uint64_t j = 1; j < n; j++)
        h_rd[(uint16_t)(resid[j] - resid[j - 1])]++;
    for (uint64_t j = 0; j < n; j++) h_raw[(uint16_t)s[j]]++;

    double hc   = entropy_of(h_c, BINS, nc);
    double hr   = entropy_of(h_r, BINS, n);
    double hrd  = entropy_of(h_rd, BINS, n - 1);
    double hraw = entropy_of(h_raw, BINS, n);

    double fill_res = (double)nc * hc + (double)n * hr + LOG_BITS;
    double fill_rd  = (double)nc * hc + (double)n * hrd + LOG_BITS;
    double raw_bits = (double)n * 16.0;

    printf("  %-14s n=%6llu | H(raw)=%5.2f | H(coarse)=%5.2f | H(res)=%5.2f | H(res-delta)=%5.2f\n",
           name, (unsigned long long)n, hraw, hc, hr, hrd);
    printf("    lossless: %s | fill=%.1f b/v (%.2f×) | res-delta: %.1f (%.2f×) | entropy-raw: %.1f (%.2f×)\n",
           ok ? "YES" : "NO",
           fill_res / (double)n, raw_bits / fill_res,
           fill_rd / (double)n, raw_bits / fill_rd,
           hraw, 16.0 / hraw);

    free(coarse); free(resid); free(h_c); free(h_r); free(h_rd); free(h_raw);
}

/* ── wav reader (PCM16, ช่องแรก) — เดียวกับ wave_delta_probe ── */
static long read_wav_pcm(const char *path, int16_t **out, uint64_t *n_out, uint64_t cap) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        { fclose(fp); return -2; }
    uint32_t channels = 1;
    int found_data = 0;
    long data_off = 0; uint32_t data_sz = 0;
    while (1) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) break;
        uint32_t sz; memcpy(&sz, ch + 4, 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (fread(fb, 1, 16, fp) != 16) break;
            uint16_t af, chans, bits;
            memcpy(&af, fb, 2); memcpy(&chans, fb + 2, 2); memcpy(&bits, fb + 14, 2);
            if (af != 1 || bits != 16) { fclose(fp); return -3; }
            channels = chans;
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) { data_off = ftell(fp); data_sz = sz; found_data = 1; break; }
        else fseek(fp, sz, SEEK_CUR);
    }
    if (!found_data) { fclose(fp); return -4; }
    uint64_t want = data_sz / 2;
    if (want > cap) want = cap;
    int16_t *buf = (int16_t *)malloc((size_t)want * 2);
    if (!buf) { fclose(fp); return -5; }
    fseek(fp, data_off, SEEK_SET);
    if (fread(buf, 2, (size_t)want, fp) != want) { free(buf); fclose(fp); return -6; }
    fclose(fp);
    uint64_t mono_n = want / channels;
    int16_t *mono = (int16_t *)malloc(mono_n * 2);
    for (uint64_t i = 0; i < mono_n; i++) mono[i] = buf[i * channels];
    free(buf);
    *out = mono; *n_out = mono_n;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Two-gap fill — deterministic transform (free) + residual (detail gap)\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    /* 1. sine (smooth) — residual ควรเล็ก */
    {
        uint64_t n = 131072;
        int16_t *s = (int16_t *)malloc(n * 2);
        for (uint64_t i = 0; i < n; i++)
            s[i] = (int16_t)(30000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / 44100.0));
        probe_signal("sine(440Hz)", s, n);
        free(s);
    }
    /* 2. sine low-freq (2 Hz, full amp) — residual ควร ≈ 0 (smooth จริง) */
    {
        uint64_t n = 131072;
        int16_t *s = (int16_t *)malloc(n * 2);
        for (uint64_t i = 0; i < n; i++)
            s[i] = (int16_t)(30000.0 * sin(2.0 * 3.14159265358979 * 2.0 * i / 44100.0));
        probe_signal("sine(2Hz)", s, n);
        free(s);
    }
    /* 3. random — residual เต็ม entropy (boundary) */
    {
        uint64_t n = 131072;
        int16_t *s = (int16_t *)malloc(n * 2);
        uint64_t seed = 987654321;
        for (uint64_t i = 0; i < n; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            s[i] = (int16_t)(seed >> 48);
        }
        probe_signal("random", s, n);
        free(s);
    }
    /* 4. wav จริง (optional) */
    if (argc > 1) {
        int16_t *s = NULL; uint64_t n = 0;
        if (read_wav_pcm(argv[1], &s, &n, 262144) == 0) {
            probe_signal("real-wav", s, n);
            free(s);
        } else {
            printf("  (wav อ่านไม่ได้: %s)\n", argv[1]);
        }
    }
    printf("══════════════════════════════════════════════════════════════════\n");
    return 0;
}
