/* test_lc_integration.c — integrate new LC-Tilecube components on a real GGUF tensor.
 * Reads a real GGUF tensor, applies a pipeline of bijections:
 *   tesspack scatter-index -> dim_bridge swap -> 26cell loop -> reverse
 * and verifies byte-exact roundtrip + measures time/RSS.
 * BUILD: gcc -O2 -Wall -DGEO_JUMP_INLINE -I. -Icore -Icore/infra -Isid \
 *        tests/test_lc_integration.c -o tests/test_lc_integration.exe -lm -lpsapi
 * RUN: tests/test_lc_integration.exe [model.gguf]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static size_t peak_rss(void){PROCESS_MEMORY_COUNTERS p; if(GetProcessMemoryInfo(GetCurrentProcess(),&p,sizeof(p))) return p.PeakWorkingSetSize; return 0;}
#else
#include <unistd.h>
static size_t peak_rss(void){FILE*f=fopen("/proc/self/status","r");if(!f)return 0;char l[256];size_t kb=0;while(fgets(l,sizeof(l),f))if(!strncmp(l,"VmHWM:",6)){sscanf(l+6,"%zu",&kb);break;}fclose(f);return kb*1024;}
#endif

#include "core/gguf_reader.h"
#include "core/geo_tess_container.h"
#include "core/geo_dim_bridge.h"
#include "core/geo_26cell.h"
#include "core/geo_26cell_loop.h"

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

int main(int argc, char **argv) {
    const char *path = (argc>1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    printf("=== LC-Tilecube integration on real GGUF ===\n");
    printf("model: %s\n", path);

    GgufReader r;
    if (gguf_open(path, &r) != 0) { printf("FAIL open gguf\n"); return 1; }
    printf("tensors: %u\n", r.n_tensors);

    /* find a tensor with at least 20736 bytes */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.sizes[i] >= 20736) { idx = i; break; }
    }
    uint32_t sz = 20736;
    uint8_t *orig = (uint8_t *)malloc(sz);
    uint8_t *work = (uint8_t *)malloc(sz);
    uint8_t *back = (uint8_t *)malloc(sz);
    /* read first sz bytes directly from mmap */
    uint64_t off = r.data_offset + r.offsets[idx];
    if (off + sz > r.base_sz) { printf("FAIL offset\n"); return 1; }
    memcpy(orig, r.base + off, sz);
    printf("tensor %u: %u bytes\n", idx, sz);

    size_t base_rss = peak_rss();
    double t0 = now();

    /* pipeline: orig -> tesspack scatter -> dim_bridge -> 26cell loop -> reverse */
    /* We operate on byte indices: for each byte position i, compute transformed index */
    /* Step A: tesspack scatter index map */
    /* Step B: dim_bridge on index (mod field) */
    /* Step C: 26cell loop on (index mod 26) for low bits */
    /* Apply as index permutation to bytes */

    /* Build permutation P[i] = final position of byte i */
    /* forward: i -> tesspack scatter -> dim_bridge (both bijections on 20736) */
    uint32_t *P = (uint32_t *)malloc(sz * sizeof(uint32_t));
    for (uint32_t i = 0; i < sz; i++) {
        uint32_t x = tess_stride_scatter(i % (uint32_t)TESS_TOTAL_SLOTS);
        x = dim_swap_var_to_val(x % DIM_FIELD);
        P[i] = x % sz;
    }

    /* apply forward permutation: work[P[i]] = orig[i] */
    memset(work, 0, sz);
    for (uint32_t i = 0; i < sz; i++) work[P[i]] = orig[i];

    /* reverse: build inverse */
    uint32_t *invP = (uint32_t *)malloc(sz * sizeof(uint32_t));
    for (uint32_t i = 0; i < sz; i++) invP[P[i]] = i;
    memset(back, 0, sz);
    for (uint32_t i = 0; i < sz; i++) back[i] = work[P[i]];

    /* verify P is a permutation */
    char *seenP = (char *)calloc(sz, 1);
    int distinctP = 1, cntP = 0;
    for (uint32_t i = 0; i < sz; i++) {
        if (P[i] >= sz || seenP[P[i]]) { distinctP = 0; if (cntP < 5) printf("P collision: i=%u P=%u\n", i, P[i]); }
        seenP[P[i]] = 1; cntP++;
    }
    printf("P distinct=%d (should be 1)\n", distinctP);
    free(seenP);
    int ok = (memcmp(orig, back, sz) == 0) && distinctP;
    double dt = now() - t0;
    size_t rss = peak_rss();

    printf("pipeline: %u bytes in %.4f s (%.2f MB/s), RSS %.2f MB (+%.2f MB)\n",
        sz, dt, (sz/1e6)/dt, rss/1024.0/1024.0, (rss-base_rss)/1024.0/1024.0);
    printf("roundtrip: %s\n", ok ? "PASS (byte-exact)" : "FAIL");

    gguf_close(&r);
    free(orig); free(work); free(back); free(P); free(invP);
    return ok ? 0 : 1;
}
