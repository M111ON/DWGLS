/*
 * test_bfs_evict.c — BFS LRU eviction + residency bound (card #46)
 * ═══════════════════════════════════════════════════════════════════
 * No hooks: v1 fail codes preserved (-3 full). Hooks set: writes evict the
 * LRU file through the spill backend and retry; reads fault evicted files
 * back via fill. Residency stays bounded while the logical set is unbounded
 * (the expert-streaming pattern for card #3).
 *
 * Oracle: byte patterns recomputed from the file index (source data), never
 * from BFS internals. Mutation check: break the spill call in bfs_evict_oldest
 * → T2/T4 go red; break the tick touch in bfs_read → T2 victim assertion red.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_evict tests/test_bfs_evict.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "breathing_fs.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* oracle: file k holds NB_EVICT blocks of byte k in every position */
#define NB_EVICT 3u
#define NF_EVICT (BFS_BLOCKS / NB_EVICT)   /* 48 files tile all 144 blocks */
static void oracle_file(int8_t *d, uint32_t k) {
    for (uint32_t i = 0; i < NB_EVICT * BFS_SLOTS_BLOCK; i++)
        d[i] = (int8_t)(k & 0xFF);
}
static void fname(char *out, uint32_t k) { snprintf(out, 32, "f%03u", k); }

/* ── in-memory spill backend (the only store; BFS stays bounded) ── */
#define SPILL_MAX 320
typedef struct { char name[32]; int8_t *data; uint32_t size; } SpillSlot;
static SpillSlot g_spill[SPILL_MAX];
static uint32_t g_nspill = 0;

static void spill_reset(void) {
    for (uint32_t i = 0; i < g_nspill; i++) free(g_spill[i].data);
    memset(g_spill, 0, sizeof(g_spill));
    g_nspill = 0;
}
static int mem_spill(const char *name, const int8_t *data, uint32_t size, void *u) {
    (void)u;
    for (uint32_t i = 0; i < g_nspill; i++)
        if (strcmp(g_spill[i].name, name) == 0) {
            free(g_spill[i].data);
            g_spill[i].data = (int8_t *)malloc(size);
            if (!g_spill[i].data) return 1;
            memcpy(g_spill[i].data, data, size);
            g_spill[i].size = size;
            return 0;
        }
    if (g_nspill >= SPILL_MAX) return 1;
    snprintf(g_spill[g_nspill].name, 32, "%s", name);
    g_spill[g_nspill].data = (int8_t *)malloc(size);
    if (!g_spill[g_nspill].data) return 1;
    memcpy(g_spill[g_nspill].data, data, size);
    g_spill[g_nspill].size = size;
    g_nspill++;
    return 0;
}
static int mem_fill(const char *name, int8_t **out, uint32_t *size, void *u) {
    (void)u;
    for (uint32_t i = 0; i < g_nspill; i++)
        if (strcmp(g_spill[i].name, name) == 0) {
            *out = (int8_t *)malloc(g_spill[i].size);
            if (!*out) return -1;
            memcpy(*out, g_spill[i].data, g_spill[i].size);
            *size = g_spill[i].size;
            return 0;
        }
    return 1;   /* no data */
}
static int veto_spill(const char *n, const int8_t *d, uint32_t s, void *u) {
    (void)n; (void)d; (void)s; (void)u; return 1;
}
static const SpillSlot *spill_find(const char *name) {
    for (uint32_t i = 0; i < g_nspill; i++)
        if (strcmp(g_spill[i].name, name) == 0) return &g_spill[i];
    return NULL;
}
static uint32_t fe_home(BreathingFS *fs, const char *name) {
    for (uint32_t i = 0; i < fs->n_files; i++)
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0)
            return fs->files[i].home_block;
    return BFS_BLOCKS;   /* not found */
}
static int verify_idx(BreathingFS *fs, uint32_t k) {    int8_t expect[NB_EVICT * BFS_SLOTS_BLOCK], got[NB_EVICT * BFS_SLOTS_BLOCK];
    char nm[32];
    oracle_file(expect, k);
    fname(nm, k);
    uint32_t act = 0;
    if (bfs_read(fs, nm, got, sizeof(got), &act) != 0) return 0;
    return act == sizeof(got) && memcmp(expect, got, sizeof(got)) == 0;
}

