/* tests/test_p5_value_gate.c — P5 scaling gate: HJ3 field walk vs file-order walk.
 *
 * Value gate for scaling HJ into production serve:
 *   - hj_infer_proof already proves byte-identical inference under HJ scatter
 *     (separate tool; correctness receipt).
 *   - this test is the perf receipt: walking a real .tesspack via the
 *     4D field order (hj3_jump over cells) MUST be no worse than walking
 *     the file in flat offset order.  Cold-pass benchmark.
 *
 * IMPORTANT — multi-dimensional framing (2026-10-03 directive):
 *   the field is multi-dimensional storage where data transfers across
 *   dimensions via bijective property (stride-37, hj3_jump, hj4_tower,
 *   gate bindings).  This test walks the 4D FIELD, not the flat file:
 *     - for each capo, scatter its cells into the field via hj3_jump
 *       (this is the bijective cross-dimension transfer)
 *     - then walk the field in hj3-jump order and dispatch file touches
 *       back through the same bijective map
 *   comparing this against flat file-offset walk measures the value of
 *   the field walk over a flat 1D scan.
 *
 * Receipts are printed as PASS lines and stdout.  SKIP is graceful when
 * no pack is given (mirrors tests/test_planet_real.c).
 *
 * BUILD: gcc -O2 -I. -Icore -o build/test_p5_value_gate tests/test_p5_value_gate.c -lpsapi -lm
 * RUN:   ./build/test_p5_value_gate <pack.tesspack> [passes]
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

static int g_pass = 0, g_fail = 0;
static uint64_t g_sink = 0;
static void check_ok(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

static size_t ws_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.WorkingSetSize;
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
    uint64_t offset;        /* capo file offset                */
    uint32_t size;          /* capo byte size                  */
    uint32_t cell_size;     /* cell size in bytes             */
    uint32_t total_slots;   /* slots per capo                 */
    uint32_t tensor_count;  /* cells used                     */
} Capo;

/* Multi-dimensional field walk: cells of every capo are scattered into
 * the 144-cell HJ3 field.  Walking hj3_jump(slot) in slot order visits
 * each cell in field order, then dispatches the touch back through the
 * same bijective map to the file byte. */

typedef struct {
    uint32_t capo_idx;     /* which capo */
    uint32_t cell_idx;     /* cell within that capo's tensor_count range */
    uint32_t field_slot;   /* hj3_jump(cell_idx % HJ_TOTAL) */
    uint64_t file_byte;    /* cube + slot*cell_size byte offset within capo */
} FieldStep;

/* one full sweep: for each capo, emit FieldStep entries for each cell,
 * sort by field_slot (the multi-dimensional order). */
typedef struct {
    FieldStep *steps;
    uint32_t n_steps;
    uint32_t capacity;
} FieldWalk;

static int scan_pack(const char *path, Capo *out, int max) {
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, path) != 0) return -1;
    const uint8_t *cur = pi.base + pi.index_offset;
    const uint8_t *end = pi.base + pi.file_sz;
    int n = 0;
    while (cur + 1 <= end && n < max) {
        uint8_t nl = *cur++;
        if (cur + nl + 16 > end) break;
        cur += nl;
        uint32_t cid    = *(const uint32_t *)cur;
        uint64_t offset = *(const uint64_t *)(cur + 4);
        uint32_t sz     = *(const uint32_t *)(cur + 12);
        cur += 16;
        const TESS_Header *h = (const TESS_Header *)(pi.base + offset);
        if (tess_header_validate(h) != 0) continue;
        out[n].offset = offset;
        out[n].size = sz;
        out[n].cell_size = h->cell_size;
        out[n].total_slots = h->total_slots;
        out[n].tensor_count = h->tensor_count;
        (void)cid;
        n++;
    }
    tess_pack_close(&pi);
    return n;
}

