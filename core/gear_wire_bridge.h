/*
 * gear_wire_bridge.h — INTEROP BRIDGE for the GHST+gear-wire container
 * ══════════════════════════════════════════════════════════════════════
 *
 * SELF-CONTAINED by contract: includes ONLY <stdint.h> and <string.h>.
 * No DWGLS headers. External tools vendor this one file (plus the spec
 * docs/GEAR-WIRE-FORMAT.md) and can read/write/validate every container
 * produced by core/geo_ghost_gear_adapter.h without pulling the core
 * stack (residual_space / pogls_bond / hyp_fusion / …).
 *
 * TWO CONTAINER FAMILIES (both detectable by magic, LE):
 *
 * Family A — GHST+gear-wire (local field, W ∈ [0,144)):
 *   [0..3]    magic "GHST"
 *   [4..5]    version u16 = 1
 *   [6..7]    reserved u16 = 0
 *   [8..11]   count u32            (entries)
 *   [12..]    entries × 5 B: block_id u16 | from u8 | to u8 | flags u8
 *             sorted by (block_id, from) — b-bond principle
 *   [base..]  GEAR WIRE (derived offset base = 12 + count·5, no TOC):
 *             event byte = q:3b | dc:3b | dx:2b  (bit7 always 0)
 *               Δ = q·24 + crt(dc,dx),  crt: s≡dc (mod 8), s≡dx (mod 3)
 *             seal byte  = 0xFF (unreachable by the event bit layout)
 *
 * Family B — FGF2 packed gear log (full field, W ∈ [0,20736)):
 *   [0..3]    magic "FGF2"
 *   [4..7]    version u32 = 1
 *   [8..9]    n u16                 (event count)
 *   [10]      rim_mode u8           (0 = FREE, 1 = RIM)
 *   [11]      reserved u8 = 0
 *   [12..]    events × 2 B (FREE mode):
 *             LE u16: q:10b | dc:3b | dx:2b | spare:1b  (bit15 always 0)
 *             seal = 0xFFFF (unreachable by valid events, max = 0x7F5F)
 *           or ceil(n×10/8) B (RIM mode): packed q:10b values
 *
 * CANONICAL WIRE ORDER (GHST): bytes grouped by block id ascending;
 * within a block, append order; seal at its call position.
 *
 * SEAL-ACCOUNTING INVARIANT (GHST, loud corrupt detection):
 *     non-seal wire bytes == entries with flag bit 0x08 set
 *
 * ENTER ANYWHERE doctrine: both families store Δ only — no absolute
 * scale anywhere. Chain replay needs the READER's own position.
 *
 * Spec: docs/GEAR-WIRE-FORMAT.md
 */
#ifndef GEAR_WIRE_BRIDGE_H
#define GEAR_WIRE_BRIDGE_H

#include <stdint.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────────────── */
#define GWB_FLAG_GEAR   0x08u      /* entry flag: "owns a wire byte"     */
#define GWB_SEAL        0xFFu      /* wire chain terminator (GHST)       */
#define GWB_MAX_ENTRIES 4096u      /* mirror of core GHOST_LOG_MAX       */
#define GWB_FIELD       144u       /* local scale ring W ∈ [0,144)       */
#define GWB_RING        24u        /* rim teeth                          */

/* ── FGF2 full-field constants ─────────────────────────────────────────── */
#define GWB_FGF2_FIELD  20736u     /* full field: 144 × 144              */
#define GWB_FGF2_TURNS  864u       /* full turns of the rim (20736/24)   */
#define GWB_FGF2_SEAL16 0xFFFFu    /* 2-byte seal (max valid = 0x7F5F)   */
#define GWB_FGF2_QMAX   863u       /* max q value in full field          */

/* ── Container format IDs (gwb_view.format) ────────────────────────────── */
#define GWB_FMT_GHST    1u         /* GHST+gear-wire (local 144)         */
#define GWB_FMT_FGF2    2u         /* FGF2 packed gear log (full 20736)   */

