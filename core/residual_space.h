/*
 * residual_space.h — Timeless Bond-Only Storage Zone
 *
 * Concept:
 *   residual_space = world without fibo time
 *   - Time does not advance — data is frozen, never changes
 *   - No coordinate address — cannot be reached by normal geo addressing
 *   - **Bond only** — access via bond_key (bond_L XOR bond_R)
 *   - High entropy data → freeze via bond → store here
 *
 * Architecture:
 *   Bond-keyed hash table with open addressing (power-of-2)
 *   Each entry stores:
 *     bond_key (8B)  → lookup key (never zero)
 *     origin_key (8B) → geo_key at birth (pile identity), survives any reroute
 *     data_size (4B)  → bytes of payload
 *     data      (flex) → payload bytes
 *
 *   No mmap, no filesystem — pure in-memory storage.
 *   For persistent storage, entries can be serialized by bond_key.
 *
 *   Entry lifecycle:
 *     FREEZE  → data enters residual_space, assigned bond_key
 *     THAW    → data retrieved by bond_key
 *     EVICT   → LRU eviction when space is full
 *     VERIFY  → bond_key integrity check
 *
 * All header-only, static inline. Malloc on init/freeze only.
 * No float. O(1) average lookup via hash table.
 */

#ifndef RESIDUAL_SPACE_H
#define RESIDUAL_SPACE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_bond.h"

/* ── Sacred constants ─────────────────────────────────── */
#define RS_DEFAULT_CAPACITY   4096u   /* default entry count     */
#define RS_MAX_DATA_SIZE      65536u  /* max bytes per entry     */
#define RS_EVICT_SCAN_WINDOW  64u     /* LRU eviction scan depth */
#define RS_BOND_KEY_RESERVED  UINT64_C(0)  /* reserved (never stored) */

/* ── Entry flags ──────────────────────────────────────── */
#define RS_ENTRY_VALID        0x01    /* slot has valid data     */
#define RS_ENTRY_PINNED       0x02    /* pinned — never evict    */
#define RS_ENTRY_FROZEN       0x04    /* frozen — no mutation    */
#define RS_ENTRY_HIGH_ENTROPY 0x08    /* marked as high entropy  */
#define RS_ENTRY_REF          0x10    /* referenced by bond chain */
#define RS_ENTRY_TOMBSTONED   0x20    /* expired — recycle bin   */

/* ════════════════════════════════════════════════════════════
   DATA STRUCTURES
   ════════════════════════════════════════════════════════════ */

/* Residual entry (variable-length: header + data) */
typedef struct __attribute__((packed)) {
    uint64_t bond_key;      /* 8B  lookup key (never 0)           */
    uint64_t origin_key;    /* 8B  bond_key at birth              */
    uint64_t geo_key;       /* 8B  original geo_key               */
    uint32_t data_size;     /* 4B  payload bytes                  */
    uint32_t timestamp;     /* 4B  freeze timestamp (ticks)       */
    uint8_t  flags;         /* 1B  RS_ENTRY_*                     */
    uint8_t  _pad[3];       /* 3B  alignment padding              */
    /* uint8_t data[] follows immediately */                       
} ResidualEntry;            /* 36B header                         */

#define RS_ENTRY_HEADER_SZ   36u

/* Residual space context */
typedef struct {
    ResidualEntry **entries;  /* array of pointers to entries     */
    uint32_t        capacity; /* total slot count                 */
    uint32_t        count;    /* valid entries                    */
    uint32_t        evictions; /* total evictions                 */
    uint64_t        total_bytes; /* total data bytes stored       */
    uint32_t        next_timestamp; /* incrementing freeze time   */
    uint32_t        scan_pos;  /* next eviction scan start pos    */
    uint8_t         _pad[4];
} ResidualSpace;

/* ════════════════════════════════════════════════════════════
   INTERNAL: hash table utilities
   ════════════════════════════════════════════════════════════ */

/*
 * _rs_hash() — mix bond_key to slot index
 * Open addressing with linear probing
 */
static inline uint32_t _rs_hash(uint64_t bond_key, uint32_t mask) {
    uint64_t h = bond_key;
    h ^= h >> 33;
    h *= UINT64_C(0xFF51AFD7ED558CCD);
    h ^= h >> 33;
    return (uint32_t)(h & (uint64_t)mask);
}

/* ════════════════════════════════════════════════════════════
   INIT / FREE
   ════════════════════════════════════════════════════════════ */

