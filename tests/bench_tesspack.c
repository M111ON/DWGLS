#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../core/geo_tess_container.h"

#ifdef _WIN32
static double now_sec(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}
#endif

int main(int argc, char **argv) {
    const char *pack = argc > 1 ? argv[1] : "F:/model/qwen3moe.tesspack";
    const char *gguf = argc > 2 ? argv[2] : "F:/model/qwen3-4b-moe-q4_k_m.gguf";
    printf("=== Tesspack Throughput Benchmark ===\nPack: %s\nGGUF: %s\n\n", pack, gguf);
    fflush(stdout);

    /* --- Part 1: File I/O baseline --- */
    printf("=== Part 1: Raw File I/O ===\n"); fflush(stdout);
    {
        FILE *f = fopen(gguf, "rb"); _fseeki64(f, 0, SEEK_END); int64_t sz = _ftelli64(f); fclose(f);
        char *buf = (char *)malloc(64*1024*1024);
        double t0 = now_sec();
        f = fopen(gguf, "rb"); size_t rd; int64_t total = 0;
        while ((rd = fread(buf, 1, 64*1024*1024, f)) > 0) total += (int64_t)rd;
        fclose(f);
        double dt = now_sec() - t0;
        printf("  GGUF seq read:    %.3f s  %.1f MB/s  (%.1f MB)\n", dt, total/dt/1e6, total/1e6);
        free(buf);
    }
    {
        FILE *f = fopen(pack, "rb"); _fseeki64(f, 0, SEEK_END); int64_t sz = _ftelli64(f); fclose(f);
        char *buf = (char *)malloc(64*1024*1024);
        double t0 = now_sec();
        f = fopen(pack, "rb"); size_t rd; int64_t total = 0;
        while ((rd = fread(buf, 1, 64*1024*1024, f)) > 0) total += (int64_t)rd;
        fclose(f);
        double dt = now_sec() - t0;
        printf("  Pack seq read:    %.3f s  %.1f MB/s  (%.1f MB)\n", dt, total/dt/1e6, total/1e6);
        free(buf);
    }
    fflush(stdout);

    /* --- Part 2: Pack index + mmap open --- */
    printf("\n=== Part 2: Pack Index ===\n"); fflush(stdout);
    double t0 = now_sec();
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack) != 0) { printf("FAIL\n"); return 1; }
    double dt = now_sec() - t0;
    printf("  tess_pack_open:   %.3f s  (%d entries, %.1f MB file)\n", dt, pi.n_entries, pi.file_sz/1e6);
    fflush(stdout);

    /* group by name */
    int n_tensors = 0;
    char names[512][256];
    int counts[512];
    memset(counts, 0, sizeof(counts));
    for (uint32_t i = 0; i < pi.n_entries && n_tensors < 512; i++) {
        int found = -1;
        for (int t = 0; t < n_tensors; t++)
            if (strcmp(names[t], pi.entries[i].name) == 0) { found = t; break; }
        if (found < 0) { found = n_tensors; strncpy(names[n_tensors], pi.entries[i].name, 255); names[n_tensors][255] = 0; n_tensors++; }
        counts[found]++;
    }
    printf("  Grouped:          %d unique tensors\n", n_tensors);

    int n_moe = 0;
    for (int t = 0; t < n_tensors; t++) if (counts[t] > 1) n_moe++;
    printf("  Multi-capo (MoE): %d tensors\n", n_moe);
    fflush(stdout);

    /* --- Part 3: MoE streaming benchmark (all tensors) --- */
    printf("\n=== Part 3: MoE Streaming (all %d multi-capo tensors) ===\n", n_moe); fflush(stdout);

    /* allocate max buffer */
    int max_need = 0;
    for (int t = 0; t < n_tensors; t++) {
        if (counts[t] > 1) {
            /* find max cell_size from first capo */
            TESS_CapoReader cr;
            if (tess_pack_get_capo(&pi, &cr, names[t], 0) == 0) {
                int need = cr.n_elems * cr.cell_size + 4096;
                if (need > max_need) max_need = need;
            }
        }
    }
    uint8_t *buf = (uint8_t *)malloc(max_need + 4096);
    printf("  Buffer: %d bytes\n", max_need); fflush(stdout);

    /* stream: 4/64 experts per MoE tensor */
    printf("\n  --- Stream (4/64 experts) ---\n"); fflush(stdout);
    double best_s = 1e9;
    for (int r = 0; r < 3; r++) {
        double t0 = now_sec();
        uint64_t total = 0; int reads = 0;
        for (int t = 0; t < n_tensors; t++) {
            if (counts[t] <= 1) continue;
            int cpe = counts[t] / 64; if (cpe < 1) cpe = 1;
            for (int e = 0; e < 4; e++) {
                for (int c = 0; c < cpe; c++) {
                    int idx = e * cpe + c;
                    if (idx >= counts[t]) break;
                    TESS_CapoReader cr;
                    if (tess_pack_get_capo(&pi, &cr, names[t], (uint32_t)idx) == 0) {
                        int got = tess_capo_load_range(&cr, 0, (uint32_t)cr.n_elems, buf);
                        total += (uint64_t)(got > 0 ? got : 0);
                        reads++;
                    }
                }
            }
        }
        double dt = now_sec() - t0;
        printf("    R%d: %.3f s  %d reads  %.1f MB  %.1f MB/s\n", r+1, dt, reads, total/1e6, total/dt/1e6);
        fflush(stdout);
        if (dt < best_s) best_s = dt;
    }

    /* full: all experts */
    printf("  --- Full (all experts) ---\n"); fflush(stdout);
    double best_f = 1e9;
    for (int r = 0; r < 3; r++) {
        double t0 = now_sec();
        uint64_t total = 0; int reads = 0;
        for (int t = 0; t < n_tensors; t++) {
            if (counts[t] <= 1) continue;
            for (int c = 0; c < counts[t]; c++) {
                TESS_CapoReader cr;
                if (tess_pack_get_capo(&pi, &cr, names[t], (uint32_t)c) == 0) {
                    int got = tess_capo_load_range(&cr, 0, (uint32_t)cr.n_elems, buf);
                    total += (uint64_t)(got > 0 ? got : 0);
                    reads++;
                }
            }
        }
        double dt = now_sec() - t0;
        printf("    R%d: %.3f s  %d reads  %.1f MB  %.1f MB/s\n", r+1, dt, reads, total/1e6, total/dt/1e6);
        fflush(stdout);
        if (dt < best_f) best_f = dt;
    }

    /* --- Part 4: Summary --- */
    printf("\n=== Summary ===\n");
    printf("  Stream (4/64):  %.3f s\n", best_s);
    printf("  Full (64/64):   %.3f s\n", best_f);
    printf("  Speedup:        %.2fx\n", best_f / best_s);
    printf("  Bandwidth savings: %.1f%%\n", (1.0 - best_s / best_f) * 100);

    free(buf);
    tess_pack_close(&pi);
    printf("\nDone.\n");
    return 0;
}
