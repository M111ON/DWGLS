/*
 * bfs_hnsw.h — Deterministic HNSW routing layer for b_fs
 * Address space: 20736 (144x144). 3 layers, M=16, integer distance.
 */
#ifndef BFS_HNSW_H
#define BFS_HNSW_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "b_fs_base.h"

#define BFS_HNSW_M 32
#define BFS_HNSW_LAYERS 3

typedef struct {
    uint16_t edges[BFS_HNSW_M];
    uint8_t  count;
} HnswNode;

typedef struct {
    HnswNode *layers[BFS_HNSW_LAYERS];
    uint32_t  sizes[BFS_HNSW_LAYERS];
} BfsHnswGraph;

/* Build deterministic graph (allocates layers). Returns 0 on success. */
static inline int bfs_hnsw_build(BfsHnswGraph *g) {
    if (!g) return -1;
    uint32_t steps[3] = {1, 16, 144};
    for (int L = 0; L < BFS_HNSW_LAYERS; L++) {
        uint32_t step = steps[L];
        uint32_t sz = BFS_TOTAL_SLOTS / step;
        g->sizes[L] = sz;
        g->layers[L] = (HnswNode*)calloc(sz, sizeof(HnswNode));
        if (!g->layers[L]) return -2;
        int M = 16;
        for (uint32_t i = 0; i < sz; i++) {
            uint32_t ai = i * step;
            int r_i = ai / 144, c_i = ai % 144;
            /* collect top M nearest neighbors */
            uint16_t best_addr[32];
            uint32_t best_dist[32];
            int best_n = 0;
            if (L == 0) {
                /* local window radius 4 */
                for (int dr = -4; dr <= 4; dr++) {
                    for (int dc = -4; dc <= 4; dc++) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r_i + dr, nc = c_i + dc;
                        if (nr < 0 || nr >= 144 || nc < 0 || nc >= 144) continue;
                        uint32_t aj = (uint32_t)(nr * 144 + nc);
                        int drr = r_i - nr, dcc = c_i - nc;
                        uint32_t d = (uint32_t)(drr*drr + dcc*dcc);
                        if (best_n < 16) {
                            best_addr[best_n] = (uint16_t)aj;
                            best_dist[best_n] = d;
                            best_n++;
                        } else {
                            int worst = 0;
                            for (int t = 1; t < best_n; t++)
                                if (best_dist[t] > best_dist[worst]) worst = t;
                            if (d < best_dist[worst]) {
                                best_addr[worst] = (uint16_t)aj;
                                best_dist[worst] = d;
                            }
                        }
                    }
                }
                /* (long-range edges disabled) */
            } else {
                for (uint32_t j = 0; j < sz; j++) {
                    if (j == i) continue;
                    uint32_t aj = j * step;
                    int rj = aj / 144, cj = aj % 144;
                    int drr = r_i - rj, dcc = c_i - cj;
                    uint32_t d = (uint32_t)(drr*drr + dcc*dcc);
                    if (best_n < M) {
                        best_addr[best_n] = (uint16_t)aj;
                        best_dist[best_n] = d;
                        best_n++;
                    } else {
                        int worst = 0;
                        for (int t = 1; t < best_n; t++)
                            if (best_dist[t] > best_dist[worst]) worst = t;
                        if (d < best_dist[worst]) {
                            best_addr[worst] = (uint16_t)aj;
                            best_dist[worst] = d;
                        }
                    }
                }
            }
            /* sort ascending by dist */
            for (int a = 1; a < best_n; a++) {
                uint16_t aa = best_addr[a];
                uint32_t da = best_dist[a];
                int b = a - 1;
                while (b >= 0 && best_dist[b] > da) {
                    best_dist[b+1] = best_dist[b];
                    best_addr[b+1] = best_addr[b];
                    b--;
                }
                best_dist[b+1] = da;
                best_addr[b+1] = aa;
            }
            g->layers[L][i].count = (uint8_t)best_n;
            for (int t = 0; t < best_n; t++)
                g->layers[L][i].edges[t] = best_addr[t];
        }
    }
    return 0;
}

static inline void bfs_hnsw_free(BfsHnswGraph *g) {
    if (!g) return;
    for (int L = 0; L < BFS_HNSW_LAYERS; L++) {
        free(g->layers[L]);
        g->layers[L] = NULL;
        g->sizes[L] = 0;
    }
}

static inline uint32_t bfs_hnsw_dist(uint32_t a, uint32_t b) {
    int ra = a / 144, ca = a % 144;
    int rb = b / 144, cb = b % 144;
    int dr = ra - rb, dc = ca - cb;
    return (uint32_t)(dr*dr + dc*dc);
}

/* Greedy search on a single layer (ef=1 upper, or ef>1 for base). */
/* Static buffers for base layer search. */
#define BFS_HNSW_MAX_EF 64
static uint32_t _hnsw_visited[20736];
static uint32_t _hnsw_token = 1;
static uint32_t hnsw_last_visited = 0;

static uint32_t bfs_hnsw_greedy_layer(const BfsHnswGraph *g, int layer,
                                      uint32_t entry, uint32_t query) {
    uint32_t cur = entry;
    uint32_t curd = bfs_hnsw_dist(cur, query);
    int changed = 1;
    while (changed) {
        changed = 0;
        uint32_t idx = (layer == 0) ? cur :
                       (layer == 1) ? cur / 16 : cur / 144;
        if (idx >= g->sizes[layer]) break;
        HnswNode *node = &g->layers[layer][idx];
        for (int e = 0; e < node->count; e++) {
            uint32_t nbr = node->edges[e];
            uint32_t d = bfs_hnsw_dist(nbr, query);
            if (d < curd) {
                cur = nbr;
                curd = d;
                changed = 1;
                break;
            }
        }
    }
    return cur;
}

