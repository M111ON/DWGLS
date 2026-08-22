/*
 * tools/bw_probe.c — raw memory READ bandwidth probe (multi-thread)
 * ══════════════════════════════════════════════════════════════════
 * Measures sustainable read bandwidth over a large buffer with N
 * threads (XOR-fold over cache lines, same pattern as cube sweeps).
 * This gives the honest ceiling number for any weight-serving layer.
 *
 * BUILD: gcc -O2 -Wall -o build/bw_probe tools/bw_probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <process.h>
#include <windows.h>

#define BUF_MB    1024u
#define BUF_SZ    ((size_t)BUF_MB * 1024u * 1024u)

typedef struct {
    const uint8_t *buf;
    size_t lo, hi;
    uint64_t xo;
} SliceArg;

static unsigned __stdcall slice_xor(void *p) {
    SliceArg *a = (SliceArg *)p;
    uint64_t x = 0;
    for (size_t i = a->lo; i + 64 <= a->hi; i += 64) {
        const uint64_t *q = (const uint64_t *)(a->buf + i);
        x ^= q[0] ^ q[1] ^ q[2] ^ q[3] ^ q[4] ^ q[5] ^ q[6] ^ q[7];
    }
    a->xo = x;
    return 0;
}

static double run_pass(uint8_t *buf, int nthreads) {
    HANDLE th[16]; SliceArg sa[16];
    LARGE_INTEGER f, c0, c1;
    QueryPerformanceFrequency(&f);
    size_t per = BUF_SZ / nthreads;
    per &= ~(size_t)63;
    QueryPerformanceCounter(&c0);
    for (int k = 0; k < nthreads; k++) {
        sa[k].buf = buf;
        sa[k].lo = (size_t)k * per;
        sa[k].hi = (k == nthreads - 1) ? BUF_SZ : sa[k].lo + per;
        th[k] = (HANDLE)_beginthreadex(NULL, 0, slice_xor, &sa[k], 0, NULL);
    }
    for (int k = 0; k < nthreads; k++) {
        WaitForSingleObject(th[k], INFINITE);
        CloseHandle(th[k]);
    }
    QueryPerformanceCounter(&c1);
    return (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

int main(void) {
    uint8_t *buf = (uint8_t *)VirtualAlloc(NULL, BUF_SZ,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buf) { printf("alloc fail\n"); return 1; }
    memset(buf, 7, BUF_SZ);                      /* pre-fault */

    /* warm */
    run_pass(buf, 1);

    printf("buffer %u MB · XOR-read bandwidth\n", BUF_MB);
    printf("%4s %10s %10s\n", "thr", "ms", "GB/s");
    double best = 0;
    int thr[] = { 1, 2, 4, 8, 16 };
    for (int t = 0; t < 5; t++) {
        /* best of 3 */
        double b = 1e9;
        for (int r = 0; r < 3; r++) {
            double ms = run_pass(buf, thr[t]);
            if (ms < b) b = ms;
        }
        double gbs = (double)BUF_SZ / 1e9 / (b / 1000.0);
        if (gbs > best) best = gbs;
        printf("%4d %10.1f %10.2f\n", thr[t], b, gbs);
    }
    printf("peak: %.2f GB/s\n", best);
    VirtualFree(buf, 0, MEM_RELEASE);
    return 0;
}
