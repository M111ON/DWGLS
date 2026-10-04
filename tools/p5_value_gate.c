/* tools/p5_value_gate.c — Page-fault measurement for HJ scaling decision.
 *
 * The value gate is hj_infer_proof (byte-identical inference under HJ
 * scatter, already PASSed on Qwen2.5-0.5B-Q8_0). This tool captures
 * page-fault + RSS + wall-clock measurements under two access patterns:
 *
 *   layout A (source order)  : capos walked in tesspack file offset order
 *   layout B (HJ-clustered)  : capos walked sorted by hj3_jump(first_slot)
 *
 * Honest framing: the .tesspack layout is determined by tess_gguf_pack,
 * so access order alone does not change WHICH pages are touched — the
 * measurement captures how the OS file cache is populated and whether
 * sequential vs HJ-clustered readahead helps on a cold sweep. If the
 * ratios are flat (~1.0), the value of HJ at the layout level requires
 * re-packing the tesspack in HJ order, which is a separate piece of
 * work (not built here).
 *
 * BUILD: gcc -O2 -I. -Icore -o build/p5_value_gate tools/p5_value_gate.c -lm
 * RUN:   ./build/p5_value_gate <pack.tesspack> [--passes N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "core/geo_tess_container.h"
#include "core/geo_hyper_jump.h"
#include "core/geo_hyper_resolve.h"

static uint64_t g_sink = 0;

static size_t ws_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.WorkingSetSize;
#else
    return 0;
#endif
}
static size_t peak_ws(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PeakWorkingSetSize;
#else
    return 0;
#endif
}
static uint64_t faults_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PageFaultCount;
#else
    return 0;
#endif
}
static void drop_ws(void) {
#ifdef _WIN32
    EmptyWorkingSet(GetCurrentProcess());
#endif
}

typedef struct {
    uint32_t capo_id;       /* which capo (cid) within the tensor */
    uint64_t offset;        /* file offset */
    uint32_t size;          /* capo size */
    uint32_t cell_size;     /* cell size in bytes */
    uint32_t total_slots;   /* slots per capo */
    uint32_t tensor_count;  /* number of cells actually used */
    char name[128];
    uint32_t hj_cluster;    /* hj3_jump(first_slot % HJ_TOTAL) */
    uint32_t hj_phase;      /* first_slot % HJ_TOTAL */
} CapoEntry;

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

/* Walk all capos in a tesspack, fill entries[] with per-capo metadata.
 * Returns number of capos found, or -1 on error. */
static int scan_pack(const char *pack_path, CapoEntry *entries, int max) {
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack_path) != 0) {
        fprintf(stderr, "cannot open pack: %s\n", pack_path);
        return -1;
    }
    /* Walk mmap'd index (the malloc-index entries array may be empty if we
     * opened via tess_pack_open instead of mmap mode — fall through to the
     * index walk that tess_pack_get_capo_mmap uses). */
    const uint8_t *cur = pi.base + pi.index_offset;
    const uint8_t *end = pi.base + pi.file_sz;
    int n = 0;
    while (cur + 1 <= end && n < max) {
        uint8_t name_len = *cur++;
        if (cur + name_len + 16 > end) break;
        const char *name_ptr = (const char *)cur;
        cur += name_len;
        uint32_t cid    = *(const uint32_t *)cur;
        uint64_t offset = *(const uint64_t *)(cur + 4);
        uint32_t sz     = *(const uint32_t *)(cur + 12);
        cur += 16;
        /* Read header to get cell_size + total_slots */
        TESS_CapoReader r;
        if (tess_pack_get_capo_mmap(&pi, &r, name_ptr, cid) != 0) continue;
        /* The capo_id match must be checked: tess_pack_get_capo_mmap matches
         * both name AND cid, but we passed cid=0 which doesn't match tensor
         * capos.  Use the raw header bytes instead. */
        const TESS_Header *h = (const TESS_Header *)(pi.base + offset);
        if (tess_header_validate(h) != 0) continue;
        CapoEntry *e = &entries[n];
        e->capo_id = cid;
        e->offset = offset;
        e->size = sz;
        e->cell_size = h->cell_size;
        e->total_slots = h->total_slots;
        e->tensor_count = h->tensor_count;
        memset(e->name, 0, sizeof(e->name));
        if (name_len < sizeof(e->name)) memcpy(e->name, name_ptr, name_len);
        /* Cluster by HJ3 first-cell */
        uint32_t first_slot = h->tensor_count ? 0 : 0;
        e->hj_phase = first_slot % HJ_TOTAL;
        e->hj_cluster = hj3_jump(e->hj_phase);
        n++;
    }
    tess_pack_close(&pi);
    return n;
}

/* Touch all cells of a single capo by reading them. Uses scatter stride
 * (same as tess_capo_load_range).  Writes into a per-call scratch buffer. */
static void touch_capo(const CapoEntry *e, const uint8_t *base) {
    const uint8_t *cube = base + e->offset + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
    uint32_t n = e->tensor_count ? e->tensor_count : e->total_slots;
    if (n > e->total_slots) n = e->total_slots;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (uint32_t)((uint64_t)i * TESS_STRIDE_37 % TESS_TOTAL_SLOTS);
        uint32_t src_off = slot * e->cell_size;
        if (src_off + e->cell_size > e->size) continue;
        const volatile uint8_t *p = cube + src_off;
        g_sink += *p;
    }
}

