/* kv_temporal_test.c — hot window + anchor/delta log roundtrip,
 * with NO llama dependency (fake log/blob callbacks in memory).
 *
 * Proves:
 *   1. the ring keeps exactly K frames hot; the (K+1)th evicts the oldest
 *   2. an anchor is cut every K frames
 *   3. seek(T) picks the nearest anchor <= T and replays the right deltas
 *   4. the log roundtrip reproduces every frame (tokens + prefix_hash)
 *   5. the anchor blob roundtrip reproduces the base bytes
 */
#include "kv_temporal.h"
#include <assert.h>

/* ── fake log: two parallel arrays, one per record kind ── */
typedef struct {
    KVTDeltaHdr deltas[128]; int n_delta;
    int32_t     dtokens[128][16];   /* tokens per delta (test uses <=9) */
    KVTAnchor   anchors[16];   int n_anchor;
    uint8_t     blobs[1 << 20];
    size_t      blob_used;
} FakeLog;

static FakeLog g_fl;   /* must not live on the stack */

static int fl_append(void *ud, const char *kind, const void *rec, size_t len) {
    FakeLog *f = (FakeLog *)ud;
    if (!strcmp(kind, "kv-delta")) {
        if (f->n_delta >= 128 || len < sizeof(KVTDeltaHdr)) return -1;
        const KVTDeltaHdr *h = (const KVTDeltaHdr *)rec;
        if (h->n_tokens < 0 || h->n_tokens > 16) return -1;
        f->deltas[f->n_delta] = *h;
        memcpy(f->dtokens[f->n_delta], (const uint8_t *)rec + KVT_DELTA_TOK_OFF,
               (size_t)h->n_tokens * sizeof(int32_t));
        f->n_delta++;
        /* verify the serialised size matches n_tokens */
        if (len != kvt_delta_size(h->n_tokens)) return -1;
    } else if (!strcmp(kind, "kv-anchor")) {
        if (f->n_anchor >= 16 || len != sizeof(KVTAnchor)) return -1;
        f->anchors[f->n_anchor++] = *(const KVTAnchor *)rec;
    } else return -1;
    return 0;
}
static long long fl_blob_append(void *ud, const void *buf, size_t len) {
    FakeLog *f = (FakeLog *)ud;
    if (f->blob_used + len > sizeof(f->blobs)) return -1;
    long long off = (long long)f->blob_used;
    memcpy(f->blobs + f->blob_used, buf, len);
    f->blob_used += len;
    return off;
}
static int fl_blob_read(void *ud, unsigned long long off, void *buf, size_t len) {
    FakeLog *f = (FakeLog *)ud;
    if (off + len > f->blob_used) return -1;
    memcpy(buf, f->blobs + off, len);
    return 0;
}

static uint64_t fnv_tokens(const int32_t *t, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)t[i]; h *= 1099511628211ULL; }
    return h;
}

