/*
 * pogls_bond.h
 * ─────────────────────────────────────────────────────────────
 * Intrinsic Bond Layer — POGLS fibo_addr wired to TetrisRouter
 *
 * Architecture position:
 *   geo_net → [THIS FILE] → geo_cylinder → pogls_geomatrix → pogls_qrpn
 *
 * Piece = 25B stateless routing unit
 *   geo_key  uint64  8B  fibo_addr(origin_seed)
 *   shape    uint8   1B  axis→shape map
 *   bond_L   uint64  8B  fibo_addr(geo_key ^ BOND_SALT_L)
 *   bond_R   uint64  8B  fibo_addr(geo_key ^ BOND_SALT_R)
 *
 * Intrinsic bond_key = bond_L XOR bond_R   (8B, self-enforcing)
 * If coordinate shifts → bond_key changes → bond invalid automatically
 * ─────────────────────────────────────────────────────────────
 */

#ifndef POGLS_BOND_H
#define POGLS_BOND_H

#include <stdint.h>
#include <string.h>
#include "pogls_config.h"

/* ── CONSTANTS ─────────────────────────────────────────────── */

/* Sacred geometry seed — digit sum = 9 */
#define POGLS_GEO_MAGIC     UINT64_C(0x00120090024005A0)  /* 18/144/576/1440 packed */
#define POGLS_FNV_PRIME     UINT64_C(0x00000100000001B3)
#define POGLS_FNV_OFFSET    UINT64_C(0xCBF29CE484222325)
#define POGLS_BOND_SALT_L   UINT64_C(0xAAAAAAAAAAAAAAAA)
#define POGLS_BOND_SALT_R   UINT64_C(0x5555555555555555)
#define POGLS_FIBO_CLOCK    1440u   /* fibo cycle length */

/* Fibonacci table — first 16 terms (used in addr mixing) */
static const uint64_t POGLS_FIBO[16] = {
    1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987
};

/* Axis → Shape mapping (fold_axis 1–7 → I O T S Z L J) */
#define SHAPE_I  0x49u  /* 'I' */
#define SHAPE_O  0x4Fu  /* 'O' */
#define SHAPE_T  0x54u  /* 'T' */
#define SHAPE_S  0x53u  /* 'S' */
#define SHAPE_Z  0x5Au  /* 'Z' */
#define SHAPE_L  0x4Cu  /* 'L' */
#define SHAPE_J  0x4Au  /* 'J' */

static const uint8_t POGLS_AXIS_SHAPE[8] = {
    0,                /* axis 0 = unused */
    SHAPE_I,          /* axis 1 → I  pipe / linear passthrough  */
    SHAPE_O,          /* axis 2 → O  latch / buffer             */
    SHAPE_T,          /* axis 3 → T  splitter / fan-out         */
    SHAPE_S,          /* axis 4 → S  transpose / cross-swap     */
    SHAPE_Z,          /* axis 5 → Z  invert / reverse-swap      */
    SHAPE_L,          /* axis 6 → L  fork-left / branch         */
    SHAPE_J,          /* axis 7 → J  fork-right / branch        */
};

/* Omega fallback shapes */
#define OMEGA_COMPRESS    SHAPE_I   /* overflow  → compress & continue */
#define OMEGA_RETRY       SHAPE_O   /* fault     → hold & respawn      */
#define OMEGA_QUARANTINE  SHAPE_L   /* mismatch  → dead-letter lane    */
#define OMEGA_WAIT        SHAPE_T   /* upstream  → fan-in hold         */

/* ── PIECE STRUCT (25B) ─────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t geo_key;   /* 8B  fibo_addr of origin seed          */
    uint8_t  shape;     /* 1B  axis-mapped shape char (I/O/T...) */
    uint64_t bond_L;    /* 8B  fibo_addr(geo_key ^ SALT_L)       */
    uint64_t bond_R;    /* 8B  fibo_addr(geo_key ^ SALT_R)       */
} PoglsPiece;           /* total = 25B                           */

/* Bond pair — intrinsic, permanent */
typedef struct {
    uint64_t bond_key;  /* bond_L XOR bond_R — self-enforcing    */
    uint8_t  valid;     /* 1 = coordinates unchanged             */
} PoglsBond;

