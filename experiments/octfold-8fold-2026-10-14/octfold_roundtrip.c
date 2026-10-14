/*
 * tools/octfold_roundtrip.c — G6: full GGUF file roundtrip through the
 * OCTFOLD (8-fold ladder) view on the 60-slot RID carrier.
 * ════════════════════════════════════════════════════════════════════════
 * Same shape as tools/gguf_roundtrip.c (9 languages) but for the 10th
 * candidate: reads an entire GGUF, bakes 128KB parts into a DtSlotRegion
 * twin via the octfold view (addr = layer*60 + viewpos(slot)), reads back,
 * verifies byte-identical, then rebuilds header+payload and memcmp's.
 *
 * Gates:
 *   G1 (view)  bijection sweep 60/60
 *   R1 BAKE + READBACK  every part byte-identical vs source
 *   R2 FULL REBUILD     header+payload from region -> memcmp vs original
 *   R3 DAMAGE           flip 1 byte -> detect -> localize -> restore -> R2
 *   R4 PERSISTENCE      twin file survives dt_slot_destroy (size check)
 *   MUT                 corrupted view -> bake/readback MUST fail (red ok)
 *
 * BUILD: make octfold-roundtrip   (or: gcc -O2 -std=c11 -Wall -I core \
 *        -o build/octfold_roundtrip tools/octfold_roundtrip.c -lm)
 * RUN:   ./build/octfold_roundtrip I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/infra/dramtile_store.h"

#define SLOTS 60
#define SECT  8

/* ── octfold view (identical grammar to tools/octfold_view_probe.c) ───── */
static int sector_of(int w) { return (3 * (w % SECT)) % SECT; }
static void build_view(uint8_t *v) {
    int n = 0;
    for (int s = 0; s < SECT; s++)
        for (int w = 0; w < SLOTS; w++)
            if (sector_of(w) == s) v[n++] = (uint8_t)w;
}

static inline uint32_t fwd_view(const uint8_t *vw, uint32_t f) {
    uint32_t w = f % SLOTS, l = f / SLOTS, pos = SLOTS;
    for (uint32_t p = 0; p < SLOTS; p++) if (vw[p] == w) { pos = p; break; }
    return l * SLOTS + pos;
}
static inline uint32_t inv_view(const uint8_t *vw, uint32_t addr, uint32_t *w_out) {
    uint32_t l = addr / SLOTS, pos = addr % SLOTS, w = vw[pos];
    *w_out = w;
    return l * SLOTS + w;
}
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)      ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16) ^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32) ^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48) ^ *(const uint64_t *)(p + b + 56);
    return x;
}

