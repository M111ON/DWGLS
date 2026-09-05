/* tools/test_scale_follow.c — Prove pointer-following scale uses window memory, not full model.
 *
 * Test: simulate MoE inference pointer (layer 0→36 sequential) accessing
 * tesspack via mmap. Measure peak RSS vs random-touch baseline.
 *
 * Hypothesis: OS page cache keeps only the working window in RAM.
 * Sequential access → automatic scale-following, no explicit management.
 *
 * HDD note: the random phase is BOUNDED by --sample N (page count) and
 * --time S (seconds). Default 8192 pages / 60 s → finishes on any disk.
 * --full restores the original exhaustive sweep (SSD / warm cache only).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_scale_follow.exe tools/test_scale_follow.c -lpsapi
 * RUN:   ./build/test_scale_follow.exe F:/model/qwen3moe.tesspack [--sample N] [--time S] [--full]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <psapi.h>

static size_t peak_rss(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PeakWorkingSetSize;
}

/* Deterministic PRNG (xorshift64) — replayable runs, full 64-bit page space.
 * rand()*rand() on mingw only spans RAND_MAX^2 = 2^30 values. */
static uint64_t rng_state = 12345;
static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <pack> [--sample N] [--time S] [--full]\n", argv[0]);
        printf("  --sample N   max random pages to touch (default 8192)\n");
        printf("  --time S     random-phase time budget in seconds (default 60, 0=off)\n");
        printf("  --full       exhaustive sweep (SSD/warm cache only)\n");
        return 1;
    }
    const char *pack_path = argv[1];

    uint64_t sample_max = 8192;   /* max pages for random phase */
    uint64_t time_cap_s = 60;     /* random-phase time budget   */
    int full = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample_max = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) time_cap_s = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--full")) full = 1;
        else { printf("unknown arg: %s\n", argv[i]); return 1; }
    }
    if (full) { sample_max = UINT64_MAX; time_cap_s = 0; }

    /* Open pack via mmap (read-only, private) */
    HANDLE hf = CreateFileA(pack_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { printf("FAIL: open pack\n"); return 1; }
    LARGE_INTEGER fsz;
    GetFileSizeEx(hf, &fsz);
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) { printf("FAIL: mmap\n"); CloseHandle(hf); return 1; }
    uint8_t *base = (uint8_t*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!base) { printf("FAIL: mapview\n"); CloseHandle(hm); CloseHandle(hf); return 1; }

    uint64_t npages = (uint64_t)fsz.QuadPart / 4096;
    printf("Pack: %s (%.1f MB mmap'd, virtual only, %llu pages)\n",
           pack_path, fsz.QuadPart / 1048576.0, (unsigned long long)npages);
    printf("RSS after mmap (before touch): %.1f MB\n", peak_rss() / 1048576.0);

    /* Phase 1: sequential touch (simulates layer 0→36 inference pointer).
     * Touch every 4096th byte (one per page) in order. OS keeps recent
     * pages, evicts old ones → working window, not full file. */
    size_t acc = 0;
    DWORD t0 = GetTickCount();
    for (uint64_t off = 0; off < (uint64_t)fsz.QuadPart; off += 4096) {
        acc += base[off]; /* touch one byte per page, in order */
    }
    DWORD seq_ms = GetTickCount() - t0;
    size_t seq_rss = peak_rss();
    printf("\nPhase 1 (sequential, layer 0→36 order):\n");
    printf("  time: %lu ms | peak RSS: %.1f MB | checksum: %llu\n",
           (unsigned long)seq_ms, seq_rss / 1048576.0, (unsigned long long)(acc % 1000000));

    /* Phase 2: random touch (worst case — defeats page cache locality).
     * BOUNDED: stops at --sample pages or --time seconds, whichever first.
     * Deterministic xorshift64 seed → same pages on every run. */
    uint64_t budget = sample_max < npages ? sample_max : npages;
    uint64_t cap_ms = time_cap_s * 1000;
    uint64_t touched = 0;
    acc = 0;
    t0 = GetTickCount();
    while (touched < budget) {
        uint64_t p = xorshift64() % npages;
        acc += base[p * 4096];
        touched++;
        if (cap_ms && (touched & 0x3FF) == 0 && GetTickCount() - t0 > cap_ms) break;
    }
    DWORD rnd_ms = GetTickCount() - t0;
    size_t rnd_rss = peak_rss();
    double pages_per_s = rnd_ms > 0 ? (double)touched * 1000.0 / rnd_ms : 0.0;
    double proj_full_s = pages_per_s > 0 ? (double)npages / pages_per_s : 0.0;
    printf("\nPhase 2 (random order%s):\n",
           touched < npages ? ", sampled" : ", full sweep");
    printf("  pages touched: %llu / %llu | time: %lu ms | peak RSS: %.1f MB | checksum: %llu\n",
           (unsigned long long)touched, (unsigned long long)npages,
           (unsigned long)rnd_ms, rnd_rss / 1048576.0, (unsigned long long)(acc % 1000000));
    if (touched < npages) {
        printf("  rate: %.0f pages/s | projected full sweep: %.0f s (skip via --sample/--full)\n",
               pages_per_s, proj_full_s);
    }

    printf("\n═══ Result ═══\n");
    printf("  Sequential RSS: %.1f MB\n", seq_rss / 1048576.0);
    printf("  Random RSS:     %.1f MB\n", rnd_rss / 1048576.0);
    printf("  File size:      %.1f MB\n", fsz.QuadPart / 1048576.0);
    if (seq_rss < (size_t)(fsz.QuadPart * 0.5)) {
        printf("  PASS: sequential uses <50%% of file (window, not full load)\n");
    } else {
        printf("  NOTE: sequential uses >50%% — page cache retains more than window\n");
    }

    UnmapViewOfFile(base);
    CloseHandle(hm);
    CloseHandle(hf);
    return 0;
}
