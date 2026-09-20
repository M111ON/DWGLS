// Window ladder — all 23 equal-area windows of 20736 = 12^4 = 144^2 = 128 x 162,
// scored against the layout that already exists in this tree.
//
// The two roles are NOT interchangeable, which is the point of this test:
//   128 x 162  = the ADDRESS route split (compute 2x64 Hilbert x geometry 162 ico;
//                also the minimal-shear rectangle of 144^2 — closest to square)
//   144 x 144  = the MEMORY window      (row index decodes to (tesseract, cube),
//                BFS 144 blocks x 144 slots, the 144-cycle the stride-37 walk lives on)
//
// Constants come from core/geo_tesseract_addr.h (included). The rest are quoted
// from the headers that own them:
//   BFS_BLOCKS 144 / BFS_SLOTS_BLOCK 144 / BFS_SEEKER_K 5184  — core/breathing_fs.h
//   TESS_AXIS_STRIDE 1728, TESS_STRIDE_37 37                  — core/geo_tess_container.h
//   tile A 32 x 36                                            — gpu_transpose.cu
//   4096 B page, 64 B cache line                              — hardware
#include <stdio.h>
#include "core/geo_tesseract_addr.h"
#include "core/geo_tess_window.h"   /* page-aligned window views (T29..T38) */

static int fails = 0;
static int checks = 0;
#define CHECK(n, c) do { checks++; printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

#define BFS_BLOCKS 144u    /* core/breathing_fs.h */
#define BFS_SLOTS  144u
#define SEEKER_K  5184u    /* BFS_SEEKER_K = (8*9)*(8*9) = 72^2 */
#define TILE_W      32u    /* gpu_transpose.cu */
#define TILE_H      36u
#define AXIS_STRIDE 1728u  /* TESS_AXIS_STRIDE = 12^3 */
#define STRIDE_37     37u  /* TESS_STRIDE_37, coprime with the field */
#define PAGE_B      4096u
#define CACHELINE     64u
#define CELL_F32       4u  /* TESS_CELL_F32 */

#define NCAND 23
static const unsigned CAND_W[NCAND] = {
    1, 2, 3, 4, 6, 8, 9, 12, 16, 18, 24, 27, 32, 36,
    48, 54, 64, 72, 81, 96, 108, 128, 144
};

static unsigned gcd_u(unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; }

static int tess_exact(unsigned w, unsigned h) {
    return (w % TESS_3D_CELLS == 0 && h % TESS_SLOTS == 0) ||
           (w % TESS_SLOTS == 0 && h % TESS_3D_CELLS == 0);
}
static int bfs_exact(unsigned w, unsigned h) {
    return (w % BFS_BLOCKS == 0) && (h % BFS_SLOTS == 0);
}
static int tile_exact(unsigned w, unsigned h) {
    return (w % TILE_W == 0 && h % TILE_H == 0) || (w % TILE_H == 0 && h % TILE_W == 0);
}
static int route_exact(unsigned w, unsigned h) {
    return (w == 128u && h == 162u) || (w == 162u && h == 128u);
}
/* how many tesseracts begin exactly on a row boundary when the flat array is
   read row-major with `cols` columns (0-row start counts, so 18 is perfect) */
static int rows_aligned(unsigned cols) {
    unsigned r, n = 0;
    for (r = 0; r * cols < TESS_GEO_FULL; r++)
        if ((r * cols) % TESS_PER_TESS == 0) n++;
    return (int)n;
}
/* what a *window* has to satisfy (each criterion weighs the same) */
static int io_score(unsigned w, unsigned h) {
    int s = 0;
    if (tess_exact(w, h))                       s++;  /* whole tesseracts, no straddling */
    if (bfs_exact(w, h))                        s++;  /* aligns with the FS 144x144 grid */
    if ((CELL_F32 * h) % CACHELINE == 0)        s++;  /* rows are whole cache lines      */
    if (h % 16u == 0)                           s++;  /* 16 rows = whole pages @ 16 B    */
    if (w % TESS_SLOTS == 0 || h % TESS_SLOTS == 0) s++; /* the 144-cycle (stride-37)   */
    return s;
}

