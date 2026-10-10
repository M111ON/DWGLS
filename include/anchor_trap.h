/* anchor_trap.h — RDH as a PASSIVE trap in the anchor (owner, 2026-10-10).
 *
 * Today an anchor build ACTIVELY fires 144 RDH captures (58 us) up front,
 * whether anything ever walks there or not. The trap inverts the cost:
 * register an EMPTY anchor (~free, no RDH fires), and let passersby paint
 * it — like a paint bucket over a door: whoever runs past shuts the door
 * and gets painted. Or an X-ray: the body passes through, only the image
 * is kept. RDH never fires on a schedule; it fires only when bytes pass.
 *
 * Rules:
 *   1. at_register(seed16): trap holds the seed + an undefined anchor.
 *      Cost ~0 (memcpy + zero). The seed selects the DOOR; the first
 *      passerby decides the COLOUR.
 *   2. First catch defines identity: patterns/letter/weight are built FROM
 *      THE PASSERBY's bytes (aw_anchor_build_w on them) at the trap's rung.
 *      Same cost as one active build — paid ONCE, at first pass, never upfront.
 *   3. Later catches verify cheap: ONE narrow centre-cell capture against
 *      the stored paint; returns the hamming distance (0 = same paint).
 *      ~4 RDH calls (~1.3 us) instead of a rebuild.
 *   4. Every catch registers a BOND HEAD with no tail yet:
 *      kind "bond-head", text "<src64> <x-letter:TOKEN> -".
 *      pool.py readers collect kind=="bond" with 3-part text only, so heads
 *      are INVISIBLE to all_bonds/bonds_for/explore until a tail completes
 *      them (at_head_complete: same line with dst filled in, written as an
 *      ordinary kind:"bond" line). pool.py needs NO change for heads to
 *      exist; it needs one INDEX_SKIP_KINDS entry when heads go live
 *      (explicitly later — machine records must skip the embedder).
 *   5. Every catch emits a FINGERPRINT (the X-ray image):
 *      "<pass#> <prefix16hex> <token>" — pass ordinal, FNV-1a of the
 *      passerby bytes, link token. The body is never stored.
 *
 * Honest trade (read before adopting): identity is ARRIVAL-DEPENDENT.
 * Two traps with the same seed but different first passersby get different
 * letters. Replay needs the same arrival order. What stays deterministic:
 * trap placement (seed), and everything after the first catch given the
 * same passerby sequence. This matches the tombstone philosophy (memory
 * #7502): learned by walking — a trap that never gets walked costs nothing
 * and means nothing.
 *
 * Header-only, std-only. Builds on anchor_weight.h (patterns/letter/weight)
 * and anchor_bond_link.h (token shape).
 */
#ifndef ANCHOR_TRAP_H
#define ANCHOR_TRAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "anchor_weight.h"    /* aw_anchor_t, aw_anchor_build_w */
#include "anchor_bond_link.h" /* ABL_TOKEN_CAP, ABL_REL_CAP, abl_from_anchor */

#define AT_SEED_LEN 16
#define AT_HEAD_KIND "bond-head"
#define AT_HEAD_PEND "-"

/* ── the trap ── */
typedef struct {
    uint8_t     seed[AT_SEED_LEN];  /* which door (placement, deterministic) */
    aw_anchor_t anchor;             /* the paint (undefined until first catch) */
    int         defined;            /* 0 = empty bucket, 1 = painted */
    uint32_t    catches;            /* passerby count (pass ordinal source) */
    uint64_t    first_prefix;       /* FNV-1a of the defining passerby */
} at_trap_t;

/* ── fingerprint (X-ray image of one pass) ── */
typedef struct {
    uint32_t pass;                  /* 1-based catch ordinal */
    uint64_t prefix;                /* FNV-1a over the passerby bytes */
    char     token[ABL_TOKEN_CAP];  /* link token at catch time */
} at_fp_t;

static inline uint64_t at_fnv(const uint8_t *b, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* register: no RDH fires. Returns 0 ok, -1 bad args. */
static inline int at_register(const uint8_t seed[AT_SEED_LEN], at_trap_t *out) {
    if (!seed || !out) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->seed, seed, AT_SEED_LEN);
    return 0;
}

/* catch: passerby bytes walk through. First catch paints (full rung build
 * from THEIR bytes); later catches verify (one narrow centre capture).
 * fp_out (optional) always filled: pass#, prefix, token-at-the-time.
 * verify_dist_out (optional): hamming between fresh narrow centre capture
 *   and stored paint (0 = same paint; -1 when trap still empty).
 * is_upper/width are caller choices (case side, rung).
 * Returns 1 = first catch (defined now), 0 = later catch, -1 = bad args. */
