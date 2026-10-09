/* kv_temporal.h — TEMPORAL KV storage: hot window in RAM, then a log.
 *
 * The model (decided with the owner, 2026-10-13):
 *
 *   live inference
 *     │  each finished chat turn is a FRAME
 *     ▼
 *   ┌──────────────── HOT WINDOW (RAM) ────────────────┐
 *   │ the last K frames' KV stays resident in the ctx  │
 *   └──────────────────────┬───────────────────────────┘
 *                          │ a frame falls out of the window
 *                          ▼
 *   append to LOG (append-only, hash-chained by the host)
 *     - every frame    -> a kv-delta  record (token suffix, ~200 B)
 *     - every K frames -> a kv-anchor record (resume point) whose base
 *                         BLOB is written to a side blob file, NOT the log
 *
 * WHY THIS SHAPE (measured, not assumed):
 *   - byte-deltas on the KV blob do not pay: 98.6% churn/token, so we
 *     never store KV diffs. A frame's delta is its TOKENS, not its KV.
 *   - storing a full base per turn duplicates the whole prefix KV each
 *     time (666 KB/turn observed) — so bases are only kept every K frames.
 *   - the log therefore stays light (delta-sized), while seek() replays at
 *     most K deltas from the nearest anchor.
 *
 * The header is HOST-AGNOSTIC: it never includes pool.h or llama.h. The
 * host supplies two callbacks (append a record, read records back), so the
 * same code drives the DWGLS KV tooling and the chat-pool log.
 */
#ifndef KV_TEMPORAL_H
#define KV_TEMPORAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef KVT_ANCHOR_EVERY
#define KVT_ANCHOR_EVERY 8     /* a full base blob every K frames */
#endif
#define KVT_MAX_TOKENS 4096    /* per-frame delta cap (one turn) */
#define KVT_PATH 128

/* ── record kinds written to the log ── */
typedef struct {
    int32_t  turn;          /* 1-based frame index */
    uint64_t prefix_hash;   /* FNV-1a over this frame's tokens */
    int32_t  parent;        /* turn of the anchor this delta hangs off */
    int32_t  n_tokens;      /* tokens in this frame */
    int32_t  tokens[KVT_MAX_TOKENS];
} KVTDelta;

typedef struct {
    int32_t  turn;          /* frame index this anchor resumes to */
    int32_t  base_n_tokens; /* tokens covered by the base blob */
    uint32_t kv_type_k;     /* ggml_type of the base K cache (opaque here) */
    uint32_t kv_type_v;
    uint64_t base_fnv;      /* FNV-1a of the base blob (integrity, not sha) */
    uint64_t blob_off;      /* offset into the side blob file */
    uint64_t blob_len;      /* blob length in bytes */
    int32_t  n_back;        /* frames between this anchor and the previous */
} KVTAnchor;

/* ── host callbacks: the log and the blob file ── */
typedef struct {
    void *ud;
    /* append one record; kind is "kv-anchor" | "kv-delta" */
    int (*log_append)(void *ud, const char *kind, const void *rec, size_t len);
    /* append base blob bytes to the side file; returns offset or -1 */
    long long (*blob_append)(void *ud, const void *buf, size_t len);
    /* read base blob bytes back */
    int (*blob_read)(void *ud, unsigned long long off, void *buf, size_t len);
} KVTLog;

/* ── the in-RAM hot window ── */
typedef struct {
    KVTDelta *frames;       /* ring of resident frames (hot) */
    int       cap;          /* = K */
    int       head;         /* next write slot */
    int       count;        /* frames currently hot */
    int       turn;         /* last committed turn */
    int       since_anchor; /* frames since the last anchor was cut */
} KVTWindow;

static inline void kvt_init(KVTWindow *w, int cap) {
    w->cap = cap > 0 ? cap : 1;
    w->frames = (KVTDelta *)calloc((size_t)w->cap, sizeof(KVTDelta));
    w->head = w->count = w->turn = w->since_anchor = 0;
}

static inline void kvt_free(KVTWindow *w) { free(w->frames); w->frames = NULL; }

