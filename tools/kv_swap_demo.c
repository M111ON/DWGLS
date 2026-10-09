/*
 * tools/kv_swap_demo.c — "use only what you use": a session longer than n_ctx.
 * ══════════════════════════════════════════════════════════════════════════
 * The earlier tools proved the mechanics. This one answers the practical
 * question: can a long chat session run on a SMALL context by keeping the
 * resident KV to just the recent turns and spilling the rest to the log?
 *
 *   n_ctx = 512                    (deliberately tiny)
 *   session = 24 turns             (far more than 512 tokens total)
 *
 * The serving loop holds only the last few turns in the ctx (the hot
 * window). When the ctx would overflow, it drops the OLDEST turns from the
 * KV (llama_memory_seq_rm) — the session keeps going, bounded RAM. To read
 * something old again, it SEEKS: resume the nearest anchor + replay deltas.
 *
 * What we measure and print:
 *   * ctx high-water mark — the MOST positions ever live at once, which must
 *     stay <= n_ctx no matter how many turns the session has;
 *   * bytes of KV held at that peak (the thing "use only what you use" cuts);
 *   * drop count and the seek plan for a turn that was already dropped.
 *
 * BUILD: gcc -O2 -std=c11 -I core -I I:/llama/include -o build/kv_swap_demo
 *          tools/kv_swap_demo.c .../llama.dll .../ggml*.dll -lm
 * RUN:   ./build/kv_swap_demo [model.gguf] [tmpdir] [backend_dir]
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include "kv_temporal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32")
#endif

#define NT 24                  /* turns in the session */
#define NCAP 8192              /* token scratch for the whole session */
#define SMALL_CTX 256          /* the point: a tiny context */
#define HOLD_TURNS 4           /* how many turns stay resident before dropping */

static void sha256_hex(const void *data, size_t len, char out[65]) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0; HCRYPTHASH hHash = 0;
    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (const BYTE *)data, (DWORD)len, 0);
    BYTE dig[32]; DWORD dl = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, dig, &dl, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", dig[i]);
    out[64] = 0;
    CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0);
#else
    (void)data; (void)len; out[0] = 0;
#endif
}

typedef struct {
    char log_path[600];
    char blob_path[600];
    char app[32];
    char prev[65];
    uint64_t blob_used;
    int n_lines;
} Host;

static void host_init(Host *h, const char *dir) {
    snprintf(h->log_path, sizeof(h->log_path), "%s\\swap.log.jsonl", dir);
    snprintf(h->blob_path, sizeof(h->blob_path), "%s\\swap.blob", dir);
    snprintf(h->app, sizeof(h->app), "kv-swap");
    snprintf(h->prev, sizeof(h->prev), "GENESIS");
    h->blob_used = 0; h->n_lines = 0;
}

static int host_line(Host *h, const char *kind, const char *text) {
    char *esc = (char *)malloc(strlen(text) * 2 + 8);
    char *o = esc;
    for (const char *i = text; *i; i++) { if (*i=='"'||*i=='\\') *o++='\\'; *o++=*i; }
    *o = 0;
    size_t hl = strlen(h->prev) + strlen(h->app) + strlen(kind) + 4 + strlen(text) + 1;
    char *hb = (char *)malloc(hl);
    snprintf(hb, hl, "%s%s%s%s%s", h->prev, h->app, kind, "all", text);
    char dig[65]; sha256_hex(hb, strlen(hb), dig); free(hb);
    FILE *f = fopen(h->log_path, "ab");
    if (!f) { free(esc); return -1; }
    fprintf(f, "{\"v\":3,\"app\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"to\":\"all\",\"prev\":\"%s\",\"hash\":\"%s\"}\n",
            h->app, kind, esc, h->prev, dig);
    fclose(f); free(esc);
    memcpy(h->prev, dig, 65); h->n_lines++;
    return 0;
}

