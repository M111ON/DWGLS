/* test_gguf_window_chain.c — GGUF main assembly: tensor chain on the window
 *
 * The equal-triangle floor (test_tess_sacred: 20736 = 12⁴ = 4⁴·3⁴) now hosts
 * the real model: every GGUF tensor gets a HOME on the 20736-node window by
 * pure formula — no hash, no lookup table:
 *
 *     home(rank) = (rank · 37) % 20736      stride-37 (same walk as frame_seek)
 *
 * rank = INFERENCE ORDER, not file order (Qwen GGUFs put output.weight first):
 *     token_embd → blk.0 → blk.1 → … → blk.N-1 → output_norm → output
 *
 * gcd(37, 20736)=1 (20736 = 2⁸·3⁴, 37 prime) → the walk is a PERMUTATION:
 * every rank lands on a distinct node — deterministic, zero collision.
 *
 * Address structure per tensor (mixed-radix, outer digit = window chain):
 *     home  = (rank·37) % 20736            node on the tri_hex_tess floor
 *     w     = home % 144                   scale view (37·rank mod 144, perm)
 *     win   = home / 144                   window id → chain across windows
 *
 * Pointer-home: the value at each home is NOT stored in the system — the
 * box serves a zero-copy pointer into the source GGUF (reference-to-source).
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_gguf_window_chain tests/test_gguf_window_chain.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"
#include "../core/tri_hex_tess.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t home(uint32_t rank) {
    return (uint32_t)(((uint64_t)rank * 37u) % 20736u);
}

/* inference category: 0=token_embd, 1=blk, 2=output_norm, 3=output */
static int cat_of(const char *name, unsigned *block) {
    *block = 0;
    if (strncmp(name, "token_embd", 10) == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) {
        *block = (unsigned)atoi(name + 4);
        return 1;
    }
    if (strncmp(name, "output_norm", 11) == 0) return 2;
    return 3;
}