int main(void) {
    const int K = KVT_ANCHOR_EVERY;     /* 8 */
    const int N = 20;                   /* frames to push */
    int pass = 0, fail = 0;
#define CHECK(c, msg) do { if (c) { pass++; } else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    FakeLog *flp = &g_fl; memset(flp, 0, sizeof(*flp));
    KVTLog L; L.ud = flp; L.log_append = fl_append;
    L.blob_append = fl_blob_append; L.blob_read = fl_blob_read;

    KVTWindow w; kvt_init(&w, K);

    /* ── push N frames; persist every evicted frame + cut anchors ── */
    int evictions = 0, anchor_cuts = 0;
    for (int t = 1; t <= N; t++) {
        int32_t toks[KVT_MAX_TOKENS];
        int32_t nt = 4 + (t % 5);
        for (int i = 0; i < nt; i++) toks[i] = t * 100 + i;
        KVTFrame fr = kvt_frame_make(t, 0, toks, nt);

        KVTFrame ev; memset(&ev, 0, sizeof(ev));
        int had = kvt_commit_frame(&w, &fr, &ev);

        if (kvt_anchor_due(&w)) {
            /* fake base blob = n_tokens * 1024 bytes */
            size_t blen = (size_t)nt * 1024;
            uint8_t *blob = (uint8_t *)malloc(blen);
            for (size_t i = 0; i < blen; i++) blob[i] = (uint8_t)(t + i);
            uint64_t bfnv = 1469598103934665603ULL;
            for (size_t i = 0; i < blen; i++) { bfnv ^= blob[i]; bfnv *= 1099511628211ULL; }
            KVTAnchor a;
            if (kvt_persist_anchor(&L, t, 999 /*base_n_tokens*/, 8, 2, bfnv,
                                   blob, blen, w.since_anchor, &a) != 0) { free(blob); fail++; }
            else anchor_cuts++;
            free(blob);
            w.since_anchor = 0;
            int idx = (w.head - 1 + w.cap) % w.cap;
            w.frames[idx].hdr.parent = t;
        }
        /* persist evicted frame (the only copy left of it) */
        if (had) { if (kvt_persist_delta(&L, &ev) != 0) fail++; else evictions++; kvt_frame_free(&ev); }
    }
    /* drain the window at shutdown */
    for (int i = 0; i < w.count; i++) {
        int idx = (w.head - w.count + i + w.cap * 2) % w.cap;
        if (kvt_persist_delta(&L, &w.frames[idx]) != 0) fail++;
    }

    printf("window K=%d, pushed N=%d, evictions=%d, anchors=%d, log deltas=%d\n",
           K, N, evictions, anchor_cuts, flp->n_delta);

    /* ── 1. ring kept the last K frames hot ── */
    CHECK(w.count == K, "ring holds exactly K frames");
    CHECK(w.turn == N, "window turn == last commit");

    /* ── 2. anchors every K: cuts land at t = K, 2K, ... <= N ── */
    CHECK(anchor_cuts == N / K, "one anchor every K frames (floor(N/K))");

    /* ── 3+4. log roundtrip: every frame reconstructable ── */
    int seen[64]; memset(seen, 0, sizeof(seen));
    for (int i = 0; i < flp->n_delta; i++) {
        KVTDeltaHdr *d = &flp->deltas[i];
        if (d->turn >= 1 && d->turn < 64) seen[d->turn] = 1;
        CHECK(d->prefix_hash == fnv_tokens(flp->dtokens[i], d->n_tokens), "delta prefix_hash matches its tokens");
    }
    int all = 1;
    for (int t = 1; t <= N; t++) if (!seen[t]) { all = 0; printf("  missing frame t%d from log\n", t); }
    CHECK(all, "every frame 1..N present in the delta log");

    /* ── 5. anchor blob roundtrip ── */
    int blob_ok = 1;
    for (int i = 0; i < flp->n_anchor; i++) {
        KVTAnchor *a = &flp->anchors[i];
        uint8_t *buf = (uint8_t *)malloc((size_t)a->blob_len);
        if (fl_blob_read(flp, a->blob_off, buf, (size_t)a->blob_len) != 0) { blob_ok = 0; free(buf); break; }
        uint64_t h = 1469598103934665603ULL;
        for (size_t k = 0; k < a->blob_len; k++) { h ^= buf[k]; h *= 1099511628211ULL; }
        if (h != a->base_fnv) { blob_ok = 0; free(buf); break; }
        for (size_t k = 0; k < a->blob_len; k++)
            if (buf[k] != (uint8_t)(a->turn + k)) { blob_ok = 0; break; }
        free(buf);
    }
    CHECK(blob_ok, "anchor blob roundtrip byte-exact (fnv + content)");

    /* ── 6. seek picks the nearest anchor and the right replay range ── */
    /* anchors were cut at t = K, 2K (8, 16) in this run */
    KVTSeek s;
    kvt_seek(flp->anchors, flp->n_anchor, 10, &s);
    CHECK(s.have_anchor && s.anchor.turn == 8, "seek(10) -> anchor@8");
    CHECK(s.first_delta == 9 && s.last_delta == 10 && s.n_deltas == 2, "seek(10) -> replay t9..t10");

    kvt_seek(flp->anchors, flp->n_anchor, 5, &s);
    CHECK(!s.have_anchor, "seek(5) -> no anchor, replay from scratch");
    CHECK(s.first_delta == 1 && s.n_deltas == 5, "seek(5) -> replay t1..t5");

    kvt_seek(flp->anchors, flp->n_anchor, 20, &s);
    CHECK(s.have_anchor && s.anchor.turn == 16, "seek(20) -> anchor@16");
    CHECK(s.first_delta == 17 && s.n_deltas == 4, "seek(20) -> replay t17..t20 (<=K)");

    kvt_seek(flp->anchors, flp->n_anchor, 8, &s);
    CHECK(s.have_anchor && s.anchor.turn == 8, "seek(8) -> anchor@8 itself");
    CHECK(s.n_deltas == 0, "seek(8) -> zero deltas (anchor covers it)");

    kvt_print_seek(&s);
    printf("=== kv_temporal: %d PASS, %d FAIL ===\n", pass, fail);
    kvt_free(&w);
    return fail ? 1 : 0;
}
