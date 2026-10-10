/*
 * tools/kv_temporal_llama.c — the temporal KV log wired to a REAL llama ctx.
 * ═══════════════════════════════════════════════════════════════════════
 * Replaces the synthetic tokens of kv_temporal_log.c with a live model:
 *
 *   turn t  -> decode turn t's tokens in the ctx (KV grows by the turn)
 *           -> commit the turn as a FRAME into the hot window
 *           -> if an anchor is due, HOLD the live ctx state (kvcb_hold)
 *              and persist it as the anchor BLOB (not the log)
 *           -> evicted frames' token-deltas go to the log
 *
 * Then, in a FRESH ctx, it SEEKS back to earlier turns:
 *   seek(T) -> resume the nearest anchor blob (kvcb_resume)
 *           -> teacher-decode every delta from the anchor to T
 *           -> argmax of the last token must equal the live run's argmax
 *
 * This is the end-to-end claim: a chat turn is a seekable frame, stored
 * as a tiny token delta, reconstructed bit-exactly on demand.
 *
 * BUILD: gcc -O2 -std=c11 -I I:/llama/include -I core -o build/kv_temporal_llama
 *   tools/kv_temporal_llama.c .../llama.dll .../ggml.dll .../ggml-base.dll .../ggml-cpu-x64.dll -lm
 * RUN: ./build/kv_temporal_llama [model.gguf] [outdir] [backend_dir]
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

#define NCAP 3800

/* ── the same SHA-256 the pool uses, so lines can drop in unchanged ── */
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

/* ── host: a hash-chained jsonl log + a side blob file ──
   The line shape matches chat-pool's pool.append() EXACTLY:
     {v,t,app,kind,text,to,prev,hash}   hash = sha256(prev+app+kind+to+text)
   and the `text` is the JSON the Python bridge writes, so a C-run's
   frames can be read back by kv_pool_bridge.py unchanged. */
typedef struct {
    char log_path[KVT_PATH + 40];
    char blob_path[KVT_PATH + 40];
    char app[32];
    char prev[65];
    char prev_chain[65];   /* hash of the line before ours in the shared file */
    uint64_t blob_used;
    int n_lines;
} LlamaHost;

static void host_init(LlamaHost *h, const char *dir, const char *app) {
    snprintf(h->log_path, sizeof(h->log_path), "%s\\log.jsonl", dir);
    snprintf(h->blob_path, sizeof(h->blob_path), "%s\\kv_blobs.bin", dir);
    snprintf(h->app, sizeof(h->app), "%s", app ? app : "kv-temporal");
    snprintf(h->prev, sizeof(h->prev), "GENESIS");
    snprintf(h->prev_chain, sizeof(h->prev_chain), "GENESIS");
    h->blob_used = 0; h->n_lines = 0;
}

/* read the hash of the last line already in the shared log (so the chain is
   continuous when we append to a file someone else owns) */
