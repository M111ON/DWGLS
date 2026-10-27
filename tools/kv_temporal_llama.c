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
 * The seek replay mirrors the producer's decode BATCHING, turn by turn: a
 * quantized KV rounds per batch, so one big catch-up batch is not the same
 * arithmetic as N small turns (measured: argmax flipped at one turn).
 *
 * Recall reads the LOG, not memory: the replay takes its resume point from a
 * kv-anchor record and its tokens from kv-delta records (core/kv_temporal_read.h),
 * so a process that never produced the frames can rebuild them. "append" mode
 * therefore also resumes the live ctx FROM the log and continues the turn
 * numbering, instead of starting a second session under the same numbers.
 *
 * BUILD: gcc -O2 -std=c11 -I I:/llama/include -I core -o build/kv_temporal_llama
 *   tools/kv_temporal_llama.c <backend>/llama.dll <backend>/ggml.dll
 *   <backend>/ggml-base.dll <backend>/ggml-cpu-x64.dll -ladvapi32 -lm
 * RUN: ./build/kv_temporal_llama [model.gguf] [outdir] [backend_dir] [NT] [append]
 *   outdir gets the bridge's names (log.jsonl + kv_blobs.bin) so
 *   chat-pool/kv_pool_bridge.py reads a C run unchanged; "append" continues an
 *   existing log (turn numbering and its hash chain).
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include "kv_temporal.h"
#include "kv_temporal_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/locking.h>
/* link with -ladvapi32 (the #pragma form is MSVC-only, hence the warning) */
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

#define NCAP 3800
#define NT_MAX 400

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
    char lock_path[KVT_PATH + 40];   /* same <log>.lock pool.append() uses */
    char claim_path[KVT_PATH + 40];  /* one producer per log, held for the run */
    char app[32];
    char prev[65];
    char prev_chain[65];   /* hash of the line before ours in the shared file */
    int  tail_found;       /* 1 when prev_chain came from a real line */
    uint64_t blob_used;
    int n_lines;
    int lock_fd;
    int claim_fd;
} LlamaHost;

/* One producer per log, claimed for the whole run.
 *
 * The per-line lock keeps the FILE valid, but two producers that both append
 * turn numbers would still be wrong: both would extend the same log with their
 * own t1..tN and every "turn" in it would become ambiguous. Sequential appends
 * (run 1 finishes, run 2 continues) are the supported pattern, so the claim is
 * non-blocking: a second live producer is refused instead of interleaved. */
static int host_claim_producer(LlamaHost *h) {
#ifdef _WIN32
    h->claim_fd = _open(h->claim_path, _O_CREAT | _O_RDWR, _S_IREAD | _S_IWRITE);
    if (h->claim_fd < 0) return -1;
    _lseek(h->claim_fd, 0, SEEK_SET);
    if (_locking(h->claim_fd, _LK_NBLCK, 1) != 0) {
        fprintf(stderr, "another producer already owns this log (%s)\n", h->claim_path);
        return -1;
    }
#else
    h->claim_fd = open(h->claim_path, O_CREAT | O_RDWR, 0644);
    if (h->claim_fd < 0) return -1;
    if (flock(h->claim_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "another producer already owns this log (%s)\n", h->claim_path);
        return -1;
    }
#endif
    return 0;
}

/* ── the pool's writer lock, taken by this host too ──
 * pool.append() serialises {read prev, compute hash, append line} under
 * <log>.lock (msvcrt.locking / flock). Two writers that both append to one log
 * MUST use the same lock file, or the chain forks: both read the same prev,
 * both write a line claiming it, and the log stops being one chain. The blob
 * file needs the same treatment for a different reason: an offset is decided by
 * "size now", so two writers can reserve the same bytes.
 *
 * Scope is deliberately tiny (one line, or one blob reservation) — the lock is
 * held for microseconds, never across a decode. */