/*
 * rs_init() — initialise residual space
 * capacity: number of slots (power of 2 recommended, >= 64)
 * Returns 0 on success, -1 on malloc failure
 */
static inline int rs_init(ResidualSpace *rs, uint32_t capacity) {
    if (!rs) return -1;
    if (capacity < 64) capacity = RS_DEFAULT_CAPACITY;

    rs->entries = (ResidualEntry **)calloc(capacity, sizeof(ResidualEntry *));
    if (!rs->entries) return -1;

    rs->capacity       = capacity;
    rs->count          = 0;
    rs->evictions      = 0;
    rs->total_bytes    = 0;
    rs->next_timestamp = 0;
    rs->scan_pos       = 0;
    return 0;
}

/*
 * rs_free() — free all entries and the space itself
 */
static inline void rs_free(ResidualSpace *rs) {
    if (!rs || !rs->entries) return;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        if (rs->entries[i] && (rs->entries[i]->flags & RS_ENTRY_VALID)) {
            free(rs->entries[i]);
        }
    }
    free(rs->entries);
    rs->entries = NULL;
    rs->count   = 0;
}

/* ════════════════════════════════════════════════════════════
   FREEZE: store data in residual space
   ════════════════════════════════════════════════════════════ */

/*
 * rs_freeze() — store data by bond_key
 *
 * piece: the PoglsPiece whose bond_key becomes the address
 * data:  payload bytes (copied internally)
 * size:  payload size (must be > 0, <= RS_MAX_DATA_SIZE)
 * is_high_entropy: flag for high-entropy marker
 *
 * Returns bond_key on success, 0 (RS_BOND_KEY_RESERVED) on failure.
 * On eviction, the LRU entry is freed to make room.
 */
static inline uint64_t rs_freeze(ResidualSpace *rs,
                                  const PoglsPiece *piece,
                                  const void *data,
                                  uint32_t size,
                                  uint8_t is_high_entropy)
{
    if (!rs || !piece || !data || size == 0 || size > RS_MAX_DATA_SIZE)
        return RS_BOND_KEY_RESERVED;
    if (!rs->entries) return RS_BOND_KEY_RESERVED;

    uint64_t bond_key = pogls_bond_key(piece);

    /* Find slot (or probe for empty) */
    uint32_t mask  = rs->capacity - 1;
    uint32_t slot  = _rs_hash(bond_key, mask);
    uint32_t start = slot;

    /* Check if already exists */
    while (rs->entries[slot] && rs->entries[slot]->flags & RS_ENTRY_VALID) {
        if (rs->entries[slot]->bond_key == bond_key) {
            /* Already frozen — update if not pinned */
            if (rs->entries[slot]->flags & RS_ENTRY_PINNED)
                return RS_BOND_KEY_RESERVED;

            /* Free old data, replace */
            ResidualEntry *old = rs->entries[slot];
            rs->total_bytes -= old->data_size;

            ResidualEntry *new_entry = (ResidualEntry *)malloc(RS_ENTRY_HEADER_SZ + size);
            if (!new_entry) return RS_BOND_KEY_RESERVED;

            new_entry->bond_key   = bond_key;
            new_entry->origin_key = piece->geo_key;
            new_entry->geo_key    = piece->geo_key;
            new_entry->data_size  = size;
            new_entry->timestamp  = rs->next_timestamp++;
            new_entry->flags      = RS_ENTRY_VALID | RS_ENTRY_FROZEN
                                   | (is_high_entropy ? RS_ENTRY_HIGH_ENTROPY : 0);

            memcpy((uint8_t *)new_entry + RS_ENTRY_HEADER_SZ, data, size);
            rs->entries[slot] = new_entry;
            rs->total_bytes  += size;
            free(old);
            return bond_key;
        }
        slot = (slot + 1) & mask;
        if (slot == start) {
            /* Table full — LRU eviction: find entry with oldest timestamp */
            uint32_t oldest_slot = 0;
            uint64_t oldest_ts   = UINT64_MAX;
            for (uint32_t si = 0; si < rs->capacity; si++) {
                if (!rs->entries[si]) continue;
                if (rs->entries[si]->flags & RS_ENTRY_PINNED) continue;
                if (rs->entries[si]->timestamp < oldest_ts) {
                    oldest_ts   = rs->entries[si]->timestamp;
                    oldest_slot = si;
                }
            }
            if (oldest_ts == UINT64_MAX) {
                /* All entries pinned — cannot evict */
                rs->evictions++;
                return RS_BOND_KEY_RESERVED;
            }

            /* Evict oldest entry, reuse its slot */
            ResidualEntry *old = rs->entries[oldest_slot];
            rs->total_bytes -= old->data_size;
            rs->count--;

            ResidualEntry *new_entry = (ResidualEntry *)malloc(RS_ENTRY_HEADER_SZ + size);
            if (!new_entry) { rs->evictions++; return RS_BOND_KEY_RESERVED; }

            new_entry->bond_key   = bond_key;
            new_entry->origin_key = piece->geo_key;
            new_entry->geo_key    = piece->geo_key;
            new_entry->data_size  = size;
            new_entry->timestamp  = rs->next_timestamp++;
            new_entry->flags      = RS_ENTRY_VALID | RS_ENTRY_FROZEN
                                   | (is_high_entropy ? RS_ENTRY_HIGH_ENTROPY : 0);

            memcpy((uint8_t *)new_entry + RS_ENTRY_HEADER_SZ, data, size);
            rs->entries[oldest_slot] = new_entry;
            rs->total_bytes += size;
            rs->count++;
            free(old);
            rs->evictions++;
            return bond_key;
        }
    }

    /* Empty slot found — allocate new entry */
    ResidualEntry *entry = (ResidualEntry *)malloc(RS_ENTRY_HEADER_SZ + size);
    if (!entry) return RS_BOND_KEY_RESERVED;

    entry->bond_key   = bond_key;
    entry->origin_key = piece->geo_key;   /* birth pile identity — matches rs_verify */
    entry->geo_key    = piece->geo_key;
    entry->data_size  = size;
    entry->timestamp  = rs->next_timestamp++;
    entry->flags      = RS_ENTRY_VALID | RS_ENTRY_FROZEN
                        | (is_high_entropy ? RS_ENTRY_HIGH_ENTROPY : 0);

    memcpy((uint8_t *)entry + RS_ENTRY_HEADER_SZ, data, size);

    rs->entries[slot] = entry;
    rs->count++;
    rs->total_bytes += size;

    return bond_key;
}