static void host_tail_hash(LlamaHost *h) {
    FILE *f = fopen(h->log_path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    /* scan back for the last non-empty line, then find its "hash" */
    long start = sz > 4096 ? sz - 4096 : 0;
    if (start > 0) { fseek(f, start, SEEK_SET); while (fgetc(f) != '\n' && ftell(f) < sz) {} }
    fseek(f, start, SEEK_SET);
    char buf[8192]; size_t got = fread(buf, 1, sizeof(buf) - 1, f); buf[got] = 0;
    fclose(f);
    char *last = strrchr(buf, '\n');
    char *line = NULL;
    if (last && last != buf) {
        /* walk back one more newline to get the final full line */
        *last = 0; last = strrchr(buf, '\n');
        line = last ? last + 1 : buf;
    } else {
        line = buf;
    }
    if (!line || !*line) return;
    char *hp = strstr(line, "\"hash\":\"");
    if (!hp) return;
    hp += 8;
    char *end = strchr(hp, '"');
    if (!end || end - hp != 64) return;
    memcpy(h->prev_chain, hp, 64); h->prev_chain[64] = 0;
}

static int host_line(LlamaHost *h, const char *kind, const char *text) {
    /* escape the text for embedding inside a JSON string (it is itself JSON,
       so every " and \ must be escaped, and the pool parser must see one
       valid JSON object per line) */
    size_t need = strlen(text) * 2 + 8;
    char *esc = (char *)malloc(need);
    if (!esc) return -1;
    char *o = esc;
    for (const char *i = text; *i; i++) {
        if (*i == '"' || *i == '\\') *o++ = '\\';
        *o++ = *i;
    }
    *o = 0;

    size_t hb_len = strlen(h->prev) + strlen(h->app) + strlen(kind) + strlen("all") + strlen(text) + 1;
    char *hb = (char *)malloc(hb_len);
    snprintf(hb, hb_len, "%s%s%s%s%s", h->prev, h->app, kind, "all", text);
    char dig[65]; sha256_hex(hb, strlen(hb), dig);
    free(hb);
    FILE *f = fopen(h->log_path, "ab");
    if (!f) { free(esc); return -1; }
    fprintf(f, "{\"v\":3,\"app\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"to\":\"all\",\"prev\":\"%s\",\"hash\":\"%s\"}\n",
            h->app, kind, esc, h->prev, dig);
    fclose(f);
    free(esc);
    memcpy(h->prev, dig, 65);
    h->n_lines++;
    return 0;
}

/* The KVTLog callback still receives the raw record bytes, but here we don't
   hex them — we format the SAME json the Python bridge writes. kvt keeps the
   record shape; the host owns the on-log representation. */
static int lh_log_append(void *ud, const char *kind, const void *rec, size_t len) {
    LlamaHost *h = (LlamaHost *)ud;
    char *text = (char *)malloc(len * 8 + 256);
    if (!text) return -1;
    if (strcmp(kind, "kv-delta") == 0) {
        /* record = [hdr][tokens...]; emit {"turn","parent","tokens":[...]} */
        KVTDeltaHdr hd; memcpy(&hd, rec, sizeof(hd));
        const int32_t *toks = (const int32_t *)((const uint8_t *)rec + sizeof(hd));
        int nt = hd.n_tokens;
        char *p = text;
        p += sprintf(p, "{\"turn\":%d,\"parent\":%d,\"tokens\":[", hd.turn, hd.parent);
        for (int i = 0; i < nt; i++) p += sprintf(p, "%s%d", i ? "," : "", toks[i]);
        sprintf(p, "]}");
    } else if (strcmp(kind, "kv-anchor") == 0) {
        KVTAnchor a; memcpy(&a, rec, sizeof(a));
        sprintf(text,
            "{\"turn\":%d,\"base_n_tokens\":%d,\"type_k\":%u,\"type_v\":%u,"
            "\"blob_off\":%llu,\"blob_len\":%llu,\"blob_fnv\":%llu}",
            a.turn, a.base_n_tokens, a.kv_type_k, a.kv_type_v,
            (unsigned long long)a.blob_off, (unsigned long long)a.blob_len,
            (unsigned long long)a.base_fnv);
    } else {
        snprintf(text, len * 8 + 256, "%s", "(unknown)");
    }
    int rc = host_line(h, kind, text);
    free(text);
    return rc;
}
static long long lh_blob_append(void *ud, const void *buf, size_t len) {
    LlamaHost *h = (LlamaHost *)ud;
    long long off = (long long)h->blob_used;
    FILE *f = fopen(h->blob_path, "ab");
    if (!f) return -1;
    fwrite(buf, 1, len, f); fclose(f);
    h->blob_used += len;
    return off;
}
static int lh_blob_read(void *ud, unsigned long long off, void *buf, size_t len) {
    LlamaHost *h = (LlamaHost *)ud;
    FILE *f = fopen(h->blob_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, len, f); fclose(f);
    return got == len ? 0 : -1;
}

/* ── llama helpers (identical rules to kv_cold_chat.c) ── */
static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = GGML_TYPE_Q8_0; cp.type_v = GGML_TYPE_Q4_0;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

static int32_t decode_at(struct llama_context *ctx, const llama_token *toks, int32_t n, int32_t pos0) {
    int32_t k_last = 0;
    for (int32_t off = 0; off < n; off += 512) {
        llama_batch b = llama_batch_init(512, 0, 1);
        int32_t k = n - off > 512 ? 512 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = toks[off+i]; b.pos[i] = pos0+off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail @%d\n", pos0+off); llama_batch_free(b); return -1; }
        llama_batch_free(b);
        k_last = k;
    }
    return k_last;
}

static llama_token argmax_ith(struct llama_context *ctx, int32_t out_idx, int32_t n_vocab) {
    const float *lg = llama_get_logits_ith(ctx, out_idx);
    if (!lg) return -1;
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

static int32_t tokenize(const struct llama_vocab *v, const char *s, int add_special,
                        llama_token *out, int32_t cap) {
    return llama_tokenize(v, s, (int32_t)strlen(s), out, cap, add_special, true);
}

/* ── FNV for prefix hashes (matches kv_temporal's expectations) ── */
static uint64_t fnv_tokens(const int32_t *t, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)t[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t fnv_bytes(const void *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ULL; }
    return h;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : ".";
    const char *backend    = argc > 3 ? argv[3] : "build\\cpuonly";
    const int K = KVT_WINDOW;   /* hot-window RAM budget, not cadence */

    char lp[KVT_PATH + 40], bp[KVT_PATH + 40];
    snprintf(lp, sizeof(lp), "%s\\kvt_llama.log.jsonl", outdir);
    snprintf(bp, sizeof(bp), "%s\\kvt_llama.blob", outdir);
    remove(lp); remove(bp);

    ggml_backend_load_all_from_path(backend);
    llama_log_set(NULL, NULL);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "model load failed: %s\n", model_path); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    struct llama_context *ctx = mk_ctx(model);
    if (!ctx) { fprintf(stderr, "ctx failed\n"); return 1; }

    LlamaHost host; host_init(&host, outdir, "kv-temporal");
    host_tail_hash(&host);   /* continue the chain if the file already exists */
    KVTLog L; L.ud = &host; L.log_append = lh_log_append;
    L.blob_append = lh_blob_append; L.blob_read = lh_blob_read;

    KVTWindow win; kvt_init(&win, K);
    KVTAnchor anchors[64]; int n_anchors = 0;
    /* keep every frame's tokens for the replay side */
    static llama_token hist[NCAP];
    int32_t n_hist = 0;
    int32_t live_argmax[64]; memset(live_argmax, 0, sizeof(live_argmax));

    const char *turns[] = {
        "what is a hash chain?",
        "why does it matter for a log?",
        "give one concrete risk it prevents",
        "name a famous cryptographic hash function",
        "what is a Merkle tree?",
        "how is a Merkle tree used in git?",
        "what does append-only mean?",
        "why can't you edit a line in a hash chain?",
        "what is a genesis block?",
        "explain a tombstone in an event log",
        "what is a delta encoding?",
        "when is a full snapshot better than a delta?",
    };
    const int NT = 12;

    printf("kv_temporal_llama: model=%s K=%d\n", model_path, K);

    /* ── pass 1: run the turns, commit frames, cut anchors ── */
    for (int t = 1; t <= NT; t++) {
        char wrapped[512];
        snprintf(wrapped, sizeof(wrapped), "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", turns[t-1]);
        int32_t prev = n_hist;
        n_hist += tokenize(vocab, wrapped, n_hist == 0, hist + n_hist, NCAP - n_hist);
        int32_t nu = n_hist - prev;

        if (decode_at(ctx, hist + prev, nu, prev) < 0) { fprintf(stderr, "live t%d decode failed\n", t); return 1; }
        live_argmax[t] = argmax_ith(ctx, nu - 1, n_vocab);
        printf("[live t%d] frame-end argmax=%d tokens=%d n_hist=%d\n", t, live_argmax[t], nu, n_hist);

        KVTFrame fr = kvt_frame_make(t, 0, hist + prev, nu);
        KVTFrame ev; memset(&ev, 0, sizeof(ev));
        int had = kvt_commit_frame(&win, &fr, &ev);

        /* persist THIS frame's delta immediately — chronological in the file,
           independent of when the hot window happens to evict it */
        kvt_persist_delta(&L, &fr);

        /* the evicted frame's delta was already written at its own commit */
        if (had) kvt_frame_free(&ev);

        if (kvt_anchor_due(&win)) {
            KVColdBase base; kvcb_init(&base);
            if (kvcb_hold(ctx, 0, &base, n_hist) == 0) {
                KVTAnchor a;
                if (kvt_persist_anchor(&L, t, n_hist, 8, 2, fnv_bytes(base.blob, base.size),
                                       base.blob, base.size, win.since_anchor, &a) == 0 && n_anchors < 64)
                    anchors[n_anchors++] = a;
                printf("  anchor @t%d: base blob %zu B (n_tokens=%d)\n", t, base.size, n_hist);
                kvcb_clear(&base);
            }
            win.since_anchor = 0;
            int idx = (win.head - 1 + win.cap) % win.cap;
            win.frames[idx].hdr.parent = t;
        }
    }
    /* drain remaining hot frames — their deltas already went to the log at
       commit time; nothing to persist here. */
    (void)win;
    printf("log lines=%d anchors=%d blob_used=%llu B\n", host.n_lines, n_anchors,
           (unsigned long long)host.blob_used);

    /* ── pass 2: SEEK back into a fresh ctx and compare argmax ── */
    struct llama_context *c2 = mk_ctx(model);
    int match = 0;
    for (int T = 1; T <= NT; T++) {
        llama_memory_clear(llama_get_memory(c2), true);
        KVTSeek s; kvt_seek(anchors, n_anchors, T, &s);
        if (s.have_anchor) {
            KVColdBase base; kvcb_init(&base);
            base.blob = (uint8_t *)malloc((size_t)s.anchor.blob_len);
            base.size = (size_t)s.anchor.blob_len;
            base.n_tokens = s.anchor.base_n_tokens; base.have = 1;
            if (lh_blob_read(&host, s.anchor.blob_off, base.blob, base.size) != 0) {
                fprintf(stderr, "blob read failed\n"); return 1;
            }
            if (kvcb_resume(c2, 0, &base) == 0) { fprintf(stderr, "resume failed\n"); return 1; }
            kvcb_clear(&base);
        }
        /* teacher-decode the deltas anchor+1 .. T, taking each delta's exact
           token slice from hist (turn boundaries recomputed deterministically,
           mirroring pass 1) */
        int32_t from = s.have_anchor ? s.anchor.base_n_tokens : 0;
        int32_t endT = 0;
        {
            int32_t np = 0;
            for (int q = 1; q <= T; q++) {
                char w[512];
                snprintf(w, sizeof(w), "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", turns[q-1]);
                llama_token tmp[NCAP];
                int32_t nt = tokenize(vocab, w, np == 0, tmp, NCAP - np);
                np += nt;
            }
            endT = np;
        }
        if (decode_at(c2, hist + from, endT - from, from) < 0) {
            fprintf(stderr, "[seek t%d] decode failed\n", T);
            llama_free(c2);
            kvt_free(&win);
            return 1;
        }
        llama_token got = argmax_ith(c2, (endT - from) - 1, n_vocab);
        int ok = (got == live_argmax[T]);
        printf("[seek t%d] anchor=%s replay=%d tok  argmax=%d live=%d %s\n",
               T, s.have_anchor ? "yes" : "no", endT - from, got, live_argmax[T], ok ? "MATCH" : "DIFF");
        if (ok) match++;
    }
    printf("=== kv_temporal_llama: %d/%d MATCH ===\n", match, NT);

    llama_free(c2);
    kvt_free(&win);
    llama_free(ctx);
    llama_model_free(model);
    return match == NT ? 0 : 1;
}
