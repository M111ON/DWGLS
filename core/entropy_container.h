/*
 * entropy_container.h — RDH Entropy Container
 * ═══════════════════════════════════════════════════════════════════
 * "Data's own path IS the address — no hash, no compression, no metadata."
 *
 * Flow:
 *   data → rdh_capture (stride walk) → flat_key
 *          → enc = flat_key % 1440 (2-byte reference)
 *          → frame_at(enc) → face/slot/phase/ico
 *          → store at ring, wedge (RDH home position)
 *
 * The walk pattern = unique DNA of data
 * The stop position = home address
 * The address IS the identity (no separate metadata)
 *
 * Field: 144 × 144 = 20736 slots, each 48B
 * Store: ec_store() → rdh_capture → address → container[ring][wedge]
 * Load:  ec_load_by_enc() → enc → address → data
 * Query: ec_load_by_addr() → ring, wedge → data
 *
 * No malloc in hot path. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef ENTROPY_CONTAINER_H
#define ENTROPY_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "rdh_capture.h"
#include "geo_frame_seek.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define EC_FIELD_W         144u
#define EC_FIELD_H         144u
#define EC_SLOTS           (EC_FIELD_W * EC_FIELD_H)  /* 20736 */
#define EC_BLOCK_SZ        48u
#define EC_EMPTY           0u
#define EC_OCCUPIED        1u

/* ══════════════════════════════════════════════════════════════
   SLOT — stores data + its RDH address
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  data[EC_BLOCK_SZ];   /* original data (lossless)           */
    uint64_t flat_key;            /* full RDH address (8B, unique)      */
    uint16_t enc;                 /* flat_key % 1440 (2B, for fibo)     */
    uint8_t  flags;               /* EC_EMPTY / EC_OCCUPIED             */
    uint8_t  entropy_class;       /* 0=structured, 1=moderate, 2=high, 3=random */
} ECSlot;                         /* total: 48 + 8 + 2 + 1 + 1 = 60B   */

/* ══════════════════════════════════════════════════════════════
   CONTAINER — 144×144 field with RDH addressing
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    ECSlot   slots[EC_FIELD_H][EC_FIELD_W];
    uint32_t occupied;           /* total occupied slots                */
    uint32_t overwrites;         /* collision count                     */
    uint32_t total_stored;       /* total store operations              */
    uint32_t total_loaded;       /* total load operations               */
} EntropyContainer;

/* ══════════════════════════════════════════════════════════════
   INIT
   ══════════════════════════════════════════════════════════════ */

static inline void ec_init(EntropyContainer *ec) {
    memset(ec, 0, sizeof(*ec));
}

/* ══════════════════════════════════════════════════════════════
   ADDRESS MAPPING — RDH point → container position
   ══════════════════════════════════════════════════════════════ */

/* flat_key → (ring, wedge) on 144×144 field */
static inline void ec_key_to_addr(uint64_t flat_key,
                                   uint16_t *ring, uint16_t *wedge)
{
    uint64_t idx = flat_key % EC_SLOTS;
    if (ring)  *ring  = (uint16_t)(idx / EC_FIELD_W);
    if (wedge) *wedge = (uint16_t)(idx % EC_FIELD_W);
}

/* enc (0..1439) → (ring, wedge) via stride-37 scatter */
static inline void ec_enc_to_field(uint16_t enc,
                                    uint16_t *ring, uint16_t *wedge)
{
    if (ring)  *ring  = (uint16_t)(enc / 12);
    if (wedge) *wedge = (uint16_t)((enc * FRAME_STRIDE) % EC_FIELD_W);
}

/* ══════════════════════════════════════════════════════════════
   STORE — data → RDH address → container
   ══════════════════════════════════════════════════════════════
   1. rdh_capture → flat_key (data walks to its home)
   2. flat_key → ring, wedge (the home position)
   3. Store data + enc at slots[ring][wedge]
   
   Collision: overwrite (last write wins)
   ══════════════════════════════════════════════════════════════ */

