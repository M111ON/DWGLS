#pragma once
#ifndef GEAR_SHIFT_H
#define GEAR_SHIFT_H

/*
 * gear_shift.h — Generic streaming router (Tier-2, no data storage)
 *
 * GearShift is a routing/scheduling layer that connects any source to any
 * destination. It does NOT store data — it orchestrates streaming.
 *
 * Design hierarchy:
 *   DRamTile  = storage (where data lives)
 *   GearShift = routing (where data goes, when, how)
 *   GearLock  = control (priority, order, speed)
 *
 * State machine (per entry):
 *   GS_IDLE → GS_STREAMING → GS_DONE → GS_IDLE (re-stream)
 *                               ↓
 *                            GS_FAILED
 *
 * Use cases:
 *   - Tensor → GPU (ggml_backend_tensor_set)
 *   - KV → disk (fwrite/fseek)
 *   - Bond → cold storage (migrate)
 *   - Any src → any dst (generic callback)
 *
 * API:
 *   gs_init()           — init with source pointer provider
 *   gs_register()       — register a streamable entry
 *   gs_set_dest()       — set destination callback for an entry
 *   gs_stream()         — stream one entry via provider
 *   gs_stream_from()    — stream one entry with explicit src
 *   gs_reset_done()     — reset DONE → IDLE for re-streaming
 *   gs_stats()          — print stats
 *   gs_destroy()        — cleanup (no data owned)
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define GS_MAX_ENTRIES     2048
#define GS_NAME_MAX        128

/* ── Stream state per entry ────────────────────────────────── */
typedef enum {
    GS_IDLE      = 0,   /* registered, ready to stream */
    GS_STREAMING = 1,   /* streaming in progress */
    GS_DONE      = 2,   /* streamed successfully */
    GS_FAILED    = 3    /* stream failed */
} GSState;

/* ── Upload/stream callback ────────────────────────────────── */
/* Returns 0 on success. user_data is per-entry private data.
 * src_ptr/src_size: source data location and size.
 * dst_ctx: destination context (GPU buffer, file handle, etc.) */
typedef int (*GSStreamFn)(const void *src_ptr, size_t src_size,
                           void *dst_ctx, void *user_data);

/* ── Source provider callback ───────────────────────────────── */
/* Called to get current source pointer for an entry.
 * Allows source to change (e.g., DRamTile pointer after re-malloc). */
typedef void *(*GSSrcFn)(const char *name, size_t *out_size, void *user_data);

/* ── Per-entry record ──────────────────────────────────────── */
typedef struct {
    char         name[GS_NAME_MAX];
    void        *src_ptr;        /* current source data pointer */
    size_t       src_size;       /* source data size in bytes */
    float        priority;       /* from GearLock (0..1, higher = stream first) */
    uint32_t     access_tick;    /* last access tick */
    uint32_t     layer;          /* optional: layer/section index */
    uint16_t     state;          /* GSState */
    uint16_t     flags;          /* reserved */
    void        *dst_ctx;        /* destination context (per-entry) */
    void        *user_data;      /* private data for callbacks */
    GSStreamFn   stream_fn;      /* destination callback */
} GSEntry;

/* ── GearShift store (no data, just routing) ───────────────── */
typedef struct {
    GSEntry     entries[GS_MAX_ENTRIES];
    int         n_entries;
    uint32_t    tick;            /* monotonic tick */
    GSSrcFn     src_provider;   /* optional: auto-refresh src_ptr */
    void       *src_user_data;  /* user_data for src_provider */
    /* stats */
    uint32_t    n_streamed;     /* lifetime successful streams */
    uint32_t    n_errors;       /* stream failures */
} GearShiftStore;

/* ── Init ──────────────────────────────────────────────────── */
static inline void gs_init(GearShiftStore *gs) {
    memset(gs, 0, sizeof(*gs));
}

/* Set optional source provider (auto-refresh src_ptr on each stream) */
static inline void gs_set_src_provider(GearShiftStore *gs,
                                        GSSrcFn fn, void *user_data) {
    gs->src_provider = fn;
    gs->src_user_data = user_data;
}

/* ── Register ──────────────────────────────────────────────── */
static inline int gs_register(GearShiftStore *gs, const char *name,
                               uint32_t layer) {
    if (gs->n_entries >= GS_MAX_ENTRIES) return -1;
    GSEntry *e = &gs->entries[gs->n_entries];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, GS_NAME_MAX - 1);
    e->name[GS_NAME_MAX - 1] = '\0';
    e->layer = layer;
    e->state = GS_IDLE;
    e->stream_fn = NULL;
    e->dst_ctx = NULL;
    e->user_data = NULL;

    /* auto-fetch src_ptr if provider is set */
    if (gs->src_provider) {
        e->src_ptr = gs->src_provider(name, &e->src_size, gs->src_user_data);
    }

    gs->n_entries++;
    return 0;
}

/* ── Set destination callback ───────────────────────────────── */
static inline int gs_set_dest(GearShiftStore *gs, const char *name,
                               GSStreamFn fn, void *dst_ctx, void *user_data) {
    for (int i = 0; i < gs->n_entries; i++) {
        if (strcmp(gs->entries[i].name, name) == 0) {
            gs->entries[i].stream_fn = fn;
            gs->entries[i].dst_ctx = dst_ctx;
            gs->entries[i].user_data = user_data;
            return 0;
        }
    }
    return -1;
}

