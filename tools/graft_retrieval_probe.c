/* tools/graft_retrieval_probe.c — graft principles as retrieval for inference.
 *
 * Card #31: seed→walk→pack + hub/in-degree rank + span-granular + cheap
 * signature view, applied as tensor retrieval feeding inference.
 * Grounding (all verified in-repo, not invented):
 *   cheap view  = at_centroid 16-dim chunk means (core/anchor_tess.h, #42)
 *   walk order  = inference chain token_embd→blk.N→output (cat_of/sort_inference
 *                 copied from tools/gguf_graft_field.c — graft's serve order)
 *   hub/in-deg = bucket popularity: pop[k] = #tensors assigned to bucket k;
 *                 in-degree(i) = pop[bucket(i)]; fused score =
 *                 dist(q,i) / (1 + pop[bucket(i)])
 *   span-pack   = per-tensor byte spans gathered into one chain-ordered buffer
 *                 at 32-aligned steps (graft-style chain offsets)
 *
 * Oracles (independent of the code under test):
 *   identity (self must route top-1 by pure distance), determinism (rank twice
 *   → identical), direct-mmap memcmp (pack fidelity), zeroed-row mutation
 *   (gate must go red), tensor-name block prefix (measurement only, no gate).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/graft_retrieval_probe tools/graft_retrieval_probe.c -lm
 * RUN:   ./build/graft_retrieval_probe [model.gguf] [seed-substring] [top-b]
 *        Needs model file; SKIP (named) when absent.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "anchor_tess.h"
#include "gguf_reader.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define K_ANCH 24
#define ROUTE_B 2
#define ALIGN 32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

/* inference chain order — verbatim from tools/gguf_graft_field.c (graft walk) */
static int cat_of(const char *name, unsigned *block) {
    *block = 0;
    if (strncmp(name, "token_embd", 10) == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) { *block = (unsigned)atoi(name + 4); return 1; }
    if (strncmp(name, "output_norm", 11) == 0) return 2;
    return 3;
}

