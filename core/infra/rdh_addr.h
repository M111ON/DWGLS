#pragma once
#include <stdint.h>

typedef struct {
    int64_t n_rings;
    int64_t n_wedges;
    int64_t n_mirror;
    int64_t max_u;
    int64_t n_v;
} RDHConfig;

#define RDH_TIER0    { 128, 162, 1, 1, 1 }

static inline int64_t rdh_key(
    const RDHConfig *cfg,
    int64_t ring, int64_t wedge, int64_t mirror,
    int64_t u, int64_t v)
{
    (void)v;
    return ((ring * cfg->n_wedges + wedge) * cfg->n_mirror + mirror)
           * cfg->max_u + u;
}

static inline void rdh_decompose(
    const RDHConfig *cfg, int64_t key,
    int64_t *ring, int64_t *wedge, int64_t *mirror, int64_t *u)
{
    int64_t t = key;
    *u = t % cfg->max_u;
    t /= cfg->max_u;
    *mirror = t % cfg->n_mirror;
    t /= cfg->n_mirror;
    *wedge = t % cfg->n_wedges;
    t /= cfg->n_wedges;
    *ring = t;
}

static inline int64_t rdh_capacity(const RDHConfig *cfg) {
    return cfg->n_rings * cfg->n_wedges * cfg->n_mirror * cfg->max_u * cfg->n_v;
}

static inline int rdh_valid(
    const RDHConfig *cfg,
    int64_t ring, int64_t wedge, int64_t mirror,
    int64_t u, int64_t v)
{
    (void)v;
    return ring   >= 0 && ring   < cfg->n_rings
        && wedge  >= 0 && wedge  < cfg->n_wedges
        && mirror >= 0 && mirror < cfg->n_mirror
        && u      >= 0 && u      < cfg->max_u;
}

static inline int64_t rdh_rotate(const RDHConfig *cfg, int64_t key, int64_t r_wedge) {
    int64_t r, w, m, u;
    rdh_decompose(cfg, key, &r, &w, &m, &u);
    w = (w + r_wedge) % cfg->n_wedges;
    return rdh_key(cfg, r, w, m, u, 0);
}
