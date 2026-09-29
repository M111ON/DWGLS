/* apex_probe.c — hourglass hidden-value test (experiment, not a committed test).
 *
 * Question: does bfs_write/read roundtrip hide a position-structured residue
 * (apex doctrine: 3walk-1skip, every-4th-unit skip) or is real-data failure
 * purely value/entropy-driven (known result #6198)?
 *
 * 4 datasets x full 20736 B field (= 144 blocks, whole FS):
 *   A smooth : ramp (structured, low entropy)
 *   B random : seeded rand (unstructured, high entropy)
 *   C real   : Qwen2.5-0.5B Q8_0 bytes @1MB (real, high entropy + real order)
 *   D sorted : C sorted ascending (same multiset, order destroyed)
 *
 * Pre-registered verdicts (printed before results):
 *   C fails && A,B,D pass -> HIDDEN-STRUCTURE signal (order-dependent, real-only)
 *   B,C,D fail && A passes -> ENTROPY explains it (#6198), no apex needed
 *   all pass               -> old episode closed (current codec covers it)
 * On any failure: failed-block list + bi%4 histogram (skip-pos prediction:
 *   doctrine says every 4th unit is the skip -> failures should cluster bi%4==3)
 *   + per-block distinct-byte stats failed-vs-ok (entropy counter-test).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/breathing_fs.h"

#define N 20736u
#define NB 144u
#define BB 144u

static int8_t d_smooth[N], d_rand[N], d_real[N], d_sorted[N];
static int8_t out[N];
static BreathingFS fs;

static int distinct_blk(const int8_t *p) {
    int seen[256] = {0}, c = 0;
    for (uint32_t i = 0; i < BB; i++) {
        unsigned char v = (unsigned char)p[i];
        if (!seen[v]) { seen[v] = 1; c++; }
    }
    return c;
}
static int cmpu(const void *a, const void *b) {
    return (int)*(const unsigned char *)a - (int)*(const unsigned char *)b;
}

static void run_one(const char *name, const int8_t *data, int have) {
    if (!have) { printf("%-7s SKIP (no data)\n", name); return; }
    memset(&fs, 0, sizeof(fs));
    bfs_init(&fs);
    int wrc = bfs_write(&fs, name, data, N);
    uint32_t actual = 0;
    int rrc = -99, bad = -1;
    if (wrc == 0) {
        rrc = bfs_read(&fs, name, out, N, &actual);
        if (rrc == 0 && actual == N) {
            for (uint32_t i = 0; i < N; i++)
                if (out[i] != data[i]) { bad = (int)i; break; }
        }
    }
    /* per-block stats */
    uint32_t pay_sum = 0, pay_min = 0xFFFFFFFF, pay_max = 0;
    int strat_cnt[8] = {0};
    for (uint32_t b = 0; b < NB; b++) {
        uint32_t ps = fs.block_encoded_size[b];
        pay_sum += ps;
        if (ps < pay_min) pay_min = ps;
        if (ps > pay_max) pay_max = ps;
        if (fs.block_meta[b].strategy < 8) strat_cnt[fs.block_meta[b].strategy]++;
    }
    printf("%-7s wrc=%-3d rrc=%-3d %s", name, wrc, rrc,
           (wrc == 0 && rrc == 0 && bad < 0) ? "PASS" : "FAIL");
    if (bad >= 0) printf(" first_bad=%d (block %d)", bad, bad / BB);
    printf(" avg_pay=%.1f min=%u max=%u strat:", (double)pay_sum / NB, pay_min, pay_max);
    for (int s = 0; s < 8; s++) if (strat_cnt[s]) printf(" %d:%d", s, strat_cnt[s]);
    printf("\n");
    if (!(wrc == 0 && rrc == 0 && bad < 0)) {
        /* failure anatomy: which blocks, skip-pos histogram, entropy split */
        int hist[4] = {0}, nf = 0, dsum_fail = 0, dsum_ok = 0, nok = 0;
        printf("  failed blocks:");
        for (uint32_t b = 0; b < NB; b++) {
            int blk_bad = 0;
            if (rrc != 0) {
                blk_bad = 1; /* whole read failed; attribute per-block via re-decode below */
            } else if (bad >= 0 && (uint32_t)(bad / BB) == b) {
                blk_bad = 1;
            } else {
                /* check this block's bytes even if first_bad is elsewhere */
                for (uint32_t i = 0; i < BB; i++)
                    if (out[b * BB + i] != data[b * BB + i]) { blk_bad = 1; break; }
            }
            int d = distinct_blk(data + b * BB);
            if (blk_bad) { printf(" %u", b); hist[b % 4]++; nf++; dsum_fail += d; }
            else { nok++; dsum_ok += d; }
        }
        printf("\n  bi%%4 hist: 0:%d 1:%d 2:%d 3:%d (skip-pos predicts cluster at 3)\n",
               hist[0], hist[1], hist[2], hist[3]);
        printf("  distinct/block: failed_avg=%.1f ok_avg=%.1f\n",
               nf ? (double)dsum_fail / nf : -1.0, nok ? (double)dsum_ok / nok : -1.0);
    }
}

int main(void) {
    printf("PREDICT: C-fail&ABD-pass=HIDDEN-STRUCTURE | BCD-fail&A-pass=ENTROPY(#6198) | all-pass=CLOSED\n");
    for (uint32_t i = 0; i < N; i++) d_smooth[i] = (int8_t)((i % 256) - 128);
    srand(1);
    for (uint32_t i = 0; i < N; i++) d_rand[i] = (int8_t)(rand() % 256 - 128);
    int have_real = 0;
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (f) {
        fseek(f, 1048576L, SEEK_SET);
        if (fread(d_real, 1, N, f) == N) {
            have_real = 1;
            memcpy(d_sorted, d_real, N);
            qsort(d_sorted, N, 1, cmpu);
        }
        fclose(f);
    }
    run_one("A-smooth", d_smooth, 1);
    run_one("B-random", d_rand, 1);
    run_one("C-real", d_real, have_real);
    run_one("D-sorted", d_sorted, have_real);
    return 0;
}