/* ── Error codes (≥0 ok, <0 loud) ──────────────────────────────────────── */
enum {
    GWB_OK = 0,
    GWB_E_BADARG = -1,      /* NULL arg / zero cap                         */
    GWB_E_SMALL  = -2,      /* buffer shorter than the 12 B header         */
    GWB_E_MAGIC  = -3,      /* first 4 bytes are not "GHST"                */
    GWB_E_VER    = -4,      /* version != 1                                */
    GWB_E_COUNT  = -5,      /* count > GWB_MAX_ENTRIES                     */
    GWB_E_TRUNC  = -6,      /* declared entries run past the buffer end    */
    GWB_E_WIRE   = -7       /* seal-accounting violated (evs != geared)    */
};

/* ── Parsed view over a raw container (zero-copy) ──────────────────────── */
typedef struct {
    uint32_t        count;      /* entry count (GHST) or event count (FGF2) */
    const uint8_t  *entries;    /* GHST: count × 5 B packed records     */
    const uint8_t  *wire;       /* GHST: raw bytes; FGF2: packed events */
    uint32_t        wire_len;
    /* --- new fields (backward-compatible: appended after existing fields) -- */
    uint8_t         format;     /* GWB_FMT_GHST or GWB_FMT_FGF2        */
    uint8_t         rim_mode;   /* FGF2: 0=FREE, 1=RIM                  */
    uint32_t        field_size; /* 144 for GHST, 20736 for FGF2          */
} gwb_view;

typedef struct {
    uint16_t block_id;
    uint8_t  from_scale;
    uint8_t  to_scale;
    uint8_t  flags;
} gwb_entry;

/* ── Parse + structural validation ─────────────────────────────────────── */
static inline int gwb_parse(const void *buf, uint64_t size, gwb_view *v) {
    if (!buf || !v) return GWB_E_BADARG;
    if (size < 12) return GWB_E_SMALL;
    const uint8_t *p = (const uint8_t *)buf;
    memset(v, 0, sizeof(*v));

    /* ── try GHST first ──────────────────────────────────────────────── */
    if (memcmp(p, "GHST", 4) == 0) {
        uint16_t ver = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
        if (ver != 1) return GWB_E_VER;
        uint32_t count = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                         ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        if (count > GWB_MAX_ENTRIES) return GWB_E_COUNT;
        uint64_t base = 12u + (uint64_t)count * 5u;
        if (size < base) return GWB_E_TRUNC;
        v->count      = count;
        v->entries    = p + 12;
        v->wire_len   = (uint32_t)(size - base);
        v->wire       = v->wire_len ? p + base : NULL;
        v->format     = GWB_FMT_GHST;
        v->field_size = GWB_FIELD;
        return GWB_OK;
    }

    /* ── try FGF2 ─────────────────────────────────────────────────────── */
    if (memcmp(p, "FGF2", 4) == 0) {
        uint32_t ver = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                       ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        if (ver != 1) return GWB_E_VER;
        uint16_t n = (uint16_t)(p[8] | ((uint16_t)p[9] << 8));
        uint8_t rim = p[10];
        uint64_t base = 12u;
        uint32_t wire_len;
        if (rim == 0) {
            /* FREE mode: n × 2 bytes */
            wire_len = (uint32_t)n * 2u;
        } else {
            /* RIM mode: ceil(n × 10 / 8) bytes */
            wire_len = (uint32_t)((n * 10u + 7u) / 8u);
        }
        if (size < base + wire_len) return GWB_E_TRUNC;
        v->count      = n;
        v->entries    = NULL;     /* FGF2 has no entry table */
        v->wire_len   = wire_len;
        v->wire       = p + base;
        v->format     = GWB_FMT_FGF2;
        v->rim_mode   = rim;
        v->field_size = GWB_FGF2_FIELD;
        return GWB_OK;
    }

    return GWB_E_MAGIC;
}