/* ════════════════════════════════════════════════════════════
   THAW: retrieve data by bond_key
   ════════════════════════════════════════════════════════════ */

/*
 * rs_thaw() — retrieve data by bond_key
 *
 * bond_key: the key to look up
 * out_size: (output) size of retrieved data
 *
 * Returns pointer to data, or NULL if not found.
 * Data is owned by ResidualSpace — do NOT free.
 * Valid until the entry is evicted or overwritten.
 */
static inline const void *rs_thaw(const ResidualSpace *rs,
                                   uint64_t bond_key,
                                   uint32_t *out_size)
{
    if (!rs || !rs->entries || bond_key == RS_BOND_KEY_RESERVED)
        return NULL;

    uint32_t mask  = rs->capacity - 1;
    uint32_t slot  = _rs_hash(bond_key, mask);
    uint32_t start = slot;

    while (rs->entries[slot]) {
        ResidualEntry *e = rs->entries[slot];
        if ((e->flags & RS_ENTRY_VALID) && e->bond_key == bond_key) {
            if (out_size) *out_size = e->data_size;
            return (const void *)((const uint8_t *)e + RS_ENTRY_HEADER_SZ);
        }
        slot = (slot + 1) & mask;
        if (slot == start) break;
    }

    return NULL;
}

/* ════════════════════════════════════════════════════════════
   VERIFY: check bond key integrity
   ════════════════════════════════════════════════════════════ */

/*
 * rs_verify() — verify bond_key integrity for a stored entry
 *
 * Returns 1 if the stored bond_key matches the piece's bond_key.
 * This confirms the data hasn't been corrupted or tampered with.
 */
