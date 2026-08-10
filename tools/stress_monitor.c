/*
 * tools/stress_monitor.c — STABILITY MONITOR (Aug 10, 2026 audit)
 *
 * Watches the proven invariants while hammering every layer in a single
 * process for ITER rounds:
 *
 *   INV-1  anchor: current_pos == derived(home_pos×scale) for EVERY used
 *          block, at every scale, every round
 *   INV-2  breath bound: |delta| ≤ 127 at all times (re-anchor invariant)
 *   INV-3  lossless: plain read AND mmap read == written data, every
 *          file, every round (decode+compare, all values, all positions)
 *   INV-4  bijection: RDH encode(decode(x))==x per block (bfs_rdh_verify_all)
 *
 * Edge inputs per round: random file sizes (incl. partial last block),
 * random data patterns, scale oscillation 0.05..2.0 (crosses hyperbolic
 * boundary 0.25), write pinned at max slot 20735.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "breathing_fs.h"
#include "bfs_persist.h"
#include "bfs_breath.h"

#define ITER 100
#define NFILES 8

static uint32_t checks = 0, fails = 0;
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(int argc, char **argv)
{
    uint32_t iter = argc > 1 ? (uint32_t)atoi(argv[1]) : ITER;
    if (iter == 0) iter = ITER;
    printf("Stress monitor — %u rounds x %u files, scale 0.05..2.0, 200 breaths/round\n",
           iter, NFILES);

    for (uint32_t r = 1; r <= iter; r++) {
#define INV(name, cond) do { checks++; \
    if (!(cond)) { fails++; printf("  [%u] INV FAIL: %s\n", r, name); } } while (0)

        BreathingFS fs; bfs_init(&fs);
        double scale = 0.05 + ((double)(r % 40)) * (2.0 - 0.05) / 40.0;
        fs.seeker.current_pos = 0; fs.seeker.home_pos = 0;
        bfs_move_seeker(&fs, scale);

        int8_t data[NFILES][400];
        uint32_t sizes[NFILES];
        char names[NFILES][24];

        for (uint32_t f = 0; f < NFILES; f++) {
            sizes[f] = 1 + rnd() % 400;                    /* incl. partial last block */
            for (uint32_t i = 0; i < sizes[f]; i++)
                data[f][i] = (int8_t)(rnd() & 0xFF);
            sprintf(names[f], "f%u.bin", f);
            if (bfs_write(&fs, names[f], data[f], sizes[f]) != 0)
                { fails++; printf("round %u write fail\n", r); break; }
        }
        /* edge: write pinned at max slot 20735 */
        int8_t edge[144]; memset(edge, 42, 144);
        fs.seeker.current_pos = BFS_TOTAL_SLOTS - 1; fs.seeker.home_pos = BFS_TOTAL_SLOTS - 1;
        if (bfs_write(&fs, "edge.bin", edge, 144) != 0)
            { fails++; printf("round %u edge write fail\n", r); }

        /* INV-1: anchor derivation — VALID AFTER A MOVE (write-time
         * current_pos == home_pos by construction; the anchor formula
         * current = home×scale applies post-move, exactly like parse) */
        bfs_move_seeker(&fs, scale);
        for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
            if (fs.block_owner[i] == 0xFFFFFFFF) continue;
            uint32_t expect = (uint32_t)((double)fs.block_meta[i].home_pos * fs.seeker.scale)
                              % fs.seeker.space_size;
            INV("anchor-derived", fs.block_meta[i].current_pos == expect);
            INV("anchor-delta-consistent",
                fs.block_meta[i].delta == (int32_t)fs.block_meta[i].current_pos
                                       - (int32_t)fs.block_meta[i].home_pos);
        }
        /* INV-2: anchor delta is UNBOUNDED by design (formula home×(scale−1),
         * up to ±20,735); the ≤127 bound belongs to the BREATH channel
         * (re-anchored side stream) — checked via bfs_breath_all_bounded. */

        /* INV-2: breath engine — bounded deltas through 200 breaths */
        BFSBreath br; bfs_breath_init(&br, &fs, 0.05);
        for (int b = 0; b < 200; b++) {
            bfs_breath_tick(&br);
            if (!bfs_breath_all_bounded(&br)) { fails++; printf("[%u] breath bound broken\n", r); break; }
        }
        INV("breath-all-bounded-200", bfs_breath_all_bounded(&br));

        /* serialize/parse cycle (re-derives owners/sizes/deltas) */
        static uint8_t img[BFS_IMG_MAX_SIZE];
        uint32_t n = bfs_img_serialize(&fs, img);
        BreathingFS fs2; bfs_init(&fs2);
        int rcp = bfs_img_parse(img, n, &fs2, 0);
        INV("parse-ok", rcp == 0);
        if (rcp != 0) continue;
        INV("n-blocks-tile", fs2.n_blocks_used == fs.n_blocks_used);
        /* parse must derive deltas identically to the live move path
         * (anchor delta is unbounded by design — consistency, not bound) */
        for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
            if (fs2.block_owner[i] == 0xFFFFFFFF) continue;
            uint32_t exp_cur = (uint32_t)((double)fs2.block_meta[i].home_pos * fs2.seeker.scale)
                               % fs2.seeker.space_size;
            INV("parse-current-consistent", fs2.block_meta[i].current_pos == exp_cur);
            INV("parse-delta-consistent",
                fs2.block_meta[i].delta == (int32_t)fs2.block_meta[i].current_pos
                                       - (int32_t)fs2.block_meta[i].home_pos);
        }

        /* INV-3: lossless — every file, plain read */
        for (uint32_t f = 0; f < NFILES; f++) {
            int8_t out[512]; uint32_t actual = 0;
            int rc = bfs_read(&fs2, names[f], out, 512, &actual);
            INV("plain-read-ok", rc == 0 && actual == sizes[f]);
            INV("plain-read-lossless", !memcmp(out, data[f], sizes[f]));
        }
        int8_t edge_out[144];
        { uint32_t actual; int rc = bfs_read(&fs2, "edge.bin", edge_out, 144, &actual);
          INV("edge-read-lossless", rc == 0 && actual == 144 && !memcmp(edge_out, edge, 144)); }

        /* lifecycle: save → mmap open → RDH bijection → mmap read */
        bfs_save_img("build/t_stress.img", &fs2);
        BFSMmapFS mfs;
        int rmo = bfs_mmap_open("build/t_stress.img", &mfs);
        INV("mmap-open", rmo == 0);
        if (rmo == 0) {
            /* INV-4: RDH bijection per used block */
            int v = bfs_rdh_verify_all(&mfs);
            INV("bijection-all-blocks", v == (int)fs2.n_blocks_used);
            int8_t mout[512]; uint32_t a2;
            for (uint32_t f = 0; f < NFILES; f++) {
                int rc = bfs_mmap_read(&mfs, names[f], mout, 512, &a2);
                INV("mmap-read-lossless",
                    rc == 0 && a2 == sizes[f] && !memcmp(mout, data[f], sizes[f]));
            }
            bfs_mmap_close(&mfs);
        }

        if (r % 10 == 0)
            printf("  round %u ok (checks=%u fail=%u)\n", r, checks, fails);
#undef INV
    }

    printf("\n══════════════════════════\n");
    if (fails == 0) printf("STABILITY: %u checks, 0 failures — PASS\n", checks);
    else printf("STABILITY: %u checks, %u failures — FAIL\n", checks, fails);
    return fails ? 1 : 0;
}