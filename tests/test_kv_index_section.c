/* test_kv_index_section.c — oracle checks for include/kv_index_section.h.
 *
 * The seam under test (#7527): KV frames -> ann index-section entries.
 * Oracles used (independent of the header where it matters):
 *   - sid: 32 hex chars, deterministic, turn/content-sensitive (recomputed
 *     expectation: call kvs_sid twice + vary inputs; no self-agreement).
 *   - anchor/anchor2: MUST equal anch_route(vec,C,K,dim,2,rb) called DIRECTLY
 *     here with a full-K scratch buffer (the 097827a lesson: out holds K).
 *   - JSONL round-trip: emit -> parse -> memcmp vecs bitwise (%.9g carries
 *     every finite float exactly through strtof).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "kv_index_section.h"

#define TK 8      /* table buckets */
#define TD 16     /* test dim (small; shape is dim-agnostic) */
#define TN 24     /* frames (section shape is cadence-agnostic; the anchor
                     rule itself lives in kv_temporal.h, tested there) */

static int passes, failures;
#define CHECK(cond, msg) do { \
    if (cond) { passes++; } \
    else { failures++; printf("  FAIL: %s\n", msg); } \
} while (0)

static int is_hex32(const char *s) {
    if (strlen(s) != 32) return 0;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* deterministic synthetic table + vecs (no RNG, no model) */
static void make_table(float *C) {
    for (int k = 0; k < TK; k++)
        for (int d = 0; d < TD; d++) {
            uint64_t x = (uint64_t)(k * 131 + d * 17 + 1) * 0x9E3779B97F4A7C15ULL;
            x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ULL; x ^= x >> 32;
            C[k * TD + d] = (float)((int32_t)(x & 0xFFFFFF) - 0x800000) / 8388608.0f;
        }
}

static void make_vec(int turn, float *v) {
    for (int d = 0; d < TD; d++) {
        uint64_t x = (uint64_t)(turn * 733 + d * 91 + 7) * 0xC2B2AE3D27D4EB4FULL;
        x ^= x >> 31; x *= 0x165667B19E3779F9ULL; x ^= x >> 27;
        v[d] = (float)((int32_t)(x & 0xFFFFFF) - 0x800000) / 8388608.0f;
    }
}

static void make_tokens(int turn, int32_t *t, int n) {
    for (int i = 0; i < n; i++) t[i] = turn * 1000 + i;
}

int main(void) {
    float C[TK * TD];
    make_table(C);

    /* ── 1. sid: shape + determinism + sensitivity ── */
    {
        int32_t t1[4]; make_tokens(3, t1, 4);
        char s1[KVS_SID_LEN + 1], s2[KVS_SID_LEN + 1], s3[KVS_SID_LEN + 1];
        char s4[KVS_SID_LEN + 1];
        int32_t t2[4]; make_tokens(4, t2, 4);          /* same shape, diff turn */
        int32_t t3[4]; memcpy(t3, t1, sizeof t1); t3[0] ^= 1; /* 1 token diff */
        kvs_sid(3, t1, 4, s1);
        kvs_sid(3, t1, 4, s2);
        kvs_sid(4, t2, 4, s3);
        kvs_sid(3, t3, 4, s4);
        CHECK(is_hex32(s1), "sid is 32 hex chars");
        CHECK(strcmp(s1, s2) == 0, "sid deterministic for same (turn,tokens)");
        CHECK(strcmp(s1, s3) != 0, "sid differs across turns");
        CHECK(strcmp(s1, s4) != 0, "sid differs on 1-token change");
        char se1[KVS_SID_LEN + 1], se2[KVS_SID_LEN + 1];
        kvs_sid(5, NULL, 0, se1);                       /* empty frame */
        kvs_sid(6, NULL, 0, se2);
        CHECK(is_hex32(se1) && strcmp(se1, se2) != 0,
              "empty frames still get distinct sids per turn");
    }

    /* ── 2. entries: fields + anchor cross-check vs direct anch_route ── */
    {
        kvs_entry_t ents[TN];
        float vecs[TN][TD];
        int32_t toks[TN][8];
        int ok = 1;
        for (int i = 0; i < TN; i++) {
            int turn = i + 1;
            int n = 1 + (turn % 8);
            make_tokens(turn, toks[i], n);
            make_vec(turn, vecs[i]);
            if (kvs_from_frame(turn, toks[i], n, vecs[i], TD,
                               C, TK, "sess9", &ents[i]) != 0) ok = 0;
            /* independent oracle: direct anch_route, full-K scratch */
            int rb[TK];
            int got = anch_route(vecs[i], C, TK, TD, 2, rb);
            if (got != 2 || ents[i].anchor != rb[0] || ents[i].anchor2 != rb[1])
                ok = 0;
            if (ents[i].anchor < 0 || ents[i].anchor >= TK ||
                ents[i].anchor2 < 0 || ents[i].anchor2 >= TK) ok = 0;
            if (ents[i].anchor == ents[i].anchor2) ok = 0;   /* K=8>1 */
            if (ents[i].npast != n) ok = 0;
            char want_file[KVS_FILE_CAP];
            snprintf(want_file, sizeof want_file, "sess9/t%d", turn);
            if (strcmp(ents[i].file, want_file) != 0) ok = 0;
            if (!is_hex32(ents[i].sid)) ok = 0;
            if (ents[i].dim != TD || ents[i].vec != vecs[i]) ok = 0;
        }
        CHECK(ok, "24 frames: fields + anchor/anchor2 == direct anch_route");
    }

    /* ── 3. default tag + K==1 collapse ── */
    {
        int32_t t[3]; make_tokens(1, t, 3);
        float v[TD]; make_vec(1, v);
        kvs_entry_t e;
        CHECK(kvs_from_frame(1, t, 3, v, TD, C, TK, "", &e) == 0 &&
              strcmp(e.file, "kv-frame/t1") == 0, "empty tag -> kv-frame/tN");
        CHECK(kvs_from_frame(1, t, 3, v, TD, C, TK, NULL, &e) == 0 &&
              strcmp(e.file, "kv-frame/t1") == 0, "NULL tag -> kv-frame/tN");
        float C1[TD]; memcpy(C1, C, sizeof C1);
        CHECK(kvs_from_frame(1, t, 3, v, TD, C1, 1, "s", &e) == 0 &&
              e.anchor == 0 && e.anchor2 == 0, "K==1: anchor==anchor2==0");
    }

    /* ── 4. JSONL round-trip, bitwise ── */
    {
        const char *path = "build/kv_section_tmp.jsonl";
        kvs_entry_t ents[TN];
        float vecs[TN][TD];
        int32_t toks[TN][8];
        for (int i = 0; i < TN; i++) {
            int turn = i + 1, n = 1 + (turn % 8);
            make_tokens(turn, toks[i], n);
            make_vec(turn, vecs[i]);
            kvs_from_frame(turn, toks[i], n, vecs[i], TD, C, TK, "sess9",
                           &ents[i]);
        }
        CHECK(kvs_emit_jsonl(path, ents, TN) == 0, "emit 24-entry section");
        FILE *fp = fopen(path, "r");
        CHECK(fp != NULL, "section file readable");
        int ok = (fp != NULL);
        char line[8192];
        for (int i = 0; ok && i < TN; i++) {
            kvs_entry_t back; float bv[TD];
            if (!fgets(line, sizeof line, fp)) { ok = 0; break; }
            if (kvs_parse_line(line, &back, bv, TD) != 0) { ok = 0; break; }
            if (strcmp(back.sid, ents[i].sid) != 0 ||
                strcmp(back.file, ents[i].file) != 0 ||
                back.npast != ents[i].npast ||
                back.anchor != ents[i].anchor ||
                back.anchor2 != ents[i].anchor2 ||
                back.dim != ents[i].dim ||
                memcmp(bv, vecs[i], sizeof bv) != 0) ok = 0;
        }
        if (fp) fclose(fp);
        remove(path);
        CHECK(ok, "round-trip: all fields equal, vecs bitwise identical");
    }

    /* ── 5. bad args + malformed lines ── */
    {
        int32_t t[2]; make_tokens(1, t, 2);
        float v[TD]; make_vec(1, v);
        kvs_entry_t e;
        int bad = 0;
        bad += (kvs_from_frame(1, t, 2, v, TD, C, TK, "s", NULL) == -1);
        bad += (kvs_from_frame(1, t, 2, NULL, TD, C, TK, "s", &e) == -1);
        bad += (kvs_from_frame(1, t, 2, v, TD, NULL, TK, "s", &e) == -1);
        bad += (kvs_from_frame(1, t, 2, v, TD, C, 0, "s", &e) == -1);
        bad += (kvs_from_frame(1, t, 2, v, 0, C, TK, "s", &e) == -1);
        bad += (kvs_from_frame(1, NULL, 2, v, TD, C, TK, "s", &e) == -1);
        bad += (kvs_emit_jsonl(NULL, &e, 1) == -1);
        kvs_entry_t p; float pv[TD];
        bad += (kvs_parse_line("not json\n", &p, pv, TD) == -1);
        bad += (kvs_parse_line("{\"sid\":\"abc\"}\n", &p, pv, TD) == -1);
        CHECK(bad == 9, "9/9 bad-arg + malformed rejections");
    }

    printf("=== test_kv_index_section: %d PASS, %d FAIL ===\n",
           passes, failures);
    return failures ? 1 : 0;
}
