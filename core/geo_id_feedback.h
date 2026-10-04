/*
 * geo_id_feedback.h — feedback adaptation layer over the P2/P3 ident contract
 * ═════════════════════════════════════════════════════════════════════════════
 *
 * Sits BEHIND the deterministic baseline:
 *   P2 baseline = (name_hash, bytes_hash, addr_slot, walk_round, walk_tick)
 *   P3 baseline = event log (WRITE / CLEAR / MOVE / ANCHOR)
 *
 * Feedback captures SYSTEM-INTRINSIC signals that the baseline cannot:
 *   • access_count    — how often this ident was read
 *   • drift_count     — how many times bytes_hash changed under same name_hash
 *   • last_reanchor   — most recent re-anchor walk_round
 *   • co_access_hash  — folded co-access pattern (XOR-fold → order-invariant)
 *   • flags           — HOT bit (auto-set when access_count >= hot_threshold)
 *
 * NONE of these fields modify the baseline 5-tuple. Turning feedback off
 * leaves the deterministic replay path from geo_id_route_log.h unchanged.
 *
 * WIRE FORMAT (32 B/record, symmetric with IR_LOG_RECORD_SIZE for storage
 * uniformity):
 *
 *   offset  size  field
 *     0     8     name_hash       (baseline-stable key, never modified here)
 *     8     4     access_count    (mutable, BE)
 *    12     4     drift_count     (mutable, BE)
 *    16     4     last_reanchor_round (mutable, BE; 0 = never)
 *    20     8     co_access_hash  (mutable; XOR fold, order-invariant)
 *    28     4     flags           (mutable; bit 0 = HOT)
 *   total   32
 *
 * No CRC. Feedback is a best-effort adaptation signal, not an integrity gate.
 * Integrity comes from the baseline log (geo_id_route_log.h).
 *
 * Lookup is by name_hash (stable across slot moves). When the same ident
 * appears at multiple slots (mirror routes), feedback is shared because
 * feedback keys on name_hash, not slot.
 */
#ifndef GEO_ID_FEEDBACK_H
#define GEO_ID_FEEDBACK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define IFB_RECORD_SIZE   32u
#define IFB_MAGIC         0x49464231u  /* "IFB1" Ident FeedBack v1 */
#define IFB_VERSION       1u

/* Compile-time: IFBCell must serialize to exactly 32 bytes (wire format
 * symmetric with geo_id_route_log.h IR_LOG_RECORD_SIZE). The in-memory
 * struct may have trailing padding, but the wire size is fixed at 32. */
typedef struct {
    uint64_t name_hash;             /*  8  baseline key (NEVER modified) */
    uint32_t access_count;          /*  4  observe-only */
    uint32_t drift_count;           /*  4  note_drift */
    uint32_t last_reanchor_round;   /*  4  reanchor */
    uint64_t co_access_hash;        /*  8  XOR fold */
    uint32_t flags;                 /*  4  HOT etc. */
} IFBCell;

/* Wire invariant: name_hash + 4×u32 + co_access_hash + flags = 32 bytes */
_Static_assert(sizeof(((IFBCell*)0)->name_hash) +
               sizeof(((IFBCell*)0)->access_count) +
               sizeof(((IFBCell*)0)->drift_count) +
               sizeof(((IFBCell*)0)->last_reanchor_round) +
               sizeof(((IFBCell*)0)->co_access_hash) +
               sizeof(((IFBCell*)0)->flags) == IFB_RECORD_SIZE,
               "IFB wire size invariant: name_hash+access+drift+reanchor+co+flags = 32 bytes");

typedef struct {
    uint32_t magic;                 /* IFB_MAGIC */
    uint32_t version;               /* IFB_VERSION */
    uint32_t n_records;             /* event count */
    uint32_t reserved;
} IFBLogHeader;                    /* 16 bytes */

/* flags */
#define IFB_FLAG_HOT      0x01u

/* hot threshold (a small constant — system-intrinsic, not configurable from
 * the log; the design choice is documented in the gate test) */
#define IFB_HOT_THRESHOLD 8u

