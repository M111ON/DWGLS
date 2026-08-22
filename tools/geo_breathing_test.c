/*
 * tools/geo_breathing_test.c — PROVE scale≠1 read on REAL GGUF weights
 * ══════════════════════════════════════════════════════════════════════
 * Breathing with REAL physical transforms (v2 — post tautology fix):
 *
 *   bake @scale1      : part p at phys slot p (through iso_fold)
 *   E1 EXPAND ×2      : bytes relocate p -> 2p (holes at odd slots)
 *   E2 SHUFFLE S₃ v4  : bytes relocate via cycle permutation in-cube
 *   E3 UNSHUFFLE S₃ v5: exact inverse of E2          (LIFO!)
 *   E4 COLLAPSE ÷2    : bytes relocate back -> p     (inverse of E1)
 *
 * Rules learned the hard way:
 *   - breathe IN changes the layout -> bytes MUST relocate (copy),
 *     otherwise "read at new position" finds nothing (v1 lesson)
 *   - collapse after an unmatched shuffle hits odd slots -> INVALID;
 *     the log unwinds LIFO like a stack (deterministic replay)
 *   - expand copies DESCENDING (dest 2p > p, never clobbers pending src);
 *     collapse copies ASCENDING (dest p/2 < p, same argument)
 *
 * Oracle: memcmp vs SOURCE file slices + XOR digest. Carried state
 * between breaths = the event list (~8 B/event).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

#include "../core/gguf_reader.h"
#include "../core/iso_fold.h"
#include "../core/kis_cube_views.h"

#define PART_BYTES (128u * 1024u)
#define N_CUBES    12u
#define MAX_PARTS  (N_CUBES * 1728u)

typedef enum { EV_EXPAND, EV_SHUFFLE, EV_UNSHUFFLE, EV_COLLAPSE } EvType;
typedef struct { EvType type; uint32_t view; } Event;
static Event evlog[64];
static uint32_t ev_n = 0;

/* forward map of ONE event on a flat id */
static int ev_apply(const Event *e, uint32_t *g) {
    switch (e->type) {
    case EV_EXPAND:
        if ((uint64_t)*g * 2u >= MAX_PARTS) return -1;
        *g *= 2u; return 0;
    case EV_SHUFFLE:
    case EV_UNSHUFFLE: {
        uint32_t k = *g / KIS_CUBE, rr = *g % KIS_CUBE;
        *g = k * KIS_CUBE + kis_view6_slot(e->view, rr);
        return 0;
    }
    case EV_COLLAPSE:
        if (*g & 1u) return -1;
        *g /= 2u; return 0;
    }
    return -1;
}