/* A host that wants to keep the ONLY copy of a frame must copy it out
 * before the ring overwrites it — hence the out param `evicted`. */
static inline int kvt_commit_frame(KVTWindow *w, const KVTDelta *fr, KVTDelta *evicted) {
    int had = (w->count == w->cap);
    if (had && evicted) *evicted = w->frames[w->head];
    w->frames[w->head] = *fr;
    w->head = (w->head + 1) % w->cap;
    if (w->count < w->cap) w->count++;
    w->turn = fr->turn;
    w->since_anchor++;
    return had;   /* 1 = a frame was evicted (caller must persist it) */
}

/* Is an anchor due now? (every K frames, and on the first frame) */
static inline int kvt_anchor_due(const KVTWindow *w) {
    return w->since_anchor >= KVT_ANCHOR_EVERY;
}

/* ── persistence: write one frame's delta to the log ── */
static inline int kvt_persist_delta(const KVTLog *L, const KVTDelta *fr) {
    return L->log_append(L->ud, "kv-delta", fr, sizeof(*fr));
}

/* ── cut an anchor: hand the caller a base blob + record; the caller
 *    (which owns the ctx) supplies the blob bytes. ── */
static inline int kvt_persist_anchor(const KVTLog *L, int32_t turn, int32_t base_n_tokens,
                                     uint32_t tk, uint32_t tv, uint64_t base_fnv,
                                     const void *blob, size_t blob_len, int32_t n_back,
                                     KVTAnchor *out) {
    long long off = L->blob_append(L->ud, blob, blob_len);
    if (off < 0) return -1;
    KVTAnchor a; memset(&a, 0, sizeof(a));
    a.turn = turn; a.base_n_tokens = base_n_tokens;
    a.kv_type_k = tk; a.kv_type_v = tv; a.base_fnv = base_fnv;
    a.blob_off = (uint64_t)off; a.blob_len = (uint64_t)blob_len; a.n_back = n_back;
    if (out) *out = a;
    return L->log_append(L->ud, "kv-anchor", &a, sizeof(a));
}

/* ── seek plan: which anchor to load and how many deltas to replay ──
 *    The host reads records back from the log and calls this to know the
 *    cheapest reconstruction of frame `want`. ── */
typedef struct {
    int      have_anchor;    /* 1 = load the anchor base first */
    KVTAnchor anchor;
    int32_t  first_delta;    /* replay deltas from this turn .. */
    int32_t  last_delta;     /* .. up to and including this turn */
    int32_t  n_deltas;       /* = last - first + 1 */
    int32_t  total_tokens;   /* tokens covered after the replay */
} KVTSeek;

static inline void kvt_seek(const KVTAnchor *anchors, int n_anchors,
                            int32_t want, KVTSeek *out) {
    memset(out, 0, sizeof(*out));
    int best = -1;
    for (int i = 0; i < n_anchors; i++)
        if (anchors[i].turn <= want && (best < 0 || anchors[i].turn > anchors[best].turn))
            best = i;
    if (best >= 0) {
        out->have_anchor = 1;
        out->anchor = anchors[best];
        out->first_delta = anchors[best].turn + 1;
        out->total_tokens = anchors[best].base_n_tokens;
    } else {
        out->first_delta = 1;
    }
    out->last_delta = want;
    out->n_deltas = want - out->first_delta + 1;
    if (out->n_deltas < 0) out->n_deltas = 0;
}

static inline void kvt_print_seek(const KVTSeek *s) {
    if (s->have_anchor)
        printf("seek: anchor@t%d (%d tok, blob +%llu/%llu B) then replay %d delta(s) t%d..t%d\n",
               s->anchor.turn, s->anchor.base_n_tokens,
               (unsigned long long)s->anchor.blob_off, (unsigned long long)s->anchor.blob_len,
               s->n_deltas, s->first_delta, s->last_delta);
    else
        printf("seek: no anchor — replay %d delta(s) t%d..t%d from scratch\n",
               s->n_deltas, s->first_delta, s->last_delta);
}

#endif /* KV_TEMPORAL_H */
