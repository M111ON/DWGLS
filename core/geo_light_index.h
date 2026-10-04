/* geo_light_index.h — light index for the ANN storage memory layer.
 *
 * One fixed record per indexed block. RAM holds ONLY these records;
 * payload bytes live in storage and are faulted on demand (ctx_plane
 * pattern: index = addresses, content on demand). Step 2 (light KV)
 * swaps the payload source from test fixtures to real KVCB/KVD bytes —
 * wiring only, no logic change — because this header never touches
 * payload bytes, only (offset, size) coordinates.
 *
 * Wire record (40 bytes, CRC-32):
 *   name_hash   u64  : FNV-1a 64 of the block name (stable key, never slot)
 *   field_slot  u32  : GJ-place, stride-37 slot in [0, 20736)
 *   hj_cluster  u32  : HJ-orbit key, hj3_jump(slot % HJ_TOTAL)
 *   store_off   u64  : byte offset of payload in the storage file
 *   store_size  u32  : payload bytes
 *   access      u32  : observe count (P4 feedback, HOT at IFB_HOT_THRESHOLD)
 *   flags       u32  : bit0 HOT; rest reserved
 *   crc32       u32  : over the preceding 36 bytes
 *
 * Separability (P4 rule): no walk_round, no bytes_hash here. Feedback
 * counters are reconstructable by replay; baseline contract untouched.
 *
 * Header-only, stdint/stdio/string only. No build step.
 */
#ifndef GEO_LIGHT_INDEX_H
#define GEO_LIGHT_INDEX_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "geo_hyper_jump.h"

#define LIX_TOTAL_SLOTS  20736u
#define LIX_STRIDE       37u
#define LIX_HOT_THRESHOLD 8u
#define LIX_FLAG_HOT     0x01u

#define LIX_REC_WIRE     40u
#define LIX_REC_BODY     36u

typedef struct {
    uint64_t name_hash;
    uint32_t field_slot;
    uint32_t hj_cluster;
    uint64_t store_off;
    uint32_t store_size;
    uint32_t access;
    uint32_t flags;
} LIXEntry;                        /* in-RAM (may carry padding; wire is exact) */

/* FNV-1a 64 (repo convention: anchor_route.h). */
static inline uint64_t lix_hash(const void *data, uint32_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static inline uint32_t lix_crc32(const uint8_t *b, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static inline void lix_put_u64(uint8_t *b, uint64_t v) { memcpy(b, &v, 8); }
static inline void lix_put_u32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static inline uint64_t lix_get_u64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return v; }
static inline uint32_t lix_get_u32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

/* Derive the GJ-place slot + HJ-orbit cluster from a cell index. */
static inline uint32_t lix_slot_of(uint32_t cell) {
    return (uint32_t)(((uint64_t)cell * LIX_STRIDE) % LIX_TOTAL_SLOTS);
}
static inline uint32_t lix_cluster_of(uint32_t slot) {
    return hj3_jump(slot % HJ_TOTAL);
}

/* Pack one entry to 40 wire bytes. Returns LIX_REC_WIRE. */
static inline uint32_t lix_pack(const LIXEntry *e, uint8_t out[LIX_REC_WIRE]) {
    lix_put_u64(out + 0,  e->name_hash);
    lix_put_u32(out + 8,  e->field_slot);
    lix_put_u32(out + 12, e->hj_cluster);
    lix_put_u64(out + 16, e->store_off);
    lix_put_u32(out + 24, e->store_size);
    lix_put_u32(out + 28, e->access);
    lix_put_u32(out + 32, e->flags);
    lix_put_u32(out + 36, lix_crc32(out, LIX_REC_BODY));
    return LIX_REC_WIRE;
}

/* Unpack + verify CRC. Returns 0 ok, -1 bad CRC. */
static inline int lix_unpack(const uint8_t in[LIX_REC_WIRE], LIXEntry *e) {
    if (lix_get_u32(in + 36) != lix_crc32(in, LIX_REC_BODY)) return -1;
    e->name_hash  = lix_get_u64(in + 0);
    e->field_slot = lix_get_u32(in + 8);
    e->hj_cluster = lix_get_u32(in + 12);
    e->store_off  = lix_get_u64(in + 16);
    e->store_size = lix_get_u32(in + 24);
    e->access     = lix_get_u32(in + 28);
    e->flags      = lix_get_u32(in + 32);
    return 0;
}

/* Append one packed record. Returns 0 ok, -1 full. */
static inline int lix_log_append(uint8_t *buf, uint32_t cap, uint32_t *n,
                                 const LIXEntry *e) {
    if ((*n + 1u) * LIX_REC_WIRE > cap) return -1;
    lix_pack(e, buf + (size_t)(*n) * LIX_REC_WIRE);
    (*n)++;
    return 0;
}

/* Replay a log into entries[]. Last-write-wins per name_hash (linear scan;
 * index sizes are hundreds-to-thousands, matching IFB table scope).
 * Returns entry count, or -1 on first bad CRC (fail loud, P3 rule). */
static inline int lix_log_replay(const uint8_t *buf, uint32_t n,
                                 LIXEntry *out, uint32_t out_cap) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        LIXEntry e;
        if (lix_unpack(buf + (size_t)i * LIX_REC_WIRE, &e) != 0) return -1;
        if (e.name_hash == 0) continue;          /* empty sentinel */
        uint32_t at = m;                          /* find existing */
        for (uint32_t j = 0; j < m; j++)
            if (out[j].name_hash == e.name_hash) { at = j; break; }
        if (at == m) {
            if (m >= out_cap) return -1;
            m++;
        }
        out[at] = e;                              /* last wins */
    }
    return (int)m;
}

/* Observe one access. Sets HOT at threshold. No-op on hash 0. */
static inline void lix_observe(LIXEntry *e) {
    if (!e || e->name_hash == 0) return;
    e->access++;
    if (e->access >= LIX_HOT_THRESHOLD) e->flags |= LIX_FLAG_HOT;
}

static inline void lix_note_drift(LIXEntry *e) {
    if (!e || e->name_hash == 0) return;
    e->access++;                                  /* drift counts as activity */
}

#endif /* GEO_LIGHT_INDEX_H */