static inline int ec_store(EntropyContainer *ec,
                            const uint8_t *data, size_t len,
                            const RDHConfig *cfg)
{
    if (!ec || !data || len == 0 || len > EC_BLOCK_SZ) return -2;

    /* 1. Data walks to its home */
    uint64_t fk = rdh_capture(data, len, cfg);
    uint16_t enc = (uint16_t)(fk % FRAME_CYCLE);

    /* 2. Home position from flat_key (20736 unique positions) */
    uint16_t r, w;
    ec_key_to_addr(fk, &r, &w);

    /* 3. Plant address + store data */
    ECSlot *slot = &ec->slots[r][w];
    if (slot->flags == EC_EMPTY) ec->occupied++;
    else ec->overwrites++;

    memcpy(slot->data, data, len);
    if (len < EC_BLOCK_SZ) memset(slot->data + len, 0, EC_BLOCK_SZ - len);
    slot->enc = enc;
    slot->flat_key = fk;
    slot->flags = EC_OCCUPIED;
    ec->total_stored++;

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   LOAD — by flat_key (direct O(1) access)
   ══════════════════════════════════════════════════════════════ */

static inline int ec_load_by_flat_key(const EntropyContainer *ec,
                                       uint64_t flat_key, uint8_t *out,
                                       size_t max_len)
{
    if (!ec || !out || max_len == 0) return -1;

    uint16_t r, w;
    ec_key_to_addr(flat_key, &r, &w);

    const ECSlot *slot = &ec->slots[r][w];
    if (slot->flags == EC_EMPTY) return 0;

    size_t n = max_len < EC_BLOCK_SZ ? max_len : EC_BLOCK_SZ;
    memcpy(out, slot->data, n);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════════
   LOAD — by enc (search, since enc alone can't determine slot)
   ══════════════════════════════════════════════════════════════ */

static inline int ec_load_by_enc(const EntropyContainer *ec,
                                  uint16_t enc, uint8_t *out,
                                  size_t max_len)
{
    if (!ec || !out || max_len == 0) return -1;

    /* enc → ring, wedge (stride-37 scatter) */
    uint16_t r, w;
    ec_enc_to_field(enc, &r, &w);

    const ECSlot *slot = &ec->slots[r][w];
    if (slot->flags == EC_EMPTY) return 0;
    /* Check if this slot's enc matches */
    if (slot->enc != enc) return 0;

    size_t n = max_len < EC_BLOCK_SZ ? max_len : EC_BLOCK_SZ;
    memcpy(out, slot->data, n);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════════
   LOAD — by address (ring, wedge)
   ══════════════════════════════════════════════════════════════ */

static inline int ec_load_by_addr(const EntropyContainer *ec,
                                   uint16_t r, uint16_t w,
                                   uint8_t *out, size_t max_len)
{
    if (!ec || !out || r >= EC_FIELD_H || w >= EC_FIELD_W) return -1;

    const ECSlot *slot = &ec->slots[r][w];
    if (slot->flags == EC_EMPTY) return 0;

    size_t n = max_len < EC_BLOCK_SZ ? max_len : EC_BLOCK_SZ;
    memcpy(out, slot->data, n);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════════
   QUERY — existence check
   ══════════════════════════════════════════════════════════════ */

static inline int ec_has(const EntropyContainer *ec, uint16_t enc) {
    uint16_t r, w;
    ec_enc_to_field(enc, &r, &w);
    return ec->slots[r][w].flags != EC_EMPTY;
}

static inline uint16_t ec_get_enc(const EntropyContainer *ec,
                                   uint16_t r, uint16_t w)
{
    if (r >= EC_FIELD_H || w >= EC_FIELD_W) return 0;
    return ec->slots[r][w].enc;
}

/* ══════════════════════════════════════════════════════════════
   ITERATOR — stride-37 walk over 1440 enc positions
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    const EntropyContainer *ec;
    uint16_t  enc;
    uint32_t  steps;
    uint16_t  ring, wedge;
    int       valid;
} ECIter;

static inline void ec_iter_init(ECIter *it, const EntropyContainer *ec) {
    it->ec = ec;
    it->enc = 0;
    it->steps = 0;
    ec_enc_to_field(0, &it->ring, &it->wedge);
    it->valid = ec->slots[it->ring][it->wedge].flags != EC_EMPTY;
}

static inline int ec_iter_next(ECIter *it) {
    if (!it || it->steps >= FRAME_CYCLE) return 0;
    it->enc = frame_next(it->enc);
    it->steps++;
    ec_enc_to_field(it->enc, &it->ring, &it->wedge);
    it->valid = it->ec->slots[it->ring][it->wedge].flags != EC_EMPTY;
    return 1;
}

/* ══════════════════════════════════════════════════════════════
   STATISTICS
   ══════════════════════════════════════════════════════════════ */

static inline void ec_stats(const EntropyContainer *ec,
                             uint32_t *occupied, uint32_t *overwrites,
                             uint32_t *stored, uint32_t *loaded)
{
    if (occupied)  *occupied  = ec->occupied;
    if (overwrites)*overwrites = ec->overwrites;
    if (stored)    *stored    = ec->total_stored;
    if (loaded)    *loaded    = ec->total_loaded;
}

#endif /* ENTROPY_CONTAINER_H */