static inline void gwb_entry_get(const gwb_view *v, uint32_t i, gwb_entry *e) {
    const uint8_t *p = v->entries + (uint64_t)i * 5u;
    e->block_id   = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    e->from_scale = p[2];
    e->to_scale   = p[3];
    e->flags      = p[4];
}

/* ── Gear math (independent re-derivation of the fan24 event format) ───── */
/* CRT closed form: s ≡ dc (mod 8), s ≡ dx (mod 3).
 * s = dc + 8k with 8k ≡ dx−dc (mod 3); 8 ≡ 2 (mod 3), 2⁻¹ ≡ 2 (mod 3). */
static inline uint8_t gwb_crt(uint8_t dc, uint8_t dx) {
    uint8_t k = (uint8_t)(((dx % 3u) + 3u - (dc % 3u)) % 3u);
    k = (uint8_t)((k * 2u) % 3u);
    return (uint8_t)(dc + 8u * k);          /* ∈ [0,24) */
}

static inline void gwb_decode(uint8_t b, uint8_t *q, uint8_t *dc, uint8_t *dx) {
    *q  = (uint8_t)(b & 7u);
    *dc = (uint8_t)((b >> 3) & 7u);
    *dx = (uint8_t)((b >> 6) & 3u);
}

static inline uint32_t gwb_delta(uint8_t q, uint8_t dc, uint8_t dx) {
    return (uint32_t)q * GWB_RING + gwb_crt(dc, dx);
}

static inline uint32_t gwb_step(uint32_t w, uint8_t q, uint8_t dc, uint8_t dx) {
    return (w + gwb_delta(q, dc, dx)) % GWB_FIELD;
}

static inline uint8_t gwb_encode_byte(uint8_t q, uint8_t dc, uint8_t dx) {
    return (uint8_t)((q & 7u) | ((dc & 7u) << 3) | ((dx & 3u) << 6));
}

/* ── Wire↔entry association (canonical layout, value-free) ───────────────
 * The k-th non-seal wire byte belongs to the k-th geared-flagged entry
 * in sorted order; a seal belongs to the block of the nearest preceding
 * event. gwb_wire_owner resolves byte index → block id (or -1 if the
 * layout is broken: seal before any event / more events than flags).   */
static inline int gwb_wire_owner(const gwb_view *v, uint32_t byte_idx) {
    if (!v || !v->wire || byte_idx >= v->wire_len) return -1;
    uint32_t g = 0;
    int have = 0, cur = -1;
    for (uint32_t k = 0; k <= byte_idx; k++) {
        uint8_t b = v->wire[k];
        if (b == GWB_SEAL) {                    /* inherits preceding ev  */
            if (!have) return -1;               /* seal before any event  */
            continue;
        }
        while (g < v->count) {
            gwb_entry e;
            gwb_entry_get(v, g, &e);
            if (e.flags & GWB_FLAG_GEAR) break;
            g++;
        }
        if (g >= v->count) return -1;           /* more events than flags */
        gwb_entry e;
        gwb_entry_get(v, g, &e);
        cur = (int)e.block_id;
        have = 1;
        g++;
    }
    return cur;
}

/* ── Block chain from WIRE ONLY ──────────────────────────────────────────
 * Walks this block's canonical run: owned events advance the chain from
 * the reader-supplied birth scale; the first owned seal bites. Other
 * blocks' bytes are skipped. Returns hops decoded (out_to[i] = scale
 * after hop i), or (uint32_t)-1 on bad input / broken layout.          */
static inline uint32_t gwb_block_chain(const gwb_view *v, uint16_t block_id,
                                       uint8_t birth, uint8_t *out_to,
                                       uint32_t cap) {
    if (!v || !v->wire || !out_to || cap == 0) return (uint32_t)-1;
    uint32_t w = birth, hops = 0;
    for (uint32_t k = 0; k < v->wire_len && hops < cap; k++) {
        int own = gwb_wire_owner(v, k);
        if (own < 0) return (uint32_t)-1;
        if ((uint16_t)own != block_id) continue;
        uint8_t b = v->wire[k];
        if (b == GWB_SEAL) break;
        uint8_t q, dc, dx;
        gwb_decode(b, &q, &dc, &dx);
        w = gwb_step(w, q, dc, dx);
        out_to[hops++] = (uint8_t)w;
    }
    return hops;
}