static int hl_log(void *ud, const char *kind, const void *rec, size_t len) {
    Host *h = (Host *)ud;
    char *t = (char *)malloc(len * 8 + 256);
    if (!strcmp(kind, "kv-delta")) {
        KVTDeltaHdr hd; memcpy(&hd, rec, sizeof(hd));
        const int32_t *tk = (const int32_t *)((const uint8_t *)rec + sizeof(hd));
        char *p = t; p += sprintf(p, "{\"turn\":%d,\"parent\":%d,\"tokens\":[", hd.turn, hd.parent);
        for (int i = 0; i < hd.n_tokens; i++) p += sprintf(p, "%s%d", i?",":"", tk[i]);
        sprintf(p, "]}");
    } else {
        KVTAnchor a; memcpy(&a, rec, sizeof(a));
        sprintf(t, "{\"turn\":%d,\"base_n_tokens\":%d,\"type_k\":%u,\"type_v\":%u,"
                   "\"blob_off\":%llu,\"blob_len\":%llu,\"blob_fnv\":%llu}",
                a.turn, a.base_n_tokens, a.kv_type_k, a.kv_type_v,
                (unsigned long long)a.blob_off, (unsigned long long)a.blob_len,
                (unsigned long long)a.base_fnv);
    }
    int rc = host_line(h, kind, t); free(t); return rc;
}
static long long hl_blob(void *ud, const void *buf, size_t len) {
    Host *h = (Host *)ud; long long off = (long long)h->blob_used;
    FILE *f = fopen(h->blob_path, "ab"); if (!f) return -1;
    fwrite(buf, 1, len, f); fclose(f); h->blob_used += len; return off;
}
static int hl_blob_read(void *ud, unsigned long long off, void *buf, size_t len) {
    Host *h = (Host *)ud; FILE *f = fopen(h->blob_path, "rb"); if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t g = fread(buf, 1, len, f); fclose(f); return g == len ? 0 : -1;
}

static uint64_t fnv_bytes(const void *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL; const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ULL; }
    return h;
}

static struct llama_context *mk_ctx(struct llama_model *m, int n_ctx) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx; cp.n_batch = 256; cp.no_perf = true;
    cp.type_k = GGML_TYPE_Q8_0; cp.type_v = GGML_TYPE_Q4_0;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

static int32_t decode_at(struct llama_context *ctx, const llama_token *t, int32_t n, int32_t pos0) {
    int32_t last = 0;
    for (int32_t off = 0; off < n; off += 256) {
        llama_batch b = llama_batch_init(256, 0, 1);
        int32_t k = n - off > 256 ? 256 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = t[off+i]; b.pos[i] = pos0+off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail @%d\n", pos0+off); llama_batch_free(b); return -1; }
        llama_batch_free(b); last = k;
    }
    return last;
}