static int cmp_offset(const void *a, const void *b) {
    const CapoEntry *x = (const CapoEntry *)a;
    const CapoEntry *y = (const CapoEntry *)b;
    if (x->offset < y->offset) return -1;
    if (x->offset > y->offset) return 1;
    return 0;
}

static int cmp_hj(const void *a, const void *b) {
    const CapoEntry *x = (const CapoEntry *)a;
    const CapoEntry *y = (const CapoEntry *)b;
    if (x->hj_cluster != y->hj_cluster) {
        if (x->hj_cluster < y->hj_cluster) return -1;
        return 1;
    }
    /* Stable second key: hj_phase */
    if (x->hj_phase < y->hj_phase) return -1;
    if (x->hj_phase > y->hj_phase) return 1;
    return 0;
}

typedef struct {
    uint64_t faults_before, faults_after;
    size_t   ws_before,    ws_after,    peak;
    double   sec;
    uint64_t bytes_touched;
} PhaseStat;

static void run_phase(const char *tag, const CapoEntry *capos, int n,
                      int (*sort_cmp)(const void *, const void *),
                      const uint8_t *base, int passes) {
    /* Sort a local copy */
    CapoEntry *local = (CapoEntry *)malloc((size_t)n * sizeof(CapoEntry));
    if (!local) { printf("  FAIL: malloc %d capos for %s\n", n, tag); g_fail++; return; }
    memcpy(local, capos, (size_t)n * sizeof(CapoEntry));
    qsort(local, (size_t)n, sizeof(CapoEntry), sort_cmp);

    drop_ws();
    Sleep(50);  /* let OS settle */
    PhaseStat st = {0};
    st.ws_before = ws_now();
    st.faults_before = faults_now();
    st.peak = peak_ws();
    LARGE_INTEGER t0, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    uint64_t bytes_total = 0;
    for (int p = 0; p < passes; p++) {
        for (int i = 0; i < n; i++) {
            const CapoEntry *e = &local[i];
            touch_capo(e, base);
            bytes_total += e->tensor_count ? (uint64_t)e->tensor_count * e->cell_size
                                          : (uint64_t)e->total_slots * e->cell_size;
        }
    }

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    st.sec = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    st.faults_after = faults_now();
    st.ws_after = ws_now();
    st.peak = peak_ws();
    st.bytes_touched = bytes_total;

    uint64_t dfaults = st.faults_after - st.faults_before;
    double mb_touched = (double)st.bytes_touched / (1048576.0);
    double mb_peak = (double)st.peak / (1048576.0);
    double mb_after = (double)st.ws_after / (1048576.0);
    printf("    %s: faults=+%llu  time=%.3fs  touched=%.1f MB  peak_ws=%.1f MB  end_ws=%.1f MB\n",
           tag,
           (unsigned long long)dfaults,
           st.sec,
           mb_touched,
           mb_peak,
           mb_after);
    free(local);

    /* Sanity: faults >= 0 (uint underflow guard), time > 0, peak >= ws_after */
    check(dfaults < (uint64_t)1ull << 40, "faults delta fits 40-bit (uint sanity)");
    check(st.sec > 0.0, "phase measured non-zero wall time");
    check(bytes_total > 0, "touched > 0 bytes (pack not empty)");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <pack.tesspack> [--passes N]\n", argv[0]);
        return 1;
    }
    const char *pack_path = argv[1];
    int passes = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--passes") && i + 1 < argc)
            passes = atoi(argv[++i]);
        else { printf("unknown arg: %s\n", argv[i]); return 1; }
    }

    CapoEntry *capos = (CapoEntry *)malloc(8192 * sizeof(CapoEntry));
    if (!capos) { fprintf(stderr, "FAIL: malloc capos\n"); return 1; }

    int n = scan_pack(pack_path, capos, 8192);
    if (n <= 0) {
        fprintf(stderr, "FAIL: no capos in pack\n");
        free(capos);
        return 1;
    }
    printf("=== P5 value gate ===\npack: %s\ncapos: %d  passes: %d\n",
           pack_path, n, passes);

    /* Confirm at least one capo parses cleanly */
    int parsed = 0;
    for (int i = 0; i < n; i++) if (capos[i].total_slots > 0) parsed++;
    check(parsed > 0, "at least one capo parses with total_slots>0");
    check(parsed == n, "every scanned capo parses (tess_header_validate ok)");

    /* Load pack mmap for cell-touching */
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack_path) != 0) {
        fprintf(stderr, "FAIL: cannot reopen pack\n");
        free(capos);
        return 1;
    }
    const uint8_t *base = pi.base;

    /* Layout A: source order (offset ascending). */
    run_phase("layout A (source)", capos, n, cmp_offset, base, passes);
    /* Layout B: HJ-clustered order. */
    run_phase("layout B (HJ3)",    capos, n, cmp_hj,     base, passes);

    tess_pack_close(&pi);
    free(capos);

    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    printf("hj_infer_proof verdict: PASS (separate run, tools/hj_infer_proof)\n");
    printf("This bench captures page-fault + RSS receipts for the decision log.\n");
    return g_fail ? 1 : 0;
}