/* Extrinsic plug — hot-swap connector */
#define PLUG_FACE_N  0
#define PLUG_FACE_S  1
#define PLUG_FACE_E  2
#define PLUG_FACE_W  3

typedef struct {
    uint32_t target_id; /* connected piece id  */
    uint8_t  face;      /* N/S/E/W             */
    uint16_t ttl;       /* hops before expire  */
    uint8_t  active;    /* 1 = connected       */
} PoglsPlug;

/* Full slot — piece + 4 plugs */
typedef struct {
    PoglsPiece piece;
    PoglsPlug  plugs[4];   /* N S E W           */
    uint32_t   agent_id;
    uint32_t   token_cap;
    uint8_t    rerouted;   /* 0=normal, else Ω  */
} PoglsSlot;

/* ── CORE: fibo_addr ────────────────────────────────────────── */
/*
 * fibo_addr(seed) → uint64
 *
 * 3-pass mixing:
 *   pass 1: FNV-64 avalanche
 *   pass 2: Fibonacci lane XOR (16 terms)
 *   pass 3: geo_magic fold + rotate
 *
 * One-direction: seed → addr only, no inverse.
 * Coordinate-bound: same seed always → same addr.
 */
static inline uint64_t pogls_fibo_addr(uint64_t seed) {
    /* pass 1 — FNV-64 */
    uint64_t h = POGLS_FNV_OFFSET;
    uint8_t  b[8];
    memcpy(b, &seed, 8);
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)b[i];
        h *= POGLS_FNV_PRIME;
    }

    /* pass 2 — Fibonacci lane XOR */
    for (int i = 0; i < 16; i++) {
        h ^= POGLS_FIBO[i] * (seed >> (i & 7));
        h  = (h << 13) | (h >> 51);   /* rotate left 13 */
    }

    /* pass 3 — geo_magic fold */
    h ^= POGLS_GEO_MAGIC;
    h *= POGLS_FNV_PRIME;
    h ^= h >> 33;

    return h;
}

/* ── PIECE FACTORY ──────────────────────────────────────────── */
/*
 * pogls_make_piece(origin_seed, fold_axis) → PoglsPiece
 *
 * geo_key  = fibo_addr(origin_seed)
 * shape    = AXIS_SHAPE[fold_axis]
 * bond_L   = fibo_addr(geo_key ^ BOND_SALT_L)
 * bond_R   = fibo_addr(geo_key ^ BOND_SALT_R)
 */
static inline PoglsPiece pogls_make_piece(uint64_t origin_seed, uint8_t fold_axis) {
    PoglsPiece p;
    p.geo_key = pogls_fibo_addr(origin_seed);
    p.shape   = (fold_axis < 8) ? POGLS_AXIS_SHAPE[fold_axis] : SHAPE_I;
    p.bond_L  = pogls_fibo_addr(p.geo_key ^ POGLS_BOND_SALT_L);
    p.bond_R  = pogls_fibo_addr(p.geo_key ^ POGLS_BOND_SALT_R);
    return p;
}

/* ── BOND OPERATIONS ────────────────────────────────────────── */

/*
 * pogls_bond_key(piece) → uint64
 * intrinsic bond = bond_L XOR bond_R
 * If piece moves to different coordinate → geo_key changes
 * → bond_L, bond_R both change → bond_key changes → bond breaks
 */
static inline uint64_t pogls_bond_key(const PoglsPiece *p) {
    return p->bond_L ^ p->bond_R;
}

/*
 * pogls_bond_verify(a, b) → PoglsBond
 *
 * v1.1 — hardened bond verification:
 *   Old: top 16 bits == 0x9009  → 1/65536 false positive rate
 *   New: POGLS_BOND_VERIFY_BITS mask match (default 32-bit)
 *        → 1/4B false positive rate (2000× improvement)
 *
 * Nonce injection: session nonce XOR'd into mixing pass.
 *   - nonce=0 (default): backward-compatible, no replay protection
 *   - nonce≠0: bond valid only within same session/deployment
 *     Set via pogls_config_set_nonce() at startup.
 *
 * Verification path:
 *   ka       = bond_L(a) XOR bond_R(a)
 *   kb       = bond_L(b) XOR bond_R(b)
 *   raw_xor  = ka XOR kb
 *   nonce_mix= fibo_addr(raw_xor XOR nonce)   ← nonce injected here
 *   combined = fibo_addr(nonce_mix XOR raw_xor) ← double-pass avalanche
 *   valid    = (combined & VERIFY_MASK) == VERIFY_TARGET
 *
 * Double fibo_addr pass ensures full avalanche even when nonce=0.
 * bond_key exposed as raw_xor (pre-nonce) for downstream use.
 */
