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
 *     - round boundary -> a kv-anchor record (resume point, memory #7501:
 *                         the anchor is born from step, cut at the first
 *                         step of every round) whose base BLOB is written
 *                         to a side blob file, NOT the log
 *
 * WHY THIS SHAPE (measured, not assumed):
 *   - byte-deltas on the KV blob do not pay: 98.6% churn/token, so we
 *     never store KV diffs. A frame's delta is its TOKENS, not its KV.
 *   - storing a full base per turn duplicates the whole prefix KV each
 *     time (666 KB/turn observed) — so bases are only kept every K frames.
 *   - the log therefore stays light (delta-sized), while seek() replays at
 *     most KVT_ROUND-1 deltas from the nearest anchor.
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
#define KVT_ANCHOR_EVERY 8     /* COMPAT: the old single knob (window cap AND
                                  cadence). Cadence was wrong in principle —
                                  8 has no meaning as a cadence (#7518).
                                  Kept so old callers compile; equals the
                                  window default below. */
#endif
#define KVT_WINDOW 8         /* hot-window RAM budget: frames kept resident */
#ifndef KVT_ROUND
#define KVT_ROUND 144        /* steps per round (memory #7501). An anchor is
                                cut at the first step of every round:
                                turn 1, 145, 289, ... */
#endif
#define KVT_MAX_TOKENS 4096    /* per-frame delta cap (one turn) */
#define KVT_PATH 128

/* ── record kinds written to the log ──
 *
 * A delta carries ONLY the tokens of its frame — but at a per-record width,
 * not a fixed 4096-slot array. A fixed array costs 16 KB PER FRAME whether
 * the turn had 5 tokens or 500 (measured: a 22-line log ballooned to 764 KB),
 * which defeats the whole point of a lightweight frame log. The serialised
 * form is therefore [header][tokens...] with n_tokens deciding the length.
 */
#define KVT_MAX_TOKENS 4096    /* per-frame cap (one turn) */
#define KVT_PATH 128

typedef struct {
    int32_t  turn;          /* 1-based frame index */
    uint64_t prefix_hash;   /* FNV-1a over this frame's tokens */
    int32_t  parent;        /* turn of the anchor this delta hangs off */
    int32_t  n_tokens;      /* tokens in this frame (<= KVT_MAX_TOKENS) */
} KVTDeltaHdr;

/* Serialised size of a delta with n tokens. */
static inline size_t kvt_delta_size(int32_t n) {
    return sizeof(KVTDeltaHdr) + (size_t)n * sizeof(int32_t);
}

/* Offset of the token array inside a serialised delta. */
#define KVT_DELTA_TOK_OFF ((size_t)sizeof(KVTDeltaHdr))

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

/* ── the in-RAM hot window ──
 * Each slot owns a small heap buffer sized to its own token count, so a
 * resident frame costs ~16 B + 4 B/token, not the fixed 16 KB a slab of
 * KVT_MAX_TOKENS would. */
typedef struct {
    KVTDeltaHdr hdr;
    int32_t    *tokens;     /* malloc'd, hdr.n_tokens long (NULL when empty) */
} KVTFrame;

typedef struct {
    KVTFrame *frames;       /* ring of resident frames (hot) */
    int       cap;          /* = K */
    int       head;         /* next write slot */
    int       count;        /* frames currently hot */
    int       turn;         /* last committed turn */
    int       since_anchor; /* frames since the last anchor was cut */
} KVTWindow;

static inline void kvt_frame_free(KVTFrame *f) { free(f->tokens); f->tokens = NULL; f->hdr.n_tokens = 0; }

static inline void kvt_init(KVTWindow *w, int cap) {
    w->cap = cap > 0 ? cap : 1;
    w->frames = (KVTFrame *)calloc((size_t)w->cap, sizeof(KVTFrame));
    w->head = w->count = w->turn = w->since_anchor = 0;
}

static inline void kvt_free(KVTWindow *w) {
    if (w->frames) for (int i = 0; i < w->cap; i++) kvt_frame_free(&w->frames[i]);
    free(w->frames); w->frames = NULL;
}

/* A host that wants to keep the ONLY copy of a frame must copy it out
 * before the ring overwrites it — hence the out param `evicted` (deep). */
static inline int kvt_commit_frame(KVTWindow *w, const KVTFrame *fr, KVTFrame *evicted) {
    int had = (w->count == w->cap);
    if (had && evicted) {
        *evicted = w->frames[w->head];              /* move ownership out */
        w->frames[w->head].tokens = NULL;
        w->frames[w->head].hdr.n_tokens = 0;
    }
    /* move the caller's frame in (caller must not free it afterwards) */
    w->frames[w->head] = *fr;
    w->head = (w->head + 1) % w->cap;
    if (w->count < w->cap) w->count++;
    w->turn = fr->hdr.turn;
    w->since_anchor++;
    return had;   /* 1 = a frame was evicted (caller must persist it) */
}

/* Build a frame from raw parts (allocates the token buffer). */
static inline KVTFrame kvt_frame_make(int32_t turn, int32_t parent,
                                      const int32_t *tokens, int32_t n) {
    KVTFrame f; memset(&f, 0, sizeof(f));
    f.hdr.turn = turn; f.hdr.parent = parent; f.hdr.n_tokens = n;
    uint64_t h = 1469598103934665603ULL;
    for (int32_t i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)tokens[i]; h *= 1099511628211ULL; }
    f.hdr.prefix_hash = h;
    if (n > 0) { f.tokens = (int32_t *)malloc((size_t)n * sizeof(int32_t)); memcpy(f.tokens, tokens, (size_t)n * sizeof(int32_t)); }
    return f;
}

/* Is an anchor due now? The anchor is born from STEP: due iff the just-
 * committed turn is the first step of a round, i.e. round(step) changes
 * here — round(step) = (step-1) / KVT_ROUND, O(1), no scan (memory #7501,
 * #7518). CALL AFTER kvt_commit_frame (w->turn is the turn just committed:
 * due at turn 1, 145, 289, ...). since_anchor stays as accounting (it
 * becomes the anchor's n_back); it no longer drives the decision. */
static inline int kvt_anchor_due(const KVTWindow *w) {
    return w->turn > 0 && ((w->turn - 1) % KVT_ROUND) == 0;
}

/* ── persistence: write one frame's delta to the log (variable width) ── */
static inline int kvt_persist_delta(const KVTLog *L, const KVTFrame *fr) {
    size_t len = kvt_delta_size(fr->hdr.n_tokens);
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return -1;
    memcpy(buf, &fr->hdr, sizeof(fr->hdr));
    if (fr->hdr.n_tokens > 0)
        memcpy(buf + KVT_DELTA_TOK_OFF, fr->tokens, (size_t)fr->hdr.n_tokens * sizeof(int32_t));
    int rc = L->log_append(L->ud, "kv-delta", buf, len);
    free(buf);
    return rc;
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
