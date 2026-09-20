/* tess_window_bench.c — .tess read path: fread vs whole-file mmap vs PAGE-ALIGNED
 *                       144 x 144 WINDOW VIEWS, measured in faults / MB/s / RSS.
 *
 * One capo == one full cube == 20736 cells == exactly ONE 144 x 144 window
 * (tests/test_window_ladder.c, T21), so every path below walks the same windows
 * over the same bytes and only the I/O strategy differs:
 *
 *   old-fread      : fread(capo) -> malloc buffer -> scan          (the old path)
 *   old-fread-row0 : same fread, but only row 0 of the window is wanted
 *                    (proves the old path cannot avoid reading the whole capo)
 *   mfile-linear   : whole-file mmap, straight scan
 *   mfile-window   : whole-file mmap, row-structured 144 x 144 walk
 *   mfile-row0     : whole-file mmap, only row 0 of every window
 *   winview        : MapViewOfFile per window GROUP (--view-mb), page-aligned,
 *                    offset rounded to the allocation granularity, view released
 *                    when the walk leaves it  ->  bounded working set
 *   winview-row0   : same views, only row 0 wanted (1/144 of the I/O)
 *   winview-copy   : window view + memcpy into a caller buffer (drop-in for
 *                    consumers that want a contiguous window)
 *
 * Working set is trimmed (EmptyWorkingSet) before every phase and views are
 * unmapped after, so RSS numbers are comparable instead of cumulative; peak RSS
 * is sampled DURING the walk, not just at the end.
 *
 * NOTE ON HONESTY: pages stay in the OS file cache once read, so after the first
 * pass this measures warm-cache (steady state) behaviour — soft faults.  Pass 1
 * vs pass 2 is where first-touch hard faults show up.  Purging the OS standby
 * list needs admin rights (RAMMap / SetSystemFileCacheSize), which this tool
 * deliberately does not do.
 *
 * DEFAULTS ARE THE MEASURED WINNER: --view-mb 0 (one window per view, so the
 * view is exactly the granularity block that holds the window) and no prefetch.
 * Prefetching a whole multi-MB view is 15-45% SLOWER than letting sequential
 * faults do the read-ahead, so it is opt-in (--prefetch), not the default.
 *
 * RUN: ./build/tess_window_bench <pack.tesspack> [--mb N] [--view-mb X]
 *                                [--passes P] [--only a,b,...] [--prefetch]
 */
#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include "core/geo_tess_container.h"
#include "core/geo_tesseract_addr.h"   /* 144 x 144 window API (TESS_WIN_COLS/ROWS) */
#include "core/geo_tess_window.h"      /* page-aligned window views                 */

#define PAGE        4096u
#define DATA_OFF    128u               /* TESS_HEADER_SIZE + TESS_FORMULA_SIZE     */

static uint64_t g_sink = 0;
static LARGE_INTEGER g_t0, g_freq;
static uint64_t      g_fault0 = 0;
static double        g_peak_ws = 0;
static uint32_t      wm_page_global = PAGE;

static double ws_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        double mb = pmc.WorkingSetSize / 1048576.0;
        if (mb > g_peak_ws) g_peak_ws = mb;
        return mb;
    }
#endif
    return 0.0;
}
static uint64_t faults_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.PageFaultCount;
#endif
    return 0;
}
static void drop_ws(void) {
#ifdef _WIN32
    EmptyWorkingSet(GetCurrentProcess());
#endif
}
static double now_sec(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
}
static void phase_start(void) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    drop_ws();                 /* trim FIRST, else the peak carries the previous phase */
    g_peak_ws = 0; ws_now();
    QueryPerformanceCounter(&g_t0);
    g_fault0 = faults_now();
}

/* ── one measured phase ──────────────────────────────────────────────────── */
typedef struct {
    const char *tag;
    double   sec, mb_want, mb_file, peak_ws, end_ws;
    uint64_t faults, bytes_want, bytes_file;
    uint64_t views, reuse, bytes_mapped;   /* deltas within this phase */
} Res;