/* Base-layer search with ef, returns count. out_ids filled with top_k. */
static inline uint32_t bfs_hnsw_search(const BfsHnswGraph *g, uint32_t query_addr,
                                       uint32_t top_k, uint32_t *out_ids) {
    if (!g || !out_ids || top_k == 0) return 0;
    if (top_k > BFS_HNSW_MAX_EF) top_k = BFS_HNSW_MAX_EF;
    uint32_t ef = top_k;
    if (ef < 14) ef = 14;

    /* entry point top layer (center) */
    uint32_t entry = (g->sizes[2] / 2) * 144;
    for (int L = BFS_HNSW_LAYERS - 1; L >= 1; L--) {
        entry = bfs_hnsw_greedy_layer(g, L, entry, query_addr);
    }

    /* base layer search */
    _hnsw_token++;
    uint32_t token = _hnsw_token;

    static uint32_t cand_addr[4096];
    static uint32_t cand_dist[4096];
    static uint8_t  cand_done[4096];
    static uint32_t res_addr[BFS_HNSW_MAX_EF];
    static uint32_t res_dist[BFS_HNSW_MAX_EF];
    int cand_size = 0;
    int res_size = 0;

    /* init with entry */
    cand_addr[cand_size] = entry;
    cand_dist[cand_size] = bfs_hnsw_dist(entry, query_addr);
    cand_done[cand_size] = 0;
    cand_size++;
    _hnsw_visited[entry] = token;
    hnsw_last_visited = 1;

    res_addr[res_size] = entry;
    res_dist[res_size] = cand_dist[0];
    res_size++;

    while (1) {
        int best = -1;
        for (int i = 0; i < cand_size; i++) {
            if (cand_done[i]) continue;
            if (best == -1 || cand_dist[i] < cand_dist[best]) best = i;
        }
        if (best == -1) break;
        cand_done[best] = 1;
        uint32_t cur = cand_addr[best];
        uint32_t curd = cand_dist[best];

        /* pruning: if curd > worst result and results full, stop */
        if (res_size == (int)ef) {
            int worst = 0;
            for (int i = 1; i < res_size; i++)
                if (res_dist[i] > res_dist[worst]) worst = i;
            if (curd > res_dist[worst]) break;
        }

        HnswNode *node = &g->layers[0][cur]; /* layer0 index = addr */
        for (int e = 0; e < node->count; e++) {
            uint32_t nbr = node->edges[e];
            if (_hnsw_visited[nbr] == token) continue;
            _hnsw_visited[nbr] = token;
            hnsw_last_visited++;
            uint32_t d = bfs_hnsw_dist(nbr, query_addr);
            /* add candidate */
            if (cand_size < 4096) {
                cand_addr[cand_size] = nbr;
                cand_dist[cand_size] = d;
                cand_done[cand_size] = 0;
                cand_size++;
            }
            /* add to results if better */
            if (res_size < (int)ef) {
                res_addr[res_size] = nbr;
                res_dist[res_size] = d;
                res_size++;
            } else {
                int worst = 0;
                for (int i = 1; i < res_size; i++)
                    if (res_dist[i] > res_dist[worst]) worst = i;
                if (d < res_dist[worst]) {
                    res_addr[worst] = nbr;
                    res_dist[worst] = d;
                }
            }
        }
    }

    /* local refinement: 4-neighbors of current results */
    for (int i = 0; i < res_size; i++) {
        uint32_t a = res_addr[i];
        int r = a / 144, c = a % 144;
        int drs[4] = {-1,1,0,0};
        int dcs[4] = {0,0,-1,1};
        for (int d = 0; d < 4; d++) {
            int nr = r + drs[d], nc = c + dcs[d];
            if (nr < 0 || nr >= 144 || nc < 0 || nc >= 144) continue;
            uint32_t nbr = (uint32_t)(nr * 144 + nc);
            if (_hnsw_visited[nbr] == token) continue;
            _hnsw_visited[nbr] = token;
            hnsw_last_visited++;
            uint32_t dd = bfs_hnsw_dist(nbr, query_addr);
            if (res_size < (int)ef) {
                res_addr[res_size] = nbr;
                res_dist[res_size] = dd;
                res_size++;
            } else {
                int worst = 0;
                for (int t = 1; t < res_size; t++)
                    if (res_dist[t] > res_dist[worst]) worst = t;
                if (dd < res_dist[worst]) {
                    res_addr[worst] = nbr;
                    res_dist[worst] = dd;
                }
            }
        }
    }

    /* sort results by dist asc */
    for (int a = 1; a < res_size; a++) {
        uint32_t da = res_dist[a], aa = res_addr[a];
        int b = a - 1;
        while (b >= 0 && res_dist[b] > da) {
            res_dist[b+1] = res_dist[b];
            res_addr[b+1] = res_addr[b];
            b--;
        }
        res_dist[b+1] = da;
        res_addr[b+1] = aa;
    }

    uint32_t cnt = (top_k < (uint32_t)res_size) ? top_k : (uint32_t)res_size;
    for (uint32_t i = 0; i < cnt; i++) out_ids[i] = res_addr[i];
    return cnt;
}

#endif /* BFS_HNSW_H */