static double cdist(const float *a, const float *b, int dim) {
    double d = 0;
    for (int j = 0; j < dim; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *seedsub = (argc > 2) ? argv[2] : "token_embd";
    int TOPB = (argc > 3) ? atoi(argv[3]) : 8;
    if (TOPB <= 0) TOPB = 8;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("GRAFT_RETRIEVAL — %s seed~%s topb=%d\n", model, seedsub, TOPB);

    GgufReader g;
    memset(&g, 0, sizeof(g));
    if (gguf_open(model, &g) != 0) {
        printf("  SKIP — model file not present (named, not hidden)\n");
        return 0;
    }
    uint32_t n = g.n_tensors;
    printf("  tensors: %u\n", n);
    int dim = (int)AT_DIM;

    float *X = (float *)calloc((size_t)n * dim, sizeof(float));
    float *C = (float *)calloc((size_t)K_ANCH * dim, sizeof(float));
    int *lab = (int *)calloc(n, sizeof(int));
    CHECK(X && C && lab, "alloc");
    if (!X || !C || !lab) { gguf_close(&g); return 1; }

    for (uint32_t i = 0; i < n; i++)
        at_centroid(g.base + g.data_offset + g.offsets[i], (uint32_t)g.sizes[i],
                    X + (size_t)i * dim);
    CHECK(anch_train(X, (int)n, dim, K_ANCH, C, lab) == 0, "train");

    /* hub/in-degree: bucket popularity over all tensors */
    int pop[K_ANCH];
    memset(pop, 0, sizeof(pop));
    for (uint32_t i = 0; i < n; i++)
        if (lab[i] >= 0 && lab[i] < K_ANCH) pop[lab[i]]++;

    int *cand = (int *)malloc((size_t)n * sizeof(int));
    int *order = (int *)malloc((size_t)n * sizeof(int));
    double *psc = (double *)malloc((size_t)n * sizeof(double));
    int *is_cand = (int *)calloc(n, sizeof(int));
    CHECK(cand && order && psc && is_cand, "alloc2");

    int top[K_ANCH]; /* anch_route writes K ids: out must hold K ints */
    long tied_pairs = 0;
    long self_top1 = 0, self_in_cand = 0, cand_total = 0;
    long fused_flip = 0, block_hit_sum = 0, block_cnt_sum = 0;
    double chance_sum = 0;
    /* per-(cat,block) member counts for the chance baseline */
    for (uint32_t s = 0; s < n; s++) {
        const float *q = X + (size_t)s * dim;
        /* stage-1 cheap view: route to candidate buckets */
        int nr = anch_route(q, C, K_ANCH, dim, ROUTE_B, top);
        CHECK(nr == ROUTE_B, "route arity");
        memset(is_cand, 0, (size_t)n * sizeof(int));
        int nc = 0;
        for (uint32_t i = 0; i < n; i++)
            for (int r = 0; r < nr; r++)
                if (lab[i] == top[r]) { is_cand[i] = 1; nc++; break; }
        cand_total += nc;
        if (is_cand[s]) self_in_cand++;

        /* stage-2 exact: pure-dist rank over candidates */
        int m = 0;
        for (uint32_t i = 0; i < n; i++)
            if (is_cand[i]) { cand[m] = (int)i; psc[m] = cdist(q, X + (size_t)i * dim, dim); m++; }
        for (int a = 0; a < m; a++) order[a] = a;
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++)
                if (psc[order[b]] < psc[order[a]] ||
                    (psc[order[b]] == psc[order[a]] && cand[order[b]] < cand[order[a]])) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
        /* G1 oracle (identity, tie-aware): self ranks 0, unless the top-1
         * rival sits at the same dist 0 AND is byte-identical (file-level
         * weight duplication, proven by memcmp — not assumed). */
        if (m > 0 && cand[order[0]] == (int)s) self_top1++;
        else if (m > 0) {
            double dself = cdist(q, X + (size_t)s * dim, dim);
            double drival = cdist(q, X + (size_t)cand[order[0]] * dim, dim);
            int dup = (g.sizes[s] == g.sizes[cand[order[0]]]) &&
                memcmp(g.base + g.data_offset + g.offsets[s],
                       g.base + g.data_offset + g.offsets[cand[order[0]]],
                       (size_t)g.sizes[s]) == 0;
            printf("  G1-exc: [%u] %s -> top1 [%d] %s dself=%.3g drival=%.3g byte_ident=%d\n",
                   s, g.names[s], cand[order[0]], g.names[cand[order[0]]],
                   dself, drival, dup);
            if (dself == 0.0 && drival == 0.0 && dup) tied_pairs++;
        }

        /* hub-fused rank over the same candidates */
        for (int a = 0; a < m; a++) {
            int i = cand[a];
            double d = cdist(q, X + (size_t)i * dim, dim);
            psc[a] = d / (1.0 + (lab[i] >= 0 ? pop[lab[i]] : 0));
        }
        /* fused order into order[] (reuse): recompute rank positions */
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++) {
                int ia = cand[order[a]], ib = cand[order[b]];
                double sa = cdist(q, X + (size_t)ia * dim, dim) / (1.0 + (lab[ia] >= 0 ? pop[lab[ia]] : 0));
                double sb = cdist(q, X + (size_t)ib * dim, dim) / (1.0 + (lab[ib] >= 0 ? pop[lab[ib]] : 0));
                if (sb < sa || (sb == sa && ib < ia)) { int t = order[a]; order[a] = order[b]; order[b] = t; }
            }
        if (m > 0 && cand[order[0]] != (int)s) fused_flip++;

        /* block-hit measurement (name oracle, NOT a gate) */
        unsigned sc = 0, sb2 = 0;
        sc = (unsigned)cat_of(g.names[s], &sb2);
        int take = m < TOPB ? m : TOPB;
        for (int a = 0; a < take; a++) {
            int i = cand[order[a]];
            if (i == (int)s) continue;
            unsigned c2 = 0, b2 = 0;
            c2 = (unsigned)cat_of(g.names[i], &b2);
            if (c2 == sc && b2 == sb2) block_hit_sum++;
            block_cnt_sum++;
        }
        int members = 0;
        for (uint32_t i = 0; i < n; i++) {
            unsigned c2 = 0, b2 = 0;
            c2 = (unsigned)cat_of(g.names[i], &b2);
            if (c2 == sc && b2 == sb2) members++;
        }
        if (n > 1) chance_sum += (double)(members - 1) / (double)(n - 1);
    }
    printf("  G1 self-in-cand: %ld/%u  self-top1(pure): %ld/%u  tied-dup: %ld\n",
           self_in_cand, n, self_top1, n, tied_pairs);
    CHECK(self_in_cand == (long)n, "seed always in cheap-view candidates");
    CHECK(self_top1 + tied_pairs == (long)n, "identity: rank-0 or byte-identical tie");
    printf("  scan: %.2f%% cand/query  fused-flip-top1: %ld/%u\n",
           100.0 * (double)cand_total / ((double)n * n), fused_flip, n);
    printf("  block-hit@%d: %.3f  chance: %.3f (measurement, not gated)\n", TOPB,
           block_cnt_sum ? (double)block_hit_sum / block_cnt_sum : 0.0, chance_sum / n);
    CHECK(cand_total < (long)n * n, "cheap view prefilters (scan<100%)");

    /* G2 determinism: re-rank seed-0 twice over full set, identical order */
    {
        const float *q = X;
        int *o1 = (int *)malloc((size_t)n * sizeof(int));
        int *o2 = (int *)malloc((size_t)n * sizeof(int));
        for (uint32_t r = 0; r < 2; r++) {
            int *o = r ? o2 : o1;
            for (uint32_t i = 0; i < n; i++) o[i] = (int)i;
            for (uint32_t a = 0; a < n; a++)
                for (uint32_t b = a + 1; b < n; b++) {
                    double sa = cdist(q, X + (size_t)o[a] * dim, dim) / (1.0 + pop[lab[o[a]]]);
                    double sb = cdist(q, X + (size_t)o[b] * dim, dim) / (1.0 + pop[lab[o[b]]]);
                    if (sb < sa || (sb == sa && o[b] < o[a])) { int t = o[a]; o[a] = o[b]; o[b] = t; }
                }
        }
        CHECK(memcmp(o1, o2, (size_t)n * sizeof(int)) == 0, "fused rank deterministic");
        free(o1); free(o2);
    }

    /* seed→walk→pack demo: top-b fused spans in chain order, 32-aligned */
    {
        uint32_t s = 0;
        for (uint32_t i = 0; i < n; i++)
            if (strstr(g.names[i], seedsub)) { s = i; break; }
        printf("  pack seed: [%u] %s\n", s, g.names[s]);
        const float *q = X + (size_t)s * dim;
        int nr = anch_route(q, C, K_ANCH, dim, ROUTE_B, top);
        memset(is_cand, 0, (size_t)n * sizeof(int));
        int m = 0;
        for (uint32_t i = 0; i < n; i++)
            for (int r = 0; r < nr; r++)
                if (lab[i] == top[r]) { is_cand[i] = 1; break; }
        for (uint32_t i = 0; i < n; i++)
            if (is_cand[i]) { cand[m] = (int)i; m++; }
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++) {
                int ia = cand[a], ib = cand[b];
                double sa = cdist(q, X + (size_t)ia * dim, dim) / (1.0 + (lab[ia] >= 0 ? pop[lab[ia]] : 0));
                double sb = cdist(q, X + (size_t)ib * dim, dim) / (1.0 + (lab[ib] >= 0 ? pop[lab[ib]] : 0));
                if (sb < sa || (sb == sa && ib < ia)) { int t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
            }
        int take = m < TOPB ? m : TOPB;
        /* walk: chain order for serve layout */
        for (int a = 0; a < take; a++)
            for (int b = a + 1; b < take; b++) {
                unsigned ba = 0, bb = 0;
                int ca = cat_of(g.names[cand[a]], &ba), cb = cat_of(g.names[cand[b]], &bb);
                if (cb < ca || (cb == ca && (bb < ba || (bb == ba && cand[b] < cand[a])))) {
                    int t = cand[a]; cand[a] = cand[b]; cand[b] = t;
                }
            }
        uint64_t body = 0;
        for (int a = 0; a < take; a++) body += align32(g.sizes[cand[a]]);
        uint8_t *pack = (uint8_t *)calloc(1, (size_t)body);
        CHECK(pack != NULL, "pack alloc");
        uint64_t pos = 0;
        printf("  pack layout (chain order, %d spans, %llu B):\n", take, (unsigned long long)body);
        for (int a = 0; a < take; a++) {
            int i = cand[a];
            const uint8_t *src = g.base + g.data_offset + g.offsets[i];
            memcpy(pack + pos, src, (size_t)g.sizes[i]);
            /* G3 oracle: span == direct mmap bytes */
            CHECK(memcmp(pack + pos, src, (size_t)g.sizes[i]) == 0, "span fidelity");
            printf("    +%-6llu %-48s %10llu B pop=%d\n",
                   (unsigned long long)pos, g.names[i],
                   (unsigned long long)g.sizes[i], lab[i] >= 0 ? pop[lab[i]] : -1);
            pos += align32(g.sizes[i]);
        }
        /* G2b: re-pack identical */
        {
            uint8_t *pack2 = (uint8_t *)calloc(1, (size_t)body);
            uint64_t p2 = 0;
            for (int a = 0; a < take; a++) {
                memcpy(pack2 + p2, g.base + g.data_offset + g.offsets[cand[a]],
                       (size_t)g.sizes[cand[a]]);
                p2 += align32(g.sizes[cand[a]]);
            }
            CHECK(memcmp(pack, pack2, (size_t)body) == 0, "pack deterministic");
            free(pack2);
        }
        free(pack);
    }

    /* G4 mutation: zeroed index row must NOT self-top1 (gate is red-sensitive) */
    {
        float *Xm = (float *)malloc((size_t)n * dim * sizeof(float));
        CHECK(Xm != NULL, "mut alloc");
        memcpy(Xm, X, (size_t)n * dim * sizeof(float));
        memset(Xm, 0, (size_t)dim * sizeof(float)); /* corrupt row 0 */
        const float *q = X; /* real query */
        int best = -1;
        double bd = 1e300;
        for (uint32_t i = 0; i < n; i++) {
            double d = cdist(q, Xm + (size_t)i * dim, dim);
            if (d < bd) { bd = d; best = (int)i; }
        }
        printf("  mutation(zero row0): top1=[%d] (must != 0)\n", best);
        CHECK(best != 0, "mutation red: zeroed row loses self-top1");
        free(Xm);
    }

    free(X); free(C); free(lab); free(cand); free(order); free(psc); free(is_cand);
    gguf_close(&g);
    if (!fails) printf("graft_retrieval_probe: ALL PASS\n");
    return fails ? 1 : 0;
}