#define PART_BYTES (128u * 1024u)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *twin_path = argc > 2 ? argv[2] : "build\\octfold_roundtrip.twin";

    printf("=== octfold_roundtrip — 10th language (8-fold ladder) full GGUF roundtrip ===\n");

    uint8_t vw[SLOTS], mut[SLOTS];
    build_view(vw);
    memcpy(mut, vw, SLOTS);
    { uint32_t t = mut[8]; mut[8] = mut[9]; mut[9] = (uint8_t)t; }  /* MUT: swap two slots */

    /* ── G1: view bijection ──────────────────────────────────────────── */
    { uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit)); int ok = 1;
      for (int p = 0; p < SLOTS; p++) { if (hit[vw[p]]) ok = 0; hit[vw[p]] = 1; }
      printf("[octfold] G1 view bijection 60/60 = %s\n", ok ? "YES" : "NO");
      if (!ok) return 1; }

    /* ── read entire file ────────────────────────────────────────────── */
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long file_sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *src = (uint8_t *)malloc((size_t)file_sz);
    size_t nr = fread(src, 1, (size_t)file_sz, fp);
    fclose(fp);
    if ((long)nr != file_sz) { printf("FAIL read %ld\n", file_sz); return 1; }

    uint32_t total_parts = (uint32_t)(((size_t)file_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + SLOTS - 1) / SLOTS;
    printf("file: %s · %.1f MB · %u parts · %u layers\n\n",
           path, (double)file_sz / 1e6, total_parts, layers);

    typedef struct { uint32_t off; uint32_t len; } Part;
    Part *parts = (Part *)malloc(sizeof(Part) * total_parts);
    uint64_t *pxor = (uint64_t *)malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++) {
        parts[f].off = f * PART_BYTES;
        parts[f].len = (uint32_t)((size_t)file_sz - parts[f].off);
        if (parts[f].len > PART_BYTES) parts[f].len = PART_BYTES;
        pxor[f] = xor64(src + parts[f].off, parts[f].len);
    }

    uint8_t *rebld = (uint8_t *)calloc(1, (size_t)layers * SLOTS * PART_BYTES);
    int all_ok = 1;

    /* ── MUT: corrupted view must fail ───────────────────────────────── */
    {
        DtSlotRegion reg;
        remove(twin_path);
        if (dt_slot_init_twin(&reg, twin_path, (size_t)layers * SLOTS, PART_BYTES) != 0) {
            printf("FAIL twin init\n"); return 1;
        }
        uint32_t rb_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++)
            dt_slot_put(&reg, fwd_view(mut, f), src + parts[f].off, parts[f].len);
        for (uint32_t f = 0; f < total_parts; f++) {
            uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw, f));
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) rb_bad++;
        }
        printf("[MUT] corrupted-view readback bad=%u (%s)\n", rb_bad,
               rb_bad ? "red ok" : "NOT RED — PROBLEM");
        if (!rb_bad) all_ok = 0;
        dt_slot_destroy(&reg);
    }

    /* ── R1 + R2 + R3 via the real octfold view ──────────────────────── */
    {
        DtSlotRegion reg;
        remove(twin_path);
        if (dt_slot_init_twin(&reg, twin_path, (size_t)layers * SLOTS, PART_BYTES) != 0) {
            printf("FAIL twin init\n"); return 1;
        }

        /* R1: bake + readback */
        int bake_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++)
            if (!dt_slot_put(&reg, fwd_view(vw, f), src + parts[f].off, parts[f].len))
                bake_bad++;
        uint32_t rb_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw, f));
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) rb_bad++;
        }
        printf("[octfold] R1 BAKE+READ  %u parts · bake bad=%u · readback bad=%u (%s)\n",
               total_parts, bake_bad, rb_bad, (bake_bad || rb_bad) ? "FAIL" : "lossless");
        if (bake_bad || rb_bad) all_ok = 0;

        /* R2: full rebuild from region -> byte-identical */
        memset(rebld, 0, (size_t)layers * SLOTS * PART_BYTES);
        for (uint32_t addr = 0; addr < layers * SLOTS; addr++) {
            uint32_t w; uint32_t f = inv_view(vw, addr, &w);
            if (f >= total_parts) continue;
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p) continue;
            memcpy(rebld + parts[f].off, p, parts[f].len);
        }
        int rebuild_ok = (memcmp(rebld, src, (size_t)file_sz) == 0);
        printf("[octfold] R2 REBUILD   rebuilt %zu MB · byte-identical=%s\n",
               (size_t)(file_sz / (1024 * 1024)), rebuild_ok ? "YES" : "NO");
        if (!rebuild_ok) all_ok = 0;

        /* R3: damage drill — flip 1 byte, localize, restore */
        if (total_parts > 9) {
            uint32_t fhit = total_parts / 2;
            uint32_t aha = fwd_view(vw, fhit);
            dt_slot_ptr(&reg, aha)[17] ^= 0x80;
            uint32_t found = total_parts;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw, f));
                if (!p || xor64(p, parts[f].len) != pxor[f]) { found = f; break; }
            }
            int loc_ok = (found == fhit);
            if (loc_ok) dt_slot_put(&reg, aha, src + parts[fhit].off, parts[fhit].len);
            uint32_t still_bad = 0;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw, f));
                if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) still_bad++;
            }
            printf("[octfold] R3 DAMAGE    flip@part %u → localized=%s · restored bad=%u (%s)\n",
                   fhit, loc_ok ? "YES" : "NO", still_bad, still_bad ? "STILL BROKEN" : "lossless again");
            if (!loc_ok || still_bad) all_ok = 0;
        }
        dt_slot_destroy(&reg);
    }

    /* ── R4: persistence ─────────────────────────────────────────────── */
    FILE *tf = fopen(twin_path, "rb");
    int persist_ok = 0;
    if (tf) {
        fseek(tf, 0, SEEK_END);
        long tsz = ftell(tf);
        fclose(tf);
        persist_ok = (tsz == (long)((size_t)layers * SLOTS * PART_BYTES));
    }
    printf("R4 PERSIST    twin file: %s (%.1f MB)\n", persist_ok ? "YES" : "NO",
           persist_ok ? (double)layers * SLOTS * PART_BYTES / 1e6 : 0.0);

    printf("\n%s\n", (all_ok && persist_ok)
        ? "RESULT: PASSED — full GGUF roundtrip through OCTFOLD slots (G6 green)"
        : "RESULT: FAILED");

    free(src); free(parts); free(pxor); free(rebld);
    return (all_ok && persist_ok) ? 0 : 1;
}