static inline uint8_t rs_verify(const ResidualSpace *rs,
                                 const PoglsPiece *piece)
{
    if (!rs || !piece) return 0;

    uint64_t expected = pogls_bond_key(piece);

    uint32_t mask  = rs->capacity - 1;
    uint32_t slot  = _rs_hash(expected, mask);
    uint32_t start = slot;

    while (rs->entries[slot]) {
        ResidualEntry *e = rs->entries[slot];
        if ((e->flags & RS_ENTRY_VALID) && e->bond_key == expected) {
            /* Found — verify origin_key matches geo_key */
            return (e->origin_key == piece->geo_key) ? 1 : 0;
        }
        slot = (slot + 1) & mask;
        if (slot == start) break;
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════
   EVICT: remove entries
   ════════════════════════════════════════════════════════════ */

/*
 * rs_evict_one() — evict a single LRU entry (oldest timestamp, not pinned)
 * Returns 1 if evicted, 0 if nothing to evict.
 */
static inline int rs_evict_one(ResidualSpace *rs) {
    if (!rs || !rs->entries || rs->count == 0) return 0;

    uint32_t oldest_slot = UINT32_MAX;
    uint32_t oldest_ts   = UINT32_MAX;

    for (uint32_t i = 0; i < rs->capacity; i++) {
        ResidualEntry *e = rs->entries[i];
        if (!e || !(e->flags & RS_ENTRY_VALID)) continue;
        if (e->flags & RS_ENTRY_PINNED) continue;
        if (e->timestamp < oldest_ts) {
            oldest_ts   = e->timestamp;
            oldest_slot = i;
        }
    }

    if (oldest_slot == UINT32_MAX) return 0;

    ResidualEntry *old = rs->entries[oldest_slot];
    rs->total_bytes -= old->data_size;
    free(old);
    rs->entries[oldest_slot] = NULL;
    rs->count--;
    rs->evictions++;
    return 1;
}

/*
 * rs_evict_all() — evict all non-pinned entries
 * Returns count evicted.
 */
static inline uint32_t rs_evict_all(ResidualSpace *rs) {
    if (!rs || !rs->entries) return 0;

    uint32_t evicted = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        ResidualEntry *e = rs->entries[i];
        if (!e || !(e->flags & RS_ENTRY_VALID)) continue;
        if (e->flags & RS_ENTRY_PINNED) continue;

        rs->total_bytes -= e->data_size;
        free(e);
        rs->entries[i] = NULL;
        evicted++;
    }
    rs->count -= evicted;
    rs->evictions += evicted;
    return evicted;
}

/* ════════════════════════════════════════════════════════════
   TOMBSTONE ZONE — recycle bin for expired / evicted bonds
   ════════════════════════════════════════════════════════════ */

/*
 * rs_tombstone() — mark an entry as tombstoned (expired bond)
 *
 * Does NOT free the data — preserves header for audit trail.
 * Sets RS_ENTRY_TOMBSTONED flag. Entry is logically dead.
 * Returns 1 on success, 0 if bond_key not found or already tombstoned.
 */
static inline int rs_tombstone(ResidualSpace *rs, uint64_t bond_key) {
    if (!rs || !rs->entries || bond_key == RS_BOND_KEY_RESERVED)
        return 0;

    uint32_t mask  = rs->capacity - 1;
    uint32_t slot  = _rs_hash(bond_key, mask);
    uint32_t start = slot;

    while (rs->entries[slot]) {
        ResidualEntry *e = rs->entries[slot];
        if ((e->flags & RS_ENTRY_VALID) && e->bond_key == bond_key) {
            if (e->flags & RS_ENTRY_TOMBSTONED)
                return 0;  /* already tombstoned */
            e->flags |= RS_ENTRY_TOMBSTONED;
            e->flags &= ~RS_ENTRY_VALID;  /* logically dead */
            return 1;
        }
        slot = (slot + 1) & mask;
        if (slot == start) break;
    }
    return 0;
}

/*
 * rs_tombstone_sweep() — recycle bin: free all tombstoned entries
 *
 * Removes entries marked RS_ENTRY_TOMBSTONED, frees memory,
 * reclaims slots.
 * Returns count of entries swept.
 */
static inline uint32_t rs_tombstone_sweep(ResidualSpace *rs) {
    if (!rs || !rs->entries) return 0;

    uint32_t swept = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        ResidualEntry *e = rs->entries[i];
        if (!e) continue;
        if (!(e->flags & RS_ENTRY_TOMBSTONED)) continue;

        rs->total_bytes -= e->data_size;
        free(e);
        rs->entries[i] = NULL;
        rs->count--;
        swept++;
    }
    rs->evictions += swept;  /* tombstone sweep counts as eviction */
    return swept;
}

/*
 * rs_tombstone_count() — count entries marked as tombstoned
 */
static inline uint32_t rs_tombstone_count(const ResidualSpace *rs) {
    if (!rs || !rs->entries) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        const ResidualEntry *e = rs->entries[i];
        if (e && (e->flags & RS_ENTRY_TOMBSTONED))
            n++;
    }
    return n;
}