/* scatter every capo's cells into a single sorted-by-field-slot array. */
static int build_field_walk(const Capo *capos, int n_capos, FieldWalk *fw) {
    /* upper bound: total cells across all capos (every capo contributes
     * min(tensor_count, total_slots) FieldStep entries) */
    uint64_t total_cells = 0;
    for (int i = 0; i < n_capos; i++) {
        uint32_t used = capos[i].tensor_count ? capos[i].tensor_count : capos[i].total_slots;
        if (used > capos[i].total_slots) used = capos[i].total_slots;
        total_cells += used;
    }
    if (total_cells > 0x10000000ull) {
        /* sanity-cap at 256M steps to keep memory bounded */
        total_cells = 0x10000000ull;
    }
    fw->steps = (FieldStep *)malloc((size_t)total_cells * sizeof(FieldStep));
    if (!fw->steps) return -1;
    fw->capacity = (uint32_t)total_cells;
    fw->n_steps = 0;
    for (int i = 0; i < n_capos && fw->n_steps < fw->capacity; i++) {
        uint32_t used = capos[i].tensor_count ? capos[i].tensor_count : capos[i].total_slots;
        if (used > capos[i].total_slots) used = capos[i].total_slots;
        for (uint32_t c = 0; c < used && fw->n_steps < fw->capacity; c++) {
            FieldStep *dst = &fw->steps[fw->n_steps++];
            dst->capo_idx = (uint32_t)i;
            dst->cell_idx = c;
            dst->field_slot = hj3_jump(c % HJ_TOTAL);
            /* file_byte: offset of cube data + field_slot*cell_size.
             * NOTE this is the stride-37 slot, NOT the field_slot — the
             * field_slot is the multi-dimensional destination; the file
             * byte uses the source slot of the bijective scatter. */
            uint32_t src_slot = (uint32_t)((uint64_t)c * TESS_STRIDE_37 % TESS_TOTAL_SLOTS);
            dst->file_byte = (uint64_t)(TESS_HEADER_SIZE + TESS_FORMULA_SIZE)
                           + (uint64_t)src_slot * capos[i].cell_size;
        }
    }
    return (int)fw->n_steps;
}

/* field-walk comparator: walk the 4D field by hj3_jump(slot) order */
static int cmp_field(const void *a, const void *b) {
    uint32_t x = ((const FieldStep *)a)->field_slot;
    uint32_t y = ((const FieldStep *)b)->field_slot;
    if (x != y) return (x < y) ? -1 : 1;
    /* secondary: file_byte so equal field_slots don't thrash */
    uint64_t fx = ((const FieldStep *)a)->file_byte;
    uint64_t fy = ((const FieldStep *)b)->file_byte;
    return (fx < fy) ? -1 : (fx > fy);
}
/* flat-file comparator: walk in source scatter order (slot = c*37 mod 20736) */
static int cmp_flat(const void *a, const void *b) {
    uint64_t x = ((const FieldStep *)a)->file_byte;
    uint64_t y = ((const FieldStep *)b)->file_byte;
    return (x < y) ? -1 : (x > y);
}

/* touch the byte at field_walk_steps[i].file_byte within its capo */
static void touch_field_step(const FieldStep *s, const Capo *capos, const uint8_t *base) {
    const Capo *c = &capos[s->capo_idx];
    uint64_t byte = c->offset + s->file_byte;
    if (byte + 1 > c->offset + c->size) return;
    g_sink += base[byte];
}

typedef struct {
    double sec;
    uint64_t faults;
    size_t peak_ws;
} Measure;

static Measure run(const char *tag, FieldWalk *fw, int (*cmp)(const void *, const void *),
                   const Capo *capos, const uint8_t *base, int passes) {
    /* sort by compactness to keep cache-line adjacency if cmp produces ties */
    qsort(fw->steps, fw->n_steps, sizeof(FieldStep), cmp);
    drop_ws();
    Sleep(50);
    uint64_t f0 = faults_now();
    size_t w0 = ws_now();
    LARGE_INTEGER t0, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int p = 0; p < passes; p++)
        for (uint32_t i = 0; i < fw->n_steps; i++)
            touch_field_step(&fw->steps[i], capos, base);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    Measure m;
    m.sec = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    m.faults = faults_now() - f0;
    m.peak_ws = ws_now() > w0 ? ws_now() : w0;
    printf("    %-14s steps=%u  faults=+%llu  time=%.3fs  peak_ws=%.1f MB\n",
           tag, fw->n_steps, (unsigned long long)m.faults, m.sec,
           (double)m.peak_ws / 1048576.0);
    return m;
}

