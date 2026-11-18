/*
 * breathe_commit.h — Bottom-up lazy commit for reserved memory (Windows)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Reserve address space up front (cheap, no physical pages), then commit
 * pages on first touch via a Vectored Exception Handler. Unfired pages
 * (e.g. MoE expert slices) never get committed, and can be decommitted
 * with VirtualFree(MEM_DECOMMIT) after warm-up.
 *
 * Usage:
 *   breathe_register();            // once at process start
 *   void *p = breathe_reserve(sz); // reserve-only, no commit
 *   ... use p; pages commit on first touch ...
 *   VirtualFree(slice, size, MEM_DECOMMIT); // release unfired slices
 *   breathe_release(p);            // at shutdown (optional)
 *   breathe_unregister();           // at process exit
 *
 * Thread-safe: region table is populated single-threaded at startup;
 * VEH scans it read-only.
 */
#ifndef BREATHE_COMMIT_H
#define BREATHE_COMMIT_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define BREATHE_MAX_REGIONS 4096

typedef struct {
    void    *base;
    size_t   size;
    size_t   n_pages;
    uint8_t *bitmap;   /* 1 bit per page: committed? */
} BreatheRegion;

static BreatheRegion g_breathe_regions[BREATHE_MAX_REGIONS];
static volatile LONG  g_breathe_nregions = 0;
static PVOID          g_breathe_veh = NULL;

static BreatheRegion *breathe_find_region(void *addr) {
    LONG n = g_breathe_nregions;
    for (LONG i = 0; i < n; i++) {
        BreatheRegion *r = &g_breathe_regions[i];
        if (addr >= r->base && (char *)addr < (char *)r->base + r->size)
            return r;
    }
    return NULL;
}

static LONG WINAPI breathe_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    /* write fault (first touch) — information[0]==1 for write */
    void *addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
    BreatheRegion *r = breathe_find_region(addr);
    if (!r) return EXCEPTION_CONTINUE_SEARCH;
    size_t page_idx = ((char *)addr - (char *)r->base) / 4096;
    if (page_idx >= r->n_pages) return EXCEPTION_CONTINUE_SEARCH;
    void *page_base = (char *)r->base + page_idx * 4096;
    if (!VirtualAlloc(page_base, 4096, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    /* mark committed (best-effort, no lock needed for correctness) */
    r->bitmap[page_idx / 8] |= (uint8_t)(1 << (page_idx % 8));
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void breathe_register(void) {
    if (!g_breathe_veh)
        g_breathe_veh = AddVectoredExceptionHandler(1, breathe_veh);
}

static void breathe_unregister(void) {
    if (g_breathe_veh) {
        RemoveVectoredExceptionHandler(g_breathe_veh);
        g_breathe_veh = NULL;
    }
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
    r->base = base;
    r->size = rsize;
    r->n_pages = pages;
    r->bitmap = (uint8_t *)calloc((pages + 7) / 8, 1);
    if (!r->bitmap) {
        /* bitmap alloc failed — release and fall back */
        VirtualFree(base, 0, MEM_RELEASE);
        /* mark slot unused by zeroing base */
        r->base = NULL;
        return NULL;
    }
    return base;
}

static void breathe_release(void *base) {
    /* Find and remove region (mark base NULL); memory released by caller
       or left reserved — VEH will ignore NULL-base slots. */
    LONG n = g_breathe_nregions;
    for (LONG i = 0; i < n; i++) {
        if (g_breathe_regions[i].base == base) {
            free(g_breathe_regions[i].bitmap);
            g_breathe_regions[i].bitmap = NULL;
            g_breathe_regions[i].base = NULL;
            break;
        }
    }
}

#endif /* BREATHE_COMMIT_H */
