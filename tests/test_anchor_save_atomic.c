/* test_anchor_save_atomic.c — does anch_save_atomic actually behave atomically?
 *
 * The patch (commit 9dc2af9 "anchor: atomic save without remove gap") replaced
 * remove(path) + rename() with flush-then-MoveFileExA(REPLACE_EXISTING |
 * WRITE_THROUGH). "Looks right" is not a receipt, so this gate drives the real
 * function and checks the properties the patch exists for:
 *
 *  T1 first save lands and loads back byte-exact
 *  T2 a second save REPLACES the file (old K gone, new K present) — the case
 *     the old remove+rename hardware kept failing at
 *  T3 no .tmp survives a successful save
 *  T4 a stale .tmp left by a crashed writer is overwritten, not trusted
 *  T5 a failed save leaves the previous file INTACT (read-only target, Windows)
 *  T6 a failed save leaves nothing behind (unwritable path)
 *  T7 the atomic path is byte-identical to the plain anch_save
 *  T8 200 successive saves: every one loads, every size is exact (no torn file)
 *
 * 0 = all green. Exit 1 on the first failure, printing which one.
 */
#include "anchor_route.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KMAX 8
#define DMAX 6
#define PATH "build/anch_atomic.bin"
#define TMP  PATH ".tmp"

static int read_all(const char *p, unsigned char *buf, int cap, int *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    *len = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return 0;
}

static void fill(float *C, int K, int dim, float seed) {
    for (int k = 0; k < K; k++)
        for (int d = 0; d < dim; d++)
            C[k * dim + d] = seed * (float)(k + 1) - (float)d * 0.125f;
}

int main(void) {
    static float C1[KMAX * DMAX], C2[KMAX * DMAX], W[KMAX * DMAX];
    static unsigned char b1[4096], b2[4096];
    int l1 = 0, l2 = 0;

    remove(PATH);
    remove(TMP);

    /* T1 first save */
    fill(C1, 2, 4, 1.5f);
    if (anch_save_atomic(PATH, C1, 2, 4, 7) != 0) { printf("FAIL T1 save\n"); return 1; }
    memset(W, 0, sizeof(W));
    int dim = 0, ntr = 0, K = anch_load(PATH, W, KMAX, DMAX, &dim, &ntr);
    if (K != 2 || dim != 4 || ntr != 7) { printf("FAIL T1 load hdr K=%d dim=%d ntr=%d\n", K, dim, ntr); return 1; }
    if (memcmp(W, C1, sizeof(float) * 8) != 0) { printf("FAIL T1 payload\n"); return 1; }

    /* T2 replace: different K and dim must be what comes back */
    fill(C2, 5, 3, -2.0f);
    if (anch_save_atomic(PATH, C2, 5, 3, 11) != 0) { printf("FAIL T2 save\n"); return 1; }
    memset(W, 0, sizeof(W));
    K = anch_load(PATH, W, KMAX, DMAX, &dim, &ntr);
    if (K != 5 || dim != 3 || ntr != 11) { printf("FAIL T2 hdr K=%d dim=%d ntr=%d\n", K, dim, ntr); return 1; }
    if (memcmp(W, C2, sizeof(float) * 15) != 0) { printf("FAIL T2 payload not replaced\n"); return 1; }

    /* T3 no tmp left */
    FILE *t = fopen(TMP, "rb");
    if (t) { fclose(t); printf("FAIL T3 tmp leftover after success\n"); return 1; }

    /* T4 stale tmp from a crashed writer must be overwritten */
    t = fopen(TMP, "wb");
    if (!t) { printf("FAIL T4 setup\n"); return 1; }
    fwrite("GARBAGE-GARBAGE-GARBAGE", 1, 23, t);
    fclose(t);
    if (anch_save_atomic(PATH, C1, 2, 4, 7) != 0) { printf("FAIL T4 save over stale tmp\n"); return 1; }
    K = anch_load(PATH, W, KMAX, DMAX, &dim, &ntr);
    if (K != 2 || memcmp(W, C1, sizeof(float) * 8) != 0) { printf("FAIL T4 stale tmp leaked into target\n"); return 1; }
    t = fopen(TMP, "rb");
    if (t) { fclose(t); printf("FAIL T4 tmp leftover\n"); return 1; }

    /* T5 failed replace must not eat the good file.
     * Windows: a read-only target makes MoveFileExA fail with ACCESS_DENIED,
     * which is the interesting branch — the old code removed the file first
     * and would have lost it here. POSIX rename() succeeds over a read-only
     * file, so this case only exists on Windows. */
    fill(C2, 3, 2, 9.0f);
#ifdef _WIN32
    if (SetFileAttributesA(PATH, FILE_ATTRIBUTE_READONLY)) {
        int rc = anch_save_atomic(PATH, C2, 3, 2, 12);
        SetFileAttributesA(PATH, FILE_ATTRIBUTE_NORMAL);
        if (rc == 0) { printf("FAIL T5 save reported success on read-only target\n"); return 1; }
        K = anch_load(PATH, W, KMAX, DMAX, &dim, &ntr);
        if (K != 2 || memcmp(W, C1, sizeof(float) * 8) != 0) {
            printf("FAIL T5 previous file lost/corrupted on failed replace (K=%d)\n", K);
            return 1;
        }
        t = fopen(TMP, "rb");
        if (t) { fclose(t); printf("FAIL T5 tmp leftover after failed replace\n"); return 1; }
    }
#endif

    /* T6 unwritable path */
    if (anch_save_atomic("/nonexistent/dir/x.bin", C2, 3, 2, 1) == 0) {
        printf("FAIL T6 bad path accepted\n"); return 1;
    }

    /* T7 byte-identical to plain anch_save */
    if (anch_save("build/anch_plain.bin", C2, 5, 3, 11) != 0) { printf("FAIL T7 plain save\n"); return 1; }
    if (anch_save_atomic(PATH, C2, 5, 3, 11) != 0) { printf("FAIL T7 atomic save\n"); return 1; }
    if (read_all("build/anch_plain.bin", b1, sizeof(b1), &l1) != 0 ||
        read_all(PATH, b2, sizeof(b2), &l2) != 0) { printf("FAIL T7 read\n"); return 1; }
    if (l1 != l2 || memcmp(b1, b2, (size_t)l1) != 0) {
        printf("FAIL T7 atomic not byte-identical (%d vs %d bytes)\n", l1, l2);
        return 1;
    }

    /* T8 hammer: 200 saves, each must be complete */
    for (int i = 0; i < 200; i++) {
        int k = 1 + (i % KMAX);
        fill(C2, k, 3, (float)i);
        if (anch_save_atomic(PATH, C2, k, 3, i) != 0) { printf("FAIL T8 save %d\n", i); return 1; }
        memset(W, 0, sizeof(W));
        int kk = anch_load(PATH, W, KMAX, DMAX, &dim, &ntr);
        if (kk != k || ntr != i || memcmp(W, C2, sizeof(float) * k * 3) != 0) {
            printf("FAIL T8 torn file at iteration %d (K=%d ntr=%d)\n", i, kk, ntr);
            return 1;
        }
    }
    t = fopen(TMP, "rb");
    if (t) { fclose(t); printf("FAIL T8 tmp leftover\n"); return 1; }

    remove(PATH);
    remove("build/anch_plain.bin");
    printf("ok\n");
    return 0;
}
