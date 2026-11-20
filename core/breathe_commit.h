/*
 * breathe_commit.h — Bottom-up lazy commit for reserved memory
 * ═══════════════════════════════════════════════════════════════════════════
 * Windows: reserve via VirtualAlloc(MEM_RESERVE), commit on first touch via
 *          VEH (VirtualAlloc MEM_COMMIT). Decommit with VirtualFree(MEM_DECOMMIT).
 * Linux:   mmap anonymous is already demand-paged (commit on first touch,
 *          zero-filled by kernel). Decommit with madvise(MADV_DONTNEED).
 *
 * Usage:
 *   breathe_register();            // once at process start (no-op on Linux)
 *   void *p = breathe_reserve(sz); // reserve-only, no commit
 *   ... use p; pages commit on first touch ...
 *   breathe_decommit(slice, size);  // release unfired slices
 *   breathe_release(p);            // at shutdown (optional)
 *   breathe_unregister();           // at process exit (no-op on Linux)
 */
#ifndef BREATHE_COMMIT_H
#define BREATHE_COMMIT_H

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define BREATHE_MAX_REGIONS 4096
typedef struct { void *base; size_t size; size_t n_pages; uint8_t *bitmap; } BreatheRegion;
static BreatheRegion g_breathe_regions[BREATHE_MAX_REGIONS];
static volatile LONG  g_breathe_nregions = 0;
static PVOID          g_breathe_veh = NULL;

static BreatheRegion *breathe_find_region(void *addr) {
    LONG n = g_breathe_nregions;
    for (LONG i = 0; i < n; i++) {
        BreatheRegion *r = &g_breathe_regions[i];
        if (addr >= r->base && (char *)addr < (char *)r->base + r->size) return r;
    }
    return NULL;
}
static LONG WINAPI breathe_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    void *addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
    BreatheRegion *r = breathe_find_region(addr);
    if (!r) return EXCEPTION_CONTINUE_SEARCH;
    size_t page_idx = ((char *)addr - (char *)r->base) / 4096;
    if (page_idx >= r->n_pages) return EXCEPTION_CONTINUE_SEARCH;
    void *page_base = (char *)r->base + page_idx * 4096;
    if (!VirtualAlloc(page_base, 4096, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    r->bitmap[page_idx / 8] |= (uint8_t)(1 << (page_idx % 8));   /* best-effort tracking */
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void breathe_register(void) {
    if (!g_breathe_veh) g_breathe_veh = AddVectoredExceptionHandler(1, breathe_veh);
}
static void breathe_unregister(void) {
    if (g_breathe_veh) { RemoveVectoredExceptionHandler(g_breathe_veh); g_breathe_veh = NULL; }
}
static void *breathe_reserve(size_t size) {
    if (size == 0) size = 1;
    size_t pages = (size + 4095) / 4096;
    size_t rsize = pages * 4096;
    void *base = VirtualAlloc(NULL, rsize, MEM_RESERVE, PAGE_READWRITE);
    if (!base) return NULL;
    LONG idx = InterlockedIncrement(&g_breathe_nregions) - 1;
    if (idx >= BREATHE_MAX_REGIONS) {
        VirtualFree(base, 0, MEM_RELEASE);
        InterlockedDecrement(&g_breathe_nregions);
        return NULL;
    }
    BreatheRegion *r = &g_breathe_regions[idx];
    r->base = base; r->size = rsize; r->n_pages = pages;
    r->bitmap = (uint8_t *)calloc((pages + 7) / 8, 1);
    if (!r->bitmap) { VirtualFree(base, 0, MEM_RELEASE); r->base = NULL; return NULL; }
    return base;
}
static void breathe_release(void *base) { (void)base; }
static void breathe_decommit(void *ptr, size_t size) {
    VirtualFree(ptr, size, MEM_DECOMMIT);
}

#else /* !_WIN32 (Linux / POSIX) */
#include <sys/mman.h>
static void breathe_register(void) { }
static void breathe_unregister(void) { }
static void *breathe_reserve(size_t size) {
    if (size == 0) size = 1;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
static void breathe_release(void *base) { /* caller tracks size; use munmap if needed */ (void)base; }
static void breathe_decommit(void *ptr, size_t size) {
    madvise(ptr, size, MADV_DONTNEED);
}

#endif /* _WIN32 */

#endif /* BREATHE_COMMIT_H */