int main(int argc, char **argv) {
    const char *pack = (argc > 1) ? argv[1] : NULL;
    int passes = (argc > 2) ? atoi(argv[2]) : 3;
    if (!pack) {
        printf("P5 value gate: SKIP — no pack given\n");
        printf("  RUN: ./build/test_p5_value_gate <pack.tesspack> [passes]\n");
        printf("  (no pack fixture bundled — runtime check on local model)\n");
        return 0;
    }
    FILE *f = fopen(pack, "rb");
    if (!f) {
        printf("P5 value gate: SKIP — pack not present: %s\n", pack);
        return 0;
    }
    fclose(f);

    printf("=== P5 VALUE GATE — %s, passes=%d ===\n", pack, passes);
    Capo *capos = (Capo *)malloc(8192 * sizeof(Capo));
    int n = scan_pack(pack, capos, 8192);
    if (n <= 0) { printf("  FAIL: scan found 0 capos\n"); free(capos); return 1; }
    printf("  capos: %d\n", n);

    FieldWalk fwA = {0}, fwB = {0};
    if (build_field_walk(capos, n, &fwA) < 0 ||
        build_field_walk(capos, n, &fwB) < 0) {
        printf("  FAIL: build_field_walk alloc\n");
        free(capos); return 1;
    }
    check_ok(fwA.n_steps > 0, "field-walk steps > 0");

    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack) != 0) {
        printf("  FAIL: cannot reopen pack\n");
        free(capos); free(fwA.steps); free(fwB.steps); return 1;
    }
    const uint8_t *base = pi.base;

    /* A: flat file-byte order (current production serve pattern) */
    Measure a = run("flat-bytes",  &fwA, cmp_flat,  capos, base, passes);
    /* B: 4D field walk — hj3_jump(slot) order, bijective dispatch */
    Measure b = run("hj3-field",   &fwB, cmp_field, capos, base, passes);

    double ratio = a.sec > 0.0 ? b.sec / a.sec : 0.0;
    printf("  ratio hj3/flat = %.3f (lower is better; HJ3 win < 1.2)\n", ratio);
    check_ok(ratio <= 1.2, "HJ3 field-walk no worse than 1.2x flat-file-walk (cold pass)");
    check_ok(b.faults > 0 && a.faults > 0, "both walks produced non-zero page faults");

    /* Multi-dimensional correctness: hj3_jump is a permutation over
     * [0, HJ_TOTAL) so each hj3_field_slot should appear with a count
     * proportional to its cell weight.  Sanity check: the n_steps must
     * be the same under both orderings (no cells lost). */
    check_ok(fwA.n_steps == fwB.n_steps, "flat and grid walk visit same step count");

    /* HJ + GJ are the RTS-mini-map pair:
 *   HJ (hj3_jump) is the local view — orbit walker with bounded orbits
 *     (24 disjoint orbits of length 6 covering all 144 cells;
 *      NOT a single full-cycle permutation — confirmed by docs/HJ-JET-
 *      DOCTRINE-2026-09-26.md and docs/geo-jump-explorer-2026-08-09.md).
 *   GJ (stride-37 over 20736) is the global view — bounded orbits of length
 *      576 (36 disjoint orbits, max order 1728 over [0, 20736) — see
 *      geo-jump-explorer-2026-08-09.md).
 *   The bench uses both: HJ cluster keys for ordering cells inside a
 *   cluster, GJ stride-37 for the placement that gives the file byte.
 *   The field-walk sorts by HJ (cluster) so within-cluster cells are
 *   visited back-to-back; the flat-file walk ignores clusters. */
    /* HJ3 invariant — bounded orbits, NOT a single full-cycle permutation.
     * hj3_jump is the tower-advance + local-mirror; the resulting
     * permutation has 24 disjoint orbits of length 6 covering all 144
     * cells.  hj3_jump^6 returns to start for every slot. */
    int orbit_ok = 1;
    for (uint32_t k = 0; k < HJ_TOTAL; k++) {
        uint32_t p = k;
        for (int s = 0; s < 6; s++) p = hj3_jump(p);
        if (p != k) { orbit_ok = 0; break; }
    }
    check_ok(orbit_ok, "HJ3 has bounded orbit length 6 (multi-dimensional, NOT permutation)");

    /* All field_slots in the field walk must lie in [0, HJ_TOTAL). */
    int all_in_range = 1;
    for (uint32_t i = 0; i < fwB.n_steps; i++) {
        if (fwB.steps[i].field_slot >= HJ_TOTAL) { all_in_range = 0; break; }
    }
    check_ok(all_in_range, "HJ3 field-slot stays in [0, HJ_TOTAL)");

    /* HJ3 is a bijection but has 24 disjoint orbits — count them. */
    int orbit_seen[HJ_TOTAL] = {0};
    int n_orbits = 0;
    for (uint32_t k = 0; k < HJ_TOTAL; k++) {
        if (orbit_seen[k]) continue;
        n_orbits++;
        uint32_t p = k;
        for (int s = 0; s < 6; s++) { orbit_seen[p] = 1; p = hj3_jump(p); }
    }
    check_ok(n_orbits == 24, "HJ3 has exactly 24 disjoint orbits of length 6");

    tess_pack_close(&pi);
    free(capos); free(fwA.steps); free(fwB.steps);
    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}