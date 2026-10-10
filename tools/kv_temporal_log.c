/* kv_temporal_log.c — standalone host for kv_temporal.h.
 *
 * Deliberately ISOLATED from chat-pool: nothing here touches pool.py, the
 * real log.jsonl, the hourly flush, or the memory bridge. It writes its own
 * append-only log + side blob file so the KV storage model can be exercised
 * end to end (commit -> window -> evict -> log -> seek -> replay) before any
 * decision to wire it into the production pool.
 *
 * The record format MIRRORS chat-pool's log.jsonl line shape
 *   {v, t, app, kind, text, prev, hash}
 * with hash = sha256(prev + app + kind + to + text) — so a later host
 * adapter can drop these lines into the pool unchanged.
 *
 * Usage:
 *   kv_temporal_log <dir> [N]         run N synthetic frames, then verify
 */
#include "kv_temporal.h"
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32")
#endif

/* ── sha256 via the OS (no OpenSSL dependency) ── */
static void sha256_hex(const void *data, size_t len, char out[65]) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0; HCRYPTHASH hHash = 0;
    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (const BYTE *)data, (DWORD)len, 0);
    BYTE digest[32]; DWORD dlen = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, digest, &dlen, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
    out[64] = 0;
    CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0);
#else
    (void)data; (void)len; snprintf(out, 65, "%.64s", "no-sha256-on-this-build-0000000000000000000000000000000000000000");
#endif
}

typedef struct {
    char   dir[KVT_PATH];
    char   log_path[KVT_PATH + 32];
    char   blob_path[KVT_PATH + 32];
    char   prev[65];        /* previous line hash (chain) */
    uint64_t blob_used;
    int    n_lines;
} Host;

static int host_resolve(Host *h, const char *dir) {
    snprintf(h->dir, sizeof(h->dir), "%s", dir);
    snprintf(h->log_path, sizeof(h->log_path), "%s\\kv_temp.log.jsonl", dir);
    snprintf(h->blob_path, sizeof(h->blob_path), "%s\\kv_temp.blob", dir);
    snprintf(h->prev, sizeof(h->prev), "GENESIS");
    h->blob_used = 0; h->n_lines = 0;
    return 0;
}

/* one JSONL line, hash-chained exactly like pool.append */
static int host_line(Host *h, const char *kind, const char *text) {
    double t = (double)time(NULL);
    /* hash covers prev+app+kind+to+text (pool convention) */
    size_t hb_len = strlen(h->prev) + strlen("kv-host") + strlen(kind) + strlen("all") + strlen(text) + 1;
    char *hb = (char *)malloc(hb_len);
    snprintf(hb, hb_len, "%s%s%s%s%s", h->prev, "kv-host", kind, "all", text);
    char digest[65]; sha256_hex(hb, strlen(hb), digest);
    free(hb);

    FILE *f = fopen(h->log_path, "ab");
    if (!f) return -1;
    fprintf(f, "{\"v\":3,\"t\":%.3f,\"app\":\"kv-host\",\"kind\":\"%s\",\"text\":\"%s\",\"to\":\"all\",\"prev\":\"%s\",\"hash\":\"%s\"}\n",
            t, kind, text, h->prev, digest);
    fclose(f);
    memcpy(h->prev, digest, 65);
    h->n_lines++;
    return 0;
}

/* text is base64-free: we store the record as hex so the line stays ASCII */
static void to_hex(const void *buf, size_t len, char *out) {
    static const char *d = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) { out[i*2] = d[p[i]>>4]; out[i*2+1] = d[p[i]&15]; }
    out[len*2] = 0;
}

static int hv_log_append(void *ud, const char *kind, const void *rec, size_t len) {
    Host *h = (Host *)ud;
    char *hex = (char *)malloc(len * 2 + 1);
    to_hex(rec, len, hex);
    char *text = (char *)malloc(len * 2 + 8);
    snprintf(text, len * 2 + 8, "rec=%s", hex);
    int rc = host_line(h, kind, text);
    free(text); free(hex);
    return rc;
}

static long long hv_blob_append(void *ud, const void *buf, size_t len) {
    Host *h = (Host *)ud;
    long long off = (long long)h->blob_used;
    FILE *f = fopen(h->blob_path, "ab");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    h->blob_used += len;
    return off;
}