/* ═══════════════════════════════════════════════════════════════════════
 * FGF2 FULL-FIELD (20736) — packed 2-byte events
 * ═══════════════════════════════════════════════════════════════════════ */

/* Decode a 2-byte LE event (FREE mode): */
static inline void gwb_fg_decode(uint8_t lo, uint8_t hi,
                                 uint16_t *q, uint8_t *dc, uint8_t *dx) {
    uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
    *q  = (uint16_t)(raw & 0x3FFu);       /* bits 0–9  */
    *dc = (uint8_t)((raw >> 10) & 7u);    /* bits 10–12 */
    *dx = (uint8_t)((raw >> 13) & 3u);    /* bits 13–14 */
}

static inline void gwb_fg_encode(uint16_t q, uint8_t dc, uint8_t dx,
                                 uint8_t *lo, uint8_t *hi) {
    uint16_t raw = (q & 0x3FFu) | ((uint16_t)(dc & 7u) << 10) |
                   ((uint16_t)(dx & 3u) << 13);
    *lo = (uint8_t)(raw & 0xFFu);
    *hi = (uint8_t)((raw >> 8) & 0xFFu);
}

static inline uint32_t gwb_fg_delta(uint16_t q, uint8_t dc, uint8_t dx) {
    return (uint32_t)q * GWB_RING + gwb_crt(dc, dx);
}

static inline uint32_t gwb_fg_step(uint32_t w, uint16_t q,
                                   uint8_t dc, uint8_t dx) {
    return (w + gwb_fg_delta(q, dc, dx)) % GWB_FGF2_FIELD;
}

/* FGF2 chain walk from packed FREE-mode wire. Reads 2 bytes per event.
 * Returns hops decoded into out_to[] (full-field u32 values), or
 * (uint32_t)-1 on bad input. Seal 0xFFFF stops the walk.          */
static inline uint32_t gwb_fg_chain(const gwb_view *v, uint32_t birth,
                                    uint32_t *out_to, uint32_t cap) {
    if (!v || v->format != GWB_FMT_FGF2 || v->rim_mode != 0)
        return (uint32_t)-1;
    if (!v->wire || !out_to || cap == 0) return (uint32_t)-1;
    uint32_t w = birth, hops = 0;
    for (uint32_t k = 0; k + 1 < v->wire_len && hops < cap; k += 2) {
        uint16_t raw = (uint16_t)v->wire[k] | ((uint16_t)v->wire[k+1] << 8);
        if (raw == GWB_FGF2_SEAL16) break;
        uint16_t q;
        uint8_t dc, dx;
        gwb_fg_decode(v->wire[k], v->wire[k + 1], &q, &dc, &dx);
        w = gwb_fg_step(w, q, dc, dx);
        out_to[hops++] = w;
    }
    return hops;
}

/* FGF2 backward reconstruct (enter-anywhere, full field). Given n packed
 * event bytes (FREE mode, no seals) and reader's cur_w, fills out_w:
 *   out_w[0] = append w … out_w[n] = cur_w.  Returns n+1 or 0.     */
static inline uint32_t gwb_fg_reconstruct(const uint8_t *packed, uint32_t n_ev,
                                          uint32_t cur_w, uint32_t *out_w,
                                          uint32_t cap) {
    if (!packed || !out_w || cap < n_ev + 1u) return 0;
    uint32_t w[4097];
    w[n_ev] = cur_w % GWB_FGF2_FIELD;
    for (uint32_t i = n_ev; i > 0; i--) {
        uint16_t q; uint8_t dc, dx;
        gwb_fg_decode(packed[2*(i-1)], packed[2*(i-1)+1], &q, &dc, &dx);
        uint32_t d = gwb_fg_delta(q, dc, dx);
        w[i-1] = (w[i] + GWB_FGF2_FIELD - d % GWB_FGF2_FIELD) % GWB_FGF2_FIELD;
    }
    memcpy(out_w, w, (n_ev + 1u) * sizeof(uint32_t));
    return n_ev + 1u;
}

