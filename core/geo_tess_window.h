/* ═══════════════════════════════════════════════════════════════════════════
 * geo_tess_window.h — page-aligned 144 x 144 WINDOW mapping for .tess / .tesspack
 * ═══════════════════════════════════════════════════════════════════════════
 * One capo == one full cube == TESS_GEO_FULL (20736) cells == exactly ONE
 * 144 x 144 window (tests/test_window_ladder.c, T21).  This header turns that
 * identity into an I/O path:
 *
 *   whole-file mmap   : one view of the pack, RSS grows with everything touched
 *   window view (here) : one view per window (or per group of windows), offset
 *                        rounded DOWN to the allocation granularity, length
 *                        rounded UP to a whole number of pages, released with
 *                        UnmapViewOfFile / munmap when the window is done
 *
 * Why rounding is not optional:
 *   Windows MapViewOfFile() requires dwFileOffset to be a multiple of
 *   GetSystemInfo()->dwAllocationGranularity (64 KiB).  A .tesspack capo starts
 *   at capo_off + 128 with an arbitrary phase mod 4096 (see the window math in
 *   tools/tess_window_bench.c), so a window's own offset is almost never a
 *   legal view offset.  tess_winmap_view() therefore maps the granularity block
 *   that CONTAINS the window and hands back view + (off - view_off).
 *
 * Windows results in the same pointer as the whole-file mmap would (same file,
 * same bytes) — what changes is the *working set*: after tess_winmap_unmap()
 * the pages can leave the process immediately instead of accumulating until
 * something trims them.
 *
 * POSIX maps at page granularity (there is no 64 KiB rule), so gran = page.
 *
 * No allocation, no copy, stdlib + OS headers only.
 * ═══════════════════════════════════════════════════════════════════════ */
#ifndef GEO_TESS_WINDOW_H
#define GEO_TESS_WINDOW_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#define TESS_WIN_PAGE_FALLBACK 4096u
#define TESS_WIN_GRAN_FALLBACK 65536u
/* 12^4 = 144 x 144 = TESS_TOTAL_SLOTS = TESS_GEO_FULL.  Kept local so this
 * header stays independent of the container; tests assert they agree. */
#define TESS_WIN_SLOTS 20736u

/* ── runtime-resolved Win8 page hints ─────────────────────────────────────
 * PrefetchVirtualMemory / DiscardVirtualMemory need _WIN32_WINNT >= 0x0602,
 * and this tree pins older values in some translation units.  Resolve them by
 * name instead so the header never fights over the macro.  The ABI is stable. */
typedef struct { void *VirtualAddress; size_t NumberOfBytes; } TESS_MemRange;
typedef int  (*TESS_PfnPrefetch)(void *proc, uintptr_t n, TESS_MemRange *r, unsigned flags);
typedef int  (*TESS_PfnDiscard)(void *addr, size_t n);

typedef struct {
    void    *h_file;      /* Windows file HANDLE (kept open for the pack's life) */
    void    *h_map;       /* Windows mapping HANDLE                              */
    int      fd;          /* POSIX fd (-1 on Windows)                            */
    uint8_t *view;        /* current view base (file offset == view_off)         */
    uint64_t view_off;    /* file offset of the view (granularity multiple)      */
    uint64_t view_len;    /* bytes mapped in the view                            */
    uint64_t file_sz;
    uint32_t gran;        /* required file-offset alignment for a view           */
    uint32_t page;        /* system page size                                    */
    TESS_PfnPrefetch prefetch;   /* may be NULL */
    TESS_PfnDiscard  discard;    /* may be NULL */

    /* stats — the bench reports these */
    uint64_t n_views;     /* views actually mapped (syscalls) */
    uint64_t n_reuse;     /* view hits: range already inside the live view */
    uint64_t bytes_mapped;
} TESS_WinMap;

/* ── pure arithmetic (no I/O — this is what the tests assert) ────────────
 * Which view must be live to read [off, off+len)?
 *   view_off = floor(off / gran) * gran          (MapViewOfFile rule)
 *   view_end = ceil((off + len) / page) * page   (never end mid-page)
 * delta = off - view_off is where the caller's data starts inside the view. */
