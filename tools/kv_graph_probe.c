/* tools/kv_graph_probe.c — graft ideas applied to KV-cache (card #32).
 *
 * Graph reading of the SID cold-KV lineage (cf. tools/sid_kv_compact.c,
 * whose eviction is a hardcoded positional live set {1,2}):
 *   node  = SID record: identity (node_id, capo_key) + name + inf.tick
 *   edge  = suffix-ref: kv.delta.X -> kv.base.X (name-suffix convention,
 *           verified structurally: edge exists iff the target exists)
 *   in-degree(b) = #delta edges into b + #access events on b
 *   resume = walk: read base bytes + replay suffix records in tick order
 *   evict  = LIVE {latest base by tick} u {deltas edged into live bases}
 *   Access-counts do NOT save superseded bases (refs dominate hotness);
 *   they are the tiebreak + hotness report. Stated plainly, gated below.
 *
 * Oracles: constructed lineage with known shape (G1/G3/G4), direct-read
 * memcmp (G2), real .tmem bytes Phase B (independent artifact from the
 * sid_kv_reanchor run — not produced by this probe).
 *
 * BUILD: gcc -O2 -Wall -I. -Isid -o build/kv_graph_probe tools/kv_graph_probe.c
 * RUN:   ./build/kv_graph_probe
 *        Phase B needs build/kvslots-sid/sid_kv_reanchor.tmem; SKIP (named) if absent.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "adaptive_route_sid.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define ARENA (256u * 1024u)
static uint8_t g_a[ARENA], g_b[ARENA], g_c[ARENA];

/* ── graph over a loaded store ── */
#define KVG_MAXN 64
typedef struct {
    uint32_t n;
    int is_base[KVG_MAXN];      /* name starts with kv.base. */
    int is_delta[KVG_MAXN];     /* name starts with kv.delta. */
    int edge[KVG_MAXN];         /* delta d -> base index, or -1 */
    int accesses[KVG_MAXN];     /* access events (resume reads) */
    int tick[KVG_MAXN];
} KVGraph;

static int startswith(const char *s, const char *pre) {
    return s && strncmp(s, pre, strlen(pre)) == 0;
}
/* suffix after the last '.'; edge delta.X -> base with equal suffix */
static const char *suffix_of(const char *s) {
    const char *p = s ? strrchr(s, '.') : NULL;
    return p ? p + 1 : "";
}

static void kvg_build(const TensorMemStore *s, KVGraph *g) {
    memset(g, 0, sizeof(*g));
    g->n = s->n_records < KVG_MAXN ? s->n_records : KVG_MAXN;
    for (uint32_t i = 0; i < g->n; i++) {
        g->edge[i] = -1;
        const char *nm = tmem_record_name(s, i);
        g->is_base[i] = startswith(nm, "kv.base.");
        g->is_delta[i] = startswith(nm, "kv.delta.");
        const ZoneCardSID *z = tmem_record_zcsid(s, i);
        g->tick[i] = z ? (int)z->inf.tick : -1;
    }
    for (uint32_t d = 0; d < g->n; d++) {
        if (!g->is_delta[d]) continue;
        const char *nm_d = tmem_record_name(s, d);
        char ds[64];
        snprintf(ds, sizeof(ds), "%s", suffix_of(nm_d));
        for (uint32_t b = 0; b < g->n; b++) {
            if (!g->is_base[b]) continue;
            const char *nm_b = tmem_record_name(s, b);
            char bs[64];
            snprintf(bs, sizeof(bs), "%s", suffix_of(nm_b));
            if (strcmp(bs, ds) == 0) { g->edge[d] = (int)b; break; }
        }
    }
}
static void kvg_access(KVGraph *g, uint32_t i) { if (i < g->n) g->accesses[i]++; }
static int kvg_indeg(const KVGraph *g, uint32_t b) {
    int d = 0;
    for (uint32_t i = 0; i < g->n; i++)
        if (g->edge[i] == (int)b) d++;
    return d + (b < g->n ? g->accesses[b] : 0);
}
/* live set: latest base by tick + deltas edged into a live base */
static void kvg_live(const KVGraph *g, int *live) {
    for (uint32_t i = 0; i < g->n; i++) live[i] = 0;
    int lb = -1;
    for (uint32_t i = 0; i < g->n; i++)
        if (g->is_base[i] && (lb < 0 || g->tick[i] > g->tick[lb])) lb = (int)i;
    if (lb < 0) return;
    live[lb] = 1;
    for (uint32_t d = 0; d < g->n; d++)
        if (g->is_delta[d] && g->edge[d] == lb) live[d] = 1;
}