/* ── Deep validation (audit-grade — stricter than parse) ─────────────────
 * Dispatches based on format:
 *   GHST  — seal accounting, entry flags, canonical order
 *   FGF2  — event count vs wire length, bit15 always 0, seal legality
 * Returns GWB_OK or the first violated code.                           */
static inline int gwb_validate(const gwb_view *v) {
    if (!v) return GWB_E_BADARG;

    /* ── FGF2 validation ────────────────────────────────────────────── */
    if (v->format == GWB_FMT_FGF2) {
        if (v->rim_mode == 0) {
            /* FREE: wire_len must equal n × 2 */
            if (v->wire_len != (uint32_t)v->count * 2u) return GWB_E_WIRE;
            /* bit15 must be 0 for every non-seal event */
            for (uint32_t k = 0; k + 1 < v->wire_len; k += 2) {
                uint8_t hi = v->wire[k + 1];
                if (hi == 0xFF) {
                    /* seal: lo must also be 0xFF */
                    if (v->wire[k] != 0xFF) return GWB_E_WIRE;
                } else if (hi & 0x80u) {
                    return GWB_E_WIRE;            /* bit15 set → corrupt */
                }
            }
            return GWB_OK;
        }
        /* RIM: ceil(n×10/8) bytes — accept any valid bitstream */
        return GWB_OK;
    }

    /* ── GHST validation (original) ──────────────────────────────────── */
    uint32_t geared = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        gwb_entry e;
        gwb_entry_get(v, i, &e);
        if (e.flags & GWB_FLAG_GEAR) geared++;
    }
    uint32_t evs = 0, seals = 0;
    int have = 0, last_block = -1;
    for (uint32_t k = 0; k < v->wire_len; k++) {
        uint8_t b = v->wire[k];
        if (b == GWB_SEAL) {
            seals++;
            if (!have) return GWB_E_WIRE;
            continue;
        }
        evs++;
        int own = gwb_wire_owner(v, k);
        if (own < 0) return GWB_E_WIRE;
        if (own < last_block) return GWB_E_WIRE;
        last_block = own;
        have = 1;
    }
    if (evs != geared) return GWB_E_WIRE;
    if (v->wire_len > 0 && geared == 0) return GWB_E_WIRE;
    (void)seals;
    return GWB_OK;
}

/* ── Enter-anywhere backward reconstruct (byte-event form) ───────────────
 * Given n_ev RAW EVENT bytes (no seals) and the reader's current scale,
 * fills out_w[0..n_ev] with the chain ending at cur_w:
 *   out_w[0] = append scale … out_w[n_ev] = cur_w
 * Returns n_ev+1, or 0 on bad input / cap too small. Needs no container —
 * feed it any Δ-run (a block's canonical bytes work verbatim).          */
static inline uint32_t gwb_reconstruct(const uint8_t *events, uint32_t n_ev,
                                       uint32_t cur_w, uint32_t *out_w,
                                       uint32_t cap) {
    if (!events || !out_w || cap < n_ev + 1u) return 0;
    uint32_t w[4097];
    w[n_ev] = cur_w % GWB_FIELD;
    for (uint32_t i = n_ev; i > 0; i--) {
        uint8_t q, dc, dx;
        gwb_decode(events[i - 1], &q, &dc, &dx);
        w[i - 1] = (w[i] + GWB_FIELD - gwb_delta(q, dc, dx) % GWB_FIELD)
                   % GWB_FIELD;
    }
    memcpy(out_w, w, (n_ev + 1u) * sizeof(uint32_t));
    return n_ev + 1u;
}

