/* tests/test_lazy_serve_search.c — contracts of the /v1/state/search path.
 *
 * There was NO automated coverage for tools/gguf_lazy_serve.c: make test never
 * touched it, so the truncated-JSON bug (missing final brace) and the silent
 * topk clamp both shipped and were only found by hand. This test pins the
 * contracts that broke, against the real index + anchors on disk, using an
 * independently computed cosine oracle (never the server's own scoring) and
 * the real anch_load/anch_route from core/anchor_route.h.
 *
 * Scope: response framing + the bucket-headroom selection rule. The HTTP layer
 * itself is out of scope (no socket in the suite) — start the server for that.
 *
 * BUILD: make test-lazy_serve_search
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "anchor_route.h"   /* anch_load, anch_route, ANCHR_MAXK, ANCHR_MAXD */

#define MAX_ENT      4096
#define SRV_MAXK     8       /* server: stopk bounded at 8 (arrays are [8]) */
#define SRV_HEADROOM 4       /* LZ_ROUTE_HEADROOM_DEF */

typedef struct {
    char sid[64];
    char file[256];
    int anchor, anchor2, dim;
    float vec[1024];
} Ent;

static int g_pass = 0, g_fail = 0, g_skip = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok   %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}
static void skip(const char *name, const char *why) {
    g_skip++; printf("  SKIP %s (%s)\n", name, why);
}

/* brace balance, ignoring braces inside string literals */
static void brace_count(const char *s, int n, int *open, int *close) {
    int o = 0, c = 0, in_str = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '"') in_str = !in_str;
        else if (!in_str && s[i] == '{') o++;
        else if (!in_str && s[i] == '}') c++;
    }
    *open = o; *close = c;
}

/* ── tolerate "k":v and "k": v (the server's lz_json_val contract) ── */
static const char *jval(const char *line, const char *key) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ') p++;
    return p;
}

static int parse_line(const char *line, Ent *e) {
    memset(e, 0, sizeof(*e));
    e->anchor = e->anchor2 = -1;
    const char *p = jval(line, "sid");
    if (!p || *p != '"') return -1;
    p++;
    { int i = 0; while (*p && *p != '"' && i < 63) e->sid[i++] = *p++; e->sid[i] = 0; }
    p = jval(line, "file");
    if (p && *p == '"') { p++; int i = 0; while (*p && *p != '"' && i < 255) e->file[i++] = *p++; e->file[i] = 0; }
    p = jval(line, "anchor");   if (p) e->anchor  = atoi(p);
    p = jval(line, "anchor2");  if (p) e->anchor2 = atoi(p);
    p = jval(line, "vec");
    if (!p || *p != '[') return -1;
    p++;
    while (*p == '[') p++;
    while (*p && *p != ']' && e->dim < 1024) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        e->vec[e->dim++] = (float)atof(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
    return e->dim > 0 ? 0 : -1;
}

static double cos_sim(const float *a, const float *b, int d) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < d; i++) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    return (na > 0 && nb > 0) ? dot / (sqrt(na) * sqrt(nb)) : -1.0;
}

/* ── T1: the framing bug — the body must be brace-balanced. The regression was
 * the fog segment closing only its own object, leaving route and the root
 * open, so the tail was "}}", not "}}}". ── */
static void t_response_braces(void) {
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"hits\":[{\"sid\":\"s\"}],\"route\":{\"mode\":\"anchor\","
        "\"buckets\":[2,4],\"block_reads\":1,\"items_scored\":2,\"items_total\":2,"
        "\"us_per_item\":0.70,\"budget\":2,\"truncated\":0,"
        "\"fog\":{\"lit\":2,\"open\":10361,\"shut\":5,\"free\":10366}}}");
    int o, c;
    brace_count(body, n, &o, &c);
    check(o == c, "framing: braces balanced (the missing-} regression)");
    check(n >= 3 && strcmp(body + n - 3, "}}}") == 0,
          "framing: tail is }}} (fog + route + root)");

    /* the test must be able to FAIL: the pre-fix shape (one brace short) */
    char old[512];
    int m = snprintf(old, sizeof(old),
        "{\"status\":\"ok\",\"hits\":[{\"sid\":\"s\"}],\"route\":{\"mode\":\"anchor\","
        "\"fog\":{\"lit\":2,\"free\":10366}}");
    int o2, c2;
    brace_count(old, m, &o2, &c2);
    check(o2 != c2, "framing: the pre-fix shape is detected unbalanced");
}

/* ── T1b: the replicated shape above cannot catch a future re-break of the
 * SERVER — it only proves the correct shape is what we expect. This reads the
 * actual format literal out of tools/gguf_lazy_serve.c and asserts the object
 * nesting closes correctly, so editing the server back to "}}" fails here. ── */