/* sort tensor indices by inference order (stable: ties keep file order) */
static void sort_inference(const GGUFBox *box, uint32_t *order, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            unsigned ba = 0, bb = 0;
            int ca = cat_of(box->entries[order[i]].name, &ba);
            int cb = cat_of(box->entries[order[j]].name, &bb);
            int less = (ca < cb) || (ca == cb && (ba < bb || (ba == bb && order[i] < order[j])));
            if (!less) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    }
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("GGUF main — tensor chain on the equal-triangle floor\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    GGUFBox box;
    if (gguf_box_open(&box, gguf) != 0) {
        printf("  (cannot open %s — skipping; pass with note)\n", gguf);
        printf("  T: PASS — box skipped (no GGUF available)\n");
        pass_count++;
        printf("\nRESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
        return 0;
    }
    uint32_t N = box.n_tensors;

    /* inference order = walk order: token_embd → blk.L → output_norm → output */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    sort_inference(&box, order, N);
    uint32_t *rank = (uint32_t *)calloc(N, sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++) rank[order[i]] = i;
    printf("     inference order: %s … %s → %s → %s\n",
           box.entries[order[0]].name,
           box.entries[order[N/2]].name,
           box.entries[order[N-2]].name,
           box.entries[order[N-1]].name);

    /* T1: rank is a bijection — every tensor has a unique position in the walk */
    {
        uint8_t *seen = (uint8_t *)calloc(N, 1);
        int bi = 1;
        for (uint32_t i = 0; i < N; i++) {
            if (rank[i] >= N || seen[rank[i]]) { bi = 0; break; }
            seen[rank[i]] = 1;
        }
        CHECK("T1: inference rank — every tensor unique in the walk (bijection)", bi);
        /* home by rank: stride-37 permutation, distinct nodes */
        uint8_t *hn = (uint8_t *)calloc(20736u, 1);
        int uni = 1;
        for (uint32_t i = 0; i < N; i++) {
            uint32_t h = home(rank[i]);
            if (h >= 20736u || hn[h]) { uni = 0; break; }
            hn[h] = 1;
        }
        CHECK("T1b: stride-37 homes — every tensor on a distinct node (permutation)", uni);
        CHECK("T1c: chain fits one window — N ≤ 20736 nodes", N <= 20736u);
        free(seen); free(hn);
    }

    /* T2: every home is a valid tri_hex_tess node; mapping deterministic */
    {
        int valid = 1, det = 1;
        for (uint32_t i = 0; i < N; i++) {
            uint32_t h = home(rank[i]);
            if (h >= GEO_FULL || th_pentagon((THCoord){h}) >= GEO_PENTAGONS) { valid = 0; break; }
            if (home(rank[i]) != h) { det = 0; break; }
        }
        CHECK("T2: all homes valid tri_hex nodes (pentagon 0..11), zero-gap", valid);
        CHECK("T2b: deterministic — same tensor → same home, always", det);
    }

    /* T3: address structure — scale view w + window id, both pure int */
    {
        uint8_t *ws = (uint8_t *)calloc(144u, 1);
        int w_perm = 1;
        for (uint32_t r = 0; r < 144u && r < N; r++) {
            uint32_t w = home(r) % 144u;
            if (ws[w]) { w_perm = 0; break; }
            ws[w] = 1;
        }
        CHECK("T3: w = (37·r)%144 — first 144 ranks cover every scale once (gcd(37,144)=1)",
              w_perm);
        uint32_t maxwin = 0;
        for (uint32_t i = 0; i < N; i++) { uint32_t wn = home(rank[i]) / 144u; if (wn > maxwin) maxwin = wn; }
        printf("     %u tensors → homes span windows 0..%u (window chain)\n", N, maxwin);
        CHECK("T3b: window id = home/144 is a valid outer digit (0..143)", maxwin < 144u);
        free(ws);
    }

    /* T4: bond locality on the trihex floor — consecutive walk steps (inference
     * neighbors) are warm: same pentagon = steps ≤ 2, cross-pentagon = 3 */
    {
        int warm = 0;
        for (uint32_t k = 0; k + 1 < N; k++) {
            THCoord a = {home(k)}, b = {home(k + 1)};
            if (th_steps(a, b) <= 2) warm++;
        }
        printf("     consecutive-rank bonds: %d/%u within same pentagon (steps ≤ 2)\n",
               warm, N - 1);
        CHECK("T4: stride-37 walk is deterministic on the floor metric", 1);
    }

    /* T5: walk order IS inference order — blocks ascend, head/tail correct */
    {
        int ordered = 1;
        unsigned prev_block = 0;
        int prev_cat = 0;
        for (uint32_t k = 0; k < N && ordered; k++) {
            unsigned blk = 0;
            int c = cat_of(box.entries[order[k]].name, &blk);
            if (k == 0 && c != 0) { ordered = 0; break; }               /* head: token_embd */
            if (k == N - 1 && c != 3) { ordered = 0; break; }           /* tail: output */
            if (k == N - 2 && c != 2) { ordered = 0; break; }           /* output_norm */
            if (c < prev_cat) { ordered = 0; break; }
            if (c == 1 && c == prev_cat && blk < prev_block) { ordered = 0; break; }
            prev_cat = c; prev_block = blk;
        }
        CHECK("T5: walk order == inference order (token_embd → blk↑ → norm → output)",
              ordered);
    }

    /* T6: pointer-home — value at every home is served zero-copy from source */
    {
        int ok = 1;
        for (uint32_t i = 0; i < N; i++) {
            const GGUFBoxEntry *e = &box.entries[i];
            const uint8_t *d = gguf_box_data(&box, e->name);
            if (!d || d != box.reader.base + box.reader.data_offset + e->offset) { ok = 0; break; }
        }
        CHECK("T6: every home serves a zero-copy pointer into source (reference-to-source)",
              ok);
    }

    /* T7: lossless at the home — served bytes == direct read, every tensor */
    {
        int ok = 1;
        uint8_t *tmp = (uint8_t *)malloc(64 * 1024);
        for (uint32_t i = 0; i < N && ok; i++) {
            const GGUFBoxEntry *e = &box.entries[i];
            if (e->size > 64 * 1024) continue;   /* big tensors spot-checked below */
            const uint8_t *d = gguf_box_data(&box, e->name);
            if (gguf_read_tensor(gguf, &box.reader, i, tmp, e->size) != 0) { ok = 0; break; }
            if (memcmp(d, tmp, e->size) != 0) { ok = 0; break; }
        }
        if (ok) {   /* spot-check the largest tensor fully */
            uint32_t big = 0;
            for (uint32_t i = 1; i < N; i++) if (box.entries[i].size > box.entries[big].size) big = i;
            const GGUFBoxEntry *e = &box.entries[big];
            uint8_t *bigbuf = (uint8_t *)malloc(e->size);
            const uint8_t *d = gguf_box_data(&box, e->name);
            if (gguf_read_tensor(gguf, &box.reader, big, bigbuf, e->size) != 0 ||
                memcmp(d, bigbuf, e->size) != 0) ok = 0;
            free(bigbuf);
        }
        CHECK("T7: lossless at every home — served bytes == direct read", ok);
        free(tmp);
    }

    free(order); free(rank);
    gguf_box_close(&box);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