/* want  = bytes the consumer actually asked for (the useful work)
 * file  = bytes moved through the file system: sum(capo size) for fread,
 *         0 for mmap paths (derived from faults x page, which is what got
 *         brought into the working set) */
static void phase_end(Res *r, uint64_t want, uint64_t file,
                      uint64_t views, uint64_t reuse, uint64_t bytes_mapped) {
    r->sec    = now_sec();
    r->faults = faults_now() - g_fault0;
    r->bytes_want = want;
    r->bytes_file = file ? file : r->faults * wm_page_global;
    r->mb_want = want / 1048576.0;
    r->mb_file = r->bytes_file / 1048576.0;
    r->end_ws = ws_now();
    r->peak_ws = g_peak_ws;
    r->views = views; r->reuse = reuse; r->bytes_mapped = bytes_mapped;
    double e = r->sec > 0 ? r->sec : 1e-9;
    printf("  %-16s %6.3f s | want %7.1f MB/s | file %7.1f MB/s (%6.1f MB) | faults %8llu | ws %5.1f->%5.1f MB%s\n",
           r->tag, r->sec, r->mb_want / e, r->mb_file / e, r->mb_file,
           (unsigned long long)r->faults, r->peak_ws, r->end_ws,
           (bytes_mapped && bytes_mapped > r->bytes_file * 2) ? "  <-- view padding > 2x" : "");
}

/* every 16th byte: cheap on CPU, but faults every page the range touches */
static inline void touch(const uint8_t *p, uint64_t n) {
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i += 16) s += p[i];
    g_sink += s;
}
static uint32_t resident_pages(const uint8_t *p, uint64_t n) {
#ifdef _WIN32
    uintptr_t first = ((uintptr_t)p) & ~(uintptr_t)(PAGE - 1);
    uintptr_t last  = (((uintptr_t)p) + (n ? n - 1 : 0)) & ~(uintptr_t)(PAGE - 1);
    uint32_t npg = (uint32_t)((last - first) / PAGE) + 1u;
    PSAPI_WORKING_SET_EX_INFORMATION *info = malloc((size_t)npg * sizeof(*info));
    if (!info) return 0;
    for (uint32_t i = 0; i < npg; i++) info[i].VirtualAddress = (PVOID)(first + (uintptr_t)i * PAGE);
    uint32_t got = 0;
    if (QueryWorkingSetEx(GetCurrentProcess(), info, (DWORD)(npg * sizeof(*info))))
        for (uint32_t i = 0; i < npg; i++) if (info[i].VirtualAttributes.Valid) got++;
    free(info);
    return got;
#else
    (void)p; (void)n; return 0;
#endif
}

/* ── capo list ───────────────────────────────────────────────────────────── */
typedef struct { uint64_t off; uint32_t sz, cell, slots; } Capo;

/* ── paths ───────────────────────────────────────────────────────────────── */

/* 1. OLD: fread the whole capo into malloc, then read what we actually want */
static uint64_t p_fread(const char *pack, Capo *c, uint32_t n, int row0, uint64_t *file_bytes) {
    uint8_t *buf = NULL; uint32_t cap = 0; uint64_t bytes = 0, fb = 0;
    FILE *f = fopen(pack, "rb");
    if (!f) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (c[i].sz > cap) { free(buf); cap = c[i].sz; buf = malloc(cap); }
        _fseeki64(f, (int64_t)c[i].off, SEEK_SET);
        if (fread(buf, 1, c[i].sz, f) != c[i].sz) break;
        fb += c[i].sz;                          /* the old path reads the whole capo */
        uint64_t want = row0 ? (uint64_t)TESS_WIN_COLS * c[i].cell : (uint64_t)c[i].slots * c[i].cell;
        touch(buf + DATA_OFF, want);
        bytes += want;
        if ((i & 15u) == 0) ws_now();
    }
    free(buf); fclose(f);
    if (file_bytes) *file_bytes = fb;
    return bytes;
}

