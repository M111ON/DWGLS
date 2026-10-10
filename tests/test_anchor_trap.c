/* test_anchor_trap.c — self-check for include/anchor_trap.h.
 *
 * Oracles: empty-register shape; first catch defines from passerby bytes
 * (token == abl_from_anchor of the trap's anchor); same passerby verifies
 * distance 0; different passerby > 0; arrival-dependence (same seed,
 * different first passerby -> different letters); head shape (3 parts,
 * dst pending, kind constant) + completion; fingerprint prefix == inline
 * FNV + round-trip.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "anchor_trap.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; }
    else { g_fail++; printf("  FAIL: %s\n", name); }
}

static uint64_t oracle_fnv(const uint8_t *b, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

static void t_register_empty(void) {
    uint8_t seed[AT_SEED_LEN];
    fill(seed, sizeof seed, 7u);
    at_trap_t t;
    int ok = (at_register(seed, &t) == 0);
    if (t.defined != 0 || t.catches != 0) ok = 0;
    if (memcmp(t.seed, seed, sizeof seed) != 0) ok = 0;
    check(ok, "register holds seed, anchor undefined, zero catches (no RDH)");
    check(at_register(NULL, &t) == -1 && at_register(seed, NULL) == -1,
          "register rejects NULL");
}

static void t_first_catch_defines(void) {
    uint8_t seed[AT_SEED_LEN], pb[48];
    fill(seed, sizeof seed, 1u);
    fill(pb, sizeof pb, 0xCA7Cu);
    at_trap_t t;
    at_register(seed, &t);
    at_fp_t fp;
    int dist = -9;
    int rc = at_catch(&t, pb, sizeof pb, 1, 64, &fp, &dist);
    int ok = (rc == 1 && t.defined == 1 && t.catches == 1 && dist == 0);
    char want[ABL_TOKEN_CAP];
    if (abl_from_anchor(&t.anchor, want) != 0) ok = 0;
    if (strcmp(fp.token, want) != 0) ok = 0;
    if (fp.pass != 1 || fp.prefix != oracle_fnv(pb, sizeof pb)) ok = 0;
    if (t.first_prefix != oracle_fnv(pb, sizeof pb)) ok = 0;
    check(ok, "first catch paints from passerby bytes (token/prefix/pass)");
}

static void t_verify_same_and_other(void) {
    uint8_t seed[AT_SEED_LEN], pa[48], pb[48];
    fill(seed, sizeof seed, 2u);
    fill(pa, sizeof pa, 0x1111u);
    fill(pb, sizeof pb, 0x2222u);
    at_trap_t t;
    at_register(seed, &t);
    at_catch(&t, pa, sizeof pa, 0, 16, NULL, NULL);
    int d_same = -1, d_other = -1, rc = 0;
    rc = at_catch(&t, pa, sizeof pa, 0, 16, NULL, &d_same);
    int ok = (rc == 0 && d_same == 0 && t.catches == 2);
    rc = at_catch(&t, pb, sizeof pb, 0, 16, NULL, &d_other);
    if (!(rc == 0 && d_other > 0 && t.catches == 3)) ok = 0;
    check(ok, "same passerby verifies 0, other passerby > 0");
    printf("  [measure] other-passerby centre distance (rung16) = %d bits\n", d_other);
}

static void t_arrival_dependent(void) {
    /* same seed, different first passerby -> different identity.
     * This is the price of passive (documented in the header). */
    uint8_t seed[AT_SEED_LEN], pa[48], pb[48];
    fill(seed, sizeof seed, 3u);
    fill(pa, sizeof pa, 0xAAAAu);
    fill(pb, sizeof pb, 0xBBBBBBu & 0xFFFFFFu);
    at_trap_t t1, t2;
    at_register(seed, &t1);
    at_register(seed, &t2);
    at_catch(&t1, pa, sizeof pa, 0, 64, NULL, NULL);
    at_catch(&t2, pb, sizeof pb, 0, 64, NULL, NULL);
    check(t1.anchor.letter != t2.anchor.letter,
          "same seed different first passerby -> different letters");
    printf("  [measure] letters: 0x%02x vs 0x%02x\n",
           t1.anchor.letter, t2.anchor.letter);
}

static void t_head_shape(void) {
    char src[65], dst[65];
    memset(src, 'a', 64); src[64] = '\0';
    memset(dst, 'b', 64 - 56); memset(dst + 8, 'c', 56); dst[64] = '\0';
    int hexok = 1;
    for (int i = 0; i < 64; i++)
        if (!((dst[i] >= '0' && dst[i] <= '9') || (dst[i] >= 'a' && dst[i] <= 'f')))
            hexok = 0;
    char head[160], full[160];
    int ok = hexok;
    if (at_head_text(src, "A[12]", head, sizeof head) != 0) ok = 0;
    /* 3 parts, dst pending, kind constant for the host's append call */
    char h1[65], h2[32], h3[8];
    if (sscanf(head, "%64s %31s %7s", h1, h2, h3) != 3) ok = 0;
    if (strcmp(h1, src) != 0 || strcmp(h2, "x-letter:A[12]") != 0) ok = 0;
    if (strcmp(h3, "-") != 0) ok = 0;
    if (strcmp(AT_HEAD_KIND, "bond-head") != 0) ok = 0;
    if (at_head_complete(src, "A[12]", dst, full, sizeof full) != 0) ok = 0;
    char f1[65], f2[32], f3[65];
    if (sscanf(full, "%64s %31s %64s", f1, f2, f3) != 3) ok = 0;
    if (strcmp(f3, dst) != 0) ok = 0;
    check(ok, "head: 3-part, dst pending; completion fills dst");
    int bad = 0;
    bad += (at_head_text("xyz", "A[12]", head, sizeof head) == -1);
    bad += (at_head_text(src, "uses", head, sizeof head) == -1);
    bad += (at_head_text(NULL, "A[12]", head, sizeof head) == -1);
    bad += (at_head_complete(src, "A[12]", "xyz", full, sizeof full) == -1);
    check(bad == 4, "4/4 bad head args rejected");
}

static void t_fp_roundtrip(void) {
    at_fp_t fp;
    fp.pass = 7;
    fp.prefix = 0x0123456789abcdefULL;
    snprintf(fp.token, sizeof fp.token, "%s", "a[07]");
    char line[64];
    int ok = 1;
    if (at_fp_render(&fp, line, sizeof line) != 0) ok = 0;
    if (strcmp(line, "7 0123456789abcdef a[07]") != 0) ok = 0;
    at_fp_t back;
    if (at_fp_parse(line, &back) != 0) ok = 0;
    if (back.pass != 7 || back.prefix != fp.prefix || strcmp(back.token, "a[07]") != 0) ok = 0;
    check(ok, "fingerprint renders/parses byte-exact");
    int bad = 0;
    bad += (at_fp_parse("7 0123456789abcdef a[07] ", &back) == -1);
    bad += (at_fp_parse("7 a[07]", &back) == -1);
    bad += (at_fp_parse(NULL, &back) == -1);
    check(bad == 3, "3/3 malformed fingerprints rejected");
}

int main(void) {
    printf("test_anchor_trap — RDH passive: trap, paint, head, fingerprint\n\n");
    t_register_empty();
    t_first_catch_defines();
    t_verify_same_and_other();
    t_arrival_dependent();
    t_head_shape();
    t_fp_roundtrip();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