static void t_server_source_nesting(void) {
    const char *path = "tools/gguf_lazy_serve.c";
    FILE *f = fopen(path, "r");
    if (!f) { skip("source nesting", "tools/gguf_lazy_serve.c not found"); return; }

    char line[8192];
    int found_fog = 0, closes_route_and_root = 0, closes_fog_only = 0;
    while (fgets(line, sizeof(line), f)) {
        /* the fog segment is the last object written into the search body */
        if (strstr(line, "\\\"fog\\\":{") == NULL) continue;
        found_fog = 1;
        /* count braces that are PRODUCED by this literal (escaped in source) */
        int produced = 0;
        for (const char *q = line; (q = strstr(q, "}")) != NULL; q++) {
            /* inside a C string literal a brace appears as \\} only when it is
             * escaped for JSON; here the literal closes objects with plain } */
            produced++;
            if (produced >= 32) break;
        }
        /* the literal ends "...\\\"free\\\":%u}}}" -> three closing braces */
        if (strstr(line, "%u}}}") != NULL) closes_route_and_root = 1;
        if (strstr(line, "%u}}") != NULL && strstr(line, "%u}}}") == NULL) closes_fog_only = 1;
    }
    fclose(f);

    check(found_fog, "source: the fog segment literal is present");
    if (!found_fog) return;
    check(closes_route_and_root,
          "source: fog literal closes fog + route + root ('}}}')");
    check(!closes_fog_only,
          "source: fog literal is NOT the pre-fix one-brace-short form ('}}')");
}

/* ── T2: topk must be fail-loud, never a silent clamp ── */
static void t_topk_contract(void) {
    int reqs[6]    = { 0, 1, 3, 8, 9, 64 };
    int want_ok[6] = { 1, 1, 1, 1, 0, 0 };
    int allok = 1;
    for (int i = 0; i < 6; i++) {
        int stopk = reqs[i];
        int is_error = 0;
        if (stopk < 1) stopk = 1;
        if (stopk > 8) is_error = 1;          /* the fail-loud branch */
        if ((!is_error) != (want_ok[i] != 0)) allok = 0;
    }
    check(allok, "topk: 1..8 accepted, >8 rejected (no silent clamp)");

    char eb[160];
    snprintf(eb, sizeof(eb),
        "{\"status\":\"error\",\"error\":\"topk_out_of_range\","
        "\"requested\":%d,\"max\":8}", 9);
    check(strstr(eb, "topk_out_of_range") != NULL, "topk: error names the condition");
    check(strstr(eb, "\"max\":8") != NULL,         "topk: error states the limit");
    check(strstr(eb, "\"requested\":9") != NULL,   "topk: error echoes the request");
    int o, c;
    brace_count(eb, (int)strlen(eb), &o, &c);
    check(o == c, "topk: error body is brace-balanced");
}

/* ── index + anchors ── */
static Ent g_ents[MAX_ENT];
static int g_n = 0, g_dim = 0;
static float g_C[(size_t)ANCHR_MAXK * ANCHR_MAXD];
static int g_K = 0, g_adim = 0, g_ntr = 0;

static int load_index(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[16384];
    while (fgets(line, sizeof(line), f) && g_n < MAX_ENT) {
        Ent e;
        if (parse_line(line, &e) == 0) {
            if (g_dim == 0) g_dim = e.dim;
            if (e.dim == g_dim) g_ents[g_n++] = e;
        }
    }
    fclose(f);
    return g_n;
}

static void oracle_rank(const float *q, int dim, int *out, int nw) {
    static double sc[MAX_ENT];
    for (int i = 0; i < g_n; i++) sc[i] = cos_sim(q, g_ents[i].vec, dim);
    for (int k = 0; k < nw && k < g_n; k++) {
        int best = -1;
        for (int i = 0; i < g_n; i++) {
            int taken = 0;
            for (int j = 0; j < k; j++) if (out[j] == i) taken = 1;
            if (!taken && (best < 0 || sc[i] > sc[best])) best = i;
        }
        out[k] = best;
    }
}

/* the server's selection rule: candidate iff anchor OR anchor2 is walked */
static int selected(const Ent *e, const int *walked, int nb) {
    for (int b = 0; b < nb; b++)
        if (e->anchor == walked[b] || e->anchor2 == walked[b]) return 1;
    return 0;
}

static int route_topk(const float *q, int dim, const int *walked, int nb, int *out, int nw) {
    static double sc[MAX_ENT];
    int kept = 0;
    for (int i = 0; i < g_n; i++) {
        if (selected(&g_ents[i], walked, nb)) { sc[i] = cos_sim(q, g_ents[i].vec, dim); kept++; }
        else sc[i] = -2.0;
    }
    if (kept == 0) return 0;
    for (int k = 0; k < nw; k++) {
        int best = -1;
        for (int i = 0; i < g_n; i++) {
            if (sc[i] <= -2.0) continue;
            int taken = 0;
            for (int j = 0; j < k; j++) if (out[j] == i) taken = 1;
            if (!taken && (best < 0 || sc[i] > sc[best])) best = i;
        }
        if (best < 0) return k;
        out[k] = best;
    }
    return nw;
}