/* ── Writer — canonical container from plain structs ─────────────────────
 * Entries must already be sorted by (block_id, from); wire must already
 * be canonical. Emits [header][entries][wire]; returns total size or 0. */
static inline uint64_t gwb_write(const gwb_entry *ev, uint32_t n,
                                 const uint8_t *wire, uint32_t wire_len,
                                 void *buf, uint64_t cap) {
    if ((!ev && n) || (!wire && wire_len) || !buf) return 0;
    if (n > GWB_MAX_ENTRIES) return 0;
    uint64_t need = 12u + (uint64_t)n * 5u + wire_len;
    if (cap < need) return 0;
    uint8_t *p = (uint8_t *)buf;
    p[0] = 'G'; p[1] = 'H'; p[2] = 'S'; p[3] = 'T';
    p[4] = 1; p[5] = 0;                     /* version 1 LE              */
    p[6] = 0; p[7] = 0;                     /* reserved                  */
    p[8]  = (uint8_t)(n & 0xFFu);
    p[9]  = (uint8_t)((n >> 8) & 0xFFu);
    p[10] = (uint8_t)((n >> 16) & 0xFFu);
    p[11] = (uint8_t)((n >> 24) & 0xFFu);
    p += 12;
    for (uint32_t i = 0; i < n; i++) {
        p[0] = (uint8_t)(ev[i].block_id & 0xFFu);
        p[1] = (uint8_t)((ev[i].block_id >> 8) & 0xFFu);
        p[2] = ev[i].from_scale;
        p[3] = ev[i].to_scale;
        p[4] = ev[i].flags;
        p += 5;
    }
    if (wire_len) memcpy(p, wire, wire_len);
    return need;
}

/* ── FGF2 writer — packed full-field gear log ────────────────────────────
 * Writes [FGF2 header 12B][n×2B packed events]. rim_mode selects FREE/RIM.
 * For FREE: events is an array of {q,dc,dx}; for RIM: events is raw q values.
 * Returns total bytes written or 0 on cap fail.                          */
typedef struct {
    uint16_t q;
    uint8_t  dc;
    uint8_t  dx;
} gwb_fg_event;

static inline uint64_t gwb_fg_write(const gwb_fg_event *ev, uint32_t n,
                                    uint8_t rim_mode, void *buf, uint64_t cap) {
    if (!buf) return 0;
    uint64_t wire_bytes;
    if (rim_mode == 0) wire_bytes = (uint64_t)n * 2u;
    else wire_bytes = (uint64_t)(n * 10u + 7u) / 8u;
    uint64_t need = 12u + wire_bytes;
    if (cap < need) return 0;
    uint8_t *p = (uint8_t *)buf;
    /* header */
    p[0] = 'F'; p[1] = 'G'; p[2] = 'F'; p[3] = '2';
    p[4] = 1; p[5] = 0; p[6] = 0; p[7] = 0;   /* version 1 LE */
    p[8]  = (uint8_t)(n & 0xFFu);
    p[9]  = (uint8_t)((n >> 8) & 0xFFu);
    p[10] = rim_mode;
    p[11] = 0;
    p += 12;
    if (rim_mode == 0) {
        /* FREE: pack each event as 2 LE bytes */
        for (uint32_t i = 0; i < n; i++) {
            uint8_t lo, hi;
            gwb_fg_encode(ev[i].q, ev[i].dc, ev[i].dx, &lo, &hi);
            p[0] = lo; p[1] = hi;
            p += 2;
        }
    } else {
        /* RIM: pack 10-bit q values into bitstream */
        uint32_t bits = 0, acc = 0;
        for (uint32_t i = 0; i < n; i++) {
            acc |= ((uint32_t)ev[i].q & 0x3FFu) << bits;
            bits += 10;
            while (bits >= 8) {
                *p++ = (uint8_t)(acc & 0xFFu);
                acc >>= 8;
                bits -= 8;
            }
        }
        if (bits > 0) *p++ = (uint8_t)(acc & 0xFFu);
    }
    return need;
}