/* Set destination for ALL entries (batch setup) */
static inline void gs_set_dest_all(GearShiftStore *gs,
                                    GSStreamFn fn, void *dst_ctx) {
    for (int i = 0; i < gs->n_entries; i++) {
        gs->entries[i].stream_fn = fn;
        gs->entries[i].dst_ctx = dst_ctx;
    }
}

/* ── Lookup ────────────────────────────────────────────────── */
static inline GSEntry *gs_find(GearShiftStore *gs, const char *name) {
    for (int i = 0; i < gs->n_entries; i++) {
        if (strcmp(gs->entries[i].name, name) == 0)
            return &gs->entries[i];
    }
    return NULL;
}

/* ── Direct index access (O(1), no strcmp) ─────────────────── */
/* Use when DRamTile slot index is already known — avoids linear scan */
static inline GSEntry *gs_get_idx(GearShiftStore *gs, int idx) {
    if (idx < 0 || idx >= gs->n_entries) return NULL;
    return &gs->entries[idx];
}

/* Stream by index — O(1) lookup, no name comparison */
static inline int gs_stream_idx(GearShiftStore *gs, int idx,
                                 void *src_ptr, size_t src_size) {
    GSEntry *e = gs_get_idx(gs, idx);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;
    if (!e->stream_fn) return -1;
    e->src_ptr = src_ptr;
    e->src_size = src_size;
    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;
    int ret = e->stream_fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data);
    if (ret == 0) { e->state = GS_DONE; gs->n_streamed++; }
    else { e->state = GS_FAILED; gs->n_errors++; }
    return ret;
}

/* ── Stream one entry ──────────────────────────────────────── */
/* Core operation: src → dst via callback. No middleman. */
static inline int gs_stream(GearShiftStore *gs, const char *name) {
    GSEntry *e = gs_find(gs, name);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;  /* already streamed this cycle */
    if (!e->stream_fn) return -1;

    /* refresh src_ptr if provider is set */
    if (gs->src_provider) {
        e->src_ptr = gs->src_provider(name, &e->src_size, gs->src_user_data);
    }
    if (!e->src_ptr || e->src_size == 0) return -1;

    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;

    int ret = e->stream_fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data);

    if (ret == 0) {
        e->state = GS_DONE;
        gs->n_streamed++;
    } else {
        e->state = GS_FAILED;
        gs->n_errors++;
    }
    return ret;
}

/* ── Stream with explicit src_ptr (override provider) ──────── */
static inline int gs_stream_from(GearShiftStore *gs, const char *name,
                                  void *src_ptr, size_t src_size) {
    GSEntry *e = gs_find(gs, name);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;  /* already streamed this cycle */
    if (!e->stream_fn) return -1;

    e->src_ptr = src_ptr;
    e->src_size = src_size;
    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;

    int ret = e->stream_fn(src_ptr, src_size, e->dst_ctx, e->user_data);

    if (ret == 0) {
        e->state = GS_DONE;
        gs->n_streamed++;
    } else {
        e->state = GS_FAILED;
        gs->n_errors++;
    }
    return ret;
}

/* ── Reset DONE → IDLE for re-streaming next cycle ─────────── */
static inline void gs_reset_done(GearShiftStore *gs) {
    for (int i = 0; i < gs->n_entries; i++) {
        if (gs->entries[i].state == GS_DONE)
            gs->entries[i].state = GS_IDLE;
    }
}

/* Reset all to IDLE */
static inline void gs_reset_all(GearShiftStore *gs) {
    for (int i = 0; i < gs->n_entries; i++)
        gs->entries[i].state = GS_IDLE;
}

/* ── Invalidate: remove entry by name (used by DRamTile evict) ─ */
static inline int gs_invalidate(GearShiftStore *gs, const char *name) {
    for (int i = 0; i < gs->n_entries; i++) {
        if (strcmp(gs->entries[i].name, name) == 0) {
            /* swap with last and shrink */
            gs->entries[i] = gs->entries[gs->n_entries - 1];
            memset(&gs->entries[gs->n_entries - 1], 0, sizeof(GSEntry));
            gs->n_entries--;
            return 0;
        }
    }
    return -1;
}

/* ── Update priority (from GearLock) ───────────────────────── */
static inline void gs_update_priorities(GearShiftStore *gs,
                                         const float *priors, int n) {
    int lim = gs->n_entries < n ? gs->n_entries : n;
    for (int i = 0; i < lim; i++)
        gs->entries[i].priority = priors[i];
}

/* ── Stats ─────────────────────────────────────────────────── */
static inline void gs_stats(const GearShiftStore *gs, FILE *fp) {
    int n_idle = 0, n_done = 0, n_failed = 0;
    for (int i = 0; i < gs->n_entries; i++) {
        switch (gs->entries[i].state) {
            case GS_IDLE:     n_idle++; break;
            case GS_DONE:     n_done++; break;
            case GS_FAILED:   n_failed++; break;
            default: break;
        }
    }
    fprintf(fp, "=== GearShift ===\n");
    fprintf(fp, "  Entries:  %d registered (%d idle, %d done, %d failed)\n",
            gs->n_entries, n_idle, n_done, n_failed);
    fprintf(fp, "  Streams:  %u done, %u errors\n",
            gs->n_streamed, gs->n_errors);
}

/* ── Destroy (clear all entry state + counters) ──────────────── */
static inline void gs_destroy(GearShiftStore *gs) {
    memset(gs->entries, 0, sizeof(gs->entries));
    gs->n_entries = 0;
    gs->tick = 0;
    gs->n_streamed = 0;
    gs->n_errors = 0;
}

#endif /* GEAR_SHIFT_H */