int main(void) {
    printf("═ BFS EVICT — LRU + residency bound ═\n");
    int8_t blk[NB_EVICT * BFS_SLOTS_BLOCK];
    int8_t blk1[BFS_SLOTS_BLOCK];
    char nm[32];

    /* T1: no hooks → v1 codes preserved (48×3-block files tile 144 blocks) */
    {
        BreathingFS fs;
        bfs_init(&fs);
        int rc = 0;
        for (uint32_t k = 0; k < NF_EVICT && rc == 0; k++) {
            oracle_file(blk, k);
            fname(nm, k);
            rc = bfs_write(&fs, nm, blk, sizeof(blk));
        }
        CHECK("T1 fill 144 blocks, all writes ok", rc == 0 && fs.n_blocks_used == 144);
        for (uint32_t i = 0; i < sizeof(blk1); i++) blk1[i] = (int8_t)200;
        rc = bfs_write(&fs, "overflow", blk1, sizeof(blk1));
        CHECK("T1 full write fails -3 (no hooks, v1 code)", rc == -3);
        int allok = 1;
        for (uint32_t k = 0; k < NF_EVICT; k++) allok &= verify_idx(&fs, k);
        CHECK("T1 all 48 files lossless after failed write", allok);
    }

    /* T2: LRU evict through spill; victim bytes land in backend intact */
    {
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, mem_spill, mem_fill, NULL);
        for (uint32_t k = 0; k < NF_EVICT; k++) {
            oracle_file(blk, k);
            fname(nm, k);
            if (bfs_write(&fs, nm, blk, sizeof(blk)) != 0) break;
        }
        /* touch f000 → MRU; oldest is now f001 */
        CHECK("T2 f000 readable (becomes MRU)", verify_idx(&fs, 0));
        for (uint32_t i = 0; i < sizeof(blk1); i++) blk1[i] = (int8_t)200;
        int rc = bfs_write(&fs, "fNEW", blk1, sizeof(blk1));
        CHECK("T2 over-full write succeeds via evict", rc == 0);
        CHECK("T2 residency 142 blocks (144-3 evicted +1 written)",
              fs.n_blocks_used == BFS_BLOCKS - NB_EVICT + 1);
        const SpillSlot *s = spill_find("f001");
        int8_t expect[NB_EVICT * BFS_SLOTS_BLOCK];
        oracle_file(expect, 1);
        CHECK("T2 victim is LRU f001 (not MRU f000)",
              s && s->size == sizeof(expect) && memcmp(s->data, expect, sizeof(expect)) == 0);
        CHECK("T2 f000 untouched by evict", verify_idx(&fs, 0));
        uint32_t act = 0;
        int8_t nb[BFS_SLOTS_BLOCK];
        CHECK("T2 new file readable lossless",
              bfs_read(&fs, "fNEW", nb, sizeof(nb), &act) == 0 &&
              act == sizeof(nb) && memcmp(nb, blk1, sizeof(nb)) == 0);
    }

    /* T3: evicted file faults back transparently via fill */
    {
        /* reuse T2's fs state shape: fresh full FS, evict f001, then read it */
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, mem_spill, mem_fill, NULL);
        for (uint32_t k = 0; k < NF_EVICT; k++) {
            oracle_file(blk, k);
            fname(nm, k);
            if (bfs_write(&fs, nm, blk, sizeof(blk)) != 0) break;
        }
        CHECK("T3 f000 readable (becomes MRU)", verify_idx(&fs, 0));
        for (uint32_t i = 0; i < sizeof(blk1); i++) blk1[i] = (int8_t)200;
        CHECK("T3 over-full write ok", bfs_write(&fs, "fNEW", blk1, sizeof(blk1)) == 0);
        CHECK("T3 evicted f001 faults back lossless", verify_idx(&fs, 1));
        /* fault-back evicts one 3-block file, restores one 3-block file */
        CHECK("T3 residency bounded after fault-back",
              fs.n_blocks_used == BFS_BLOCKS - NB_EVICT + 1);
        int allok = verify_idx(&fs, 0);
        uint32_t act = 0;
        int8_t nb[BFS_SLOTS_BLOCK];
        allok &= (bfs_read(&fs, "fNEW", nb, sizeof(nb), &act) == 0 && act == sizeof(nb) &&
                  memcmp(nb, blk1, sizeof(nb)) == 0);
        CHECK("T3 survivors readable after fault-back churn", allok);
    }

    /* T4: capacity bound — 20 logical files (3 blocks each) in 8 resident */
    {
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, mem_spill, mem_fill, NULL);
        bfs_set_capacity(&fs, 8);
        uint32_t blocks, bytes, cap;
        bfs_residency(&fs, &blocks, &bytes, &cap);
        CHECK("T4 cap reports 8", cap == 8);
        int rc = 0;
        for (uint32_t k = 0; k < 20 && rc == 0; k++) {
            oracle_file(blk, k);
            fname(nm, k);
            rc = bfs_write(&fs, nm, blk, sizeof(blk));
        }
        CHECK("T4 20 writes ok under cap 8", rc == 0);
        bfs_residency(&fs, &blocks, NULL, NULL);
        CHECK("T4 residency <= 8 blocks", blocks <= 8);
        int allok = 1;
        for (uint32_t k = 0; k < 20; k++) allok &= verify_idx(&fs, k);
        CHECK("T4 all 20 files lossless (fault-back)", allok);
        bfs_residency(&fs, &blocks, NULL, NULL);
        CHECK("T4 residency still <= 8 after full fault-back sweep", blocks <= 8);
    }

    /* T5: spill veto → fail loud, nothing lost */
    {
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, veto_spill, NULL, NULL);
        int rc = 0;
        for (uint32_t k = 0; k < NF_EVICT && rc == 0; k++) {
            oracle_file(blk, k);
            fname(nm, k);
            rc = bfs_write(&fs, nm, blk, sizeof(blk));
        }
        CHECK("T5 fill 144 ok", rc == 0);
        for (uint32_t i = 0; i < sizeof(blk1); i++) blk1[i] = (int8_t)200;
        rc = bfs_write(&fs, "overflow", blk1, sizeof(blk1));
        CHECK("T5 vetoed write fails (not silent)", rc != 0);
        int allok = 1;
        for (uint32_t k = 0; k < NF_EVICT; k++) allok &= verify_idx(&fs, k);
        CHECK("T5 all 48 files intact after veto", allok);
    }

    /* T6: default cap is the full field */
    {
        BreathingFS fs;
        bfs_init(&fs);
        uint32_t cap = 0;
        bfs_residency(&fs, NULL, NULL, &cap);
        CHECK("T6 default cap 144 (BFS_BLOCKS)", cap == BFS_BLOCKS);
    }

    /* T7: evict-the-only-file (loop-bound regression: bound must be a
     * snapshot — rereading shrinking n_files exits early with stale -3) */
    {
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, mem_spill, mem_fill, NULL);
        bfs_set_capacity(&fs, NB_EVICT);
        oracle_file(blk, 7);
        fname(nm, 7);
        CHECK("T7 single 3-block file fills cap", bfs_write(&fs, nm, blk, sizeof(blk)) == 0);
        for (uint32_t i = 0; i < sizeof(blk1); i++) blk1[i] = (int8_t)77;
        CHECK("T7 second write evicts the only file",
              bfs_write(&fs, "lone", blk1, sizeof(blk1)) == 0);
        const SpillSlot *s = spill_find(nm);
        CHECK("T7 victim spilled intact", s && s->size == sizeof(blk) &&
              memcmp(s->data, blk, sizeof(blk)) == 0);
        uint32_t act = 0;
        int8_t nb[BFS_SLOTS_BLOCK];
        CHECK("T7 new file lossless",
              bfs_read(&fs, "lone", nb, sizeof(nb), &act) == 0 &&
              act == sizeof(nb) && memcmp(nb, blk1, sizeof(nb)) == 0);
        CHECK("T7 victim faults back lossless", verify_idx(&fs, 7));
    }

    /* T8: planet separation (doctrine §0) — layer 3 off: residency intact,
     * no births, no tombs, no mismatch collection */
    {
        BreathingFS fs;
        bfs_init(&fs);
        CHECK("T8 planets on by default", fs.planets_off == 0);
        bfs_set_planets(&fs, 0);
        oracle_file(blk, 9);
        fname(nm, 9);
        CHECK("T8 write ok (planets off)", bfs_write(&fs, nm, blk, sizeof(blk)) == 0);
        CHECK("T8 no births when off", fs.planets[fe_home(&fs, nm)].magic != PLANET_MAGIC);
        CHECK("T8 read lossless (planets off)", verify_idx(&fs, 9));
        CHECK("T8 delete ok (planets off)", bfs_delete(&fs, nm) == 0);
        CHECK("T8 no tombs when off", fs.tomb_count == 0);
        CHECK("T8 residency books balance", fs.n_blocks_used == 0 && fs.total_bytes == 0);
        /* on-path sanity: fresh FS retires into tombs */
        BreathingFS fs2;
        bfs_init(&fs2);
        CHECK("T8 write ok (planets on)", bfs_write(&fs2, nm, blk, sizeof(blk)) == 0);
        CHECK("T8 births when on", fs2.planets[fe_home(&fs2, nm)].magic == PLANET_MAGIC);
        CHECK("T8 delete retires (planets on)",
              bfs_delete(&fs2, nm) == 0 && fs2.tomb_count == NB_EVICT);
    }

    /* T9: freeride counters (doctrine §5) — hits vs faults vs spills */
    {
        spill_reset();
        BreathingFS fs;
        bfs_init(&fs);
        bfs_set_spill(&fs, mem_spill, mem_fill, NULL);
        bfs_set_capacity(&fs, 3);
        int8_t one[BFS_SLOTS_BLOCK];
        for (uint32_t i = 0; i < sizeof(one); i++) one[i] = (int8_t)(10 + i);
        CHECK("T9 fill cap (A,B,C)", bfs_write(&fs, "A", one, sizeof(one)) == 0 &&
              bfs_write(&fs, "B", one, sizeof(one)) == 0 &&
              bfs_write(&fs, "C", one, sizeof(one)) == 0);
        CHECK("T9 counters idle before reads", bfs_jet_ratio(&fs) == 0 &&
              bfs_jet_alarm(&fs) == 0);
        CHECK("T9 write D evicts LRU A", bfs_write(&fs, "D", one, sizeof(one)) == 0);
        uint64_t dq, db, fq, fb, sq, sb;
        bfs_jet_report(&fs, &dq, &db, &fq, &fb, &sq, &sb);
        CHECK("T9 one spill-out counted", sq == 1 && sb == sizeof(one));
        uint32_t act = 0;
        int8_t got[BFS_SLOTS_BLOCK];
        CHECK("T9 read D hits resident", bfs_read(&fs, "D", got, sizeof(got), &act) == 0);
        CHECK("T9 read A faults back", bfs_read(&fs, "A", got, sizeof(got), &act) == 0 &&
              memcmp(got, one, sizeof(one)) == 0);
        bfs_jet_report(&fs, &dq, &db, &fq, &fb, NULL, NULL);
        CHECK("T9 1 hit + 1 fault counted", dq == 1 && fq == 1 &&
              db == sizeof(one) && fb == sizeof(one));
        CHECK("T9 ratio 500 permille", bfs_jet_ratio(&fs) == 500);
        CHECK("T9 alarm trips above 5%", bfs_jet_alarm(&fs) == 1);
        CHECK("T9 read D hits again", bfs_read(&fs, "D", got, sizeof(got), &act) == 0);
        CHECK("T9 ratio falls to 333", bfs_jet_ratio(&fs) == 333);
    }

    spill_reset();
    printf("PASS: %d | FAIL: %d\n", pass_count, fail_count);
    return fail_count;
}
