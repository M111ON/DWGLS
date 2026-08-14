/*
 * hex_codec_full.c — Full Encode/Decode: weight → Dual Square → Hexagon
 * ═══════════════════════════════════════════════════════════════════
 *
 * Pipeline:
 *   weight w → calibrate zero_ref → d = |w - ref|
 *   → k = floor(d / R_wrap), d_remain = d - k*R_wrap
 *   → θ = (d_remain × rate) mod 360  (sawtooth → sector 0..5)
 *   → φ = (d_remain × rate_phi) mod 360
 *   → encode: layer|k|sector|phi|within|mag = 32-bit delta
 *   → decode: reconstruct d from k + θ → w
 *
 * Compile: gcc -O2 -I. hex_codec_full.c -o hex_codec_full.exe -lm
 * Run: ./hex_codec_full.exe [model.gguf]
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gguf_reader.h"

#define SQ_RES      360u
#define SECTOR_DIV  60u      /* 360/60 = 6 */

/* ── Dual Square XOR ── */
static uint8_t xor_magnitude(uint16_t t, uint16_t p) {
    uint16_t m = t ^ p;
    m = m ^ (m>>3) ^ (m>>5) ^ (m>>7);
    return (uint8_t)(m & 0xFF);
}

/* ── Codec Config ── */
typedef struct {
    double zero_ref;
    double rate;       /* sawtooth rotation rate */
    double rate_phi;   /* secondary rate for φ */
} CodecConfig;

static CodecConfig codec_default(void) {
    CodecConfig c;
    c.zero_ref = 0.0;
    c.rate = 97.0;
    c.rate_phi = 67.9;  /* another prime-ish */
    return c;
}

static CodecConfig codec_calibrate(const int8_t *w, uint64_t n) {
    CodecConfig c = codec_default();
    int64_t s = 0;
    uint64_t m = (n > 100000) ? 100000 : n;
    for (uint64_t i = 0; i < m; i++) s += w[i];
    c.zero_ref = (double)s / (double)m;
    return c;
}

/* ── Encode: weight → DualHex ── */
typedef struct {
    uint32_t k;         /* wrap count */
    int      sector;    /* 0..5 */
    double   within;    /* 0..1 */
    uint8_t  phi_q;     /* φ quantized 0..127 */
    uint8_t  mag;       /* XOR magnitude */
    int      layer;     /* 0=outer/+, 1=inner/- */
} DualHex;

static DualHex encode_weight(const CodecConfig *cfg, double w) {
    DualHex dh;
    double d = fabs(w - cfg->zero_ref);
    double R_wrap = 360.0 / cfg->rate;
    dh.k = (uint32_t)(d / R_wrap);
    double rem = d - (double)dh.k * R_wrap;
    double theta = fmod(rem * cfg->rate, 360.0);
    if (theta < 0) theta += 360.0;
    uint16_t th = (uint16_t)theta;
    double phi = fmod(rem * cfg->rate_phi, 360.0);
    if (phi < 0) phi += 360.0;
    uint16_t ph = (uint16_t)phi;

    dh.sector = (int)(th / SECTOR_DIV);
    if (dh.sector > 5) dh.sector = 5;
    dh.within = (double)(th % SECTOR_DIV) / (double)SECTOR_DIV;
    dh.phi_q = (uint8_t)(ph * 127 / 359);
    dh.mag = xor_magnitude(th, ph);
    dh.layer = (w - cfg->zero_ref >= 0) ? 0 : 1;
    return dh;
}

/* ── Pack: DualHex → 32-bit delta ──
 *   [k:8][sector:3][phi:7][within:6][layer:1][mag:7] = 32
 */
static uint32_t pack_delta(const DualHex *dh) {
    uint32_t d = 0;
    d |= (dh->k & 0xFF) << 24;
    d |= (dh->sector & 0x7) << 21;
    d |= (dh->phi_q & 0x7F) << 14;
    d |= ((int)(dh->within * 63) & 0x3F) << 8;
    d |= ((dh->layer & 1) << 7) | (dh->mag & 0x7F);
    return d;
}

/* ── Unpack: 32-bit delta → DualHex ── */
static DualHex unpack_delta(uint32_t d) {
    DualHex dh;
    dh.k = (d >> 24) & 0xFF;
    dh.sector = (int)((d >> 21) & 0x7);
    dh.phi_q = (uint8_t)((d >> 14) & 0x7F);
    dh.within = (double)((d >> 8) & 0x3F) / 63.0;
    dh.layer = (int)((d >> 7) & 1);
    dh.mag = (uint8_t)(d & 0x7F);
    return dh;
}

/* ── Decode: delta → approximate weight ── */
static double decode_weight(const CodecConfig *cfg, uint32_t delta) {
    DualHex dh = unpack_delta(delta);
    double theta_approx = ((double)dh.sector + dh.within) * SECTOR_DIV;
    double R_wrap = 360.0 / cfg->rate;
    double d = (double)dh.k * R_wrap + theta_approx / cfg->rate;
    return cfg->zero_ref + (dh.layer == 0 ? d : -d);
}

