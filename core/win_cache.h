/*
 * core/win_cache.h — bounded window cache + prefetch skeleton (DWGLS)
 *
 * STATUS: skeleton only — admission + prefetch HINTS, no enforcement yet.
 * The field mmap stays the source of truth; this layer only records WHICH
 * windows are hot and in WHAT order, so a future enforcement step (OS trim /
 * pre-touch) has honest data to act on. Eviction policy is DELIBERATELY
 * absent — it needs field tuning, not armchair design.
 *
 * Design (2 parts, stdlib only, no OS calls):
 *
 *  Part 1 — bounded window cache: fixed-cap array of window ids + per-entry
 *  last_tick. wc_touch() admits or refreshes; when full it returns the
 *  COLD victim id but does NOT remove it (caller decides; enforcement later).
 *  Cap = RAM budget / WIN (e.g. 1000 windows x 20736 B ~= 20 MB).
 *
 *  Part 2 — prefetch hints: LLM inference is layer-ordered, so after serving
 *  tensor at inference rank r, ranks r+1..r+K are the predicted next touches.
 *  wc_prefetch() maps those ranks to window ranges via the caller's
 *  rank->windows function and records them as prefetch hints with a tick.
 *  MoE: router selects top-K experts per layer — the caller passes only the
 *  SELECTED expert ranks, so unselected experts (~69 MB on Huihui) are never
 *  even hinted. That is the whole MoE win, encoded as "don't predict".
 *
 * Verification: tests/test_win_cache.c — independent oracle (hand-computed
 * tick sequences), mutation-checked (cap off-by-one, tick-stale, rank-window
 * mapping shift must all go red).
 */
#ifndef DWGLS_WIN_CACHE_H
#define DWGLS_WIN_CACHE_H

#include <stdint.h>

#define WC_WIN 20736u

typedef struct {
    uint64_t win;        /* window id */
    uint64_t last_tick;  /* last touch tick */
    uint8_t  prefetched; /* 1 = admitted via prefetch hint, 0 = demand touch */
} wc_entry_t;

typedef struct {
    wc_entry_t *tab;     /* caller-owned storage, cap entries */
    uint32_t    cap;     /* max entries (0 = uninit) */
    uint32_t    n;       /* live entries (<= cap) */
    uint64_t    tick;    /* monotonic counter */
    /* stats (honest accounting, no enforcement) */
    uint64_t    hits, misses, prefetch_hints, prefetch_used;
} win_cache_t;

/* init over caller storage. cap = budget_bytes / WC_WIN. */
static inline void wc_init(win_cache_t *c, wc_entry_t *tab, uint32_t cap) {
    c->tab = tab; c->cap = cap; c->n = 0; c->tick = 0;
    c->hits = c->misses = c->prefetch_hints = c->prefetch_used = 0;
}

/* linear lookup; n <= cap is small (1000s) — ponytail: linear scan, hash if cap grows. */
static inline int wc_find(const win_cache_t *c, uint64_t win) {
    for (uint32_t i = 0; i < c->n; i++)
        if (c->tab[i].win == win) return (int)i;
    return -1;
}

/* Demand touch. Returns: 1 = hit, 0 = miss-admitted, -1 = miss+full (victim
 * id written to *victim_out; entry NOT removed — enforcement is later). */
static inline int wc_touch(win_cache_t *c, uint64_t win, uint64_t *victim_out) {
    c->tick++;
    int idx = wc_find(c, win);
    if (idx >= 0) {
        if (c->tab[idx].prefetched) { c->tab[idx].prefetched = 0; c->prefetch_used++; }
        c->tab[idx].last_tick = c->tick;
        c->hits++;
        return 1;
    }
    c->misses++;
    if (c->n < c->cap) {
        c->tab[c->n].win = win;
        c->tab[c->n].last_tick = c->tick;
        c->tab[c->n].prefetched = 0;
        c->n++;
        return 0;
    }
    /* full: pick oldest tick as victim, report it, do NOT remove. */
    uint32_t v = 0;
    for (uint32_t i = 1; i < c->n; i++)
        if (c->tab[i].last_tick < c->tab[v].last_tick) v = i;
    if (victim_out) *victim_out = c->tab[v].win;
    return -1;
}

/* Prefetch hint for one window: admit-if-absent flagged prefetched=1,
 * refresh tick if present. Never evicts. Counts hints. */
static inline void wc_hint(win_cache_t *c, uint64_t win) {
    c->tick++;
    int idx = wc_find(c, win);
    if (idx >= 0) { c->tab[idx].last_tick = c->tick; return; }
    c->prefetch_hints++;
    if (c->n < c->cap) {
        c->tab[c->n].win = win;
        c->tab[c->n].last_tick = c->tick;
        c->tab[c->n].prefetched = 1;
        c->n++;
    }
    /* full: hint dropped + counted (honest: prefetch never forces admission). */
}

/* Prefetch next K inference ranks after rank r. rank_windows(rank, ctx)
 * writes the window range [w0,w1] for that rank; called for r+1..r+K.
 * Returns hints recorded. Caller passes SELECTED ranks only (MoE: top-K). */
typedef void (*wc_rank_fn)(uint32_t rank, void *ctx, uint64_t *w0, uint64_t *w1);
static inline uint32_t wc_prefetch(win_cache_t *c, uint32_t rank, uint32_t k,
                                   wc_rank_fn rank_windows, void *ctx) {
    uint32_t hints = 0;
    for (uint32_t d = 1; d <= k; d++) {
        uint64_t w0 = 0, w1 = 0;
        rank_windows(rank + d, ctx, &w0, &w1);
        for (uint64_t w = w0; w <= w1; w++) { wc_hint(c, w); hints++; }
    }
    return hints;
}

/* Enforcement helper: collect all victims (oldest entries) up to max_victims.
 * Returns number of victims collected. Caller must perform actual eviction
 * (e.g., unmap+remap) and then call wc_remove_victims() to clean the cache.
 * This separates policy (which windows) from mechanism (OS eviction). */
static inline uint32_t wc_collect_victims(win_cache_t *c, uint64_t *victims_out,
                                          uint32_t max_victims) {
    uint32_t collected = 0;
    while (c->n > 0 && collected < max_victims) {
        uint32_t v = 0;
        for (uint32_t i = 1; i < c->n; i++)
            if (c->tab[i].last_tick < c->tab[v].last_tick) v = i;
        victims_out[collected++] = c->tab[v].win;
        /* remove victim by swapping with last */
        c->tab[v] = c->tab[c->n - 1];
        c->n--;
    }
    return collected;
}

/* Remove specific windows from cache (caller has evicted them). */
static inline void wc_remove(win_cache_t *c, uint64_t win) {
    int idx = wc_find(c, win);
    if (idx >= 0) {
        c->tab[idx] = c->tab[c->n - 1];
        c->n--;
    }
}

#endif /* DWGLS_WIN_CACHE_H */
