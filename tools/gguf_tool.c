/*
 * gguf_tool.c — GGUF ops backend for FGLS UI
 *
 * Modes:
 *   info <path>                  — tensor list JSON
 *   tensor <path> <idx> <n>      — decode first n weights, stats JSON
 *   roundtrip <path> <idx> <n>   — adaptive store roundtrip, result JSON
 *
 * Compile:
 *   gcc -O2 -I. -Icore -Icore/infra -I.hermes/desktop-attachments \
 *       -o build/gguf_tool tools/gguf_tool.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "core/geo_adaptive_store.h"
#include "core/geo_kis_container.h"

/* ── FP16 → float ─────────────────────────────────────── */
static float fp16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    float f;
    if (exp == 0) {
        f = (float)mant / 1024.0f * 5.960464478e-8f; /* subnormal: 2^-24 */
    } else if (exp == 31) {
        f = mant ? NAN : INFINITY;
    } else {
        f = (float)mant / 1024.0f;
        f += 1.0f;
        f = ldexpf(f, (int)exp - 15);
    }
    return sign ? -f : f;
}

/* ── decode Q8_0 tensor to floats ─────────────────────── */
static float *decode_q8(const uint8_t *buf, uint32_t sz, uint32_t *out_n) {
    uint32_t max_w = 4096; /* decode up to 4096 weights */
    float *w = (float*)malloc(max_w * sizeof(float));
    uint32_t n = 0;
    for (uint32_t off = 0; off + 34 <= sz && n < max_w; off += 34) {
        uint16_t su; memcpy(&su, buf + off, 2);
        float scale = fp16_to_f32(su);
        for (int i = 0; i < 32 && n < max_w; i++) {
            int8_t q = (int8_t)buf[off + 2 + i];
            w[n++] = q * scale;
        }
    }
    *out_n = n;
    return w;
}

/* ── decode F32 tensor ────────────────────────────────── */
static float *decode_f32(const uint8_t *buf, uint32_t sz, uint32_t *out_n) {
    uint32_t max_w = 4096;
    float *w = (float*)malloc(max_w * sizeof(float));
    uint32_t n = 0;
    for (uint32_t off = 0; off + 4 <= sz && n < max_w; off += 4) {
        memcpy(&w[n], buf + off, 4);
        n++;
    }
    *out_n = n;
    return w;
}

/* ── helper: distinct buckets ─────────────────────────── */
static int distinct_buckets(const float *w, int n) {
    uint8_t seen[256]; memset(seen, 0, sizeof(seen));
    int d = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = (uint8_t)((int)(w[i] * 100) & 0xFF);
        if (!seen[b]) { seen[b] = 1; d++; }
    }
    return d;
}