/* 2. whole-file mmap (open pointer), three access shapes */
static uint64_t p_mfile(const TESS_PackIndex *pi, Capo *c, uint32_t n, int mode) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *cube = pi->base + c[i].off + DATA_OFF;
        if (mode == 0) {                                    /* linear */
            uint64_t wb = (uint64_t)c[i].slots * c[i].cell;
            touch(cube, wb); bytes += wb;
        } else if (mode == 1) {                             /* window rows */
            for (uint32_t r = 0; r < TESS_WIN_ROWS; r++) {
                uint64_t rl = (uint64_t)TESS_WIN_COLS * c[i].cell;
                touch(cube + (uint64_t)r * rl, rl); bytes += rl;
            }
        } else {                                            /* row 0 only */
            uint64_t rl = (uint64_t)TESS_WIN_COLS * c[i].cell;
            touch(cube, rl); bytes += rl;
        }
        if ((i & 15u) == 0) ws_now();
    }
    return bytes;
}

/* 3. page-aligned window views: map a GROUP of windows, stream inside it */
typedef struct { uint64_t views, reuse, mapped; } WinStats;

static uint64_t p_winview(TESS_WinMap *wm, Capo *c, uint32_t n, int row0,
                          uint64_t view_bytes, int prefetch, WinStats *st) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t wb   = (uint64_t)c[i].slots * c[i].cell;    /* the whole window  */
        uint64_t want = row0 ? (uint64_t)TESS_WIN_COLS * c[i].cell : wb;
        uint64_t span = view_bytes > want ? view_bytes : want;   /* group to map  */

        /* Reuse the live view while THIS window fits inside it.  That is the
         * whole point of a group: one syscall serves every window in it. */
        const uint8_t *d;
        if (wm->view && c[i].off + DATA_OFF >= wm->view_off &&
            c[i].off + DATA_OFF + want <= wm->view_off + wm->view_len) {
            d = wm->view + (c[i].off + DATA_OFF - wm->view_off);
            wm->n_reuse++;
        } else {
            tess_winmap_unmap(wm);                      /* bounded working set */
            d = tess_winmap_view(wm, c[i].off + DATA_OFF, span);
            if (!d) break;
            if (prefetch) tess_winmap_prefetch(wm, c[i].off + DATA_OFF, span);
        }
        if (row0) {
            uint64_t rl = (uint64_t)TESS_WIN_COLS * c[i].cell;
            touch(d, rl); bytes += rl;
        } else {
            for (uint32_t r = 0; r < TESS_WIN_ROWS; r++) {
                uint64_t rl = (uint64_t)TESS_WIN_COLS * c[i].cell;
                touch(d + (uint64_t)r * rl, rl); bytes += rl;
            }
        }
        if ((i & 15u) == 0) ws_now();
    }
    tess_winmap_unmap(wm);
    st->views = wm->n_views; st->reuse = wm->n_reuse; st->mapped = wm->bytes_mapped;
    return bytes;
}

/* 4. window view + copy out (the drop-in for contiguous-buffer consumers) */
static uint64_t p_winview_copy(TESS_WinMap *wm, Capo *c, uint32_t n, uint8_t *dst,
                               uint64_t view_bytes, WinStats *st) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t wb   = (uint64_t)c[i].slots * c[i].cell;
        uint64_t span = view_bytes > wb ? view_bytes : wb;
        const uint8_t *d;
        if (wm->view && c[i].off + DATA_OFF >= wm->view_off &&
            c[i].off + DATA_OFF + wb <= wm->view_off + wm->view_len) {
            d = wm->view + (c[i].off + DATA_OFF - wm->view_off);
            wm->n_reuse++;
        } else {
            tess_winmap_unmap(wm);
            d = tess_winmap_view(wm, c[i].off + DATA_OFF, span);
            if (!d) break;
            tess_winmap_prefetch(wm, c[i].off + DATA_OFF, span);
        }
        memcpy(dst, d, (size_t)wb);   /* memcpy reads the source: no extra touch */
        g_sink += ((const uint8_t *)dst)[0] + dst[wb - 1];
        bytes += wb;
        if ((i & 15u) == 0) ws_now();
    }
    tess_winmap_unmap(wm);
    st->views = wm->n_views; st->reuse = wm->n_reuse; st->mapped = wm->bytes_mapped;
    return bytes;
}