/* ── Record pack/unpack (network byte order, big-endian on the wire) ── */
static inline void ifb_cell_serialize(uint8_t out[IFB_RECORD_SIZE],
                                      const IFBCell *c) {
    uint64_t nh  = c->name_hash;
    uint32_t ac  = c->access_count;
    uint32_t dc  = c->drift_count;
    uint32_t lr  = c->last_reanchor_round;
    uint64_t co  = c->co_access_hash;
    uint32_t fl  = c->flags;
    /* big-endian */
    out[ 0] = (uint8_t)(nh >> 56); out[ 1] = (uint8_t)(nh >> 48);
    out[ 2] = (uint8_t)(nh >> 40); out[ 3] = (uint8_t)(nh >> 32);
    out[ 4] = (uint8_t)(nh >> 24); out[ 5] = (uint8_t)(nh >> 16);
    out[ 6] = (uint8_t)(nh >>  8); out[ 7] = (uint8_t)(nh);
    out[ 8] = (uint8_t)(ac >> 24); out[ 9] = (uint8_t)(ac >> 16);
    out[10] = (uint8_t)(ac >>  8); out[11] = (uint8_t)(ac);
    out[12] = (uint8_t)(dc >> 24); out[13] = (uint8_t)(dc >> 16);
    out[14] = (uint8_t)(dc >>  8); out[15] = (uint8_t)(dc);
    out[16] = (uint8_t)(lr >> 24); out[17] = (uint8_t)(lr >> 16);
    out[18] = (uint8_t)(lr >>  8); out[19] = (uint8_t)(lr);
    out[20] = (uint8_t)(co >> 56); out[21] = (uint8_t)(co >> 48);
    out[22] = (uint8_t)(co >> 40); out[23] = (uint8_t)(co >> 32);
    out[24] = (uint8_t)(co >> 24); out[25] = (uint8_t)(co >> 16);
    out[26] = (uint8_t)(co >>  8); out[27] = (uint8_t)(co);
    out[28] = (uint8_t)(fl >> 24); out[29] = (uint8_t)(fl >> 16);
    out[30] = (uint8_t)(fl >>  8); out[31] = (uint8_t)(fl);
}

static inline void ifb_cell_deserialize(const uint8_t in[IFB_RECORD_SIZE],
                                        IFBCell *c) {
    c->name_hash =
        ((uint64_t)in[ 0] << 56) | ((uint64_t)in[ 1] << 48) |
        ((uint64_t)in[ 2] << 40) | ((uint64_t)in[ 3] << 32) |
        ((uint64_t)in[ 4] << 24) | ((uint64_t)in[ 5] << 16) |
        ((uint64_t)in[ 6] <<  8) |  (uint64_t)in[ 7];
    c->access_count =
        ((uint32_t)in[ 8] << 24) | ((uint32_t)in[ 9] << 16) |
        ((uint32_t)in[10] <<  8) |  (uint32_t)in[11];
    c->drift_count =
        ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) |
        ((uint32_t)in[14] <<  8) |  (uint32_t)in[15];
    c->last_reanchor_round =
        ((uint32_t)in[16] << 24) | ((uint32_t)in[17] << 16) |
        ((uint32_t)in[18] <<  8) |  (uint32_t)in[19];
    c->co_access_hash =
        ((uint64_t)in[20] << 56) | ((uint64_t)in[21] << 48) |
        ((uint64_t)in[22] << 40) | ((uint64_t)in[23] << 32) |
        ((uint64_t)in[24] << 24) | ((uint64_t)in[25] << 16) |
        ((uint64_t)in[26] <<  8) |  (uint64_t)in[27];
    c->flags =
        ((uint32_t)in[28] << 24) | ((uint32_t)in[29] << 16) |
        ((uint32_t)in[30] <<  8) |  (uint32_t)in[31];
}

/* ── Feedback operations (all operate on a feedback TABLE, not the log) ──
 *
 * A feedback table is a small open-addressed map keyed by name_hash.
 * Buckets: 4096 (chosen as a 2x load on ~2000 typical entries; over-full
 * tables degrade gracefully — table_size() reports the used count).
 *
 * Resolution: name_hash MOD 4096, linear probe on collision (open addr).
 * For each slot, we mark slot as empty when name_hash == 0 and access_count==0
 * AND drift_count==0 (the all-zero sentinel — initial table state).
 */
#define IFB_TABLE_BUCKETS 4096u

typedef struct {
    IFBCell bucket[IFB_TABLE_BUCKETS];
} IFBTable;

static inline void ifb_table_init(IFBTable *t) {
    memset(t, 0, sizeof(*t));
}

static inline uint32_t ifb_table_size(const IFBTable *t) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < IFB_TABLE_BUCKETS; i++) {
        if (t->bucket[i].name_hash != 0u) n++;
    }
    return n;
}

/* internal: lookup or insert; returns pointer into bucket[] */
static inline IFBCell *ifb_lookup(IFBTable *t, uint64_t name_hash,
                                  int *inserted) {
    if (inserted) *inserted = 0;
    if (name_hash == 0u) {
        /* never observe the zero name_hash — caller misuse */
        return 0;
    }
    uint32_t mask = IFB_TABLE_BUCKETS - 1u;
    uint32_t idx = (uint32_t)(name_hash & mask);
    uint32_t first_empty = IFB_TABLE_BUCKETS;
    for (uint32_t probe = 0; probe < IFB_TABLE_BUCKETS; probe++) {
        IFBCell *c = &t->bucket[idx];
        if (c->name_hash == name_hash) return c;
        if (c->name_hash == 0u && first_empty == IFB_TABLE_BUCKETS) {
            first_empty = idx;
        }
        idx = (idx + 1u) & mask;
    }
    /* table full — fail soft: use first empty slot we passed. */
    if (first_empty < IFB_TABLE_BUCKETS) {
        IFBCell *c = &t->bucket[first_empty];
        c->name_hash = name_hash;
        if (inserted) *inserted = 1;
        return c;
    }
    return 0;  /* table saturated */
}

