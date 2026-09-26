/* test_vol6_res.c — residency + BFS-inside proofs. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_vol6_res.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* pool ~3MB: static, never stack. */
static V6Res r;
static V6Res r2;

static uint8_t spill_key[4];
static uint8_t spill_byte0;
static int spill_calls;
static int spill_veto;
static int test_spill(const uint8_t key[4], const uint8_t *data, uint32_t tombs) {
    (void)tombs;
    memcpy(spill_key, key, 4);
    spill_byte0 = data[0];
    spill_calls++;
    return spill_veto;
}

int main(void) {
    v6res_init(&r);

    /* T1: miss attaches, hit returns same cell. */
    V6Cell *a = v6res_touch(&r, 1, 2, 3, 4);
    CHECK(a && a->used, "attach");
    V6Cell *b = v6res_touch(&r, 1, 2, 3, 4);
    CHECK(b == a, "hit same cell");
    CHECK(!v6res_touch(&r, 144, 0, 0, 0), "axis bounds");

    /* T2: fill pool (8), 9th touch evicts LRU (the first key). */
    for (unsigned k = 0; k < V6RES_POOL; k++) v6res_touch(&r, 10, 20, 30, (uint8_t)k);
    V6Cell *ev = v6res_touch(&r, 10, 20, 30, 99);
    CHECK(ev != NULL, "evict attach");
    int found_first = 0, found_last = 0;
    for (unsigned c = 0; c < V6RES_POOL; c++) {
        if (v6res_key_eq(r.cells[c].key, 1, 2, 3, 4)) found_first = 1;
        if (v6res_key_eq(r.cells[c].key, 10, 20, 30, 99)) found_last = 1;
    }
    CHECK(!found_first && found_last, "LRU evicted oldest");

    /* T3: write/read roundtrip across full (j,k). */
    V6Cell *w = v6res_touch(&r, 5, 6, 7, 8);
    for (unsigned j = 0; j < 144; j++)
        for (unsigned k = 0; k < 144; k++)
            CHECK(v6res_write(w, (uint8_t)j, (uint8_t)k,
                              (uint8_t)((j * 37u + k * 11u) & 0xFF)) == 0, "write");
    int ok = 1;
    for (unsigned j = 0; j < 144 && ok; j++)
        for (unsigned k = 0; k < 144 && ok; k++) {
            uint8_t v = 0;
            if (v6res_read(w, (uint8_t)j, (uint8_t)k, &v) != 0 ||
                v != (uint8_t)((j * 37u + k * 11u) & 0xFF)) ok = 0;
        }
    CHECK(ok, "read roundtrip");

    /* T4: BFS inside — sync then bfs_read returns the cell bytes. */
    CHECK(v6res_sync_bfs(w) == 0, "sync_bfs");
    int8_t back[20736];
    uint32_t actual = 0;
    CHECK(bfs_read(&w->bfs, "cell", back, sizeof(back), &actual) == 0, "bfs read");
    CHECK(actual == 20736, "bfs size");
    CHECK(memcmp(back, w->data, sizeof(back)) == 0, "bfs==cell bytes");
    /* block j of the inner BFS is axis-j line: owner + home layout. */
    CHECK(w->bfs.files[0].home_block == 0 && w->bfs.files[0].n_blocks == 144,
          "cell fills blocks [0,144)");
    /* redundant sync skips (tombs survive, bytes identical). */
    CHECK(v6res_sync_bfs(w) == 0, "resync");
    CHECK(bfs_read(&w->bfs, "cell", back, sizeof(back), &actual) == 0, "re-read");
    CHECK(memcmp(back, w->data, sizeof(back)) == 0, "resync bytes");

    /* T5: spill-before-evict with retire semantics. */
    v6res_init(&r2);
    r2.spill = test_spill;
    V6Cell *sa = v6res_touch(&r2, 1, 1, 1, 1);
    CHECK(sa != NULL, "t5 attach");
    CHECK(v6res_write(sa, 0, 0, 0x5A) == 0, "t5 dirty");
    for (unsigned k = 0; k < 7; k++) v6res_touch(&r2, 2, 2, 2, (uint8_t)k);
    spill_calls = 0; spill_veto = 0;
    V6Cell *n = v6res_touch(&r2, 9, 9, 9, 9);   /* evicts dirty (1,1,1,1) */
    CHECK(n != NULL, "t5 evict ok");
    CHECK(spill_calls == 1, "t5 spill called once");
    CHECK(memcmp(spill_key, (uint8_t[]){1,1,1,1}, 4) == 0, "t5 spill key");
    CHECK(spill_byte0 == 0x5A, "t5 spill bytes");
    /* veto keeps the cell. */
    V6Cell *v = v6res_touch(&r2, 3, 3, 3, 3);
    CHECK(v6res_write(v, 0, 0, 0xA5) == 0, "t5 dirty2");
    for (unsigned k = 10; k < 17; k++) v6res_touch(&r2, 4, 4, 4, (uint8_t)k);
    spill_calls = 0; spill_veto = 1;
    CHECK(v6res_touch(&r2, 8, 8, 8, 8) == NULL, "t5 veto NULL");
    CHECK(spill_calls == 1, "t5 veto spill attempted");
    {
        int kept = 0;
        for (unsigned c = 0; c < V6RES_POOL; c++)
            if (v6res_key_eq(r2.cells[c].key, 3, 3, 3, 3)) kept = 1;
        CHECK(kept, "t5 veto keeps cell");
    }
    /* no hook + dirty evict = fail loud (fresh pool: victim is the dirty A). */
    {
        static V6Res r3;
        v6res_init(&r3);   /* spill == NULL */
        V6Cell *d = v6res_touch(&r3, 1, 1, 1, 1);
        CHECK(d != NULL && v6res_write(d, 0, 0, 0x5A) == 0, "t5 dirty3");
        for (unsigned k = 0; k < 7; k++) v6res_touch(&r3, 2, 2, 2, (uint8_t)k);
        CHECK(v6res_touch(&r3, 7, 7, 7, 7) == NULL, "t5 no-hook NULL");
    }

    if (!fails) printf("vol6_res: ALL PASS\n");
    return fails ? 1 : 0;
}
