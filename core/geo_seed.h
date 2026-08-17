/*
 * geo_seed.h — CPU port of geo_kernel_seed_v2.cu
 *
 * Pure integer seed-derivation engine via 12-coset dodeca topology.
 * Zero float, zero heap, zero CUDA — O(1) per seed.
 *
 * Use as pre-filter: 12 coset checksums encode topology-aware entropy
 * signature for each seed position.
 *
 * Ported from: ZGLS/phase5/geo_kernel_seed_v2.cu
 *
 * Compile test:
 *   gcc -O2 -o test_geo_seed test_geo_seed.c -lm
 */

#ifndef GEO_SEED_H
#define GEO_SEED_H

#include <stdint.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────── */
#define GS_COSET_COUNT   12u
#define GS_FRUSTUM_MOUNT  6u
#define GS_PHI_SCALE     (1u << 20)
#define GS_PHI_UP         1696631u
#define GS_PHI_DOWN        648055u

/* ── ApexSeed: 12B nominally, naturally padded to 16B ── */
typedef struct {
    uint64_t seed;
    uint32_t dispatch_id;
} GsSeed;

/* ── ApexResult: 64B output per seed ─────────────────────── */
typedef struct {
    uint32_t coset_checksum[GS_COSET_COUNT];
    uint32_t master_fold;
    uint32_t verify_ok;
    uint32_t dispatch_id;
    uint32_t _pad;
} GsResult;

/* ── Core primitives (exact match to GPU kernel) ─────────── */
static inline uint64_t gs_mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}

static inline uint64_t gs_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t gs_derive(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t salt = ((uint64_t)face << 56) ^ (uint64_t)step;
    uint64_t a    = gs_mix64(core ^ salt);
    uint64_t b    = gs_rotl64(core, (int)((face + step) & 63u));
    return gs_mix64(a ^ b);
}

/* ── One coset checksum (12 directions per seed) ─────────── */
static inline uint32_t gs_coset_checksum(uint64_t seed, uint8_t coset)
{
    uint64_t cseed     = gs_derive(seed, coset, 0u);
    uint64_t acc       = cseed;
    uint32_t chk       = (uint32_t)(cseed ^ (cseed >> 32));

    for (uint8_t f = 0; f < GS_FRUSTUM_MOUNT; f++) {
        uint64_t cur = gs_derive(acc, f, (uint32_t)coset + 1u);
        for (uint8_t lv = 0; lv < 4u; lv++) {
            chk ^= (uint32_t)(cur ^ (cur >> 32));
            cur  = gs_derive(cur, f, (uint32_t)(lv + 1u));
        }
        acc = cur;
    }
    return chk;
}

/* ── Process one seed → one result ──────────────────────── */
static inline void gs_process(const GsSeed *seed, GsResult *result)
{
    uint32_t master = 0u;
    for (uint8_t c = 0; c < GS_COSET_COUNT; c++) {
        uint32_t chk = gs_coset_checksum(seed->seed, c);
        result->coset_checksum[c] = chk;
        master ^= chk;
    }
    result->master_fold  = master;
    result->verify_ok    = (master != 0u) ? 1u : 0u;
    result->dispatch_id  = seed->dispatch_id;
    result->_pad         = 0u;
}

/* ── Batch: N seeds → N results ─────────────────────────── */
static inline void gs_batch(const GsSeed *seeds, GsResult *results,
                            uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        gs_process(&seeds[i], &results[i]);
}

#endif /* GEO_SEED_H */