static int hv_blob_read(void *ud, unsigned long long off, void *buf, size_t len) {
    Host *h = (Host *)ud;
    FILE *f = fopen(h->blob_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

static uint64_t fnv(const void *b, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    int N = argc > 2 ? atoi(argv[2]) : 20;
    const int K = KVT_WINDOW;   /* hot-window RAM budget, not cadence */

    /* fresh run */
    char lp[KVT_PATH + 40], bp[KVT_PATH + 40];
    snprintf(lp, sizeof(lp), "%s\\kv_temp.log.jsonl", dir);
    snprintf(bp, sizeof(bp), "%s\\kv_temp.blob", dir);
    remove(lp); remove(bp);

    Host h; host_resolve(&h, dir);
    KVTLog L; L.ud = &h; L.log_append = hv_log_append;
    L.blob_append = hv_blob_append; L.blob_read = hv_blob_read;

    KVTWindow w; kvt_init(&w, K);
    KVTAnchor anchors[64]; int n_anchors = 0;

    printf("kv_temporal_log: dir=%s N=%d K=%d\n", dir, N, K);

    for (int t = 1; t <= N; t++) {
        int32_t toks[KVT_MAX_TOKENS];
        int32_t nt = 4 + (t % 5);
        for (int i = 0; i < nt; i++) toks[i] = t * 1000 + i;
        KVTFrame fr = kvt_frame_make(t, 0, toks, nt);

        KVTFrame ev; memset(&ev, 0, sizeof(ev));
        int had = kvt_commit_frame(&w, &fr, &ev);

        if (kvt_anchor_due(&w)) {
            size_t blen = (size_t)nt * 1024;
            uint8_t *blob = (uint8_t *)malloc(blen);
            for (size_t i = 0; i < blen; i++) blob[i] = (uint8_t)(t * 7 + i);
            KVTAnchor a;
            if (kvt_persist_anchor(&L, t, 0, 8, 2, fnv(blob, blen), blob, blen,
                                   w.since_anchor, &a) == 0 && n_anchors < 64) {
                anchors[n_anchors++] = a;
            }
            free(blob);
            w.since_anchor = 0;
            int idx = (w.head - 1 + w.cap) % w.cap;
            w.frames[idx].hdr.parent = t;
            printf("  anchor cut @t%d (%zu B blob)\n", t, blen);
        }
        if (had) { kvt_persist_delta(&L, &ev); kvt_frame_free(&ev); }
    }
    for (int i = 0; i < w.count; i++) {
        int idx = (w.head - w.count + i + w.cap * 2) % w.cap;
        kvt_persist_delta(&L, &w.frames[idx]);
    }

    /* ── verify: seek a few turns and check the plan ── */
    printf("log lines=%d anchors=%d\n", h.n_lines, n_anchors);
    int ok = 1;
    struct { int want, exp_anchor; } probe[] = {{3,1},{8,1},{13,1},{16,1},{20,1}};
    for (int i = 0; i < 5 && probe[i].want <= N; i++) {
        KVTSeek s; kvt_seek(anchors, n_anchors, probe[i].want, &s);
        int got = s.have_anchor ? s.anchor.turn : -1;
        printf("  seek(%d): anchor=%d (expect %d) replay=%d t%d..t%d\n",
               probe[i].want, got, probe[i].exp_anchor, s.n_deltas, s.first_delta, s.last_delta);
        if (got != probe[i].exp_anchor) ok = 0;
    }

    /* blob roundtrip check of the last anchor */
    if (n_anchors) {
        KVTAnchor *a = &anchors[n_anchors-1];
        uint8_t *buf = (uint8_t *)malloc((size_t)a->blob_len);
        int rc = hv_blob_read(&h, a->blob_off, buf, (size_t)a->blob_len);
        uint64_t hh = rc == 0 ? fnv(buf, (size_t)a->blob_len) : 0;
        printf("blob roundtrip last anchor t%d: %s (%llu B)\n", a->turn,
               hh == a->base_fnv ? "OK" : "MISMATCH", (unsigned long long)a->blob_len);
        if (rc != 0 || hh != a->base_fnv) ok = 0;
        free(buf);
    }

    kvt_free(&w);
    printf("=== kv_temporal_log: %s ===\n", ok ? "ALL OK" : "FAILED");
    return ok ? 0 : 1;
}