/* ── JSON emitter (canonical interchange for non-C tools) ────────────────
 * Stable key order; numbers only where possible. Returns bytes written
 * (excluding NUL), 0 on overflow. Layout:
 * { "format":"ghst-gear-wire","version":1,"entries":N,"geared":G,
 *   "seals":S,"valid":true|false,
 *   "records":[{"block":7,"from":3,"to":51,"flags":"LG"},…],
 *   "wire":[{"block":7,"event":{"q":2,"dc":0,"dx":0},"delta":48}|…],
 *   "wire_bytes_hex":"02FF…" }                                          */
static inline uint64_t _gwb_put(char *dst, uint64_t cap, uint64_t pos,
                                const char *src, uint64_t len) {
    if (pos + len + 1 > cap) return 0;      /* caller propagates failure */
    memcpy(dst + pos, src, len);
    dst[pos + len] = 0;
    return pos + len;
}

static inline uint64_t _gwb_put_u(char *dst, uint64_t cap, uint64_t pos,
                                  uint32_t x) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (x % 10u)); x /= 10u; } while (x);
    char out[12];
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return _gwb_put(dst, cap, pos, out, (uint64_t)n);
}

static inline uint64_t _gwb_put_i(char *dst, uint64_t cap, uint64_t pos,
                                  int v) {
    uint64_t p = pos;
    if (v < 0) {
        p = _gwb_put(dst, cap, p, "-", 1);
        if (!p) return 0;
        v = -v;
    }
    return _gwb_put_u(dst, cap, p, (uint32_t)v);
}

/* flags → short letter tag: L=lift E=expired D=delta G=gear, '.'=other */
static inline void _gwb_flags_tag(uint8_t flags, char *out) {
    static const char lut[8] = { '.', 'L', 'E', '?', 'D', '?', '?', '?' };
    out[0] = lut[flags & 7u];
    out[1] = (flags & GWB_FLAG_GEAR) ? 'G' : '.';
    out[2] = 0;
}