static inline void tess_win_view_align(const TESS_WinMap *wm, uint64_t off, uint64_t len,
                                       uint64_t *view_off, uint64_t *view_len) {
    uint64_t g = wm && wm->gran ? wm->gran : TESS_WIN_GRAN_FALLBACK;
    uint64_t p = wm && wm->page ? wm->page : TESS_WIN_PAGE_FALLBACK;
    uint64_t end = off + (len ? len : 1u);
    uint64_t vo  = (off / g) * g;
    uint64_t ve  = ((end + p - 1u) / p) * p;
    *view_off = vo;
    *view_len = ve - vo;
}

/* Pages the view for [off, off+len) occupies (== resident pages if fully read). */
static inline uint64_t tess_win_view_pages(const TESS_WinMap *wm, uint64_t off, uint64_t len) {
    uint64_t vo, vl, p = wm && wm->page ? wm->page : TESS_WIN_PAGE_FALLBACK;
    tess_win_view_align(wm, off, len, &vo, &vl);
    return vl / p;
}

/* Pages actually needed by the DATA [off, off+len) itself — the minimum any
 * reader must fault, independent of view alignment. */
static inline uint64_t tess_win_data_pages(const TESS_WinMap *wm, uint64_t off, uint64_t len) {
    uint64_t p = wm && wm->page ? wm->page : TESS_WIN_PAGE_FALLBACK;
    if (!len) return 0;
    return ((off % p) + len + p - 1u) / p;
}

/* First/last page index of the data, for bookkeeping. */
static inline uint64_t tess_win_first_page(const TESS_WinMap *wm, uint64_t off) {
    return off / (wm && wm->page ? wm->page : TESS_WIN_PAGE_FALLBACK);
}

/* ── open / close ────────────────────────────────────────────────────────── */

static inline int tess_winmap_open(TESS_WinMap *wm, const char *pack_path) {
    memset(wm, 0, sizeof(*wm));
    wm->fd = -1;
#ifdef _WIN32
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        wm->page = si.dwPageSize ? si.dwPageSize : TESS_WIN_PAGE_FALLBACK;
        /* 0 on ReactOS-ish/minimal configs — never allow a zero modulus */
        wm->gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : TESS_WIN_GRAN_FALLBACK;
        wm->prefetch = (TESS_PfnPrefetch)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                               "PrefetchVirtualMemory");
        wm->discard  = (TESS_PfnDiscard)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                               "DiscardVirtualMemory");

        HANDLE hf = CreateFileA(pack_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) return -1;
        HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hm) { CloseHandle(hf); return -2; }
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hf, &li)) { CloseHandle(hm); CloseHandle(hf); return -3; }
        wm->h_file = (void *)hf;
        wm->h_map  = (void *)hm;
        wm->file_sz = (uint64_t)li.QuadPart;
    }
#else
    {
        long ps = sysconf(_SC_PAGESIZE);
        wm->page = ps > 0 ? (uint32_t)ps : TESS_WIN_PAGE_FALLBACK;
        wm->gran = wm->page;            /* POSIX has no 64 KiB view rule */
        int fd = open(pack_path, O_RDONLY);
        if (fd < 0) return -1;
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); return -3; }
        wm->fd = fd;
        wm->file_sz = (uint64_t)st.st_size;
    }
#endif
    if (wm->file_sz == 0) {                 /* empty pack: nothing to map */
#ifdef _WIN32
        if (wm->h_map)  CloseHandle((HANDLE)wm->h_map);
        if (wm->h_file) CloseHandle((HANDLE)wm->h_file);
#else
        if (wm->fd >= 0) close(wm->fd);
#endif
        return -4;
    }
    return 0;
}

/* Release the live view (keeps the pack open). */
static inline void tess_winmap_unmap(TESS_WinMap *wm) {
    if (!wm || !wm->view) return;
#ifdef _WIN32
    UnmapViewOfFile(wm->view);
#else
    munmap(wm->view, (size_t)wm->view_len);
#endif
    wm->view = NULL;
    wm->view_off = 0;
    wm->view_len = 0;
}

static inline void tess_winmap_close(TESS_WinMap *wm) {
    if (!wm) return;
    tess_winmap_unmap(wm);
#ifdef _WIN32
    if (wm->h_map)  { CloseHandle((HANDLE)wm->h_map);  wm->h_map  = NULL; }
    if (wm->h_file) { CloseHandle((HANDLE)wm->h_file); wm->h_file = NULL; }
#else
    if (wm->fd >= 0) { close(wm->fd); wm->fd = -1; }
#endif
}