/* ── Public feedback operations ─────────────────────────────────────────── */

/* observe a read on this ident; auto-sets HOT bit at threshold */
static inline void ifb_observe(IFBTable *t, uint64_t name_hash) {
    int ins;
    IFBCell *c = ifb_lookup(t, name_hash, &ins);
    if (!c) return;
    c->access_count++;
    if (c->access_count >= IFB_HOT_THRESHOLD) c->flags |= IFB_FLAG_HOT;
}

/* note that bytes_hash drifted for this ident (e.g., a repair happened) */
static inline void ifb_note_drift(IFBTable *t, uint64_t name_hash) {
    int ins;
    IFBCell *c = ifb_lookup(t, name_hash, &ins);
    if (!c) return;
    c->drift_count++;
}

/* observe co-access: (a, b) seen together. Order-invariant via XOR fold. */
static inline void ifb_observe_co_access(IFBTable *t,
                                         uint64_t a, uint64_t b) {
    if (a == 0u || b == 0u) return;
    /* fold = a XOR b (ordered) AND a XOR b (b-a) — but b-a is also XOR so
     * this collapses. Use hash(a) XOR hash(b) for clearer separation. */
    /* For the ident use-case, plain XOR is already order-invariant since
     * XOR is commutative. */
    uint64_t mix = a ^ b;
    int ins;
    IFBCell *c = ifb_lookup(t, a, &ins);
    if (!c) return;
    c->co_access_hash ^= mix;
    /* also fold into b for symmetry — same key, same fold value */
    IFBCell *c2 = ifb_lookup(t, b, &ins);
    if (c2) c2->co_access_hash ^= mix;
}

/* mark a re-anchor at this walk_round for this ident */
static inline void ifb_reanchor(IFBTable *t, uint64_t name_hash,
                                uint32_t walk_round) {
    int ins;
    IFBCell *c = ifb_lookup(t, name_hash, &ins);
    if (!c) return;
    if (walk_round > c->last_reanchor_round) c->last_reanchor_round = walk_round;
}

/* ── Log append / replay ──────────────────────────────────────────────── */

/* Append a feedback event to the log. Caller is responsible for capacity. */
static inline void ifb_log_append(uint8_t *buf, uint32_t idx,
                                  const IFBCell *c) {
    ifb_cell_serialize(buf + (size_t)idx * IFB_RECORD_SIZE, c);
}

/* Replay the feedback log into a fresh table. Last-write-wins per name_hash.
 * Returns count of records applied, or -1 if header is malformed.
 */
static inline int ifb_log_replay(const uint8_t *buf, size_t total_bytes,
                                 IFBTable *out) {
    ifb_table_init(out);
    if (total_bytes < sizeof(IFBLogHeader)) return -1;
    IFBLogHeader hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != IFB_MAGIC) return -1;
    if (hdr.version != IFB_VERSION) return -1;
    uint32_t n = hdr.n_records;
    if ((size_t)n * IFB_RECORD_SIZE + sizeof(hdr) != total_bytes) return -1;
    for (uint32_t i = 0; i < n; i++) {
        IFBCell c;
        ifb_cell_deserialize(buf + sizeof(hdr) + (size_t)i * IFB_RECORD_SIZE,
                             &c);
        int ins;
        IFBCell *slot = ifb_lookup(out, c.name_hash, &ins);
        if (!slot) continue;  /* saturated table */
        /* last-write-wins on counters; max-wins on last_reanchor; XOR-fold
         * accumulates co_access_hash (commutative). */
        slot->name_hash = c.name_hash;
        slot->access_count = c.access_count;
        slot->drift_count = c.drift_count;
        if (c.last_reanchor_round > slot->last_reanchor_round)
            slot->last_reanchor_round = c.last_reanchor_round;
        slot->co_access_hash ^= c.co_access_hash;
        slot->flags = c.flags;
    }
    return (int)n;
}

/* Convenience: write the header into buf at offset 0. */
static inline void ifb_log_write_header(uint8_t *buf, uint32_t n_records) {
    IFBLogHeader hdr;
    hdr.magic = IFB_MAGIC;
    hdr.version = IFB_VERSION;
    hdr.n_records = n_records;
    hdr.reserved = 0u;
    memcpy(buf, &hdr, sizeof(hdr));
}

#endif /* GEO_ID_FEEDBACK_H */