/* ── JSON escape ──────────────────────────────────────── */
static void json_str(const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { putchar('\\'); putchar(*s); }
        else if (*s == '\n') fputs("\\n", stdout);
        else putchar(*s);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("{}"); return 1; }

    if (strcmp(argv[1], "info") == 0 && argc >= 3) {
        GgufReader r;
        if (gguf_open(argv[2], &r) != 0) { printf("{}"); return 1; }
        printf("{\"n_tensors\":%u,\"tensors\":[", r.n_tensors);
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            if (i) putchar(',');
            printf("{\"name\":\"");
            json_str(r.names[i]);
            printf("\",\"size\":%u,\"idx\":%u}", r.sizes[i], i);
        }
        printf("]}");
        gguf_close(&r);
        return 0;
    }

    if (strcmp(argv[1], "tensor") == 0 && argc >= 4) {
        uint32_t idx = (uint32_t)atoi(argv[3]);
        uint32_t want_n = argc >= 5 ? (uint32_t)atoi(argv[4]) : 1024;
        GgufReader r;
        if (gguf_open(argv[2], &r) != 0) { printf("{}"); return 1; }
        if (idx >= r.n_tensors) { printf("{\"error\":\"bad idx\"}"); return 1; }
        uint8_t *buf = (uint8_t*)malloc(r.sizes[idx]);
        int rc = gguf_read_tensor(argv[2], &r, idx, buf, r.sizes[idx]);
        if (rc != 0) { printf("{\"error\":\"read %d\"}", rc); return 1; }

        uint32_t n = 0;
        float *w = NULL;
        /* Detect Q8_0 by size pattern: multiple of 34 */
        if (r.sizes[idx] % 34 == 0) w = decode_q8(buf, r.sizes[idx], &n);
        else if (r.sizes[idx] % 4 == 0) w = decode_f32(buf, r.sizes[idx], &n);
        if (n > want_n) n = want_n;

        if (!w || n == 0) { printf("{\"error\":\"decode failed\"}"); return 1; }

        /* Stats */
        float mn = w[0], mx = w[0], sum = 0;
        uint8_t seen[256]; memset(seen, 0, sizeof(seen));
        int distinct = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (w[i] < mn) mn = w[i];
            if (w[i] > mx) mx = w[i];
            sum += w[i];
            uint8_t b = (uint8_t)((int)(w[i] * 100) & 0xFF);
            if (!seen[b]) { seen[b] = 1; distinct++; }
        }
        double mean = sum / n;

        printf("{\"name\":\"");
        json_str(r.names[idx]);
        printf("\",\"idx\":%u,\"size\":%u,\"decoded\":%u,\"min\":%.6f,\"max\":%.6f,"
               "\"mean\":%.6f,\"distinct_buckets\":%d,\"spread\":%.6f}",
               idx, r.sizes[idx], n, mn, mx, mean, distinct, mx - mn);

        free(w); free(buf); gguf_close(&r);
        return 0;
    }

    if (strcmp(argv[1], "roundtrip") == 0 && argc >= 4) {
        uint32_t idx = (uint32_t)atoi(argv[3]);
        uint32_t want_n = argc >= 5 ? (uint32_t)atoi(argv[4]) : 256;
        GgufReader r;
        if (gguf_open(argv[2], &r) != 0) { printf("{}"); return 1; }
        if (idx >= r.n_tensors) { printf("{\"error\":\"bad idx\"}"); return 1; }
        uint8_t *buf = (uint8_t*)malloc(r.sizes[idx]);
        int rc = gguf_read_tensor(argv[2], &r, idx, buf, r.sizes[idx]);
        if (rc != 0) { printf("{\"error\":\"read %d\"}", rc); return 1; }

        uint32_t n = 0;
        float *w = NULL;
        if (r.sizes[idx] % 34 == 0) w = decode_q8(buf, r.sizes[idx], &n);
        else if (r.sizes[idx] % 4 == 0) w = decode_f32(buf, r.sizes[idx], &n);
        if (!w || n == 0) { printf("{\"error\":\"decode\"}"); return 1; }

        /* Adaptive store roundtrip on first want_n weights */
        uint32_t chunk = n < want_n ? n : want_n;
        uint8_t entropy = (uint8_t)(distinct_buckets(w, chunk));
        AdaptiveStore as;
        adaptive_init(&as);
        int wrc = adaptive_write(&as, 0, w, chunk, entropy);
        int vrc = adaptive_verify(&as);
        float rb[256];
        int rrc = adaptive_read(&as, 0, rb, chunk);
        int match = 1;
        for (uint32_t i = 0; i < chunk; i++)
            if (fabsf(rb[i] - w[i]) > 1e-6f) { match = 0; break; }

        /* Container */
        KisHeader hdr;
        kis_container_init(&hdr, &as);
        uint32_t csz = kis_container_size(&hdr);
        uint8_t *cbuf = (uint8_t*)malloc(csz + 64);
        int wrote = kis_container_serialize(&hdr, as.frames, as.blocks, cbuf, csz + 64);
        int cver = kis_container_verify(cbuf, csz);

        printf("{\"name\":\"");
        json_str(r.names[idx]);
        printf("\",\"weights\":%u,\"entropy\":%u,\"write\":%d,\"verify\":%d,"
               "\"read\":%d,\"lossless\":%d,\"container_size\":%u,\"container_ok\":%d}",
               chunk, entropy, wrc, vrc, rrc, match, csz, (wrote == (int)csz && cver == 0));

        free(cbuf); free(w); free(buf); gguf_close(&r);
        return 0;
    }

    printf("{}");
    return 1;
}
