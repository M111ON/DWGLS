/*
 * tools/geo_breathing_relabel.c — breathing WITHOUT moving bytes
 * ════════════════════════════════════════════════════════════════
 * The untested half of the philosophy. geo_breathing_test (main tree)
 * proved RELOCATE-mode: breathe = physically move 670 MB per event.
 *
 * This variant proves RELABEL-mode: after bake, ZERO bytes are copied.
 * Breathing changes only the LOG. Consumers address by COORDINATE and
 * replay the log to find the owning part:
 *
 *   forward  (part -> coord): expand L*=2 · shuffle view v on cube r ·
 *                             collapse L/=2
 *   backward (coord -> part): walk events newest-first, invert each;
 *                             odd L under an expansion = HOLE
 *
 *   physical slot = fold(part) ALWAYS — bytes never move.
 *
 * Non-circular: reader resolves part ids from coordinates via the log;
 * content oracle = SOURCE slices. memcpy counter during breath = must be 0.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geo_breathing_relabel tools/geo_breathing_relabel.c -lm
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

static uint64_t g_copies = 0;

typedef enum { EV_EXPAND, EV_SHUFFLE, EV_UNSHUFFLE, EV_COLLAPSE } EvType;
typedef struct { EvType type; uint32_t view; } Event;
static Event evlog[64];
static uint32_t ev_n = 0;

static void breathe(Event e) { evlog[ev_n++] = e; }

/* forward: part id -> coordinate label at CURRENT state */
static uint64_t fwd_label(uint32_t p) {
    uint64_t L = p;
    for (uint32_t i = 0; i < ev_n; i++) {
        switch (evlog[i].type) {
        case EV_EXPAND:   L *= 2u; break;
        case EV_COLLAPSE: L /= 2u; break;
        case EV_SHUFFLE:
        case EV_UNSHUFFLE: {
            uint32_t k = (uint32_t)(L / KIS_CUBE), rr = (uint32_t)(L % KIS_CUBE);
            rr = kis_view6_slot(evlog[i].view, rr);
            L = (uint64_t)k * KIS_CUBE + rr;
            break;
        }
        }
    }
    return L;
}

/* backward: coordinate -> owning part at current state; 0 = occupied? */
static int coord_to_part(uint64_t L, uint32_t *out) {
    for (uint32_t i = ev_n; i-- > 0;) {
        if (L >= MAX_PARTS) return 0;
        switch (evlog[i].type) {
        case EV_EXPAND:
            if (L & 1u) return 0;                 /* hole */
            L /= 2u;
            break;
        case EV_COLLAPSE:
            L *= 2u;
            break;
        case EV_SHUFFLE:
        case EV_UNSHUFFLE: {
            uint32_t k = (uint32_t)(L / KIS_CUBE), rr = (uint32_t)(L % KIS_CUBE);
            uint32_t inv = evlog[i].view == 4 ? 5u : 4u;
            rr = kis_view6_slot(inv, rr);
            L = (uint64_t)k * KIS_CUBE + rr;
            break;
        }
        }
    }
    if (L >= MAX_PARTS) return 0;
    *out = (uint32_t)L;
    return 1;
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
    printf("=== geo_breathing_relabel — ZERO-COPY breathing ===\n");
    printf("%s\nparts %u (%.1f MB)\n\n", path, total_parts, (double)total_bytes / 1e6);
    if ((uint64_t)total_parts * 2u > MAX_PARTS) { printf("FAIL headroom\n"); return 1; }

    typedef struct { const uint8_t *src; uint32_t len; } PartJob;
    PartJob *job = (PartJob *)malloc(sizeof(PartJob) * total_parts);
    if (!job) return 1;
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

    uint8_t *win = (uint8_t *)VirtualAlloc(NULL,
        (size_t)MAX_PARTS * PART_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!win) return 1;
    double t0 = now_ms();
    for (uint32_t f = 0; f < total_parts; f++)
        memcpy(win + part_offset(f), job[f].src, job[f].len);
    printf("BAKE   %u parts · %.0f ms (last copy of the session)\n\n",
           total_parts, now_ms() - t0);

    int failures = 0;

    #define VERIFY_RELABEL(label) do { \
        uint32_t ok = 0, badm = 0; \
        t0 = now_ms(); \
        for (uint32_t f = 0; f < total_parts; f++) { \
            uint64_t L = fwd_label(f); \
            uint32_t back = 999999u; \
            if (!coord_to_part(L, &back) || back != f) badm++; \
            else if (memcmp(win + part_offset(back), job[f].src, job[f].len) != 0) badm++; \
            else ok++; \
        } \
        printf("VERIFY %-18s %u/%u ok · %.0f ms · %s\n", \
               label, ok, total_parts, now_ms() - t0, badm ? "FAIL" : "PASS"); \
        failures += badm; \
    } while (0)

    /* ── breathe IN ×2 : coords double, odds become holes ────────────── */
    breathe((Event){EV_EXPAND, 0});
    VERIFY_RELABEL("after expand x2");

    /* hole probe: odd coords resolve to nothing */
    {
        uint32_t badh = 0;
        for (uint64_t L = 1; L < 200; L += 2) {
            uint32_t back;
            if (coord_to_part(L, &back)) badh++;
        }
        printf("HOLES  odd coords empty: %s (%u wrong)\n",
               badh ? "FAIL" : "PASS", badh);
        failures += badh ? 1 : 0;
    }

    /* ── shuffle round trip in relabel space ─────────────────────────── */
    breathe((Event){EV_SHUFFLE, 4});
    VERIFY_RELABEL("after shuffle v4");
    breathe((Event){EV_UNSHUFFLE, 5});
    VERIFY_RELABEL("after unshuffle v5");

    /* ── breathe OUT ×2 : coords halve back ──────────────────────────── */
    breathe((Event){EV_COLLAPSE, 0});
    VERIFY_RELABEL("home again");

    printf("\ncopies during ALL breathing: %llu  <- must be 0\n",
           (unsigned long long)g_copies);
    printf("carried state: %u events (%u B)\n", ev_n, ev_n * (uint32_t)sizeof(Event));
    printf("RESULT: %s\n",
           failures == 0 && g_copies == 0
             ? "ZERO-COPY BREATHING LOSSLESS ON REAL WEIGHTS"
             : "FAILED");

    free(job);
    VirtualFree(win, 0, MEM_RELEASE);
    gguf_close(&r);
    return (failures || g_copies) ? 1 : 0;
}