static inline PoglsBond pogls_bond_verify(const PoglsPiece *a, const PoglsPiece *b) {
    PoglsBond bond;
    uint64_t  ka      = pogls_bond_key(a);
    uint64_t  kb      = pogls_bond_key(b);
    uint64_t  raw_xor = ka ^ kb;

    /* pass 1: nonce injection */
    uint64_t  nonce     = pogls_config_get_nonce();
    uint64_t  nonce_mix = pogls_fibo_addr(raw_xor ^ nonce);

    /* pass 2: full avalanche — double fibo ensures diffusion */
    uint64_t  combined  = pogls_fibo_addr(nonce_mix ^ raw_xor);

    bond.bond_key = raw_xor;   /* pre-nonce, stable cross-session reference */
    bond.valid    = ((combined & POGLS_BOND_VERIFY_MASK) == POGLS_BOND_VERIFY_TARGET)
                    ? 1 : 0;
    return bond;
}

/* ── REROUTE (Ω substitution) ──────────────────────────────── */

typedef enum {
    POGLS_OK         = 0,
    POGLS_OVERFLOW   = 1,   /* → OMEGA_COMPRESS    */
    POGLS_FAULT      = 2,   /* → OMEGA_QUARANTINE  */
    POGLS_UPSTREAM   = 3,   /* → OMEGA_WAIT        */
    POGLS_RETRY      = 4,   /* → OMEGA_RETRY       */
} PoglsFault;

static const uint8_t POGLS_FAULT_SHAPE[5] = {
    SHAPE_I,          /* OK          — no change    */
    OMEGA_COMPRESS,   /* OVERFLOW    → I            */
    OMEGA_QUARANTINE, /* FAULT       → L            */
    OMEGA_WAIT,       /* UPSTREAM    → T            */
    OMEGA_RETRY,      /* RETRY       → O            */
};

/*
 * pogls_reroute(slot, fault) → mutates slot in place
 * Substitutes shape + remixes geo_key via fibo fold
 * Preserves bond_L/bond_R from original (bond survives reroute)
 */
static inline void pogls_reroute(PoglsSlot *slot, PoglsFault fault) {
    if (fault == POGLS_OK) return;
    slot->piece.shape  = POGLS_FAULT_SHAPE[fault];
    slot->piece.geo_key = pogls_fibo_addr(slot->piece.geo_key ^ (uint64_t)fault);
    slot->rerouted     = (uint8_t)fault;
}

/* ── EXTRINSIC PLUG ─────────────────────────────────────────── */

static inline void pogls_plug_connect(PoglsSlot *a, uint8_t face_a,
                                      PoglsSlot *b, uint8_t face_b,
                                      uint16_t ttl) {
    if (face_a > 3 || face_b > 3) return;
    a->plugs[face_a].target_id = b->agent_id;
    a->plugs[face_a].face      = face_b;
    a->plugs[face_a].ttl       = ttl;
    a->plugs[face_a].active    = 1;
    /* symmetric */
    b->plugs[face_b].target_id = a->agent_id;
    b->plugs[face_b].face      = face_a;
    b->plugs[face_b].ttl       = ttl;
    b->plugs[face_b].active    = 1;
}

static inline void pogls_plug_disconnect(PoglsSlot *slot, uint8_t face) {
    if (face > 3) return;
    memset(&slot->plugs[face], 0, sizeof(PoglsPlug));
}

/* ── SESSION SEED from wallet topology_fp ───────────────────── */
/*
 * wallet topology_fp (hex string 16 chars) → uint64 origin_seed
 * Used to derive piece geo_key from Python-side coord_wallet
 */
static inline uint64_t pogls_seed_from_fp(const char *topology_fp) {
    uint64_t seed = POGLS_FNV_OFFSET;
    for (int i = 0; topology_fp[i] && i < 16; i++) {
        seed ^= (uint64_t)(unsigned char)topology_fp[i];
        seed *= POGLS_FNV_PRIME;
    }
    return pogls_fibo_addr(seed);
}

#endif /* POGLS_BOND_H */
