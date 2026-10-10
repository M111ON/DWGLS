/* anchor_bond_link.h — the centre letter IS the bond<->tombstone link.
 *
 * Owner rule (2026-10-10): the anchor's centre letter (pair + weight + case)
 * is the shared key between the bond graph and the tombstone. A bond edge
 * and a walked-path record that carry the SAME letter are the same edge
 * seen from two sides — structure (bond) and footprints (tombstone) join
 * on the token, never on content.
 *
 * Carriers (chosen to fit TODAY's formats, no live change):
 *   - bond side: pool bond text is "<src64> <rel> <dst64>" and rel accepts
 *     the open `x-<free>` slot, so the letter rides as one spaceless token:
 *       rel = "x-letter:A[12]"
 *     pool.py needs NO change (x- is the designed escape hatch).
 *   - tombstone side: the P3 route-log record is fixed 32-byte binary
 *     (latch id, name hash, round, seq, action) with NO free field, so the
 *     letter rides a SIDE log, one text line per traverse:
 *       "<round> <seq> <token>"      e.g. "12 45 A[12]"
 *     round+seq are the same keys the P3 record carries, so the side log
 *     joins the binary log without touching its format or replay.
 *   - join: bond-rel token == side-log token  -> LINKED (same walk, same edge).
 *     same pair, different token (Aa, weight differs) -> PAIR-LINKED
 *     (the case-pair relation). Otherwise unlinked.
 *
 * Token shape: "A[12]" — pair letter A-Z/a-z (case = side) + [00..16]
 * weight, 2 digits. Pair identity is case-folded (letter % 26).
 *
 * NOT built here (explicitly later, each a live touch):
 *   - appending the side log from wl_traverse_log call sites (ann server);
 *   - pool.py changes (none needed — x- already allowed);
 *   - letter labels in kv_index_section `file` (third hanging point).
 *
 * Header-only, std-only. Depends on anchor_weight.h for pair/weight/case.
 */
#ifndef ANCHOR_BOND_LINK_H
#define ANCHOR_BOND_LINK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "anchor_weight.h"   /* AW_PAIRS, aw_pair_id, aw_anchor_t */

#define ABL_TOKEN_CAP 8      /* "a[16]" + NUL */
#define ABL_REL_CAP   24     /* "x-letter:" + token + NUL */
#define ABL_REL_PREFIX "x-letter:"

/* ── token: pair + weight + side -> "A[12]" ──
 * pair 0..25, weight 0..16, is_upper != 0 -> 'A'..'Z' else 'a'..'z'.
 * Returns 0 ok, -1 bad args. */
static inline int abl_render(int pair, int weight, int is_upper,
                             char out[ABL_TOKEN_CAP]) {
    if (!out || pair < 0 || pair >= (int)AW_PAIRS || weight < 0 || weight > 16)
        return -1;
    char L = (char)((is_upper ? 'A' : 'a') + pair);
    snprintf(out, ABL_TOKEN_CAP, "%c[%02d]", L, weight);
    return 0;
}

/* ── token -> pair + weight + side. Returns 0 ok, -1 malformed.
 * Accepts exactly L[dd] (one letter, two digits, nothing else). */
static inline int abl_parse(const char *tok, int *pair, int *weight,
                            int *is_upper) {
    if (!tok || strlen(tok) != 5) return -1;
    char L = tok[0];
    int up;
    if (L >= 'A' && L <= 'Z') up = 1;
    else if (L >= 'a' && L <= 'z') up = 0;
    else return -1;
    if (tok[1] != '[' || tok[4] != ']') return -1;
    if (tok[2] < '0' || tok[2] > '9' || tok[3] < '0' || tok[3] > '9')
        return -1;
    int w = (tok[2] - '0') * 10 + (tok[3] - '0');
    if (w > 16) return -1;
    int p = (L - (up ? 'A' : 'a')) % (int)AW_PAIRS;
    if (pair) *pair = p;
    if (weight) *weight = w;
    if (is_upper) *is_upper = up;
    return 0;
}

/* ── token from a built anchor (centre letter + its [##] + case) ── */
static inline int abl_from_anchor(const aw_anchor_t *a, char out[ABL_TOKEN_CAP]) {
    if (!a) return -1;
    int pair = aw_pair_id(a);   /* already case-folded */
    return abl_render(pair, a->weight[AW_CENTER], a->is_upper, out);
}

/* ── bond rel: "x-letter:A[12]". Fits pool bond() untouched. ── */
static inline int abl_bond_rel(const char *tok, char out[ABL_REL_CAP]) {
    if (!tok || !out) return -1;
    int p, w, u;
    if (abl_parse(tok, &p, &w, &u) != 0) return -1;
    snprintf(out, ABL_REL_CAP, "%s%s", ABL_REL_PREFIX, tok);
    return 0;
}

/* ── rel -> token (strip prefix, validate). Returns 0 ok, -1 not-a-letter-rel. */
static inline int abl_rel_token(const char *rel, char out[ABL_TOKEN_CAP]) {
    size_t n = sizeof(ABL_REL_PREFIX) - 1;
    if (!rel || strncmp(rel, ABL_REL_PREFIX, n) != 0) return -1;
    const char *tok = rel + n;
    int p, w, u;
    if (abl_parse(tok, &p, &w, &u) != 0) return -1;
    snprintf(out, ABL_TOKEN_CAP, "%s", tok);
    return 0;
}

/* ── side-log line: "<round> <seq> <token>". Joins the P3 binary log on
 * (round, seq) — the keys both records carry. ── */
static inline int abl_log_render(uint32_t round, uint32_t seq, const char *tok,
                                 char *out, size_t cap) {
    if (!tok || !out || cap == 0) return -1;
    int p, w, u;
    if (abl_parse(tok, &p, &w, &u) != 0) return -1;
    snprintf(out, cap, "%u %u %s", round, seq, tok);
    return 0;
}

static inline int abl_log_parse(const char *line, uint32_t *round,
                                uint32_t *seq, char tok[ABL_TOKEN_CAP]) {
    if (!line) return -1;
    unsigned r, s;
    char t[ABL_TOKEN_CAP + 8];
    if (sscanf(line, "%u %u %15s", &r, &s, t) != 3) return -1;
    int p, w, u;
    if (abl_parse(t, &p, &w, &u) != 0) return -1;
    /* exact single-space shape: re-render and compare (rejects padding) */
    char canon[64];
    snprintf(canon, sizeof canon, "%u %u %s", r, s, t);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n != strlen(canon) || strncmp(line, canon, n) != 0) return -1;
    if (round) *round = r;
    if (seq) *seq = s;
    if (tok) snprintf(tok, ABL_TOKEN_CAP, "%s", t);
    return 0;
}

/* ── join: 0 = unlinked, 1 = LINKED (same token), 2 = PAIR-LINKED
 * (same pair, token differs: Aa / weight differs). ── */
static inline int abl_join(const char *bond_tok, const char *log_tok) {
    int p1, w1, u1, p2, w2, u2;
    if (!bond_tok || !log_tok) return 0;
    if (abl_parse(bond_tok, &p1, &w1, &u1) != 0) return 0;
    if (abl_parse(log_tok, &p2, &w2, &u2) != 0) return 0;
    if (strcmp(bond_tok, log_tok) == 0) return 1;
    if (p1 == p2) return 2;
    return 0;
}

#endif /* ANCHOR_BOND_LINK_H */
