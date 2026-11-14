/* bench_dwgls_memory.c — standalone memory bench for geo_jump / hyper_jump / tesspack.
 * Measures peak working set (RSS) while exercising each component.
 * BUILD (Windows/MSYS): gcc -O2 -Wall -DGEO_JUMP_INLINE -I. -Icore -Icore/infra -Isid bench_dwgls_memory.c -o bench_dwgls_memory.exe -lm -lpsapi
 * BUILD (Linux/molab):  gcc -O2 -Wall -DGEO_JUMP_INLINE -I. -Icore -Icore/infra -Isid bench_dwgls_memory.c -o bench_dwgls_memory -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static size_t peak_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (size_t)pmc.PeakWorkingSetSize;
    return 0;
}
#else
#include <unistd.h>
static size_t peak_rss_bytes(void) {
    /* read VmHWM from /proc/self/status */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) { sscanf(line+6, "%zu", &kb); break; }
    }
    fclose(f);
    return kb * 1024;
}
#endif

#include "core/geo_hyper_jump.h"
#include "core/geo_tess_container.h"
#include "sid/geo_jump.h"

#define N 20736u

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec/1e9;
}

int main(void) {
    printf("=== DWGLS memory bench ===\n");
    size_t base = peak_rss_bytes();
    printf("baseline RSS: %.2f MB\n", base/1024.0/1024.0);

    /* 1. geo_jump: full walk over all 20736 nodes for all types */
    {
        uint32_t *buf = (uint32_t *)malloc(N * sizeof(uint32_t));
        double t0 = now_sec();
        uint64_t acc = 0;
        for (int t = 0; t < 8; t++)
            for (uint32_t x = 0; x < N; x++) { uint32_t j = geo_jump(x, (GeoJumpType)t, 7); buf[x]=j; acc += j; }
        double dt = now_sec() - t0;
        size_t rss = peak_rss_bytes();
        printf("geo_jump: 8 types x %u nodes in %.4f s (%.2f Mops/s), buf=%d B, peak RSS %.2f MB (+%.2f MB)\n",
            N, dt, (8.0*N/1e6)/dt, (int)((size_t)N*sizeof(uint32_t)), rss/1024.0/1024.0, (rss-base)/1024.0/1024.0);
        free(buf);
        (void)acc;
    }

    /* 2. hyper_jump: tower/span functions over 144 slots */
    {
        double t0 = now_sec();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < 1000000u; i++) {
            uint32_t p = i % N;
            acc += hj3_tower(p) + hj3_local(p) + hj4_tower(p) + hj4_local(p)
                 + hj_antipode(p, N) + hj3_jump(p) + hj4_jump(p);
        }
        double dt = now_sec() - t0;
        size_t rss = peak_rss_bytes();
        printf("hyper_jump: 1M iters in %.4f s, acc=%lu, peak RSS %.2f MB (+%.2f MB)\n",
            dt, (unsigned long)acc, rss/1024.0/1024.0, (rss-base)/1024.0/1024.0);
    }

    /* 3. tesspack: stride scatter over 20736 slots */
    {
        uint32_t *slots = (uint32_t *)malloc(N * sizeof(uint32_t));
        double t0 = now_sec();
        for (uint32_t i = 0; i < N; i++) slots[i] = tess_stride_scatter(i);
        double dt = now_sec() - t0;
        /* verify bijective: count distinct */
        char *seen = (char *)calloc(N, 1);
        uint32_t distinct = 0;
        for (uint32_t i = 0; i < N; i++) if (!seen[slots[i] % N]) { seen[slots[i] % N]=1; distinct++; }
        size_t rss = peak_rss_bytes();
        printf("tesspack: scatter %u slots in %.4f s, distinct=%u/%u, peak RSS %.2f MB (+%.2f MB)\n",
            N, dt, distinct, N, rss/1024.0/1024.0, (rss-base)/1024.0/1024.0);
        free(slots); free(seen);
    }

    printf("=== done ===\n");
    return 0;
}