int main(void) {
    int i;
    unsigned maxscore = 0, nwin = 0;

    printf("window ladder over %u = 12^4 = 144^2 (cell = %u B, %u tesseracts x %u)\n\n",
           TESS_GEO_FULL, CELL_F32, TESS_COUNT, TESS_PER_TESS);
    printf("      w x     h    w/h   startrow  tess bfs tile route 64cl score\n");
    for (i = 0; i < NCAND; i++) {
        unsigned w = CAND_W[i], h = TESS_GEO_FULL / w;
        int sc = io_score(w, h);
        printf("  %5u x %5u  %6.3f     %2d/18    %c    %c   %c    %c    %c    %d\n",
               w, h, (double)w / (double)h, rows_aligned(h),
               tess_exact(w, h) ? 'Y' : '.', bfs_exact(w, h) ? 'Y' : '.',
               tile_exact(w, h) ? 'Y' : '.', route_exact(w, h) ? 'Y' : '.',
               (CELL_F32 * h) % CACHELINE == 0 ? 'Y' : '.', sc);
        if (sc > (int)maxscore) maxscore = (unsigned)sc;
        if (sc == 5) nwin++;
    }

    /* ── the family ────────────────────────────────────────────────────── */
    CHECK(0, TESS_GEO_FULL == 12u * 12u * 12u * 12u && TESS_GEO_FULL == 144u * 144u);
    {
        int ok = 1, mono = 0, sq = 0;
        unsigned sqw = 0;
        for (i = 0; i < NCAND; i++) {
            unsigned w = CAND_W[i], h = TESS_GEO_FULL / w;
            if (w * h != TESS_GEO_FULL || TESS_GEO_FULL % w) ok = 0;
            if (i && CAND_W[i] <= CAND_W[i - 1]) mono = 1;
            if (w == h) { sq++; sqw = w; }
        }
        CHECK(1, ok && !mono);                       /* exactly the 23 divisor pairs        */
        CHECK(2, sq == 1 && sqw == 144);             /* 144 x 144 is the only square        */
    }

    /* ── the owner's equation, and why it is the closest-to-square pair ── */
    CHECK(3, 128u * 162u == 144u * 144u && 128u * 162u == TESS_GEO_FULL);
    CHECK(4, TESS_GEO_FULL % 128u == 0 && TESS_GEO_FULL % 162u == 0);
    {
        int tight = 0, viol = 0;
        for (i = 0; i < NCAND; i++) {
            unsigned w = CAND_W[i], h = TESS_GEO_FULL / w;
            if (w == h) continue;                    /* ratio h/w >= 81/64 among non-squares */
            if (64u * h < 81u * w) viol++;
            if (64u * h == 81u * w) tight++;
        }
        CHECK(5, viol == 0 && tight == 1);           /* one minimal shear: 128 x 162        */
    }

    /* ── role split: route vs window ───────────────────────────────────── */
    {
        int n_tess = 0, n_bfs = 0, n_tile = 0, n_route = 0;
        int w144_tess = 0, w144_bfs = 0, w144_tile = 0;
        int r_tess = 0, r_bfs = 0, r_tile = 0, r_score = 0;
        for (i = 0; i < NCAND; i++) {
            unsigned w = CAND_W[i], h = TESS_GEO_FULL / w;
            if (tess_exact(w, h)) n_tess++;
            if (bfs_exact(w, h))  n_bfs++;
            if (tile_exact(w, h)) n_tile++;
            if (route_exact(w, h)) n_route++;
            if (w == 144u) {
                w144_tess = tess_exact(w, h);
                w144_bfs  = bfs_exact(w, h);
                w144_tile = tile_exact(w, h);
            }
            if (route_exact(w, h)) {
                r_tess = tess_exact(w, h);
                r_bfs  = bfs_exact(w, h);
                r_tile = tile_exact(w, h);
                r_score = io_score(w, h);
            }
        }
        CHECK(6, n_route == 1);                       /* the route split is unique           */
        CHECK(7, n_tess == 6 && w144_tess == 1 && r_tess == 0);
        CHECK(8, n_bfs == 1 && w144_bfs == 1 && r_bfs == 0);
        CHECK(9, n_tile == 6 && w144_tile == 0 && tile_exact(108u, 192u)); /* 108x192 = 18 tiles */
        CHECK(10, r_score == 0 && r_tile == 0);       /* 128x162 satisfies NO I/O criterion */
    }

    /* ── rows: does the flat array decompose on row boundaries? ─────────── */
    CHECK(11, rows_aligned(144) == 18);              /* every tesseract starts on a row     */
    CHECK(12, rows_aligned(162) == 2);               /* the route split does not            */
    CHECK(13, rows_aligned(192) == 18);              /* the 32x36-tiled one also does       */
    {
        /* through the window API in the addressing header: the 144 x 144 row-major
           index IS the existing flat() address for every one of the 20736 slots */
        int bij = 1;
        unsigned t, c, s;
        for (t = 0; t < TESS_COUNT; t++)
            for (c = 0; c < TESS_3D_CELLS; c++)
                for (s = 0; s < TESS_SLOTS; s++) {
                    unsigned flat = t * TESS_PER_TESS + c * TESS_SLOTS + s;
                    unsigned row  = t * TESS_3D_CELLS + c;
                    if (tess_win_flat(row, s) != flat) bij = 0;             /* row*144 + col */
                    if (tess_win_row(flat) != row || tess_win_col(flat) != s) bij = 0;
                    if (tess_row_tess(row) != t || tess_row_cell(row) != c) bij = 0;
                    if (tess_flat(t, c, s) != flat) bij = 0;
                }
        CHECK(14, bij);                              /* 144x144 needs no stride table       */
    }

    /* ── window bytes, pages, rows ─────────────────────────────────────── */
    CHECK(15, (TESS_GEO_FULL * 16u) % PAGE_B == 0 && (TESS_GEO_FULL * 16u) / PAGE_B == 81u
               && 81u == 9u * 9u);
    CHECK(16, (TESS_GEO_FULL * 64u) / PAGE_B == 324u && 324u == 18u * 18u);
    CHECK(17, (TESS_GEO_FULL * CELL_F32) % PAGE_B != 0
               && (TESS_GEO_FULL * 34u) % PAGE_B != 0);
    CHECK(18, (144u * 16u) == 36u * CACHELINE && 16u * (144u * 16u) == 9u * PAGE_B);
    CHECK(19, (144u * CELL_F32) == 9u * CACHELINE && (162u * CELL_F32) % CACHELINE != 0);

    /* ── the walk and the axis structure ──────────────────────────────── */
    CHECK(20, gcd_u(STRIDE_37, TESS_GEO_FULL) == 1 && TESS_GEO_FULL % STRIDE_37 != 0);
    CHECK(21, TESS_PER_TESS == TESS_3D_CELLS * TESS_SLOTS
               && TESS_GEO_FULL == TESS_PER_TESS * TESS_COUNT);
    CHECK(22, TESS_GEO_FULL == AXIS_STRIDE * 12u && TESS_GEO_FULL / 4u == SEEKER_K
               && SEEKER_K == 72u * 72u);

    /* ── the choice ───────────────────────────────────────────────────── */
    CHECK(23, maxscore == 5 && nwin == 1);            /* unique argmax = 144 x 144          */

    /* ── the header's own verify, now including the window API ────────── */
    CHECK(24, TESS_WIN_COLS == TESS_SLOTS && TESS_WIN_ROWS * TESS_WIN_COLS == TESS_GEO_FULL);
    CHECK(25, geo_tesseract_verify() == 0);           /* -9/-10/-11 = window broke          */

    /* ── paging: which cell sizes actually page-align ─────────────────── */
    CHECK(26, tess_win_pages(CELL_F32) == 0 && tess_win_pages(34u) == 0
               && tess_win_pages(16u) == 81u && tess_win_pages(64u) == 324u);
    CHECK(27, tess_win_pages(16u) == 9u * 9u && tess_win_pages(64u) == 18u * 18u);

    /* ═══ window VIEW alignment (core/geo_tess_window.h) ═══════════════════
     * A .tesspack capo starts at capo_off + 128 with an arbitrary phase, and
     * Windows MapViewOfFile() demands a granularity multiple — so every window
     * view is the 64 KiB block containing the window, rounded up to whole
     * pages.  These checks pin down the arithmetic that guarrantees it, plus
     * how much padding that alignment costs per window size.                */
    {
        TESS_WinMap wm; memset(&wm, 0, sizeof(wm));
        wm.page = 4096u; wm.gran = 65536u;
        unsigned long long vo, vl;

        /* the window constant must be the same 20736 everywhere */
        CHECK(29, TESS_WIN_SLOTS == TESS_GEO_FULL && TESS_WIN_SLOTS == TESS_WIN_COLS * TESS_WIN_ROWS);
        CHECK(30, tess_win_window_bytes(CELL_F32) == TESS_GEO_FULL * CELL_F32
                   && tess_win_window_bytes(34u) == 705024ull);

        /* page-alignedness + granularity alignment hold for every offset */
        int aligned_ok = 1, cover_ok = 1;
        for (unsigned long long off = 0; off < 300000ull; off += 4096u * 7u + 1u) {
            unsigned long long len = 705024ull;
            tess_win_view_align(&wm, off, len, &vo, &vl);
            if (vo % 65536ull) aligned_ok = 0;                 /* MapViewOfFile rule */
            if (vl % 4096ull) aligned_ok = 0;
            if (vo > off || vo + vl < off + len) cover_ok = 0; /* must actually cover */
            if (vl > len + 65536ull + 4096ull) cover_ok = 0;   /* ...without exploding  */
        }
        CHECK(31, aligned_ok);
        CHECK(32, cover_ok);

        /* a window that is already page-multiple needs no view padding */
        tess_win_view_align(&wm, 0, 705024ull, &vo, &vl);
        unsigned long long pages = vl / 4096ull;
        CHECK(33, vo == 0 && pages == 173ull && pages == tess_win_data_pages(&wm, 0, 705024ull));

        /* a shifted window: view padding is at most one granularity block */
        tess_win_view_align(&wm, 5000ull, 12288ull, &vo, &vl);
        CHECK(34, vo == 0 && vl == 20480ull && vl / 4096ull == 5ull
                   && tess_win_data_pages(&wm, 5000ull, 12288ull) == 4ull);
        CHECK(35, tess_win_data_pages(&wm, 0, 0) == 0ull);

        /* cell 144: the window IS a whole number of pages (729), so starting a
           window on a page costs at most ONE extra page — and that page comes
           from the granularity block, which is why view_off is 0, not 4096 */
        tess_win_view_align(&wm, 4096ull, 20736ull * 144ull, &vo, &vl);
        CHECK(36, vo == 0ull && (20736ull * 144ull) % 4096ull == 0ull
                   && tess_win_data_pages(&wm, 4096ull, 20736ull * 144ull) == 729ull
                   && vl == 730ull * 4096ull);

        /* cell sizes whose windows are page multiples: 16 (81 pages), 64 (324),
           144 (729) — the sizes the paging table in tess-format-spec.md names */
        CHECK(37, tess_win_window_bytes(16u) == 81ull * 4096ull
                   && tess_win_window_bytes(64u) == 324ull * 4096ull
                   && tess_win_window_bytes(144u) == 729ull * 4096ull);

        /* and the smallest page-aligned GROUP for the sizes that are not */
        {
            unsigned g4 = 1; while ((tess_win_window_bytes(CELL_F32) * g4) % 4096ull) g4++;
            unsigned g34 = 1; while ((tess_win_window_bytes(34u) * g34) % 4096ull) g34++;
            unsigned g144 = 1; while ((tess_win_window_bytes(144u) * g144) % 4096ull) g144++;
            /* 4 x 21 = 84 pages · 8 x 173 = 1384 pages · cell 144 already aligned */
            CHECK(38, g4 == 4u && g34 == 8u && g144 == 1u);
        }
    }

    printf("\n");
    if (fails) { printf("FAIL %d/%d\n", fails, checks); return fails; }
    printf("WINDOW_LADDER %d/%d OK\n", checks, checks);
    return fails;
}