static llama_token argmax_ith(struct llama_context *ctx, int32_t oi, int32_t nv) {
    const float *lg = llama_get_logits_ith(ctx, oi); if (!lg) return -1;
    int bi = 0; for (int32_t i = 1; i < nv; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

int main(int argc, char **argv) {
    const char *mp  = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *dir = argc > 2 ? argv[2] : ".";
    const char *bk  = argc > 3 ? argv[3] : "build\\cpuonly";

    char lp[600], bp[600];
    snprintf(lp, sizeof(lp), "%s\\swap.log.jsonl", dir);
    snprintf(bp, sizeof(bp), "%s\\swap.blob", dir);
    remove(lp); remove(bp);

    ggml_backend_load_all_from_path(bk);
    llama_log_set(NULL, NULL);

    struct llama_model_params mpar = llama_model_default_params(); mpar.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(mp, mpar);
    if (!model) { fprintf(stderr, "model load failed\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    struct llama_context *ctx = mk_ctx(model, SMALL_CTX);
    if (!ctx) { fprintf(stderr, "ctx failed\n"); return 1; }

    Host host; host_init(&host, dir);
    KVTLog L; L.ud = &host; L.log_append = hl_log; L.blob_append = hl_blob; L.blob_read = hl_blob_read;

    /* session: 24 short turns, each a fresh question */
    static const char *asks[NT] = {
        "what is a hash?","why chain them?","name a hash function","what is sha256?",
        "what is a log?","why append only?","what is a tombstone?","what is a delta?",
        "what is a snapshot?","when to snapshot?","what is a frame?","what is a seek?",
        "what is a keyframe?","why keyframes?","what is a window?","why a window?",
        "what is eviction?","what is a cache?","what is ram?","what is vram?",
        "what is quant?","why quantize?","what is 8 bit?","what is 4 bit?",
    };

    static llama_token hist[NCAP];
    int32_t n_hist = 0;
    int32_t live_argmax[NT + 1];
    /* per-turn token ranges, so we can drop them from the KV by position */
    static int32_t turn_pos0[NT + 1], turn_pos1[NT + 1];   /* [pos0, pos1) */
    int32_t resident_first = 1;   /* first turn still in the ctx */

    KVTWindow win; kvt_init(&win, 8);
    KVTAnchor anchors[64]; int n_anchors = 0;
    int drops = 0, peak_live = 0;
    size_t peak_bytes = 0, held_now = 0;

    printf("kv_swap_demo: n_ctx=%d  turns=%d  hold=%d turns\n", SMALL_CTX, NT, HOLD_TURNS);

    for (int t = 1; t <= NT; t++) {
        char w[512];
        snprintf(w, sizeof(w), "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", asks[t-1]);
        int32_t prev = n_hist;
        n_hist += llama_tokenize(vocab, w, (int32_t)strlen(w), hist + n_hist, NCAP - n_hist, n_hist == 0, true);
        int32_t nu = n_hist - prev;
        turn_pos0[t] = prev; turn_pos1[t] = n_hist;

        /* ── the swap: if adding this turn would exceed n_ctx, drop old turns
              out of the KV first. The session does not stop — it forgets the
              oldest turns and can seek them back later. ── */
        while ((n_hist - turn_pos0[resident_first]) > SMALL_CTX && resident_first < t) {
            llama_memory_t mem = llama_get_memory(ctx);
            llama_memory_seq_rm(mem, 0, turn_pos0[resident_first], turn_pos1[resident_first]);
            held_now -= (size_t)(turn_pos1[resident_first] - turn_pos0[resident_first]) * 2 * 1024;  /* est, K8V4 */
            resident_first++;
            drops++;
        }
        int32_t live = n_hist - turn_pos0[resident_first];
        if (live > peak_live) peak_live = live;
        size_t bytes = (size_t)live * 2 * 1024;
        if (bytes > peak_bytes) peak_bytes = bytes;
        held_now = bytes;

        if (decode_at(ctx, hist + prev, nu, prev) < 0) { fprintf(stderr, "turn %d decode failed\n", t); return 1; }
        live_argmax[t] = argmax_ith(ctx, nu - 1, n_vocab);

        KVTFrame fr = kvt_frame_make(t, 0, hist + prev, nu);
        KVTFrame ev; memset(&ev, 0, sizeof(ev));
        int had = kvt_commit_frame(&win, &fr, &ev);
        kvt_persist_delta(&L, &fr);
        if (had) kvt_frame_free(&ev);

        /* anchor at t1 as well: the session's START is a resume point, so a
           seek to an early turn never has to re-decode from scratch */
        if (t == 1 || kvt_anchor_due(&win)) {
            KVColdBase base; kvcb_init(&base);
            if (kvcb_hold(ctx, 0, &base, n_hist) == 0) {
                KVTAnchor a;
                if (kvt_persist_anchor(&L, t, n_hist, 8, 2, fnv_bytes(base.blob, base.size),
                                       base.blob, base.size, win.since_anchor, &a) == 0 && n_anchors < 64)
                    anchors[n_anchors++] = a;
                kvcb_clear(&base);
            }
            win.since_anchor = 0;
            int idx = (win.head - 1 + win.cap) % win.cap;
            win.frames[idx].hdr.parent = t;
        }
        if (t % 4 == 0 || t == NT)
            printf("  t%-2d  live=%4d tok (resident t%d..t%d)  drops=%d  argmax=%d\n",
                   t, live, resident_first, t, drops, live_argmax[t]);
    }

    printf("\n  ctx high-water : %d positions (cap %d)  %s\n",
           peak_live, SMALL_CTX, peak_live <= SMALL_CTX ? "OK" : "OVER");
    printf("  KV held at peak: ~%zu KB   (session total %d tok would be ~%zu KB)\n",
           peak_bytes / 1024, n_hist, (size_t)n_hist * 2);
    printf("  turns dropped  : %d of %d\n", drops, NT);
    printf("  log            : %d lines, %llu B of anchor blobs\n",
           host.n_lines, (unsigned long long)host.blob_used);

    /* ── now read a turn that was dropped, by seeking ──
       pick a dropped turn whose live argmax is NOT the most common value
       (a repeated argmax would make the MATCH say nothing) ── */
    int most_common = live_argmax[1];
    {   /* cheap mode of the live answers */
        int best = 0;
        for (int a = 1; a <= NT; a++) {
            int c = 0; for (int b = 1; b <= NT; b++) if (live_argmax[b] == live_argmax[a]) c++;
            if (c > best) { best = c; most_common = live_argmax[a]; }
        }
    }
    /* pick a dropped turn whose live answer is RARE — a unique value is
       unambiguous proof the seek reconstructed THAT turn, not a common token */
    int target = -1, best_count = NT + 1;
    for (int t = 1; t < resident_first; t++) {
        int c = 0; for (int b = 1; b <= NT; b++) if (live_argmax[b] == live_argmax[t]) c++;
        if (c < best_count) { best_count = c; target = t; }
    }
    if (target < 0) target = 1;                        /* fallback */

    printf("\n  live answers: ");
    for (int t = 1; t <= NT; t++) printf("%d%s", live_argmax[t], t == NT ? "\n" : " ");

    KVTSeek s; kvt_seek(anchors, n_anchors, target, &s);
    printf("  [seek] turn %d (dropped): anchor=%s", target, s.have_anchor ? "yes" : "no");
    if (s.have_anchor) printf(" @t%d (%d tok)", s.anchor.turn, s.anchor.base_n_tokens);
    printf(" -> replay t%d..t%d\n", s.first_delta, s.last_delta);

    struct llama_context *c2 = mk_ctx(model, SMALL_CTX);
    int32_t from = s.have_anchor ? s.anchor.base_n_tokens : 0;
    int32_t endT = turn_pos1[target];
    if (s.have_anchor) {
        KVColdBase base; kvcb_init(&base);
        base.blob = (uint8_t *)malloc((size_t)s.anchor.blob_len);
        base.size = (size_t)s.anchor.blob_len; base.n_tokens = s.anchor.base_n_tokens; base.have = 1;
        hl_blob_read(&host, s.anchor.blob_off, base.blob, base.size);
        kvcb_resume(c2, 0, &base);
        kvcb_clear(&base);
    }
    if (decode_at(c2, hist + from, endT - from, from) >= 0) {
        llama_token got = argmax_ith(c2, (endT - from) - 1, n_vocab);
        printf("  [seek] turn %d argmax=%d  live=%d  %s\n", target, got, live_argmax[target],
               got == live_argmax[target] ? "MATCH (recovered)" : "DIFF");
    } else {
        fprintf(stderr, "  [seek] decode failed\n");
    }

    printf("\n  === kv_swap_demo: context stays <= %d while the session ran %d turns ===\n",
           SMALL_CTX, NT);
    llama_free(c2); kvt_free(&win); llama_free(ctx); llama_model_free(model);
    return (peak_live <= SMALL_CTX) ? 0 : 1;
}
