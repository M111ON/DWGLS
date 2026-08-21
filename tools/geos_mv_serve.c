/*
 * tools/geos_mv_serve.c — bake a REAL GGUF into GeosMV, then serve tensor pulls
 * ═══════════════════════════════════════════════════════════════════════════
 * The weight-pull layer that llama.cpp / GPU kernels would call:
 *
 *   1. BAKE   — every tensor is split into ≤128 KB parts; each part is one
 *               hyper key-frame file ("name#pNNN") placed across auto-opened
 *               GeosVolumes (Option A multi-volume)
 *   2. VERIFY — pull every tensor back part-by-part through the geometric
 *               address walk → memcmp against the mapped source bytes
 *   3. SEQ    — full-model sweep in pull order (DRAM-scale stream GB/s)
 *   4. RAND   — random whole-tensor pulls by name (latency + MB/s)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geos_mv_serve tools/geos_mv_serve.c -lm
 * RUN:   ./build/geos_mv_serve [model.gguf]   (default I:\model Qwen2.5-0.5B)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../core/gguf_reader.h"
#include "../core/geofs_multivol.h"

#define PART_BLOCKS  2048u
#define PART_BYTES   (PART_BLOCKS * GEOS_BLOCK_SZ)     /* 128 KB */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint32_t xs32(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

/* part file name: "<tensor>#p<NNN>" — must fit GEOS_MAX_NAME */
static int part_name(char *dst, size_t cap, const char *tn, uint32_t pi) {
    int n = snprintf(dst, cap, "%s#p%u", tn, pi);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) {
        printf("FAIL: cannot open %s\n", path); return 1;
    }
    printf("═══ geos_mv_serve — %s ═══\n", path);
    printf("  tensors: %u · file %.1f MB (mmap)\n\n",
           r.n_tensors, (double)r.base_sz / 1e6);

    GeosMV mv;
    if (geos_mv_init(&mv, 16) != 0) { printf("FAIL: mv_init\n"); return 1; }

    /* ── 1. BAKE ─────────────────────────────────────────────────────── */
    double t0 = now_ms();
    uint64_t baked_bytes = 0;
    uint32_t baked_parts = 0, long_names = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        uint32_t sz = r.sizes[i];
        if (sz == 0) continue;
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t n_parts = (sz + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < n_parts; p++) {
            char pn[GEOS_MAX_NAME];
            if (part_name(pn, sizeof(pn), r.names[i], p) != 0) {
                long_names++; break;
            }
            uint32_t off = p * PART_BYTES;
            uint32_t len = sz - off; if (len > PART_BYTES) len = PART_BYTES;
            if (geos_mv_place(&mv, pn, len, src + off, 1) != 0) {
                printf("FAIL: place %s\n", pn); return 1;
            }
            baked_parts++;
        }
        baked_bytes += sz;
    }
    double t_bake = now_ms() - t0;
    printf("  BAKE   %u parts · %.1f MB · %.0f ms (%.0f MB/s scatter write)\n"
           "         volumes opened: %u · long-name skips: %u\n",
           baked_parts,
           (double)baked_bytes / 1e6, t_bake,
           (double)baked_bytes / 1e6 / (t_bake / 1000.0 + 1e-9),
           mv.n_used, long_names);

    /* ── 2. VERIFY — pull every tensor, memcmp vs mmap ───────────────── */
    t0 = now_ms();
    uint64_t verified = 0, mismatches = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        uint32_t sz = r.sizes[i];
        if (sz == 0) continue;
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t n_parts = (sz + PART_BYTES - 1) / PART_BYTES;
        static uint8_t buf[PART_BYTES];
        for (uint32_t p = 0; p < n_parts; p++) {
            char pn[GEOS_MAX_NAME];
            if (part_name(pn, sizeof(pn), r.names[i], p) != 0) break;
            uint32_t off = p * PART_BYTES;
            uint32_t len = sz - off; if (len > PART_BYTES) len = PART_BYTES;
            if (geos_mv_read(&mv, pn, buf, len) != (int)len ||
                memcmp(src + off, buf, len) != 0) mismatches++;
            else verified += len;
        }
    }
    double t_verify = now_ms() - t0;
    printf("  VERIFY %.1f MB byte-identical · %u mismatched parts · %.0f ms\n"
           "         (%s)\n",
           (double)verified / 1e6, (unsigned)mismatches, t_verify,
           mismatches ? "LOSSLESS BROKEN" : "lossless");

    if (mismatches || long_names) { geos_mv_free(&mv); gguf_close(&r); return 1; }

    /* ── 3. SEQ — full-model sweep (pull order) ──────────────────────── */
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        t0 = now_ms();
        static uint8_t buf[PART_BYTES];
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            uint32_t sz = r.sizes[i];
            if (sz == 0) continue;
            uint32_t n_parts = (sz + PART_BYTES - 1) / PART_BYTES;
            for (uint32_t p = 0; p < n_parts; p++) {
                char pn[GEOS_MAX_NAME];
                if (part_name(pn, sizeof(pn), r.names[i], p) != 0) break;
                geos_mv_read(&mv, pn, buf, PART_BYTES);
            }
        }
        double t = now_ms() - t0;
        if (t < best) best = t;
    }
    printf("  SEQ    full sweep %.0f ms → %.0f MB/s\n",
           best, (double)baked_bytes / 1e6 / (best / 1000.0));

    /* ── 4. RAND — random whole-tensor pulls by name ─────────────────── */
    const uint32_t N_PULLS = 500u;
    uint64_t rand_bytes = 0;
    double worst = 0.0, sum = 0.0;
    uint32_t st = 0x1234567u;
    for (uint32_t k = 0; k < N_PULLS; k++) {
        uint32_t i = xs32(&st) % r.n_tensors;
        uint32_t sz = r.sizes[i];
        if (sz == 0) { continue; }
        uint8_t *out = (uint8_t *)malloc(sz);
        if (!out) break;
        t0 = now_ms();
        uint32_t n_parts = (sz + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < n_parts; p++) {
            char pn[GEOS_MAX_NAME];
            if (part_name(pn, sizeof(pn), r.names[i], p) != 0) break;
            uint32_t off = p * PART_BYTES;
            uint32_t len = sz - off; if (len > PART_BYTES) len = PART_BYTES;
            geos_mv_read(&mv, pn, out + off, len);
        }
        double dt = now_ms() - t0;
        sum += dt; if (dt > worst) worst = dt;
        rand_bytes += sz;
        free(out);
    }
    printf("  RAND   %u pulls · %.2f MB pulled · avg %.2f ms · worst %.2f ms"
           " · %.0f MB/s\n",
           N_PULLS, (double)rand_bytes / 1e6, sum / N_PULLS, worst,
           (double)rand_bytes / 1e6 / (sum / 1000.0 + 1e-9));

    printf("═══════════════════════════════════════\n");
    geos_mv_free(&mv);
    gguf_close(&r);
    return 0;
}