/* ── the path table ──────────────────────────────────────────────────────── */
typedef enum { P_FREAD, P_FREAD_ROW0, P_MFILE_LIN, P_MFILE_WIN, P_MFILE_ROW0,
               P_WINVIEW, P_WINVIEW_ROW0, P_WINVIEW_COPY, P_COUNT } PathId;
static const char *P_NAME[P_COUNT] = {
    "old-fread", "old-fread-row0", "mfile-linear", "mfile-window", "mfile-row0",
    "winview", "winview-row0", "winview-copy"
};

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <pack.tesspack> [--mb N] [--view-mb X] [--passes P]"
               " [--only a,b,...] [--prefetch]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);      /* a crash must not eat the report */
    const char *pack = argv[1];
    double mb_budget = 256.0, view_mb = 0.0;   /* 0 = exactly one window per view */
    int passes = 2, prefetch = 0;
    int only[P_COUNT] = {0}; int n_only = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb_budget = atof(argv[++i]);
        else if (!strcmp(argv[i], "--view-mb") && i + 1 < argc) view_mb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prefetch")) prefetch = 1;
        else if (!strcmp(argv[i], "--no-prefetch")) prefetch = 0;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            char *s = argv[++i], *tok = strtok(s, ",");
            while (tok) { for (int p = 0; p < P_COUNT; p++) if (!strcmp(tok, P_NAME[p])) { only[p] = 1; n_only++; } tok = strtok(NULL, ","); }
            if (!n_only) { printf("--only: unknown path name\n"); return 2; }
        }
    }
    if (!n_only) for (int p = 0; p < P_COUNT; p++) only[p] = 1;

    TESS_PackIndex pi;
    if (tess_pack_open(&pi, pack) != 0) { printf("tess_pack_open failed\n"); return 1; }
    TESS_WinMap wm;
    if (tess_winmap_open(&wm, pack) != 0) { printf("tess_winmap_open failed\n"); return 1; }

    printf("═══ tess_window_bench — %s ═══\n", pack);
    printf("file %.1f MB · capos %u · pack v%u · page %u B · view granularity %u B\n",
           pi.file_sz / 1048576.0, pi.n_capos, pi.pack_version, wm.page, wm.gran);
    printf("prefetch hint: %s\n\n", wm.prefetch ? (prefetch ? "on" : "available, disabled") : "unavailable");

    /* ── capo list inside the byte budget ─────────────────────────────────── */
    Capo *c = calloc(pi.n_entries ? pi.n_entries : 1, sizeof(Capo));
    uint32_t n = 0; uint64_t bytes = 0, bytes_row0 = 0;
    uint64_t budget = (uint64_t)(mb_budget * 1048576.0);
    for (uint32_t i = 0; i < pi.n_entries && bytes < budget; i++) {
        uint64_t off = pi.entries[i].offset; uint32_t sz = pi.entries[i].size;
        if (off + sz > pi.file_sz || sz < DATA_OFF + 16) continue;
        const TESS_Header *h = (const TESS_Header *)(pi.base + off);
        if (h->magic != 0x54455353u) continue;
        uint32_t slots = h->total_slots ? h->total_slots : TESS_GEO_FULL;
        c[n].off = off; c[n].sz = sz; c[n].cell = h->cell_size; c[n].slots = slots; n++;
        bytes += (uint64_t)slots * h->cell_size;
        bytes_row0 += (uint64_t)TESS_WIN_COLS * h->cell_size;
    }
    if (!n) { printf("no TESS capos found\n"); return 1; }
    printf("walking %u capos · %.1f MB full · %.3f MB row-0 only (1/%u of it)\n",
           n, bytes / 1048576.0, bytes_row0 / 1048576.0, TESS_WIN_ROWS);

    /* ── window / view math straight from the index ───────────────────────── */
    {
        uint64_t need = 0, touch_pages = 0, shared = 0, pad = 0;
        uint32_t ph4[4] = {0,0,0,0}, phg[4] = {0,0,0,0}, cells[8] = {0}; uint32_t ncell = 0;
        uint64_t prev_end = 0; int have_prev = 0, aligned_windows = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t off = c[i].off + DATA_OFF, wb = (uint64_t)c[i].slots * c[i].cell;
            uint64_t ph = off % wm.page;
            need += (wb + wm.page - 1) / wm.page;
            touch_pages += (ph + wb + wm.page - 1) / wm.page;
            ph4[(ph * 4) / wm.page]++;
            phg[(off % wm.gran) * 4 / wm.gran]++;
            if (ph == 0) aligned_windows++;
            uint64_t vo, vl; tess_win_view_align(&wm, off, wb, &vo, &vl);
            if (vo + vl > pi.file_sz) vl = pi.file_sz - vo;
            pad += vl / wm.page - (wb + wm.page - 1) / wm.page;   /* view vs data pages */
            uint64_t s = off / wm.page, e = (off + wb - 1) / wm.page;
            if (have_prev && s == prev_end) shared++;
            prev_end = e; have_prev = 1;
            int seen = 0;
            for (uint32_t k = 0; k < ncell; k++) if (cells[k] == c[i].cell) seen = 1;
            if (!seen && ncell < 8) cells[ncell++] = c[i].cell;
        }
        printf("\n── window math ────────────────────────────────────────────────\n");
        printf("pages the DATA needs (minimum)          : %llu (%.2f MB)\n",
               (unsigned long long)need, need * (wm.page / 1048576.0));
        printf("pages actually touched today            : %llu  (+%.2f%% page straddle)\n",
               (unsigned long long)touch_pages,
               100.0 * ((double)touch_pages / (double)need - 1.0));
        printf("pages ADDED by granularity-aligned views : %llu  (+%.2f%%)\n",
               (unsigned long long)pad, 100.0 * ((double)pad / (double)need));
        printf("windows starting exactly on a page      : %d/%u  (phase mod 4096 quartiles %u/%u/%u/%u)\n",
               aligned_windows, n, ph4[0], ph4[1], ph4[2], ph4[3]);
        printf("phase mod 64 KiB (view alignment)       : %u/%u/%u/%u\n",
               phg[0], phg[1], phg[2], phg[3]);
        printf("pages SHARED by adjacent windows        : %llu\n", (unsigned long long)shared);
        printf("capo cell sizes seen                    :");
        for (uint32_t k = 0; k < ncell; k++) printf(" %u B", cells[k]);
        printf("\n");
        for (uint32_t k = 0; k < ncell; k++) {
            uint64_t wb = (uint64_t)TESS_GEO_FULL * cells[k];
            uint32_t g = 1; while ((wb * g) % wm.page) g++;
            printf("  cell %3u B -> window %7llu B = %4llu pages%s  group of %u = %.2f MB\n",
                   cells[k], (unsigned long long)wb, (unsigned long long)((wb + wm.page - 1) / wm.page),
                   (wb % wm.page) ? "  (not aligned)" : "  (aligned!)", g, wb * (double)g / 1048576.0);
        }
    }

    /* ── contiguous buffer for the copy path ──────────────────────────────── */
    uint64_t max_win = 0;
    for (uint32_t i = 0; i < n; i++) { uint64_t wb = (uint64_t)c[i].slots * c[i].cell; if (wb > max_win) max_win = wb; }
    uint8_t *dst = malloc((size_t)max_win);

    /* ── run the paths ────────────────────────────────────────────────────── */
    Res best[P_COUNT]; memset(best, 0, sizeof(best));
    wm_page_global = wm.page;      /* lets phase_end turn faults into file bytes */
    for (int pass = 0; pass < passes; pass++) {
        printf("\n── pass %d %s ──────────────────────────────────────────────\n",
               pass + 1, pass ? "(warm page cache)" : "(first touch of the file)");
        for (int p = 0; p < P_COUNT; p++) {
            if (!only[p]) continue;
            Res r; memset(&r, 0, sizeof(r)); r.tag = P_NAME[p];
            WinStats st; memset(&st, 0, sizeof(st));
            uint64_t v0 = wm.n_views, r0 = wm.n_reuse, m0 = wm.bytes_mapped;
            uint64_t br, fb = 0;
            phase_start();
            switch ((PathId)p) {
                case P_FREAD:        br = p_fread(pack, c, n, 0, &fb); break;
                case P_FREAD_ROW0:   br = p_fread(pack, c, n, 1, &fb); break;
                case P_MFILE_LIN:    br = p_mfile(&pi, c, n, 0); break;
                case P_MFILE_WIN:    br = p_mfile(&pi, c, n, 1); break;
                case P_MFILE_ROW0:   br = p_mfile(&pi, c, n, 2); break;
                case P_WINVIEW:      br = p_winview(&wm, c, n, 0, (uint64_t)(view_mb*1048576.0), prefetch, &st); break;
                case P_WINVIEW_ROW0: br = p_winview(&wm, c, n, 1, (uint64_t)(view_mb*1048576.0), prefetch, &st); break;
                default:             br = p_winview_copy(&wm, c, n, dst, (uint64_t)(view_mb*1048576.0), &st); break;
            }
            phase_end(&r, br, fb, wm.n_views - v0, wm.n_reuse - r0, wm.bytes_mapped - m0);
            if (r.views || r.reuse)
                printf("      %-16s %6llu view map(s) · %6llu reuse(s) = %.1f windows/view · mapped %.1f MB (%.2fx of read)\n",
                       "", (unsigned long long)r.views, (unsigned long long)r.reuse,
                       r.views ? 1.0 + (double)r.reuse / (double)r.views : 0.0,
                       r.bytes_mapped / 1048576.0,
                       r.bytes_want ? (double)r.bytes_mapped / (double)r.bytes_want : 0.0);
            if (!best[p].sec || (r.sec > 0 && r.sec < best[p].sec)) best[p] = r;
        }
    }

    /* ── summary ──────────────────────────────────────────────────────────── */
    printf("\n═══ summary — best of %d pass(es), page %u B, view %.2f MB ═══\n", passes, wm.page, view_mb);
    printf("  %-16s %8s %12s %12s %12s %10s  %s\n",
           "path", "sec", "want MB/s", "file MB/s", "faults/MB(w)", "peak ws MB", "note");
    for (int p = 0; p < P_COUNT; p++) {
        if (!only[p]) continue;
        Res *r = &best[p];
        printf("  %-16s %8.3f %12.1f %12.1f %12.0f %10.1f  %s\n",
               P_NAME[p], r->sec,
               r->mb_want / (r->sec > 0 ? r->sec : 1e-9),
               r->mb_file / (r->sec > 0 ? r->sec : 1e-9),
               r->mb_want > 0 ? (double)r->faults / r->mb_want : 0.0, r->peak_ws,
               r->views ? "window views" : (r->tag[0] == 'o' ? "fread + malloc" : "whole-file mmap"));
        if (r->views)
            printf("      %llu view map(s) · %llu reuse(s) · %.1f MB mapped (%s)\n",
                   (unsigned long long)r->views, (unsigned long long)r->reuse,
                   r->bytes_mapped / 1048576.0, prefetch ? "prefetch on" : "prefetch off");
    }

    /* ── verdict ──────────────────────────────────────────────────────────── */
    if (only[P_FREAD_ROW0] && only[P_WINVIEW_ROW0]) {
        Res *o = &best[P_FREAD_ROW0], *w = &best[P_WINVIEW_ROW0];
        printf("\n── verdict ─────────────────────────────────────────────────────\n");
        printf("  row-0 subset: the old path moves %.1f MB through the file to deliver %.2f MB\n"
               "                window views move %.1f MB (%.0fx less I/O) for the same %.2f MB\n",
               o->mb_file, o->mb_want, w->mb_file, w->mb_file > 0 ? o->mb_file / w->mb_file : 0.0, w->mb_want);
    }
    if (only[P_MFILE_WIN] && only[P_WINVIEW]) {
        Res *m = &best[P_MFILE_WIN], *w = &best[P_WINVIEW];
        double tp = m->sec > 0 ? 100.0 * (w->mb_want / w->sec) / (m->mb_want / m->sec) - 100.0 : 0.0;
        printf("  full scan:    whole-file mmap peak ws %.1f MB vs window views %.1f MB (%.0fx smaller)\n"
               "                throughput %.0f vs %.0f MB/s (%+.0f%%) with %.0f MB/s of page faults either way\n",
               m->peak_ws, w->peak_ws, w->peak_ws > 0 ? m->peak_ws / w->peak_ws : 0.0,
               m->mb_want / m->sec, w->mb_want / w->sec, tp,
               w->faults * wm.page / 1048576.0 / w->sec);
    }

    /* ── residency probe: does a group view hand window i+1 over for free? ── */
    printf("\n── residency probe (group view of %.2f MB, window i read, i+1 checked) ──\n", view_mb);
    for (uint32_t i = 0; i + 1 < n && i < 3; i++) {
        uint64_t off      = c[i].off + DATA_OFF;
        uint64_t wb       = (uint64_t)c[i].slots * c[i].cell;
        uint64_t next_off = c[i + 1].off + DATA_OFF;
        uint64_t span     = (uint64_t)(view_mb * 1048576.0);
        if (span < wb) span = wb;
        tess_winmap_unmap(&wm);
        drop_ws();
        const uint8_t *d = tess_winmap_view(&wm, off, span);
        if (!d) break;
        double pre = ws_now();
        touch(d, wb);
        double post = ws_now();
        uint32_t already = resident_pages(d, wb);
        uint32_t handed  = 0;
        if (next_off + wb <= wm.view_off + wm.view_len)
            handed = resident_pages(wm.view + (next_off - wm.view_off), wb);
        printf("  window %u: cell %u B · data %llu B in %llu page(s) · view %llu page(s) "
               "(gran %u B, +%llu B padding) · ws %.1f -> %.1f MB · "
               "window i resident %u/%llu · window i+1 already %u/%llu pages\n",
               i, c[i].cell, (unsigned long long)wb,
               (unsigned long long)tess_win_data_pages(&wm, off, wb),
               (unsigned long long)tess_win_view_pages(&wm, off, span), wm.gran,
               (unsigned long long)(wm.view_len - tess_win_data_pages(&wm, off, wb) * wm.page),
               pre, post, already,
               (unsigned long long)tess_win_data_pages(&wm, off, wb),
               handed, (unsigned long long)tess_win_data_pages(&wm, next_off, wb));
        tess_winmap_unmap(&wm);
    }

    printf("\nsink %llu (ignore) · peak ws overall %.1f MB\n", (unsigned long long)g_sink, ws_now());
    free(dst); free(c); tess_winmap_close(&wm); tess_pack_close(&pi);
    return 0;
}