/* Bytes in one window (20736 cells) for a given cell size. */
static inline uint64_t tess_win_window_bytes(uint32_t cell_size) {
    return (uint64_t)TESS_WIN_SLOTS * cell_size;
}

/* ── the read path ────────────────────────────────────────────────────────
 * Pointer to [off, off+len) inside a page-aligned, granularity-aligned view.
 * Reuses the live view when it already covers the range (walking a window row
 * by row costs ONE view, not one per row).  Returns NULL on failure. */
static inline const uint8_t *tess_winmap_view(TESS_WinMap *wm, uint64_t off, uint64_t len) {
    if (!wm || !len || off + len > wm->file_sz) return NULL;
    if (wm->view && off >= wm->view_off && off + len <= wm->view_off + wm->view_len) {
        wm->n_reuse++;
        return wm->view + (off - wm->view_off);
    }
    uint64_t vo, vl;
    tess_win_view_align(wm, off, len, &vo, &vl);
    if (vo + vl > wm->file_sz) vl = wm->file_sz - vo;      /* last granularity block may be short */
    if (!vl || vo + vl < off + len) return NULL;

    tess_winmap_unmap(wm);
#ifdef _WIN32
    /* dwFileOffsetHigh/Low are byte counts, not pages — but they MUST be a
     * multiple of dwAllocationGranularity, which tess_win_view_align guarantees. */
    uint8_t *p = (uint8_t *)MapViewOfFile((HANDLE)wm->h_map, FILE_MAP_READ,
                                          (DWORD)(vo >> 32), (DWORD)(vo & 0xFFFFFFFFu),
                                          (SIZE_T)vl);
    if (!p) return NULL;
#else
    uint8_t *p = (uint8_t *)mmap(NULL, (size_t)vl, PROT_READ, MAP_PRIVATE, wm->fd, (off_t)vo);
    if (p == MAP_FAILED) return NULL;
#endif
    wm->view = p;
    wm->view_off = vo;
    wm->view_len = vl;
    wm->n_views++;
    wm->bytes_mapped += vl;
    return wm->view + (off - wm->view_off);
}

/* Ask the OS to bring the view's pages in now (one hint for a whole window
 * instead of one fault per page).  Best-effort: no-op without the API. */
static inline void tess_winmap_prefetch(TESS_WinMap *wm, uint64_t off, uint64_t len) {
    if (!wm || !wm->prefetch || !wm->view) return;
    TESS_MemRange r;
    r.VirtualAddress = wm->view;
    r.NumberOfBytes  = (size_t)wm->view_len;
    (void)off; (void)len;
    wm->prefetch(GetCurrentProcess(), 1, &r, 0);
}

/* Drop the view's pages from the working set once read (streaming).  The view
 * stays mapped; the next touch re-faults softly from the OS file cache. */
static inline void tess_winmap_discard(TESS_WinMap *wm, uint64_t off, uint64_t len) {
    if (!wm || !wm->discard || !wm->view) return;
    (void)off; (void)len;
    wm->discard(wm->view, (size_t)wm->view_len);
}

/* One window, one view: the whole 144 x 144 window starting at `cube_off`
 * (== capo_off + 128), valid until the next tess_winmap_* call or close.
 * Row r of the returned array is (tesseract, cube) = (r / 8, r % 8) and its 144
 * cells are the columns — no stride table, see tests/test_window_ladder.c T14. */
static inline const uint8_t *tess_winmap_window(TESS_WinMap *wm, uint64_t cube_off,
                                               uint32_t cell_size) {
    return tess_winmap_view(wm, cube_off, tess_win_window_bytes(cell_size));
}

/* Drop-in for consumers that want a CONTIGUOUS window buffer (what the
 * existing fread paths hand back): copy len bytes out of a page-aligned view. */
static inline int tess_winmap_copy(TESS_WinMap *wm, uint64_t off, uint64_t len, void *dst) {
    const uint8_t *p = tess_winmap_view(wm, off, len);
    if (!p) return -1;
    tess_winmap_prefetch(wm, off, len);
    memcpy(dst, p, (size_t)len);
    return 0;
}

#endif /* GEO_TESS_WINDOW_H */