static inline uint64_t gwb_json(const gwb_view *v, char *buf, uint64_t cap) {
    if (!v || !buf || cap == 0) return 0;
    uint64_t p = 0;
#define PUT(s)   do { p = _gwb_put(buf, cap, p, s, sizeof(s) - 1); if (!p) return 0; } while (0)
#define PUTU(x)  do { p = _gwb_put_u(buf, cap, p, (uint32_t)(x)); if (!p) return 0; } while (0)
#define PUTI(x)  do { p = _gwb_put_i(buf, cap, p, (int)(x)); if (!p) return 0; } while (0)

    int valid = gwb_validate(v) == GWB_OK;
    const char HEX[] = "0123456789ABCDEF";

    /* ── FGF2 JSON ────────────────────────────────────────────────────── */
    if (v->format == GWB_FMT_FGF2) {
        uint32_t seals = 0;
        if (v->rim_mode == 0) {
            for (uint32_t k = 0; k + 1 < v->wire_len; k += 2)
                if (v->wire[k] == 0xFF && v->wire[k+1] == 0xFF) seals++;
        }
        PUT("{\"format\":\"fgf2-gear-log\",\"version\":1,");
        PUT("\"field\":"); PUTU(GWB_FGF2_FIELD);
        PUT(",\"events\":"); PUTU(v->count);
        PUT(",\"rim_mode\":"); PUTU(v->rim_mode);
        PUT(",\"seals\":"); PUTU(seals);
        PUT(",\"valid\":");
        {
            const char *vstr = valid ? "true" : "false";
            p = _gwb_put(buf, cap, p, vstr, strlen(vstr));
            if (!p) return 0;
        }
        if (v->rim_mode == 0) {
            PUT(",\"wire\":[");
            uint32_t ev_idx = 0;
            for (uint32_t k = 0; k + 1 < v->wire_len; k += 2) {
                uint16_t raw = (uint16_t)v->wire[k] |
                              ((uint16_t)v->wire[k+1] << 8);
                if (ev_idx) PUT(",");
                if (raw == GWB_FGF2_SEAL16) {
                    PUT("{\"seal\":true}");
                } else {
                    uint16_t q; uint8_t dc, dx;
                    gwb_fg_decode(v->wire[k], v->wire[k+1], &q, &dc, &dx);
                    PUT("{\"idx\":"); PUTU(ev_idx);
                    PUT(",\"q\":");   PUTU(q);
                    PUT(",\"dc\":");  PUTU(dc);
                    PUT(",\"dx\":");  PUTU(dx);
                    PUT(",\"delta\":"); PUTU(gwb_fg_delta(q, dc, dx));
                    PUT("}");
                }
                ev_idx++;
            }
            PUT("]");
        }
        PUT(",\"wire_bytes_hex\":\"");
        for (uint32_t k = 0; k < v->wire_len; k++) {
            char h[2] = { HEX[v->wire[k] >> 4], HEX[v->wire[k] & 15u] };
            p = _gwb_put(buf, cap, p, h, 2);
            if (!p) return 0;
        }
        PUT("\"}");
        return p;
    }

    /* ── GHST JSON ─────────────────────────────────────────────────────── */
    uint32_t geared = 0, seals = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        gwb_entry e;
        gwb_entry_get(v, i, &e);
        if (e.flags & GWB_FLAG_GEAR) geared++;
    }
    for (uint32_t k = 0; k < v->wire_len; k++)
        if (v->wire[k] == GWB_SEAL) seals++;

    PUT("{\"format\":\"ghst-gear-wire\",\"version\":1,\"entries\":");
    PUTU(v->count);
    PUT(",\"geared\":");  PUTU(geared);
    PUT(",\"seals\":");   PUTU(seals);
    PUT(",\"valid\":");
    {
        const char *vstr = valid ? "true" : "false";
        p = _gwb_put(buf, cap, p, vstr, strlen(vstr));
        if (!p) return 0;
    }

    PUT(",\"records\":[");
    for (uint32_t i = 0; i < v->count; i++) {
        gwb_entry e;
        gwb_entry_get(v, i, &e);
        char tag[3];
        _gwb_flags_tag(e.flags, tag);
        if (i) PUT(",");
        PUT("{\"block\":"); PUTU(e.block_id);
        PUT(",\"from\":");  PUTU(e.from_scale);
        PUT(",\"to\":");    PUTU(e.to_scale);
        PUT(",\"flags\":\""); PUT(tag); PUT("\"}");
    }
    PUT("],\"wire\":[");
    {
        uint32_t emitted = 0;
        for (uint32_t k = 0; k < v->wire_len; k++) {
            uint8_t b = v->wire[k];
            int own = gwb_wire_owner(v, k);
            if (emitted) PUT(",");
            PUT("{\"block\":");
            if (own < 0) PUT("null"); else PUTU((uint32_t)own);
            if (b == GWB_SEAL) {
                PUT(",\"seal\":true}");
            } else {
                uint8_t q, dc, dx;
                gwb_decode(b, &q, &dc, &dx);
                PUT(",\"event\":{\"q\":"); PUTU(q);
                PUT(",\"dc\":");           PUTU(dc);
                PUT(",\"dx\":");           PUTU(dx);
                PUT("},\"delta\":");
                if (own >= 0) PUTU(gwb_delta(q, dc, dx));
                else PUT("null");
                PUT("}");
            }
            emitted++;
        }
    }
    PUT("],\"wire_bytes_hex\":\"");
    for (uint32_t k = 0; k < v->wire_len; k++) {
        char h[2] = { HEX[v->wire[k] >> 4], HEX[v->wire[k] & 15u] };
        p = _gwb_put(buf, cap, p, h, 2);
        if (!p) return 0;
    }
    PUT("\"}");
#undef PUT
#undef PUTU
#undef PUTI
    return p;
}

#endif /* GEAR_WIRE_BRIDGE_H */
