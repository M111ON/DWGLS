/* kv_temporal_test.c — hot window + anchor/delta log roundtrip,
 * with NO llama dependency (fake log/blob callbacks in memory).
 *
 * Proves:
 *   1. the ring keeps exactly K frames hot; the (K+1)th evicts the oldest
 *   2. an anchor is cut at each round boundary (turn 1, 145, 289, ...),
 *      NOT every K frames (memory #7518: the anchor is born from step)
 *   3. seek(T) picks the nearest anchor <= T and replays the right deltas
 *   4. the log roundtrip reproduces every frame (tokens + prefix_hash)
 *   5. the anchor blob roundtrip reproduces the base bytes
 */
#include "kv_temporal.h"
#include "kv_temporal_read.h"
#include <assert.h>

/* ── fake log: two parallel arrays, one per record kind ── */
typedef struct {
    KVTDeltaHdr deltas[512]; int n_delta;
    int32_t     dtokens[512][16];   /* tokens per delta (test uses <=9) */
    KVTAnchor   anchors[16];   int n_anchor;
    uint8_t     blobs[1 << 20];
    size_t      blob_used;
} FakeLog;

static FakeLog g_fl;   /* must not live on the stack */

static int fl_append(void *ud, const char *kind, const void *rec, size_t len) {
    FakeLog *f = (FakeLog *)ud;
    if (!strcmp(kind, "kv-delta")) {
        if (f->n_delta >= 512 || len < sizeof(KVTDeltaHdr)) return -1;
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
    const int K = KVT_WINDOW;         /* 8: hot-window RAM budget only */
    const int N = 300;                  /* crosses 3 round boundaries */
    const int R = KVT_ROUND;            /* 144: anchor cadence */
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

    /* ── 2. anchors at round boundaries only: t = 1, 145, 289 ── */
    CHECK(anchor_cuts == 3, "3 anchors in 300 frames (t=1,145,289)");
    int exp_turns[3] = {1, 1 + R, 1 + 2 * R};
    int turns_ok = (flp->n_anchor == 3);
    for (int i = 0; turns_ok && i < 3; i++)
        if (flp->anchors[i].turn != exp_turns[i]) turns_ok = 0;
    CHECK(turns_ok, "anchor turns are exactly {1,145,289}");

    /* ── 3+4. log roundtrip: every frame reconstructable ── */
    int seen[512]; memset(seen, 0, sizeof(seen));
    for (int i = 0; i < flp->n_delta; i++) {
        KVTDeltaHdr *d = &flp->deltas[i];
        if (d->turn >= 1 && d->turn < 512) seen[d->turn] = 1;
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
    /* anchors were cut at t = 1, 145, 289 in this run */
    KVTSeek s;
    kvt_seek(flp->anchors, flp->n_anchor, 10, &s);
    CHECK(s.have_anchor && s.anchor.turn == 1, "seek(10) -> anchor@1");
    CHECK(s.first_delta == 2 && s.last_delta == 10 && s.n_deltas == 9, "seek(10) -> replay t2..t10");

    kvt_seek(flp->anchors, flp->n_anchor, 144, &s);
    CHECK(s.have_anchor && s.anchor.turn == 1, "seek(144) -> anchor@1 (round edge)");
    CHECK(s.first_delta == 2 && s.n_deltas == 143, "seek(144) -> replay t2..t144 (<=ROUND-1)");

    kvt_seek(flp->anchors, flp->n_anchor, 145, &s);
    CHECK(s.have_anchor && s.anchor.turn == 145, "seek(145) -> anchor@145 itself");
    CHECK(s.n_deltas == 0, "seek(145) -> zero deltas (anchor covers it)");

    kvt_seek(flp->anchors, flp->n_anchor, 200, &s);
    CHECK(s.have_anchor && s.anchor.turn == 145, "seek(200) -> anchor@145");
    CHECK(s.first_delta == 146 && s.n_deltas == 55, "seek(200) -> replay t146..t200");

    kvt_seek(flp->anchors, flp->n_anchor, 300, &s);
    CHECK(s.have_anchor && s.anchor.turn == 289, "seek(300) -> anchor@289");
    CHECK(s.first_delta == 290 && s.n_deltas == 11, "seek(300) -> replay t290..t300");

    kvt_print_seek(&s);

    /* ── 7. the log is readable by a process that never produced it ──
     * The replay path (a later process) has only the file: it must recover the
     * anchor records and each frame's tokens, must IGNORE other pool records,
     * and must not trust a truncated last line from a killed writer. */
    {
        const char *tmp = "build/kvt_read_test.log";
        FILE *f = fopen(tmp, "wb");
        CHECK(f != NULL, "reader: temp log created");
        if (f) {
            /* anchor, one foreign record, two deltas out of order, then a
               truncated line with no closing brace */
            fputs("{\"v\":3,\"app\":\"t\",\"kind\":\"kv-anchor\",\"text\":\"{\\\"turn\\\":145,"
                  "\\\"base_n_tokens\\\":99,\\\"type_k\\\":8,\\\"type_v\\\":2,\\\"blob_off\\\":70,"
                  "\\\"blob_len\\\":5,\\\"blob_fnv\\\":18446744073709551615}\",\"to\":\"all\","
                  "\"prev\":\"GENESIS\",\"hash\":\"aa\"}\n", f);
            fputs("{\"v\":3,\"app\":\"other\",\"kind\":\"note\",\"text\":\"unrelated\",\"to\":\"all\","
                  "\"prev\":\"aa\",\"hash\":\"bb\"}\n", f);
            fputs("{\"v\":3,\"app\":\"t\",\"kind\":\"kv-delta\",\"text\":\"{\\\"turn\\\":2,"
                  "\\\"parent\\\":1,\\\"tokens\\\":[7,8,9]}\",\"to\":\"all\",\"prev\":\"bb\","
                  "\"hash\":\"cc\"}\n", f);
            fputs("{\"v\":3,\"app\":\"t\",\"kind\":\"kv-delta\",\"text\":\"{\\\"turn\\\":1,"
                  "\\\"parent\\\":1,\\\"tokens\\\":[1,2]}\",\"to\":\"all\",\"prev\":\"cc\","
                  "\"hash\":\"dd\"}\n", f);
            fputs("{\"v\":3,\"app\":\"t\",\"kind\":\"kv-delta\",\"text\":\"{\\\"turn\\\":3,"
                  "\\\"parent\\\":1,\\\"tokens\\\":[5,6", f);   /* killed mid-line */
            fclose(f);
        }

        KVTAnchor ra[8]; KVLogDelta rd[8];
        int na = 0, nd = 0;
        kvl_read(tmp, ra, 8, &na, rd, 8, &nd);
        CHECK(na == 1, "reader: one anchor recovered");
        CHECK(nd == 2, "reader: two deltas recovered (foreign + truncated skipped)");
        if (na == 1) {
            CHECK(ra[0].turn == 145 && ra[0].base_n_tokens == 99 &&
                  ra[0].blob_off == 70 && ra[0].blob_len == 5,
                  "reader: anchor fields exact (turn, base, blob)");
            CHECK(ra[0].base_fnv == 18446744073709551615ULL,
                  "reader: blob_fnv above INT64_MAX survives (unsigned parse)");
            CHECK(ra[0].kv_type_k == 8 && ra[0].kv_type_v == 2, "reader: kv types recovered");
        }
        if (nd == 2) {
            CHECK(rd[0].turn == 1 && rd[1].turn == 2, "reader: deltas sorted ascending");
            CHECK(rd[0].n == 2 && rd[0].tok[0] == 1 && rd[0].tok[1] == 2, "reader: turn1 tokens exact");
            CHECK(rd[1].n == 3 && rd[1].tok[2] == 9 && rd[1].parent == 1, "reader: turn2 tokens + parent exact");
        }
        KVLogDelta *hit = kvl_find(rd, nd, 2);
        CHECK(hit != NULL && hit->n == 3, "reader: kvl_find locates a turn");
        CHECK(kvl_find(rd, nd, 99) == NULL, "reader: kvl_find misses a turn that is not there");
        kvl_free(rd, nd);
        remove(tmp);
    }

    /* ── 8. the OTHER writer's punctuation ──
     * pool.py emits json.dumps defaults — `"kind": "kv-delta"` and
     * `\"turn\": 7` with spaces — while our host writes compact JSON. A reader
     * that matches only one shape silently ignores half the log (measured: the
     * chain forked because the tail hash of a pool line could not be read). */
    {
        const char *tmp = "build/kvt_read_spaced.log";
        FILE *f = fopen(tmp, "wb");
        CHECK(f != NULL, "spaced log: created");
        if (f) {
            fputs("{ \"v\": 3, \"t\": 1, \"app\": \"kv-temporal\", \"kind\": \"kv-anchor\", "
                  "\"text\": \"{\\\"turn\\\": 145, \\\"base_n_tokens\\\": 99, \\\"type_k\\\": 8, "
                  "\\\"type_v\\\": 2, \\\"blob_off\\\": 5, \\\"blob_len\\\": 6, \\\"blob_fnv\\\": 7}\", "
                  "\"to\": \"all\", \"prev\": \"GENESIS\", \"hash\": \"aa\"}\n", f);
            fputs("{ \"v\": 3, \"t\": 2, \"app\": \"kv-temporal\", \"kind\": \"kv-delta\", "
                  "\"text\": \"{\\\"turn\\\": 7, \\\"parent\\\": 1, \\\"tokens\\\": [11, 12, 13]}\", "
                  "\"to\": \"all\", \"prev\": \"aa\", \"hash\": \"bb\"}\n", f);
            fclose(f);
        }
        KVTAnchor sa[4]; KVLogDelta sd[4];
        int na = 0, nd = 0;
        kvl_read(tmp, sa, 4, &na, sd, 4, &nd);
        CHECK(na == 1 && sa[0].turn == 145 && sa[0].blob_off == 5 && sa[0].base_fnv == 7,
              "spaced log: anchor parsed through json.dumps spacing");
        CHECK(nd == 1 && sd[0].turn == 7 && sd[0].n == 3 && sd[0].tok[2] == 13,
              "spaced log: delta tokens parsed through json.dumps spacing");
        kvl_free(sd, nd);
        remove(tmp);
    }

    printf("=== kv_temporal: %d PASS, %d FAIL ===\n", pass, fail);
    kvt_free(&w);
    return fail ? 1 : 0;
}
