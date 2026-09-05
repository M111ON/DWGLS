/* tools/test_scale_follow.c — Prove pointer-following scale uses window memory, not full model.
 *
 * Test: simulate MoE inference pointer (layer 0→36 sequential) accessing
 * tesspack via mmap. Measure peak RSS vs full-touch baseline.
 *
 * Hypothesis: OS page cache keeps only the working window in RAM.
 * Sequential access → automatic scale-following, no explicit management.
 *
 * BUILD: gcc -O2 -o build/test_scale_follow tools/test_scale_follow.c -I. -Icore
 * RUN: ./build/test_scale_follow F:/model/qwen3moe.tesspack
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

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <pack>\n", argv[0]); return 1; }
    const char *pack_path = argv[1];

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

    printf("Pack: %s (%.1f MB mmap'd, virtual only)\n", pack_path, fsz.QuadPart / 1048576.0);
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
    printf("  time: %u ms | peak RSS: %.1f MB | checksum: %zu\n",
           seq_ms, seq_rss / 1048576.0, acc % 1000000);

    /* Phase 2: random touch (worst case — defeats page cache locality).
     * Same number of pages, random order. Forces more pages resident. */
    srand(12345);
    uint64_t npages = fsz.QuadPart / 4096;
    acc = 0;
    t0 = GetTickCount();
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t p = ((uint64_t)rand() * rand()) % npages;
        acc += base[p * 4096];
    }
    DWORD rnd_ms = GetTickCount() - t0;
    size_t rnd_rss = peak_rss();
    printf("\nPhase 2 (random order, same page count):\n");
    printf("  time: %u ms | peak RSS: %.1f MB | checksum: %zu\n",
           rnd_ms, rnd_rss / 1048576.0, acc % 1000000);

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