/* ── Read Q8_0 weights ── */
static uint64_t read_w(GGUF_File *gf, int ti, int8_t **out) {
    GGUF_Tensor *t = &gf->tensors[ti];
    uint64_t nb = (t->n_weights + 31) / 32;
    uint64_t ds = gf->tensor_data_start + t->offset;
    ds = (ds + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)ds, SEEK_SET);
    int8_t *w = (int8_t *)malloc(nb * 32);
    if (!w) return 0;
    uint64_t tot = 0;
    for (uint64_t b = 0; b < nb; b++) {
        uint16_t sc;
        if (fread(&sc,2,1,gf->fp)!=1) break;
        if (fread(w+tot,1,32,gf->fp)!=32) break;
        tot += 32;
    }
    *out = w;
    return tot;
}

/* ── Main ── */
int main(int argc, char **argv) {
    const char *mp = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  Hex Codec Full — weight → DualSquare → Hexagon   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    GGUF_File *gf = gguf_open(mp);
    if (!gf) { fprintf(stderr,"FAIL: open\n"); return 1; }
    int ti = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == 8) { ti = (int)i; break; }
    if (ti < 0) { fprintf(stderr,"FAIL: no Q8\n"); return 1; }
    printf("  Model: %s (%llu weights)\n\n", mp,
           (unsigned long long)gf->tensors[ti].n_weights);

    int8_t *w = NULL;
    uint64_t nw = read_w(gf, ti, &w);
    gguf_close(gf);
    if (!w || nw == 0) { fprintf(stderr,"FAIL: read\n"); return 1; }

    CodecConfig cfg = codec_calibrate(w, nw);
    printf("  zero_ref=%.4f  rate=%.0f  rate_phi=%.1f\n",
           cfg.zero_ref, cfg.rate, cfg.rate_phi);
    printf("  R_wrap = 360/%.0f = %.4f units per wrap\n\n",
           cfg.rate, 360.0/cfg.rate);

    /* ── Encode/Decode roundtrip ── */
    uint64_t N = (nw > 100000) ? 100000 : nw;
    int64_t total_err = 0;
    int max_err = 0, exact = 0, off1 = 0;
    printf("─── Roundtrip (sample=%llu) ───\n", (unsigned long long)N);
    for (uint64_t i = 0; i < N; i++) {
        DualHex dh = encode_weight(&cfg, (double)w[i]);
        uint32_t d = pack_delta(&dh);
        double w2 = decode_weight(&cfg, d);
        int e = abs((int)(w2 - (double)w[i]));
        total_err += e;
        if (e > max_err) max_err = e;
        if (e == 0) exact++;
        else if (e <= 1) off1++;
    }
    printf("  Exact:     %d (%.1f%%)\n", exact, 100.0*exact/N);
    printf("  Off-by-1:  %d (%.1f%%)\n", off1, 100.0*off1/N);
    printf("  Max err:   %d (Q8 units)\n", max_err);
    printf("  Avg err:   %.4f\n\n", (double)total_err/N);

    /* ── Sector distribution ── */
    uint64_t sh[6] = {0};
    for (uint64_t i = 0; i < N; i++) {
        DualHex dh = encode_weight(&cfg, (double)w[i]);
        sh[dh.sector % 6]++;
    }
    printf("─── Sector Distribution ───\n");
    char *sn[] = {"E(0°)","NE(60°)","NW(120°)","W(180°)","SW(240°)","SE(300°)"};
    for (int s = 0; s < 6; s++)
        printf("  %s: %llu (%.1f%%)\n", sn[s],
               (unsigned long long)sh[s], 100.0*sh[s]/N);

    /* ── Entropy ── */
    uint64_t mh[256] = {0};
    for (uint64_t i = 0; i < N; i++) {
        DualHex dh = encode_weight(&cfg, (double)w[i]);
        mh[dh.mag]++;
    }
    double ent = 0;
    uint64_t dis = 0;
    for (int i = 0; i < 256; i++)
        if (mh[i] > 0) { dis++; double p=(double)mh[i]/N; ent -= p*log2(p); }
    printf("\n─── Entropy ───\n  Distinct XOR: %llu/256  Entropy: %.4f bits\n\n",
           (unsigned long long)dis, ent);

    /* ── Size ── */
    uint64_t nb = (nw + 31) / 32;
    uint64_t q8s = nb * 34, geos = nw * 4;
    printf("─── Size ───\n  Q8_0: %llu MB (%.4f B/w)\n  Geo:  %llu MB (%.4f B/w)  ratio=%.2f×\n\n",
           (unsigned long long)(q8s/(1024*1024)), (double)q8s/nw,
           (unsigned long long)(geos/(1024*1024)), (double)geos/nw,
           (double)geos/q8s);

    /* ── K distribution (how many wraps needed) ── */
    uint64_t k_max = 0;
    for (uint64_t i = 0; i < N; i++) {
        DualHex dh = encode_weight(&cfg, (double)w[i]);
        if (dh.k > k_max) k_max = dh.k;
    }
    printf("─── Wrap Count ───\n  k max = %llu (needs %d bits)\n",
           (unsigned long long)k_max, k_max > 0 ? (int)(log2(k_max)+1) : 1);
    printf("  R_wrap = %.4f, weight range ~255 → k up to %d\n\n",
           360.0/cfg.rate, (int)(255.0 / (360.0/cfg.rate)));

    free(w);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  Complete                                           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    return 0;
}