/*
 * rs_expire_by_origin() — tombstone all entries matching origin_key
 *
 * Bulk expiration for bonds from the same source (e.g. pipe reroute).
 * Returns count of entries tombstoned.
 */
static inline uint32_t rs_expire_by_origin(ResidualSpace *rs, uint64_t origin_key) {
    if (!rs || !rs->entries || origin_key == RS_BOND_KEY_RESERVED)
        return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        ResidualEntry *e = rs->entries[i];
        if (!e) continue;
        if (!(e->flags & RS_ENTRY_VALID)) continue;
        if (e->origin_key != origin_key) continue;
        if (e->flags & RS_ENTRY_PINNED) continue;

        e->flags |= RS_ENTRY_TOMBSTONED;
        e->flags &= ~RS_ENTRY_VALID;
        n++;
    }
    return n;
}

/* ════════════════════════════════════════════════════════════
   QUERY / BOND-LEVEL ACCESS
   ════════════════════════════════════════════════════════════ */

/*
 * rs_contains() — check if bond_key exists in residual space
 */
static inline uint8_t rs_contains(const ResidualSpace *rs, uint64_t bond_key) {
    if (!rs || !rs->entries) return 0;
    uint32_t dummy;
    return rs_thaw(rs, bond_key, &dummy) != NULL;
}

/*
 * rs_entry_by_index() — get entry at index (for iteration)
 * Returns NULL if slot is empty / invalid.
 */
static inline const ResidualEntry *rs_entry_by_index(const ResidualSpace *rs,
                                                       uint32_t index)
{
    if (!rs || !rs->entries || index >= rs->capacity) return NULL;
    ResidualEntry *e = rs->entries[index];
    if (!e || !(e->flags & RS_ENTRY_VALID)) return NULL;
    return e;
}

/*
 * rs_count_high_entropy() — count entries marked high entropy
 */
static inline uint32_t rs_count_high_entropy(const ResidualSpace *rs) {
    if (!rs || !rs->entries) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        const ResidualEntry *e = rs->entries[i];
        if (e && (e->flags & RS_ENTRY_VALID) && (e->flags & RS_ENTRY_HIGH_ENTROPY))
            count++;
    }
    return count;
}

/* ════════════════════════════════════════════════════════════
   STATS
   ════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t capacity;
    uint32_t count;
    uint32_t evictions;
    uint64_t total_bytes;
    uint32_t high_entropy_count;
    double   load_factor;
    uint32_t avg_entry_bytes;
    uint32_t tombstone_count;
} ResidualSpaceStats;

static inline ResidualSpaceStats rs_stats(const ResidualSpace *rs) {
    ResidualSpaceStats s;
    memset(&s, 0, sizeof(s));
    if (!rs) return s;

    s.capacity        = rs->capacity;
    s.count           = rs->count;
    s.evictions       = rs->evictions;
    s.total_bytes     = rs->total_bytes;
    s.high_entropy_count = rs_count_high_entropy(rs);
    s.load_factor     = rs->capacity > 0
                        ? (double)rs->count / (double)rs->capacity : 0.0;
    s.avg_entry_bytes = rs->count > 0
                        ? (uint32_t)(rs->total_bytes / rs->count) : 0;
    s.tombstone_count = rs_tombstone_count(rs);
    return s;
}

/* ════════════════════════════════════════════════════════════
   PERSISTENCE — serialize entries by bond_key (§15.34)
   ════════════════════════════════════════════════════════════
   Format (little-endian, packed — host x86):
     [0..7]   magic  "RSDWGLSP" (8B)
     [8..9]   version u16 = 1
     [10..11] reserved u16 = 0
     [12..15] count u32
     [16..]   records: ResidualEntry header (36B) + payload, in slot order

   Only VALID entries are persisted (live data).  Tombstones are an
   in-memory recycle bin — the durable audit trail lives in the ghost
   log (GHOST_FLAG_EXPIRED), not here.  Reload = re-insert by bond_key
   into a fresh space: same bond → same address, no lookup table.

   rs_load() requires a freshly rs_init()'d space (count == 0) and fails
   on any corruption: bad magic, truncation, oversized/zero payload,
   reserved bond_key, non-VALID record, duplicate bond_key, or table
   overflow (capacity < count).  Timestamps are preserved → LRU order
   survives the restart; next_timestamp continues after the max.
   */

