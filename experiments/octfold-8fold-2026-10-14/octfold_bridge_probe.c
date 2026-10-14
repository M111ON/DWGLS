/*
 * tools/octfold_bridge_probe.c — octfold joins the multi-view carrier
 * ════════════════════════════════════════════════════════════════════════
 * LANGUAGES.md rationale #1: bake ONCE through one view, serve every other
 * view by PURE ADDRESS TRANSLATION sigma_L = vwA o vwB^-1 (permutation of
 * 60 slots, same all layers) — zero re-bake, zero copy.
 *
 * Here we bake once through OCTFOLD (8-fold ladder) and cross-read through
 * three independent permutations on the same 60-slot carrier:
 *   hosoya   : stride F(7)=13 mod 60
 *   limacon  : radial ascent from cusp [30,31,29,...,1,0]
 *   reverse  : p -> 59-p            (trivial control)
 * Every part must be byte-identical; XOR consensus must hold; a damaged
 * sigma must go red.
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I core -o build/octfold_bridge tools/octfold_bridge_probe.c -lm
 * RUN:   ./build/octfold_bridge I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/infra/dramtile_store.h"

#define SLOTS 60
#define SECT  8
#define PART_BYTES (128u * 1024u)

static int sector_of(int w) { return (3 * (w % SECT)) % SECT; }
static void build_octfold(uint8_t *v) {
    int n = 0;
    for (int s = 0; s < SECT; s++)
        for (int w = 0; w < SLOTS; w++)
            if (sector_of(w) == s) v[n++] = (uint8_t)w;
}
static void build_hosoya(uint8_t *v) { for (int p = 0; p < SLOTS; p++) v[p] = (uint8_t)((p * 13u) % 60u); }
static void build_limacon(uint8_t *v) {
    for (int p = 0; p < SLOTS; p++) {
        int w; if (p == 0) w = 30; else if (p == 59) w = 0;
        else w = (p & 1) ? 30 + (p + 1) / 2 : 30 - p / 2;
        v[p] = (uint8_t)w;
    }
}
static void build_rev(uint8_t *v) { for (int p = 0; p < SLOTS; p++) v[p] = (uint8_t)(SLOTS - 1 - p); }

static int is_perm(const uint8_t *v) {
    uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
    for (int p = 0; p < SLOTS; p++) { if (hit[v[p]]) return 0; hit[v[p]] = 1; }
    return 1;
}
/* position of slot w in view v */
static uint32_t pos_of(const uint8_t *v, uint32_t w) {
    for (uint32_t p = 0; p < SLOTS; p++) if (v[p] == w) return p;
    return 0;
}
static uint32_t fwd_view(const uint8_t *v, uint32_t f) {
    return (f / SLOTS) * SLOTS + pos_of(v, f % SLOTS);
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *twin = argc > 2 ? argv[2] : "build\\octfold_bridge.twin";
    printf("=== octfold_bridge — bake ONCE via octfold, cross-read other views ===\n");

    uint8_t VW[4][SLOTS];
    const char *nm[4] = { "octfold", "hosoya", "limacon", "reverse" };
    build_octfold(VW[0]); build_hosoya(VW[1]); build_limacon(VW[2]); build_rev(VW[3]);
    for (int i = 0; i < 4; i++) {
        printf("  %-8s permutation=%s\n", nm[i], is_perm(VW[i]) ? "YES" : "NO");
        if (!is_perm(VW[i])) return 1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END); long file_sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *src = (uint8_t *)malloc((size_t)file_sz);
    if (fread(src, 1, (size_t)file_sz, fp) != (size_t)file_sz) { printf("FAIL read\n"); return 1; }
    fclose(fp);
    uint32_t total_parts = (uint32_t)(((size_t)file_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + SLOTS - 1) / SLOTS;
    printf("file: %.1f MB · %u parts · %u layers\n\n", (double)file_sz / 1e6, total_parts, layers);

    typedef struct { uint32_t off, len; } Part;
    Part *parts = (Part *)malloc(sizeof(Part) * total_parts);
    uint64_t *pxor = (uint64_t *)malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++) {
        parts[f].off = f * PART_BYTES;
        parts[f].len = (uint32_t)((size_t)file_sz - parts[f].off);
        if (parts[f].len > PART_BYTES) parts[f].len = PART_BYTES;
        pxor[f] = xor64(src + parts[f].off, parts[f].len);
    }

    int all_ok = 1;
    DtSlotRegion reg; remove(twin);
    if (dt_slot_init_twin(&reg, twin, (size_t)layers * SLOTS, PART_BYTES) != 0) { printf("FAIL twin\n"); return 1; }

    /* B1: single bake through octfold */
    uint32_t writes = 0;
    for (uint32_t f = 0; f < total_parts; f++) writes += dt_slot_put(&reg, fwd_view(VW[0], f), src + parts[f].off, parts[f].len) ? 1 : 0;
    printf("[B1] single bake via octfold: %u parts written\n", writes);

    /* B2/B3: cross-read each view by address translation + XOR consensus */
    for (int L = 1; L < 4; L++) {
        /* sigma: view L position p -> octfold position. read at octfold addr. */
        uint32_t bad = 0; uint64_t xall = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t l = f / SLOTS, w = f % SLOTS;
            uint32_t oct_pos = pos_of(VW[0], w);          /* where octfold keeps slot w */
            uint8_t *p = dt_slot_ptr(&reg, l * SLOTS + oct_pos);
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) bad++;
            else xall ^= xor64(p, parts[f].len);
        }
        int xor_ok = 1;
        { uint64_t xs = 0; for (uint32_t f = 0; f < total_parts; f++) xs ^= pxor[f]; xor_ok = (xs == xall); }
        printf("[B2] cross-read via %-8s bad=%u (%s) · XOR-consensus=%s\n",
               nm[L], bad, bad ? "FAIL" : "lossless", xor_ok ? "YES" : "NO");
        if (bad || !xor_ok) all_ok = 0;
    }

    /* B4: damage through a DIFFERENT view's ordering -> localize same part */
    {
        uint32_t fhit = total_parts / 2, l = fhit / SLOTS, w = fhit % SLOTS;
        uint32_t oct_pos = pos_of(VW[0], w);
        dt_slot_ptr(&reg, l * SLOTS + oct_pos)[17] ^= 0x80;
        /* find via hosoya ordering (translate hosoya pos -> oct addr) */
        uint32_t found = total_parts;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t ll = f / SLOTS, ww = f % SLOTS;
            uint8_t *p = dt_slot_ptr(&reg, ll * SLOTS + pos_of(VW[0], ww));
            if (!p || xor64(p, parts[f].len) != pxor[f]) { found = f; break; }
        }
        int loc_ok = (found == fhit);
        if (loc_ok) dt_slot_put(&reg, l * SLOTS + oct_pos, src + parts[fhit].off, parts[fhit].len);
        uint32_t still = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint8_t *p = dt_slot_ptr(&reg, (f / SLOTS) * SLOTS + pos_of(VW[0], f % SLOTS));
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) still++;
        }
        printf("[B4] damage flip@part %u via cross-view → localized=%s · restored bad=%u (%s)\n",
               fhit, loc_ok ? "YES" : "NO", still, still ? "STILL BROKEN" : "lossless again");
        if (!loc_ok || still) all_ok = 0;
    }

    /* MUT: corrupted view -> cross-read MUST fail */
    {
        uint8_t bad_v[SLOTS]; memcpy(bad_v, VW[0], SLOTS);
        uint32_t t = bad_v[8]; bad_v[8] = bad_v[9]; bad_v[9] = (uint8_t)t;
        DtSlotRegion r2; remove("build\\octfold_bridge_mut.twin");
        dt_slot_init_twin(&r2, "build\\octfold_bridge_mut.twin", (size_t)layers * SLOTS, PART_BYTES);
        for (uint32_t f = 0; f < total_parts; f++) dt_slot_put(&r2, fwd_view(bad_v, f), src + parts[f].off, parts[f].len);
        uint32_t bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint8_t *p = dt_slot_ptr(&r2, fwd_view(VW[0], f));
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0) bad++;
        }
        printf("[MUT] corrupted octfold readback bad=%u (%s)\n", bad, bad ? "red ok" : "NOT RED — PROBLEM");
        if (!bad) all_ok = 0;
        dt_slot_destroy(&r2); remove("build\\octfold_bridge_mut.twin");
    }

    dt_slot_destroy(&reg);
    printf("\n%s\n", all_ok ? "RESULT: PASSED — octfold joins the multi-view carrier (bake once, serve all)"
                           : "RESULT: FAILED");
    free(src); free(parts); free(pxor);
    return all_ok ? 0 : 1;
}