static inline int at_catch(at_trap_t *t, const uint8_t *bytes, size_t len,
                           uint8_t is_upper, int width, at_fp_t *fp_out,
                           int *verify_dist_out) {
    if (!t || (!bytes && len > 0) || aw_groups_for(width) < 0) return -1;
    if (verify_dist_out) *verify_dist_out = -1;
    int first = !t->defined;
    if (first) {
        if (aw_anchor_build_w(bytes, len, is_upper, width, &t->anchor) != 0)
            return -1;
        t->defined = 1;
        t->first_prefix = at_fnv(bytes, len);
    }
    t->catches++;
    /* verify at the STORED rung (same width both sides, direct XOR):
     * width 64 -> 16 captures, 16 -> 4, 4/1 -> 2. Always far below a
     * full 9-cell rebuild (144/36/18). Same passerby -> distance 0. */
    {
        int w = t->anchor.width ? t->anchor.width : 64;
        uint64_t fresh = aw_cell_pattern_w(bytes, len, (int)AW_CENTER, w);
        uint64_t x = fresh ^ t->anchor.pattern[AW_CENTER];
        int d = 0;
        while (x) { d += (int)(x & 1u); x >>= 1; }
        if (verify_dist_out) *verify_dist_out = d;
    }
    if (fp_out) {
        fp_out->pass = t->catches;
        fp_out->prefix = at_fnv(bytes, len);
        if (abl_from_anchor(&t->anchor, fp_out->token) != 0) return -1;
    }
    return first ? 1 : 0;
}

/* bond head (no tail yet): "<src64> <x-letter:TOK> -" under kind "bond-head".
 * src must be a full 64-hex line hash (same rule as pool bond()). The "-"
 * dst keeps the 3-part shape; readers that only collect kind=="bond" never
 * see heads. Returns 0 ok, -1 bad args. */
static inline int at_head_text(const char *src64, const char *tok,
                               char *out, size_t cap) {
    if (!src64 || !tok || !out || cap == 0) return -1;
    for (int i = 0; i < 64; i++) {
        char c = src64[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    if (src64[64] != '\0') return -1;
    char rel[ABL_REL_CAP];
    if (abl_bond_rel(tok, rel) != 0) return -1;
    snprintf(out, cap, "%s %s %s", src64, rel, AT_HEAD_PEND);
    return 0;
}

/* complete a head once the tail is known: same line with dst filled.
 * The completed line is written by the host as an ordinary kind:"bond" line. */
static inline int at_head_complete(const char *src64, const char *tok,
                                   const char *dst64, char *out, size_t cap) {
    if (!dst64 || !out || cap == 0) return -1;
    for (int i = 0; i < 64; i++) {
        char c = dst64[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    if (dst64[64] != '\0') return -1;
    char rel[ABL_REL_CAP];
    if (abl_bond_rel(tok, rel) != 0) return -1;
    if (!src64) return -1;
    snprintf(out, cap, "%s %s %s", src64, rel, dst64);
    return 0;
}

/* 16-hex render/parse without %llx (MinGW format gap — do it by hand). */
static inline void at_hex16(uint64_t v, char out[17]) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = d[(v >> (60 - i * 4)) & 0xF];
    out[16] = '\0';
}

static inline int at_unhex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else return -1;
        v = (v << 4) | (uint64_t)n;
    }
    if (out) *out = v;
    return 0;
}

/* fingerprint render/parse: "<pass> <prefix16hex> <token>". */
static inline int at_fp_render(const at_fp_t *fp, char *out, size_t cap) {
    if (!fp || !out || cap == 0) return -1;
    int p, w, u;
    if (abl_parse(fp->token, &p, &w, &u) != 0) return -1;
    char hx[17];
    at_hex16(fp->prefix, hx);
    snprintf(out, cap, "%u %s %s", fp->pass, hx, fp->token);
    return 0;
}

static inline int at_fp_parse(const char *line, at_fp_t *out) {
    if (!line || !out) return -1;
    unsigned ps;
    char xs[17], t[ABL_TOKEN_CAP + 8];
    if (sscanf(line, "%u %16s %15s", &ps, xs, t) != 3) return -1;
    if (strlen(xs) != 16) return -1;
    uint64_t px;
    if (at_unhex16(xs, &px) != 0) return -1;
    int p, w, u;
    if (abl_parse(t, &p, &w, &u) != 0) return -1;
    char hx[17];
    at_hex16(px, hx);
    char canon[64];
    snprintf(canon, sizeof canon, "%u %s %s", ps, hx, t);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n != strlen(canon) || strncmp(line, canon, n) != 0) return -1;
    out->pass = ps;
    out->prefix = (uint64_t)px;
    snprintf(out->token, sizeof out->token, "%s", t);
    return 0;
}

#endif /* ANCHOR_TRAP_H */