static int host_lock(LlamaHost *h) {
    if (h->lock_fd < 0) return -1;
#ifdef _WIN32
    _lseek(h->lock_fd, 0, SEEK_SET);
    return _locking(h->lock_fd, _LK_LOCK, 1) == 0 ? 0 : -1;   /* waits, retries ~10 s */
#else
    return flock(h->lock_fd, LOCK_EX) == 0 ? 0 : -1;
#endif
}

static void host_unlock(LlamaHost *h) {
    if (h->lock_fd < 0) return;
#ifdef _WIN32
    _lseek(h->lock_fd, 0, SEEK_SET);
    _locking(h->lock_fd, _LK_UNLCK, 1);
#else
    flock(h->lock_fd, LOCK_UN);
#endif
}

static void host_init(LlamaHost *h, const char *dir, const char *app) {
    snprintf(h->log_path, sizeof(h->log_path), "%s\\log.jsonl", dir);
    snprintf(h->blob_path, sizeof(h->blob_path), "%s\\kv_blobs.bin", dir);
    snprintf(h->lock_path, sizeof(h->lock_path), "%s\\log.jsonl.lock", dir);
    snprintf(h->claim_path, sizeof(h->claim_path), "%s\\kv_producer.lock", dir);
    snprintf(h->app, sizeof(h->app), "%s", app ? app : "kv-temporal");
    snprintf(h->prev, sizeof(h->prev), "GENESIS");
    snprintf(h->prev_chain, sizeof(h->prev_chain), "GENESIS");
    h->tail_found = 0;
    h->lock_fd = -1;
    h->claim_fd = -1;
#ifdef _WIN32
    h->lock_fd = _open(h->lock_path, _O_CREAT | _O_RDWR, _S_IREAD | _S_IWRITE);
#else
    h->lock_fd = open(h->lock_path, O_CREAT | O_RDWR, 0644);
#endif
    /* offsets are absolute positions in the side blob file: if the file already
     * exists (append mode, shared dir) they MUST start after it, or every
     * anchor of the second run points at the first run's bytes. */
    h->blob_used = 0;
    FILE *bf = fopen(h->blob_path, "rb");
    if (bf) {
        if (fseek(bf, 0, SEEK_END) == 0) {
            long sz = ftell(bf);
            if (sz > 0) h->blob_used = (uint64_t)sz;
        }
        fclose(bf);
    }
    h->n_lines = 0;
}

/* read the hash of the last line already in the shared log (so the chain is
   continuous when we append to a file someone else owns) */
#define TAIL_SCAN 65536
static void host_tail_hash(LlamaHost *h) {
    FILE *f = fopen(h->log_path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    /* a pool line can be long (a frame delta carries its tokens), so scan back
       far enough that the FINAL line is fully inside the window */
    long start = sz > TAIL_SCAN ? sz - TAIL_SCAN : 0;
    fseek(f, start, SEEK_SET);
    char *buf = (char *)malloc((size_t)(sz - start) + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)(sz - start), f);
    fclose(f);
    buf[got] = 0;
    /* trim trailing newlines/CRs, then take everything after the last '\n' */
    while (got > 0 && (buf[got-1] == '\n' || buf[got-1] == '\r')) buf[--got] = 0;
    char *line = buf;
    if (got > 0) {
        char *nl = strrchr(buf, '\n');
        if (nl) line = nl + 1;
    }
    /* if the window began mid-line the prefix is garbage but the tail after
       the last newline is still a whole line — unless no newline was found,
       in which case the whole buffer may be a fragment: skip it */
    if (start > 0 && !strrchr(buf, '\n')) { free(buf); return; }
    /* the hash of the last line. Both writers use this key but pool.py emits
       `"hash": "…"` (space after the colon) while our host emits `"hash":"…"`,
       so skip punctuation explicitly — matching one shape silently ignored the
       other writer's line and forked the chain. */
    char *hp = strstr(line, "\"hash\"");
    if (hp) {
        hp += 6;
        while (*hp == ' ' || *hp == '\t') hp++;
        if (*hp == ':') {
            hp++;
            while (*hp == ' ' || *hp == '\t') hp++;
            if (*hp == '"') hp++;
            char *end = strchr(hp, '"');
            if (end && end - hp == 64) {
                memcpy(h->prev_chain, hp, 64); h->prev_chain[64] = 0;
                h->tail_found = 1;
            }
        }
    }
    free(buf);
}

