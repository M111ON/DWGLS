/* test_anchor_bond_link.c — self-check for include/anchor_bond_link.h.
 *
 * Oracles: token shape (52 letters x weights 00/07/16 round-trip byte-exact),
 * pair rule (A[12]/a[07] join at pair level, exact only on full equality),
 * bond rel + side-log line round-trips, malformed rejections.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "anchor_bond_link.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; }
    else { g_fail++; printf("  FAIL: %s\n", name); }
}

static void t_token_roundtrip(void) {
    int ok = 1;
    for (int p = 0; p < 26 && ok; p++)
        for (int u = 0; u < 2 && ok; u++)
            for (int wi = 0; wi < 3 && ok; wi++) {
                int w = (int[]){0, 7, 16}[wi];
                char t[ABL_TOKEN_CAP];
                if (abl_render(p, w, u, t) != 0) { ok = 0; break; }
                int rp, rw, ru;
                if (abl_parse(t, &rp, &rw, &ru) != 0) { ok = 0; break; }
                if (rp != p || rw != w || ru != u) { ok = 0; break; }
                char want[ABL_TOKEN_CAP];
                snprintf(want, sizeof want, "%c[%02d]",
                         (char)((u ? 'A' : 'a') + p), w);
                if (strcmp(t, want) != 0) { ok = 0; break; }
            }
    check(ok, "52 letters x {00,07,16} render/parse round-trip byte-exact");
}

static void t_token_rejects(void) {
    char t[ABL_TOKEN_CAP];
    int p, w, u, bad = 0;
    bad += (abl_render(-1, 5, 1, t) == -1);
    bad += (abl_render(26, 5, 1, t) == -1);
    bad += (abl_render(0, 17, 1, t) == -1);
    bad += (abl_render(0, -1, 1, t) == -1);
    bad += (abl_render(0, 5, 1, NULL) == -1);
    bad += (abl_parse("A[17]", &p, &w, &u) == -1);
    bad += (abl_parse("A[5]", &p, &w, &u) == -1);
    bad += (abl_parse("A[05]x", &p, &w, &u) == -1);
    bad += (abl_parse("[05]", &p, &w, &u) == -1);
    bad += (abl_parse("1[05]", &p, &w, &u) == -1);
    bad += (abl_parse("A[0x]", &p, &w, &u) == -1);
    bad += (abl_parse(NULL, &p, &w, &u) == -1);
    bad += (abl_parse("a[16] ", &p, &w, &u) == -1);
    check(bad == 13, "13/13 malformed tokens rejected");
}

static void t_bond_rel(void) {
    char rel[ABL_REL_CAP], tok[ABL_TOKEN_CAP];
    int ok = 1;
    if (abl_bond_rel("A[12]", rel) != 0) ok = 0;
    if (strcmp(rel, "x-letter:A[12]") != 0) ok = 0;
    if (abl_rel_token(rel, tok) != 0 || strcmp(tok, "A[12]") != 0) ok = 0;
    /* no spaces: survives pool's 3-part bond split */
    if (strchr(rel, ' ') != NULL) ok = 0;
    check(ok, "bond rel x-letter:A[12] round-trips, spaceless");
    int bad = 0;
    bad += (abl_bond_rel("A[17]", rel) == -1);
    bad += (abl_bond_rel("uses", rel) == -1);
    bad += (abl_rel_token("uses", tok) == -1);
    bad += (abl_rel_token("x-letter:A[17]", tok) == -1);
    bad += (abl_rel_token(NULL, tok) == -1);
    check(bad == 5, "5/5 bad rels rejected");
}

static void t_log_line(void) {
    char line[64], tok[ABL_TOKEN_CAP];
    uint32_t r, s;
    int ok = 1;
    if (abl_log_render(12, 45, "A[12]", line, sizeof line) != 0) ok = 0;
    if (strcmp(line, "12 45 A[12]") != 0) ok = 0;
    if (abl_log_parse(line, &r, &s, tok) != 0) ok = 0;
    if (r != 12 || s != 45 || strcmp(tok, "A[12]") != 0) ok = 0;
    check(ok, "side-log line 12 45 A[12] round-trips");
    int bad = 0;
    bad += (abl_log_parse("12 45 A[12] ", &r, &s, tok) == -1);  /* padding */
    bad += (abl_log_parse("12  45 A[12]", &r, &s, tok) == -1);  /* padding */
    bad += (abl_log_parse("12 A[12]", &r, &s, tok) == -1);
    bad += (abl_log_parse("x y A[12]", &r, &s, tok) == -1);
    bad += (abl_log_parse("12 45 uses", &r, &s, tok) == -1);
    bad += (abl_log_parse(NULL, &r, &s, tok) == -1);
    check(bad == 6, "6/6 malformed log lines rejected");
}

static void t_join(void) {
    int ok = 1;
    if (abl_join("A[12]", "A[12]") != 1) ok = 0;   /* same walk, same edge */
    if (abl_join("A[12]", "a[07]") != 2) ok = 0;   /* Aa pair, weight differs */
    if (abl_join("a[07]", "A[12]") != 2) ok = 0;   /* symmetric */
    if (abl_join("A[12]", "B[12]") != 0) ok = 0;   /* different pairs */
    check(abl_join("A[12]", "A[13]") == 2, "same pair different weight -> pair-linked");
    if (abl_join("uses", "A[12]") != 0) ok = 0;
    if (abl_join(NULL, "A[12]") != 0) ok = 0;
    check(ok, "join: exact=1, pair=2, else 0 (malformed safe)");
}

static void t_from_anchor(void) {
    /* token from a real built anchor == manual render of its parts */
    uint8_t s[32];
    for (int i = 0; i < 32; i++) s[i] = (uint8_t)(i * 3 + 1);
    aw_anchor_t a;
    aw_anchor_build(s, sizeof s, 1, &a);
    char t1[ABL_TOKEN_CAP], t2[ABL_TOKEN_CAP];
    int ok = 1;
    if (abl_from_anchor(&a, t1) != 0) ok = 0;
    if (abl_render(aw_pair_id(&a), a.weight[AW_CENTER], a.is_upper, t2) != 0) ok = 0;
    if (strcmp(t1, t2) != 0) ok = 0;
    if (abl_from_anchor(NULL, t1) == 0) ok = 0;
    check(ok, "token from built anchor matches its (pair, centre-[##], case)");
}

int main(void) {
    printf("test_anchor_bond_link — centre letter joins bond and tombstone\n\n");
    t_token_roundtrip();
    t_token_rejects();
    t_bond_rel();
    t_log_line();
    t_join();
    t_from_anchor();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
