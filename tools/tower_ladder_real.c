/* tools/tower_ladder_real.c — real-data exercise of core/geo_tower_ladder.h
 *
 * The unit test (tests/test_tower_ladder.c) proves the addressing MATH is
 * right. This tool proves it carries REAL bytes: it maps real GGUF tensor
 * bytes into the 192-slot 3-tower stack through the ladder address, reads them
 * back through four independent traversal views, and requires every view to be
 * a real bijection (write through a view, read through the same view, compare
 * against the untouched tensor bytes).
 *
 * The view test is written so it can FAIL. The trap it avoids: placing
 * stack[order[i]] back onto itself reconstructs the stack for ANY order, so
 * that formulation proves nothing. Here the view order decides which source
 * chunk is stored at which slot, so a view with a duplicate or a missing slot
 * loses real bytes and the comparison fails.
 *
 * It also runs the ORIGINAL simulator arithmetic on the same real data as a
 * negative control, and reports what that arithmetic cannot address.
 *
 * Usage: tower_ladder_real <model.gguf> [mib]
 * BUILD: gcc -O2 -I. -Icore -o build/tower_ladder_real tools/tower_ladder_real.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "geo_tower_ladder.h"
#include "gguf_reader.h"

#define CELL_BYTES  64u
#define SLOTS       TL_TOTAL            /* 192 */
#define STACK_BYTES (SLOTS * CELL_BYTES)

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name, const char *note) {
    if (ok) { g_pass++; printf("  ok   %-54s %s\n", name, note); }
    else    { g_fail++; printf("  FAIL %-54s %s\n", name, note); }
}

/* ── view orders: each must be a permutation of 0..191 ──────────── */
static size_t view_slot_order(uint32_t *o) {                 /* V0 identity   */
    for (uint32_t i = 0; i < SLOTS; i++) o[i] = i;
    return SLOTS;
}
static size_t view_ladder_walk(uint32_t *o) {               /* V1 active walk */
    size_t n = 0;
    uint32_t p = tl_active_slot(0u, 0u, 0u);
    for (uint32_t i = 0; i < 144u; i++) { o[n++] = p; p = tl_next_active(p); }
    for (uint32_t t = 0; t < TL_TOWERS; t++)
        for (uint32_t l = TL_ACTIVE_PER_TOWER; l < TL_SLOTS_PER_TOWER; l++)
            o[n++] = t * TL_SLOTS_PER_TOWER + l;
    return n;
}
/* V2: coprime-stride permutation. Indexed by i, NOT iterated: p*37 mod 192 is
 * a bijection but 0 is a fixed point, so iterating it never leaves slot 0. */