static inline size_t part_offset(uint32_t f) {
    uint32_t tes = f / ISO_TES_SIZE;
    uint32_t rem = f % ISO_TES_SIZE;
    IsoFold fo   = iso_fold(tes, rem / ISO_TES_SLOTS, rem % ISO_TES_SLOTS);
    return (size_t)iso_unfold(&fo) * PART_BYTES;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open\n"); return 1; }

    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    printf("=== geo_breathing_test v2 — REAL relocations on REAL weights ===\n");
    printf("%s\nparts %u (%.1f MB)\n\n", path, total_parts, (double)total_bytes / 1e6);
    if ((uint64_t)total_parts * 2u > MAX_PARTS) { printf("FAIL: needs x2 headroom\n"); return 1; }

    typedef struct { const uint8_t *src; uint32_t len; } PartJob;
    PartJob *job = (PartJob *)malloc(sizeof(PartJob) * total_parts);
    uint32_t *cur = (uint32_t *)malloc(sizeof(uint32_t) * total_parts);
    if (!job || !cur) { printf("FAIL alloc jobs\n"); return 1; }
    {
        uint32_t f = 0;
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            const uint8_t *src = r.base + r.data_offset + r.offsets[i];
            uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
            for (uint32_t p = 0; p < np; p++, f++) {
                uint32_t off = p * PART_BYTES;
                uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
                job[f].src = src + off;
                job[f].len = len;
            }
        }
    }

    uint8_t *win  = (uint8_t *)VirtualAlloc(NULL,
        (size_t)MAX_PARTS * PART_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *tmpw = (uint8_t *)VirtualAlloc(NULL,
        (size_t)MAX_PARTS * PART_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!win || !tmpw) { printf("FAIL alloc window\n"); return 1; }

    /* ── BAKE @ scale1 ───────────────────────────────────────────────── */
    double t0 = now_ms();
    for (uint32_t f = 0; f < total_parts; f++) {
        memcpy(win + part_offset(f), job[f].src, job[f].len);
        cur[f] = f;
    }
    printf("BAKE   %u parts @ scale1 · %.0f ms\n", total_parts, now_ms() - t0);

    /* oracle digest over padded chunks in the WINDOW (order-free) */
    uint64_t ref_xor(void) {
        uint64_t x = 0;
        for (uint32_t g = 0; g < MAX_PARTS; g++) {
            const uint8_t *c = win + part_offset(g);
            for (uint32_t b = 0; b < PART_BYTES; b += 64)
                x ^= *(const uint64_t *)(c + b)     ^ *(const uint64_t *)(c + b + 8)
                  ^  *(const uint64_t *)(c + b + 16) ^ *(const uint64_t *)(c + b + 24)
                  ^  *(const uint64_t *)(c + b + 32) ^ *(const uint64_t *)(c + b + 40)
                  ^  *(const uint64_t *)(c + b + 48) ^ *(const uint64_t *)(c + b + 56);
        }
        return x;
    }
    uint64_t xor_at_bake = ref_xor();

    int failures = 0;

    #define VERIFY_STATE(label) do { \
        uint32_t ok = 0, badm = 0; \
        t0 = now_ms(); \
        for (uint32_t f = 0; f < total_parts; f++) \
            if (memcmp(win + part_offset(cur[f]), job[f].src, job[f].len) != 0) badm++; \
            else ok++; \
        printf("VERIFY %-20s %u/%u ok · %.0f ms · %s\n", \
               label, ok, total_parts, now_ms() - t0, badm ? "FAIL" : "PASS"); \
        failures += badm; \
    } while (0)

    /* ── E1 EXPAND ×2 : relocate DESCENDING p->2p, zero vacated odds ─── */
    APPLY_LABEL:;
    t0 = now_ms();
    for (uint32_t f = total_parts; f-- > 0;) {
        memcpy(tmpw + part_offset(f * 2u), win + part_offset(f), PART_BYTES);
    }
    for (uint32_t f = 0; f < total_parts * 2u; f++) {
        uint8_t *d = win + part_offset(f);
        memcpy(d, tmpw + part_offset(f), PART_BYTES);
        if (f & 1u) memset(d, 0, PART_BYTES);      /* holes = ODD slots only */
    }
    for (uint32_t f = 0; f < total_parts; f++)
        if (ev_apply(&(Event){EV_EXPAND, 0}, &cur[f]) != 0) { printf("FAIL rng\n"); return 1; }
    evlog[ev_n++] = (Event){EV_EXPAND, 0};
    printf("E1 EXPAND ×2   · %.0f ms · carried %u B\n", now_ms() - t0,
           ev_n * (uint32_t)sizeof(Event));
    VERIFY_STATE("after expand");

    /* holes: odd slots must be zeros */
    {
        uint32_t checked = 0, dirty = 0;
        for (uint32_t g = 1; g < total_parts * 2u && checked < 200; g += 2, checked++) {
            const uint8_t *c = win + part_offset(g);
            for (uint32_t b = 0; b < PART_BYTES; b++)
                if (c[b]) { dirty++; break; }
        }
        printf("HOLES  odd slots %u/%u clean · %s\n",
               checked - dirty, checked, dirty ? "FAIL" : "PASS");
        failures += dirty ? 1 : 0;
    }

    /* ── E2 SHUFFLE S₃ v4 : snapshot@old -> scatter to new ───────────── */
    t0 = now_ms();
    {
        uint32_t *oldp = (uint32_t *)malloc(sizeof(uint32_t) * total_parts);
        memcpy(oldp, cur, sizeof(uint32_t) * total_parts);
        for (uint32_t f = 0; f < total_parts; f++)
            memcpy(tmpw + part_offset(oldp[f]), win + part_offset(oldp[f]), PART_BYTES);
        for (uint32_t f = 0; f < total_parts; f++)
            if (ev_apply(&(Event){EV_SHUFFLE, 4}, &cur[f]) != 0) { printf("FAIL rng\n"); return 1; }
        for (uint32_t f = 0; f < total_parts; f++)
            memcpy(win + part_offset(cur[f]), tmpw + part_offset(oldp[f]), PART_BYTES);
        /* zero VACATED origins only: origins that are nobody's
           destination (in a pure bijection this set is empty) */
        {
            static uint8_t mark[MAX_PARTS];
            memset(mark, 0, sizeof(mark));
            for (uint32_t f = 0; f < total_parts; f++) mark[cur[f]] = 1;
            for (uint32_t f = 0; f < total_parts; f++)
                if (!mark[oldp[f]])
                    memset(win + part_offset(oldp[f]), 0, PART_BYTES);
        }
        free(oldp);
    }
    evlog[ev_n++] = (Event){EV_SHUFFLE, 4};
    printf("E2 SHUFFLE v4  · %.0f ms · carried %u B\n", now_ms() - t0,
           ev_n * (uint32_t)sizeof(Event));
    VERIFY_STATE("after shuffle");

    /* bijection: all cur distinct */
    {
        uint8_t *seen = (uint8_t *)calloc(MAX_PARTS, 1);
        uint32_t dup = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            if (seen[cur[f]]) dup++;
            seen[cur[f]] = 1;
        }
        free(seen);
        printf("BIJECT unique: %u dups · %s\n", dup, dup ? "FAIL" : "PASS");
        failures += dup ? 1 : 0;
    }

    /* ── E3 UNSHUFFLE v5 (exact inverse) : same snapshot->scatter ────── */
    t0 = now_ms();
    {
        uint32_t *oldp = (uint32_t *)malloc(sizeof(uint32_t) * total_parts);
        memcpy(oldp, cur, sizeof(uint32_t) * total_parts);
        for (uint32_t f = 0; f < total_parts; f++)
            memcpy(tmpw + part_offset(oldp[f]), win + part_offset(oldp[f]), PART_BYTES);
        for (uint32_t f = 0; f < total_parts; f++)
            if (ev_apply(&(Event){EV_UNSHUFFLE, 5}, &cur[f]) != 0) { printf("FAIL rng\n"); return 1; }
        for (uint32_t f = 0; f < total_parts; f++)
            memcpy(win + part_offset(cur[f]), tmpw + part_offset(oldp[f]), PART_BYTES);
        /* zero VACATED origins only */
        {
            static uint8_t mark[MAX_PARTS];
            memset(mark, 0, sizeof(mark));
            for (uint32_t f = 0; f < total_parts; f++) mark[cur[f]] = 1;
            for (uint32_t f = 0; f < total_parts; f++)
                if (!mark[oldp[f]])
                    memset(win + part_offset(oldp[f]), 0, PART_BYTES);
        }
        free(oldp);
    }
    evlog[ev_n++] = (Event){EV_UNSHUFFLE, 5};
    printf("E3 UNSHUFFLE v5 · %.0f ms · carried %u B\n", now_ms() - t0,
           ev_n * (uint32_t)sizeof(Event));
    VERIFY_STATE("after unshuffle");

    /* ── E4 COLLAPSE ÷2 : relocate ASCENDING, zero vacated odds ──────── */
    t0 = now_ms();
    for (uint32_t f = 0; f < total_parts; f++) {
        memcpy(tmpw + part_offset(f), win + part_offset(f * 2u), PART_BYTES);
    }
    for (uint32_t f = 0; f < total_parts * 2u; f++) {
        uint8_t *d = win + part_offset(f);
        memcpy(d, tmpw + part_offset(f), PART_BYTES);
        if (f >= total_parts) memset(d, 0, PART_BYTES);
    }
    for (uint32_t f = 0; f < total_parts; f++)
        if (ev_apply(&(Event){EV_COLLAPSE, 0}, &cur[f]) != 0) { printf("FAIL rng\n"); return 1; }
    evlog[ev_n++] = (Event){EV_COLLAPSE, 0};
    printf("E4 COLLAPSE ÷2 · %.0f ms · carried %u B\n", now_ms() - t0,
           ev_n * (uint32_t)sizeof(Event));
    VERIFY_STATE("home again");

    /* xor sweep vs bake-time digest (whole padded window) */
    uint64_t xnow = ref_xor();
    printf("XOR    whole-window vs bake: %s\n",
           xnow == xor_at_bake ? "MATCH" : "MISMATCH");
    failures += (xnow == xor_at_bake) ? 0 : 1;

    /* diagnose: expected byte model = src+len pad zeros, zeros beyond N */
    {
        uint32_t bad_slots = 0, first_g = 0, first_b = 0;
        for (uint32_t g = 0; g < MAX_PARTS; g++) {
            const uint8_t *exp;
            uint32_t explen = 0;
            static const uint8_t zeros[PART_BYTES];
            if (g < total_parts) { exp = job[g].src; explen = job[g].len; }
            else exp = zeros;
            const uint8_t *c = win + part_offset(g);
            uint32_t mism = 0;
            for (uint32_t b = 0; b < PART_BYTES; b++) {
                uint8_t e = b < explen ? exp[b] : 0;
                if (c[b] != e) { if (!mism) { first_g = g; first_b = b; } mism++; }
            }
            if (mism) {
                if (!bad_slots) printf("DIAG first diff @slot %u byte %u\n", first_g, first_b);
                bad_slots++;
            }
        }
        printf("DIAG   %u/%u slots differ from expected model\n",
               bad_slots, MAX_PARTS);
        failures += bad_slots ? 1 : 0;
    }

    printf("\nRESULT: %s · carried state = %u events (%u bytes)\n",
           failures ? "FAILED" : "BREATHING LOSSLESS ON REAL WEIGHTS",
           ev_n, ev_n * (uint32_t)sizeof(Event));

    free(job); free(cur);
    VirtualFree(win, 0, MEM_RELEASE);
    VirtualFree(tmpw, 0, MEM_RELEASE);
    gguf_close(&r);
    return failures ? 1 : 0;
}