static int host_line(LlamaHost *h, const char *kind, const char *text) {
    if (host_lock(h) != 0) { fprintf(stderr, "log: writer lock busy (<log>.lock)\n"); return -1; }
    /* the chain must follow the FILE, not just our own records: if another
       writer appended after our last line, the next line's `prev` has to be
       that writer's hash or the pool's chain check breaks at our record.
       This read is inside the lock on purpose — reading the tail outside it is
       the classic fork: both writers see the same prev and both claim it. */
    {
        char own[65];
        memcpy(own, h->prev, sizeof(own));
        host_tail_hash(h);
        /* tail_found, not a "GENESIS" compare: the sentinel means OUR chain has
           no line yet, which is exactly the state when someone else already
           wrote to the file (measured: a raced run kept prev=GENESIS and forked
           the chain at line 301) */
        if (h->tail_found && strcmp(h->prev_chain, own) != 0)
            memcpy(h->prev, h->prev_chain, sizeof(h->prev));
    }
    /* escape the text for embedding inside a JSON string (it is itself JSON,
       so every " and \ must be escaped, and the pool parser must see one
       valid JSON object per line) */
    size_t need = strlen(text) * 2 + 8;
    char *esc = (char *)malloc(need);
    if (!esc) { host_unlock(h); return -1; }
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
    if (!f) { free(esc); host_unlock(h); return -1; }
    fprintf(f, "{\"v\":3,\"t\":%" PRId64 ",\"app\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"to\":\"all\",\"prev\":\"%s\",\"hash\":\"%s\"}\n",
            (int64_t)time(NULL),
            h->app, kind, esc, h->prev, dig);
    fclose(f);
    free(esc);
    memcpy(h->prev, dig, 65);
    h->n_lines++;
    host_unlock(h);
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
            "\"blob_off\":%" PRIu64 ",\"blob_len\":%" PRIu64 ",\"blob_fnv\":%" PRIu64 "}",
            a.turn, a.base_n_tokens, a.kv_type_k, a.kv_type_v,
            a.blob_off, a.blob_len, a.base_fnv);
    } else {
        snprintf(text, len * 8 + 256, "%s", "(unknown)");
    }
    int rc = host_line(h, kind, text);
    free(text);
    return rc;
}
static long long lh_blob_append(void *ud, const void *buf, size_t len) {
    LlamaHost *h = (LlamaHost *)ud;
    /* reserve under the lock: the offset is "size now", so two writers that
       each compute it outside the lock can reserve the same bytes */
    if (host_lock(h) != 0) { fprintf(stderr, "blob: writer lock busy\n"); return -1; }
    long long off = -1;
    FILE *f = fopen(h->blob_path, "ab");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz >= 0) off = (long long)sz;
        }
        if (off >= 0 && fwrite(buf, 1, len, f) != len) off = -1;
        fclose(f);
        if (off >= 0) h->blob_used = (uint64_t)off + len;
    }
    host_unlock(h);
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

/* ── FNV for blob integrity (the per-frame prefix hash is computed by
 *    kvt_frame_make in the header, so there is exactly one implementation) ── */
static uint64_t fnv_bytes(const void *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ULL; }
    return h;
}

/* Rebuild the KV state for absolute turn W from the LOG: resume the nearest
 * anchor blob, then decode the log's own deltas for turns anchor+1..W at their
 * exact token positions (the positions the producer used, so quantized KV
 * rounding is reproduced turn by turn).
 *
 * Returns 0 on success. *argmax receives the frame-end argmax, or -1 when W is
 * the anchor turn itself (that frame ends inside the blob, so nothing is left
 * to decode and the argmax is not comparable). */