static size_t view_jump37(uint32_t *o) {
    for (uint32_t i = 0; i < SLOTS; i++) o[i] = (i * 37u) % SLOTS;
    return SLOTS;
}
static size_t view_reverse(uint32_t *o) {                   /* V3 reverse    */
    for (uint32_t i = 0; i < SLOTS; i++) o[i] = SLOTS - 1u - i;
    return SLOTS;
}
static size_t (*const VIEWS[4])(uint32_t *) = {
    view_slot_order, view_ladder_walk, view_jump37, view_reverse
};
static const char *const VNAME[4] = {
    "slot order", "ladder walk (144 active + 48 residual)", "MOD-37 jump", "reverse"
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <model.gguf> [mib]\n", argv[0]); return 2; }
    const char *path = argv[1];
    uint64_t mib = (argc > 2) ? (uint64_t)atoll(argv[2]) : 256u;

    printf("tower_ladder_real\n  model: %s\n", path);

    GgufReader g;
    if (gguf_open(path, &g) != 0 || g.base == NULL) { printf("  FAIL cannot open/mmap model\n"); return 1; }

    uint32_t best = 0;
    for (uint32_t i = 1; i < g.n_tensors; i++) if (g.sizes[i] > g.sizes[best]) best = i;
    uint64_t tsize = g.sizes[best];
    uint64_t toff  = g.data_offset + g.offsets[best];
    if (toff + tsize > g.base_sz) { printf("  FAIL tensor range outside file\n"); gguf_close(&g); return 1; }
    const uint8_t *tsrc = g.base + toff;
    printf("  tensor: %s  dtype=%u  %I64u bytes\n",
           g.names[best] ? g.names[best] : "?", g.dtypes[best], tsize);

    uint64_t total = mib * 1024u * 1024u;
    if (total > tsize) total = tsize;
    uint64_t total_full = (total / STACK_BYTES) * STACK_BYTES;
    uint64_t passes = total_full / STACK_BYTES;
    if (passes == 0) { printf("  FAIL tensor smaller than one 12 KiB pass\n"); gguf_close(&g); return 1; }
    printf("  passes: %I64u  real bytes exercised: %I64u (%.0f MiB)\n",
           passes, total_full, (double)total_full / (1024.0 * 1024.0));

    uint8_t *stack   = malloc(STACK_BYTES);
    uint8_t *rebuilt = malloc(STACK_BYTES);
    uint32_t order[SLOTS];
    if (!stack || !rebuilt) { printf("  FAIL oom\n"); return 1; }

    uint64_t bytes_moved = 0;
    clock_t t0 = clock();
    for (uint64_t pass = 0; pass < passes; pass++) {
        const uint8_t *src = tsrc + pass * STACK_BYTES;
        for (int v = 0; v < 4; v++) {
            if (VIEWS[v](order) != SLOTS) { check(0, "view length", VNAME[v]); continue; }
            /* WRITE: the view order decides which source chunk lands at which slot */
            for (size_t i = 0; i < SLOTS; i++)
                memcpy(stack + order[i] * CELL_BYTES, src + i * CELL_BYTES, CELL_BYTES);
            /* READ: same view, back out in stream order */
            for (size_t i = 0; i < SLOTS; i++)
                memcpy(rebuilt + i * CELL_BYTES, stack + order[i] * CELL_BYTES, CELL_BYTES);
            if (memcmp(rebuilt, src, STACK_BYTES) != 0) {
                printf("  FAIL view '%s' pass %I64u: bytes differ\n", VNAME[v], pass);
                g_fail++; goto done;
            }
        }
        bytes_moved += 4ull * STACK_BYTES * 2ull;
    }
    check(1, "real tensor bytes roundtrip through 4 views", "0 bytes differ");
    {
        char n[96];
        snprintf(n, sizeof n, "%I64u passes x 4 views, %I64u MiB moved", passes, bytes_moved / (1024ull*1024ull));
        printf("       %s\n", n);
    }

    /* ── views must be genuinely different orderings ──────────────── */
    {
        uint32_t a[SLOTS], b[SLOTS];
        int all_differ = 1, all_perm = 1;
        for (int v = 0; v < 4; v++) {
            unsigned char seen[SLOTS]; memset(seen, 0, sizeof seen);
            VIEWS[v](a);
            for (uint32_t i = 0; i < SLOTS; i++) if (a[i] >= SLOTS || seen[a[i]]++) all_perm = 0;
            for (int w = 0; w < v; w++) {
                int same = 1;
                VIEWS[w](b);
                for (uint32_t i = 0; i < SLOTS; i++) if (a[i] != b[i]) { same = 0; break; }
                if (same) all_differ = 0;
            }
        }
        check(all_perm, "all 4 views are permutations of 0..191", "no duplicate, no gap");
        check(all_differ, "no two views are secretly the same order", "tests are independent");
    }

    /* ── throughput: address-driven write+read ────────────────────── */
    {
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        double mbs = (double)bytes_moved / (1024.0*1024.0) / (secs > 1e-9 ? secs : 1e-9);
        char n[96];
        snprintf(n, sizeof n, "%.0f MB/s over %.2f s", mbs, secs);
        check(mbs > 200.0, "address walk throughput", n);
    }

    /* ── NEGATIVE CONTROL: the original simulator arithmetic ───────── */
    {
        /* (a) & 0xBF: bit 6 is clear, so slots 64..127 are never produced */
        uint32_t masked_out = 0, wrong = 0;
        for (uint32_t s = 0; s < SLOTS; s++) {
            if (s & 0x40u) masked_out++;                    /* bit 6 set -> unreachable */
            if (((s << 1) & 0xBFu) != (s << 1) % SLOTS) wrong++;
        }
        char n[96];
        snprintf(n, sizeof n, "%u of 192 slots have bit 6 set and are unreachable", masked_out);
        check(masked_out == 64u, "0xBF mask cannot reach 64 of 192 slots (artifact defect)", n);
        snprintf(n, sizeof n, "differs from mod on %u of 192 pointers", wrong);
        check(wrong == 128u, "0xBF shift agrees with modulo only 64/192", n);

        /* (b) artifact residual zone: float formula -> distinct coordinates.
         * rx/ry are -0.8 or 3.8; coded as bits 0/1. layerIdx = idx/5 reaches 3
         * but only 3 layers exist. Per tower: layers 0..2 contribute 4 distinct
         * positions each (subIdx 3 and 4 collide), layer 3 contributes 1. */
        {
            int seen[64]; memset(seen, 0, sizeof seen);
            uint32_t distinct = 0, collide = 0;
            for (uint32_t t = 0; t < 3u; t++)
                for (uint32_t idx = 0; idx < 16u; idx++) {
                    uint32_t layerIdx = idx / 5u, subIdx = idx % 5u;
                    int rx = (subIdx % 2u == 0u) ? 0 : 1;   /* -0.8 -> 0, 3.8 -> 1 */
                    int ry = (subIdx < 2u)    ? 0 : 1;
                    int key = (int)((t * 4u + layerIdx) * 2u + (uint32_t)rx) * 2 + ry;
                    if (!seen[key]) { seen[key] = 1; distinct++; }
                    else collide++;
                }
            snprintf(n, sizeof n, "%u distinct coordinates for 48 slots (%u collide)", distinct, collide);
            check(distinct == 39u, "artifact residual zone loses 9 of 48 addresses", n);
        }

        /* (c) our residual zone, same 48 slots */
        {
            int seen[16]; memset(seen, 0, sizeof seen);
            uint32_t distinct = 0;
            for (uint32_t t = 0; t < 3u; t++)
                for (uint32_t l = TL_ACTIVE_PER_TOWER; l < TL_SLOTS_PER_TOWER; l++) {
                    uint32_t sl = t * TL_SLOTS_PER_TOWER + l;
                    int k = (int)(tl_rung(sl) * TL_CORNERS + tl_corner(sl));
                    if (!seen[k]) { seen[k] = 1; distinct++; }
                }
            snprintf(n, sizeof n, "%u (rung,corner) x 3 towers = 48 unique slots", distinct);
            check(distinct == 16u, "tl residual zone is a bijection", n);
        }
    }

done:
    free(stack); free(rebuilt);
    gguf_close(&g);
    printf("tower_ladder_real: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