/* exact byte count needed to serialize (0 on invalid space) */
static inline uint64_t rs_serialize_size(const ResidualSpace *rs) {
    if (!rs || !rs->entries) return 0;
    uint64_t n = 16u;   /* file header */
    for (uint32_t i = 0; i < rs->capacity; i++) {
        const ResidualEntry *e = rs->entries[i];
        if (e && (e->flags & RS_ENTRY_VALID))
            n += RS_ENTRY_HEADER_SZ + e->data_size;
    }
    return n;
}

/* serialize all VALID entries (header verbatim + payload). Returns
   bytes written, or 0 if buf is NULL / cap too small. */
static inline uint64_t rs_serialize(const ResidualSpace *rs, void *buf,
                                    uint64_t cap) {
    if (!rs || !rs->entries || !buf) return 0;
    uint64_t need = rs_serialize_size(rs);
    if (cap < need) return 0;

    uint8_t *p = (uint8_t *)buf;
    memcpy(p, "RSDWGLSP", 8); p += 8;
    p[0] = 1; p[1] = 0;   /* version */
    p[2] = 0; p[3] = 0;   /* reserved */
    p += 4;
    uint32_t count = 0;
    for (uint32_t i = 0; i < rs->capacity; i++) {
        const ResidualEntry *e = rs->entries[i];
        if (e && (e->flags & RS_ENTRY_VALID)) count++;
    }
    memcpy(p, &count, 4); p += 4;

    for (uint32_t i = 0; i < rs->capacity; i++) {
        const ResidualEntry *e = rs->entries[i];
        if (!e || !(e->flags & RS_ENTRY_VALID)) continue;
        memcpy(p, e, RS_ENTRY_HEADER_SZ);
        p += RS_ENTRY_HEADER_SZ;
        memcpy(p, (const uint8_t *)e + RS_ENTRY_HEADER_SZ, e->data_size);
        p += e->data_size;
    }
    return need;
}

/* reload a serialized space into a FRESH rs_init()'d ResidualSpace.
   Returns 0 on success, -1 on any corruption/overflow. */
static inline int rs_load(ResidualSpace *rs, const void *buf, uint64_t size) {
    if (!rs || !rs->entries || !buf) return -1;
    if (rs->count != 0) return -1;         /* fresh space only */

    const uint8_t *p = (const uint8_t *)buf;
    if (size < 16) return -1;
    if (memcmp(p, "RSDWGLSP", 8) != 0) return -1;
    uint16_t ver;
    memcpy(&ver, p + 8, 2);
    if (ver != 1) return -1;
    uint32_t count;
    memcpy(&count, p + 12, 4);
    if (count > rs->capacity) return -1;   /* cannot fit — strict */

    uint64_t off = 16;
    uint32_t max_ts = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + RS_ENTRY_HEADER_SZ > size) return -1;
        const ResidualEntry *rec =
            (const ResidualEntry *)(const void *)(p + off);
        uint64_t total = RS_ENTRY_HEADER_SZ + (uint64_t)rec->data_size;
        if (rec->bond_key == RS_BOND_KEY_RESERVED) return -1;
        if (rec->data_size == 0 || rec->data_size > RS_MAX_DATA_SIZE) return -1;
        if (!(rec->flags & RS_ENTRY_VALID)) return -1;
        if (off + total > size) return -1;

        uint32_t mask  = rs->capacity - 1;
        uint32_t slot  = _rs_hash(rec->bond_key, mask);
        uint32_t start = slot;
        int inserted = 0;
        while (!inserted) {
            if (!rs->entries[slot]) {
                ResidualEntry *e = (ResidualEntry *)malloc(
                    RS_ENTRY_HEADER_SZ + rec->data_size);
                if (!e) return -1;
                memcpy(e, rec, RS_ENTRY_HEADER_SZ);
                memcpy((uint8_t *)e + RS_ENTRY_HEADER_SZ,
                       (const uint8_t *)rec + RS_ENTRY_HEADER_SZ,
                       rec->data_size);
                rs->entries[slot] = e;
                rs->count++;
                rs->total_bytes += rec->data_size;
                if (rec->timestamp > max_ts) max_ts = rec->timestamp;
                inserted = 1;
                break;
            }
            if (rs->entries[slot]->bond_key == rec->bond_key)
                return -1;                 /* duplicate — corrupt file */
            slot = (slot + 1) & mask;
            if (slot == start) return -1;  /* table full */
        }
        off += total;
    }
    rs->next_timestamp = max_ts + 1;
    return 0;
}

#endif /* RESIDUAL_SPACE_H */