/* append one lineage record with reanchor-shaped identity */
static int put_rec(TensorMemStore *s, const char *name, uint32_t node,
                   uint16_t tick, const uint8_t *data, size_t len) {
    ZoneCard card;
    memset(&card, 0, sizeof(card));
    card.entropy = 12;
    AdaptiveRouteSIDEvent e = { 2, 15, 1, node, 37, 1, 0 };
    return adaptive_route_sid_append(s, &e, &card, name, data, len, 0, 0, tick, 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("KV_GRAPH — card #32 probe\n");

    /* ══ Phase A: constructed lineage base.0 -> base.10 + delta.10 ══ */
    TensorMemStore st;
    tmem_init(&st, g_a, sizeof(g_a), 0);
    uint8_t p0[1024], p10[2048];
    for (size_t i = 0; i < sizeof(p0); i++) p0[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < sizeof(p10); i++) p10[i] = (uint8_t)(i * 13 + 5);
    uint32_t suf[10];
    for (int i = 0; i < 10; i++) suf[i] = 1000u + (uint32_t)i;
    CHECK(put_rec(&st, "kv.base.0", 1728, 0, p0, sizeof(p0)) == 0, "append base.0");
    CHECK(put_rec(&st, "kv.base.10", 1738, 10, p10, sizeof(p10)) == 0, "append base.10");
    CHECK(put_rec(&st, "kv.delta.10", 1739, 11, (uint8_t *)suf, sizeof(suf)) == 0, "append delta.10");
    CHECK(st.n_records == 3, "3-record lineage");

    KVGraph g;
    kvg_build(&st, &g);
    CHECK(g.edge[2] == 1, "edge delta.10 -> base.10 (record 1)");
    /* access events: base.0 HOT (x5) but superseded; base.10 x1; delta x1 */
    for (int i = 0; i < 5; i++) kvg_access(&g, 0);
    kvg_access(&g, 1); kvg_access(&g, 2);
    printf("  indeg: base.0=%d base.10=%d delta.10=%d\n",
           kvg_indeg(&g, 0), kvg_indeg(&g, 1), kvg_indeg(&g, 2));

    /* G1: refs dominate hotness — evict base.0 despite 5 accesses */
    int live[KVG_MAXN] = {0};
    kvg_live(&g, live);
    printf("  live: {%d %d %d} (expect 0 1 1)\n", live[0], live[1], live[2]);
    CHECK(!live[0] && live[1] && live[2], "G1 live=={base.10,delta.10}, hot base.0 evicted");

    /* G2: compacted copy (copy-live-set-forward) resumes byte-identical */
    {
        TensorMemStore dst;
        tmem_init(&dst, g_b, sizeof(g_b), 0);
        for (uint32_t i = 0; i < g.n; i++) {
            if (!live[i]) continue;
            int sz = tmem_read_record(&st, i, NULL, 0, NULL, NULL);
            uint8_t *buf = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
            const ZoneCardSID *z = tmem_record_zcsid(&st, i);
            const char *nm = tmem_record_name(&st, i);
            char name[140];
            snprintf(name, sizeof(name), "%s", nm ? nm : "kv.unnamed");
            CHECK(sz > 0 && buf && z, "carry read");
            CHECK(tmem_read_record(&st, i, buf, (size_t)sz, NULL, NULL) == sz, "carry bytes");
            CHECK(tmem_append_raw(&dst, z, name, buf, (size_t)sz) == 0, "carry append");
            free(buf);
        }
        CHECK(dst.n_records == 2, "compacted holds 2");
        /* resume walk: base + suffix replay, memcmp vs direct source reads */
        int bsz = tmem_read_record(&dst, 0, NULL, 0, NULL, NULL);
        int dsz = tmem_read_record(&dst, 1, NULL, 0, NULL, NULL);
        uint8_t *sb = (uint8_t *)malloc(bsz > 0 ? (size_t)bsz : 1);
        uint8_t *sd = (uint8_t *)malloc(dsz > 0 ? (size_t)dsz : 1);
        CHECK(tmem_read_record(&dst, 0, sb, (size_t)bsz, NULL, NULL) == bsz, "walk base");
        CHECK(tmem_read_record(&dst, 1, sd, (size_t)dsz, NULL, NULL) == dsz, "walk suffix");
        CHECK(bsz == (int)sizeof(p10) && memcmp(sb, p10, sizeof(p10)) == 0, "G2 base bytes");
        CHECK(dsz == (int)sizeof(suf) && memcmp(sd, suf, sizeof(suf)) == 0, "G2 suffix bytes");
        free(sb); free(sd);
    }

    /* G3: rebuild determinism — identical adjacency + live set */
    {
        KVGraph g2;
        kvg_build(&st, &g2);
        int live2[KVG_MAXN] = {0};
        kvg_live(&g2, live2);
        CHECK(memcmp(g.edge, g2.edge, sizeof(g.edge)) == 0, "G3 adjacency deterministic");
        CHECK(memcmp(live, live2, sizeof(live)) == 0, "G3 live deterministic");
    }

    /* G4 mutation: rewire delta -> base.0; live must FOLLOW the edge {0,2} */
    {
        TensorMemStore mu;
        tmem_init(&mu, g_c, sizeof(g_c), 0);
        CHECK(put_rec(&mu, "kv.base.0", 1728, 0, p0, sizeof(p0)) == 0, "mu base.0");
        CHECK(put_rec(&mu, "kv.base.10", 1738, 10, p10, sizeof(p10)) == 0, "mu base.10");
        CHECK(put_rec(&mu, "kv.delta.0", 1739, 11, (uint8_t *)suf, sizeof(suf)) == 0, "mu delta.0");
        KVGraph gm;
        kvg_build(&mu, &gm);
        CHECK(gm.edge[2] == 0, "mu edge delta.0 -> base.0");
        int livem[KVG_MAXN] = {0};
        kvg_live(&gm, livem);
        printf("  mu live: {%d %d %d} (edge-driven: latest base.10 unreferenced)\n",
               livem[0], livem[1], livem[2]);
        /* latest base by tick is STILL base.10 (tick 10 > tick 0): rule keeps
         * latest base + deltas into it. delta.0 points at base.0, so live =
         * {base.10} only, delta.0 DANGLING (edge to non-live base). A
         * positional hardcode {1,2} would wrongly carry the dangling delta. */
        CHECK(livem[1] && !livem[2], "G4 edge-driven: dangling delta.0 NOT live");
    }

    /* ══ Phase B: real .tmem from the sid_kv_reanchor run ══ */
    {
        const char *rp = "build/kvslots-sid/sid_kv_reanchor.tmem";
        static uint8_t g_r[4u * 1024u * 1024u];
        TensorMemStore rs;
        if (tmem_load(&rs, g_r, sizeof(g_r), rp) != 0) {
            printf("  SKIP Phase B — %s not present (named, not hidden)\n", rp);
        } else {
            printf("  real store: %u records\n", rs.n_records);
            CHECK(rs.n_records == 3, "real lineage shape (base.0, base.10, delta.10)");
            KVGraph rg;
            kvg_build(&rs, &rg);
            for (uint32_t i = 0; i < rg.n; i++) {
                const char *rn = tmem_record_name(&rs, i);
                char rnb[80];
                snprintf(rnb, sizeof(rnb), "%s", rn ? rn : "?");
                printf("    [%u] %-14s tick=%d node=%u capo=%u edge->%d\n", i, rnb,
                       rg.tick[i], tmem_record_zcsid(&rs, i)->node_id,
                       tmem_record_zcsid(&rs, i)->capo_key, rg.edge[i]);
            }
            int liver[KVG_MAXN] = {0};
            kvg_live(&rg, liver);
            printf("  real live: {%d %d %d} (expect 0 1 1 = compact's {1,2})\n",
                   liver[0], liver[1], liver[2]);
            CHECK(!liver[0] && liver[1] && liver[2], "real live matches compact decision");
            int dsz = tmem_read_record(&rs, 2, NULL, 0, NULL, NULL);
            printf("  real delta bytes: %d (expect %d = 10 suffix tokens)\n",
                   dsz, 10 * (int)sizeof(uint32_t));
            CHECK(dsz == 10 * (int)sizeof(uint32_t), "real suffix shape");
        }
    }

    /* Phase B2: compacted file — everything live, nothing evictable */
    {
        const char *cp = "build/kvslots-sid/sid_kv_compact.tmem";
        static uint8_t g_r2[4u * 1024u * 1024u];
        TensorMemStore cs;
        if (tmem_load(&cs, g_r2, sizeof(g_r2), cp) != 0) {
            printf("  SKIP Phase B2 — %s not present (named, not hidden)\n", cp);
        } else {
            KVGraph cg;
            kvg_build(&cs, &cg);
            int livec[KVG_MAXN] = {0};
            kvg_live(&cg, livec);
            printf("  compact live: {%d %d} (expect 1 1 — nothing left to evict)\n",
                   livec[0], livec[1]);
            CHECK(livec[0] && livec[1], "compacted store fully live");
        }
    }

    if (!fails) printf("kv_graph_probe: ALL PASS\n");
    return fails ? 1 : 0;
}