static int kv_rebuild(struct llama_context *ctx, const KVTLog *L,
                      const KVTAnchor *anchors, int na,
                      KVLogDelta *deltas, int nd,
                      int32_t W, int32_t *pos_out, llama_token *argmax, int32_t n_vocab) {
    llama_memory_clear(llama_get_memory(ctx), true);
    KVTSeek s; kvt_seek(anchors, na, W, &s);
    int32_t pos = 0, first = 1;
    if (s.have_anchor) {
        size_t blen = (size_t)s.anchor.blob_len;
        uint8_t *blob = (uint8_t *)malloc(blen ? blen : 1);
        if (!blob) return -1;
        if (L->blob_read(L->ud, s.anchor.blob_off, blob, blen) != 0 ||
            fnv_bytes(blob, blen) != s.anchor.base_fnv) {
            fprintf(stderr, "anchor t%d: blob at +%" PRIu64 " does not verify\n",
                    s.anchor.turn, s.anchor.blob_off);
            free(blob);
            return -1;
        }
        KVColdBase base; kvcb_init(&base);
        base.blob = blob; base.size = blen; base.n_tokens = s.anchor.base_n_tokens; base.have = 1;
        size_t applied = kvcb_resume(ctx, 0, &base);   /* copies; blob stays ours */
        free(blob);
        if (applied == 0) { fprintf(stderr, "anchor t%d: resume failed\n", s.anchor.turn); return -1; }
        pos = s.anchor.base_n_tokens;
        first = s.anchor.turn + 1;
    }
    if (argmax) *argmax = -1;
    for (int32_t q = first; q <= W; q++) {
        KVLogDelta *d = kvl_find(deltas, nd, q);
        if (!d) { fprintf(stderr, "log: no delta for turn %d\n", q); return -1; }
        if (decode_at(ctx, d->tok, d->n, pos) < 0) return -1;
        pos += d->n;
        if (q == W && argmax) *argmax = argmax_ith(ctx, d->n - 1, n_vocab);
    }
    if (pos_out) *pos_out = pos;
    return 0;
}

/* Which turns to replay-compare? Full sweep for short runs; for long runs the
 * first 8, the last 8 and every 16th — a rebuild from an anchor costs
 * O(turns-since-anchor), so a full sweep of a 145-turn log is quadratic and
 * proves nothing the sample does not. Every compared turn is still a complete
 * rebuild from (anchor blob + log deltas), never a shortcut. */