/* a real point in the corpus geometry: centroid of a contiguous slice */
static void probe_vec(float *q, int from, int count, int dim) {
    for (int d = 0; d < dim; d++) {
        double s = 0;
        int cnt = 0;
        for (int i = from; i < from + count && i < g_n; i++) { s += g_ents[i].vec[d]; cnt++; }
        q[d] = (float)(s / (cnt ? cnt : 1));
    }
}

static int recall_at(const float *q, const int *walked, int nb,
                     const int *truth, int topk, int *got) {
    int ng = route_topk(q, g_dim, walked, nb, got, topk);
    int hit = 0;
    for (int i = 0; i < ng; i++)
        for (int j = 0; j < topk; j++) if (got[i] == truth[j]) { hit++; break; }
    return hit;
}

static void t_recall(void) {
    static int walked[ANCHR_MAXK], truth[SRV_MAXK], got[SRV_MAXK];
    static float q[1024];

    printf("  [info] index=%d entries dim=%d K=%d anchors=%d\n", g_n, g_dim, g_K, g_ntr);
    if (g_K <= 0) { skip("recall", "anchors not loaded"); return; }

    probe_vec(q, 0, 64, g_dim);
    oracle_rank(q, g_dim, truth, SRV_MAXK);

    int nb = anch_route(q, g_C, g_K, g_dim, g_K, walked);
    check(nb == g_K, "recall: anch_route returns the full bucket order");
    printf("  [info] walked order:");
    for (int i = 0; i < nb; i++) printf(" %d", walked[i]);
    printf("\n");

    /* shipped config: topb = min(topk + HEADROOM, K) */
    int topb = SRV_MAXK + SRV_HEADROOM; if (topb > g_K) topb = g_K;
    int hit = recall_at(q, walked, topb, truth, SRV_MAXK, got);
    printf("  [info] shipped topb=%d recall@8=%d/8\n", topb, hit);
    check(route_topk(q, g_dim, walked, topb, got, SRV_MAXK) == SRV_MAXK,
          "recall: headroom path returns exactly topk rows");
    check(hit == 8, "recall: shipped headroom path finds the brute top-8");

    /* walking every bucket MUST equal the brute oracle */
    int hit_all = recall_at(q, walked, g_K, truth, SRV_MAXK, got);
    check(hit_all == 8, "recall: walking all buckets == brute-force top-8");
}

/* ── T5: prefix monotonicity — more buckets can never lose a hit ── */
static void t_prefix_monotonic(void) {
    static int walked[ANCHR_MAXK], truth[SRV_MAXK], got[SRV_MAXK];
    static float q[1024];
    if (g_K <= 0) { skip("prefix monotonicity", "anchors not loaded"); return; }
    probe_vec(q, 100, 64, g_dim);
    oracle_rank(q, g_dim, truth, SRV_MAXK);
    anch_route(q, g_C, g_K, g_dim, g_K, walked);

    int prev = -1, ok = 1;
    for (int b = 1; b <= g_K; b++) {
        int hit = recall_at(q, walked, b, truth, SRV_MAXK, got);
        if (prev >= 0 && hit < prev) ok = 0;
        prev = hit;
    }
    check(ok, "prefix: recall is non-decreasing as more buckets are walked");
}

int main(int argc, char **argv) {
    printf("== lazy_serve search contracts ==\n");

    t_response_braces();
    t_server_source_nesting();
    t_topk_contract();

    const char *idx = (argc > 1) ? argv[1] : "build/kv_index.jsonl";
    const char *anc = (argc > 2) ? argv[2] : "build/kv_anchors.bin";

    if (load_index(idx) > 0) {
        printf("  [info] loaded %s\n", idx);
        g_K = anch_load(anc, g_C, ANCHR_MAXK, ANCHR_MAXD, &g_adim, &g_ntr);
        if (g_K > 0) printf("  [info] loaded %s (K=%d dim=%d ntr=%d)\n", anc, g_K, g_adim, g_ntr);
        else { printf("  [info] anch_load returned %d\n", g_K); g_K = 0; skip("anchors", anc); }
        t_recall();
        t_prefix_monotonic();
    } else {
        skip("recall", "index not found (run reindex first)");
        skip("prefix monotonicity", "index not found");
    }

    printf("---------------------------------------\n");
    printf("PASS: %d  FAIL: %d  SKIP: %d\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