static int verify_turn(int32_t T, int32_t T0, int32_t T1) {
    int32_t n = T1 - T0 + 1;
    if (n <= 32) return 1;
    if (T - T0 < 8) return 1;
    if (T1 - T < 8) return 1;
    return ((T - T0) % 16) == 0;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : ".";
    const char *backend    = argc > 3 ? argv[3] : "build\\cpuonly";
    const int   NT         = argc > 4 ? atoi(argv[4]) : 12;
    const int   append     = (argc > 5 && strcmp(argv[5], "append") == 0);
    const int   skip_seek  = (argc > 6 && strcmp(argv[6], "noseek") == 0)
                          || (argc > 5 && strcmp(argv[5], "noseek") == 0);
    const int K = KVT_WINDOW;   /* hot-window RAM budget, not cadence */
    if (NT < 1 || NT > NT_MAX) { fprintf(stderr, "NT out of range (1..%d)\n", NT_MAX); return 2; }

    /* the host writes the bridge's names from the start: same dir, same
       two files, so a Python reader sees exactly what a C run produced */
    char lp[KVT_PATH + 40], bp[KVT_PATH + 40];
    snprintf(lp, sizeof(lp), "%s\\log.jsonl", outdir);
    snprintf(bp, sizeof(bp), "%s\\kv_blobs.bin", outdir);

    /* claim the log BEFORE touching it: a fresh run deletes the files below, so
       a second producer must be refused before it can wipe a live run's log */
    LlamaHost host; host_init(&host, outdir, "kv-temporal");
    if (host_claim_producer(&host) != 0) return 3;

    if (!append) { remove(lp); remove(bp); }

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

    host_tail_hash(&host);   /* continue the chain if the file already exists */
    KVTLog L; L.ud = &host; L.log_append = lh_log_append;
    L.blob_append = lh_blob_append; L.blob_read = lh_blob_read;

    KVTWindow win; kvt_init(&win, K);
    KVTAnchor anchors[64]; int n_anchors = 0;
    /* tokens of THIS run's turns (for the live decode); replay reads the LOG,
     * never this array — that is what makes the log the source of truth. */
    static llama_token hist[NCAP];
    int32_t n_hist = 0;
    static int32_t live_argmax[NT_MAX];
    static char tbuf[NT_MAX][96];
    const char *turns[NT_MAX];
    memset(live_argmax, 0, sizeof(live_argmax));

    /* ── read the log this run appends to, BEFORE recording anything ──
     * Append mode continues a conversation: turn numbering continues where the
     * log ends, and the live ctx is REBUILT from the log (nearest anchor blob +
     * that round's deltas) so new turns extend the same context instead of
     * starting a second, unrelated session under the same turn numbers. */
    static KVTAnchor log_anchors[64];
    static KVLogDelta log_deltas[LOG_MAX_FRAMES];
    int na_log = 0, nd_log = 0;
    kvl_read(host.log_path, log_anchors, 64, &na_log, log_deltas, LOG_MAX_FRAMES, &nd_log);
    /* refuse a log that is not one path BEFORE resuming from it: a delta from
       another branch would resume on the wrong base and continue numbering as if
       nothing happened */
    {
        int32_t bt = 0, bp = 0;
        if (kvl_check_parents(log_anchors, na_log, log_deltas, nd_log, &bt, &bp) != 0) {
            fprintf(stderr, "log: turn %d hangs off parent %d, which is not its nearest "
                            "anchor — refusing to resume from a branched log\n", bt, bp);
            kvl_free(log_deltas, nd_log);
            llama_free(ctx);
            llama_model_free(model);
            return 1;
        }
    }
    int32_t turn_base = nd_log > 0 ? log_deltas[nd_log - 1].turn : 0;
    int32_t pos = 0;
    int32_t last_anchor_turn = na_log > 0 ? log_anchors[na_log - 1].turn : 0;
    if (turn_base > 0) {
        if (kv_rebuild(ctx, &L, log_anchors, na_log, log_deltas, nd_log,
                       turn_base, &pos, NULL, n_vocab) != 0) {
            fprintf(stderr, "resume from log failed (turn_base=%d)\n", turn_base);
            return 1;
        }
        printf("resumed from log at t%d: %d anchor(s), %d delta(s), ctx pos=%d\n",
               turn_base, na_log, nd_log, pos);
    }

    static const char *seed_turns[] = {
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
    const int NSEED = (int)(sizeof(seed_turns) / sizeof(seed_turns[0]));
    for (int t = 1; t <= NT; t++) {
        if (t <= NSEED) {
            turns[t-1] = seed_turns[t-1];
        } else {
            snprintf(tbuf[t-1], sizeof(tbuf[t-1]),
                     "turn %d: name one more property of an append-only log", t);
            turns[t-1] = tbuf[t-1];
        }
    }

    printf("kv_temporal_llama: model=%s K=%d turn_base=%d\n", model_path, K, turn_base);

    /* ── pass 1: run the turns, commit frames, cut anchors ── */
    for (int t = 1; t <= NT; t++) {
        char wrapped[512];
        snprintf(wrapped, sizeof(wrapped), "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", turns[t-1]);
        int32_t prev = n_hist;
        int32_t nt_room = NCAP - n_hist;
        int32_t nu_add = tokenize(vocab, wrapped, n_hist == 0, hist + n_hist, nt_room);
        if (nu_add <= 0 || nu_add > nt_room) { fprintf(stderr, "token budget exceeded at t%d (need %d, room %d)\n", t, nu_add, nt_room); return 1; }
        n_hist += nu_add;
        int32_t nu = n_hist - prev;
        const int32_t t_abs = turn_base + t;

        if (decode_at(ctx, hist + prev, nu, pos) < 0) { fprintf(stderr, "live t%d decode failed\n", t); return 1; }
        pos += nu;
        live_argmax[t] = argmax_ith(ctx, nu - 1, n_vocab);
        printf("[live t%d abs:%d] frame-end argmax=%d tokens=%d pos=%d\n",
               t, t_abs, live_argmax[t], nu, pos);

        KVTFrame fr = kvt_frame_make(t_abs, last_anchor_turn, hist + prev, nu);
        KVTFrame ev; memset(&ev, 0, sizeof(ev));
        int had = kvt_commit_frame(&win, &fr, &ev);

        /* the evicted frame's delta was already written at its own commit */
        if (had) kvt_frame_free(&ev);

        if (kvt_anchor_due(&win)) {
            KVColdBase base; kvcb_init(&base);
            if (kvcb_hold(ctx, 0, &base, pos) == 0) {
                KVTAnchor a;
                if (kvt_persist_anchor(&L, t_abs, pos, 8, 2, fnv_bytes(base.blob, base.size),
                                       base.blob, base.size, win.since_anchor, &a) == 0 && n_anchors < 64)
                    anchors[n_anchors++] = a;
                printf("  anchor @t%d: base blob %" PRIu64 " B (n_tokens=%d)\n", t_abs, (uint64_t)base.size, pos);
                kvcb_clear(&base);
            }
            last_anchor_turn = t_abs;     /* this turn IS the anchor its round hangs off */
            win.since_anchor = 0;
        }

        /* Persist THIS frame's delta at commit time (chronological in the file,
           independent of when the hot window evicts it) — written AFTER the
           anchor decision so `parent` names the anchor this frame hangs off,
           which is itself when this turn is the anchor. Written from the ring
           slot, because commit moved the frame and freed the local pointer. */
        {
            int just = (win.head - 1 + win.cap) % win.cap;
            win.frames[just].hdr.parent = last_anchor_turn;   /* round this frame hangs off */
            kvt_persist_delta(&L, &win.frames[just]);
        }
    }
    /* drain remaining hot frames — their deltas already went to the log at
       commit time; nothing to persist here. */
    (void)win;
    printf("log lines=%d anchors=%d blob_used=%" PRIu64 " B\n", host.n_lines, n_anchors, host.blob_used);

    /* re-read the log FIRST: pass 1 just appended to it, and both the
       verification and the replay below have to use what is ON DISK, not the
       snapshot taken before the run */
    kvl_free(log_deltas, nd_log);
    kvl_read(host.log_path, log_anchors, 64, &na_log, log_deltas, LOG_MAX_FRAMES, &nd_log);
    printf("log re-read: %d anchor(s), %d delta(s)\n", na_log, nd_log);

    /* the log must hang together before anything is replayed through it: every
       delta's parent has to be the nearest anchor on this path. A record from
       another branch would otherwise replay on the wrong base and produce a
       plausible wrong state. */
    {
        int32_t bt = 0, bp = 0;
        if (kvl_check_parents(log_anchors, na_log, log_deltas, nd_log, &bt, &bp) != 0) {
            fprintf(stderr, "log: turn %d hangs off parent %d, which is not its nearest "
                            "anchor — this log is not one path\n", bt, bp);
            kvl_free(log_deltas, nd_log);
            kvt_free(&win);
            llama_free(ctx);
            llama_model_free(model);
            return 1;
        }
    }

    /* ── pass 1b: the log must be self-verifying ──
     * Re-read every anchor BLOB through the same path a later process uses and
     * check its FNV against the record — for every anchor IN THE LOG, not just
     * the ones this run wrote (an append run that cuts no new anchor still has
     * to prove the existing resume points are intact). */
    int n_verify = nd_log ? na_log : n_anchors;
    const KVTAnchor *va = nd_log ? log_anchors : anchors;
    int bad_anchor = 0;
    for (int i = 0; i < n_verify; i++) {
        size_t len = (size_t)va[i].blob_len;
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        if (!buf) { bad_anchor++; continue; }
        if (lh_blob_read(&host, va[i].blob_off, buf, len) != 0 ||
            fnv_bytes(buf, len) != va[i].base_fnv) {
            fprintf(stderr, "anchor @t%d: blob at +%" PRIu64 "/%" PRIu64 " does not match its FNV\n",
                    va[i].turn, va[i].blob_off, (uint64_t)len);
            bad_anchor++;
        }
        free(buf);
    }
    printf("anchor blobs self-verified: %d/%d\n", n_verify - bad_anchor, n_verify);
    if (bad_anchor) { kvt_free(&win); llama_free(ctx); llama_model_free(model); return 1; }

    /* ── pass 2: SEEK back into a fresh ctx and compare argmax ──
     *
     * Everything the replay needs comes from the log and the side blob file:
     * the anchor record names the resume point, the delta records carry the
     * tokens. Nothing is taken from the producer's memory, which is the whole
     * point — a later process must be able to recall a frame it never saw.
     *
     * The replay mirrors the LIVE decode schedule turn by turn, deliberately.
     * A quantized KV (K=Q8_0, V=Q4_0) rounds per batch, so replaying 125 tokens
     * in ONE batch is not the same arithmetic as decoding 16 straight turns of
     * 14-20 tokens — measured at t9 of the 12-turn run: argmax flipped
     * (785 -> 40) with identical bytes in, purely from batch shape
     * (same class as memory #7032). Seeking must reproduce the producer's
     * batching, not just its tokens. */
    if (skip_seek) {
        /* Producer-only run: a long log is expensive to sweep (each rebuild is
           O(turns-since-anchor)), so a caller that only wants the log and the
           anchor blob asks for pass 1 alone; a later append run then proves the
           resume point it produced. */
        printf("=== kv_temporal_llama: pass 1 only (noseek) — %d delta(s), %d anchor(s) ===\n",
               nd_log, na_log);
        kvl_free(log_deltas, nd_log);
        kvt_free(&win);
        llama_free(ctx);
        llama_model_free(model);
        return 0;
    }
    struct llama_context *c2 = mk_ctx(model);
    int match = 0, compared = 0, skipped = 0;
    llama_token got = -1;
    int32_t pos2 = 0;
    KVTSeek s; memset(&s, 0, sizeof(s));
    const int32_t T_first = turn_base + 1, T_last = turn_base + NT;
    for (int32_t T = T_first; T <= T_last; T++) {
        if (!verify_turn(T, T_first, T_last)) continue;
        if (kv_rebuild(c2, &L, log_anchors, na_log, log_deltas, nd_log, T, &pos2, &got, n_vocab) != 0) {
            fprintf(stderr, "[seek t%d] rebuild failed\n", T);
            llama_free(c2);
            kvt_free(&win);
            return 1;
        }
        kvt_seek(log_anchors, na_log, T, &s);
        if (s.have_anchor && T == s.anchor.turn) {
            /* the anchor blob ends exactly at this frame's last token, so there
               is nothing left to decode: argmax is not comparable here. Saying
               so is the honest result — not a MATCH and not a DIFF. */
            skipped++;
            printf("[seek t%d] anchor=yes replay=0 tok  argmax=n/a (frame ends inside the anchor)\n", T);
            continue;
        }
        const int32_t live = live_argmax[T - turn_base];
        const int32_t replay = pos2 - (s.have_anchor ? s.anchor.base_n_tokens : 0);
        int ok = (got == live);
        compared++;
        printf("[seek t%d] anchor=%s replay=%d tok  argmax=%d live=%d %s\n",
               T, s.have_anchor ? "yes" : "no", replay, got, live, ok ? "MATCH" : "DIFF");
        if (ok) match++;
    }
    printf("=== kv_temporal_llama: %d/%d MATCH (%d skipped: anchor-turn frame) ===\n",
           match, compared, skipped);

    kvl_free(log_deltas, nd_log);
    llama_free(c2);
    kvt_free(&win);
    llama_free(ctx);
    llama_model_free(model);
    return match == compared ? 0 : 1;
}
