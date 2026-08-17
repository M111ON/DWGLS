/* tools/ghost_delta_measure.c — §15.74: delta-mode ghost footprint vs thaw ตรง
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "ต่อ HyperDeltaEnt (pred+ent residual จาก T1.1d) เข้า ghost_read_rule:
 *        เมื่ออ่าน block ที่ freeze ผ่าน ghost ให้ replay route แล้ว materialize
 *        ข้อมูลด้วย delta แทนการ thaw payload เต็ม — วัด footprint เทียบ thaw ตรง"
 *
 * ทุก chunk ผ่าน chain เดียวกับระบบ: cap_rule_scale(i) → ghost_lift (raw)
 * เทียบ ghost_lift_delta (pred+ent adaptive) → อ่านกลับด้วย
 * ghost_read_rule_materialize (กฎเดียว §15.73) → พิสูจน์ lossless + วัด bytes.
 *
 * วัด 3 granularity: 64 B (geos block) · 1024 B · 16384 B (block ของ T1.1d)
 * — เปิดเผยว่า codebook+base overhead กลืนกำไรเมื่อ block เล็กแค่ไหน.
 *
 * BUILD: gcc -O2 -I. -Icore -o build/ghost_delta_measure tools/ghost_delta_measure.c -lm
 * RUN:   build/ghost_delta_measure --file <path> | --syn <kind> <n> | --gguf <model> <idx>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/geo_ghost_lift.h"
#include "core/geofs_core.h"   /* includes ghost_delta.h + geo_ghost_lift.h */
#include "gguf_reader.h"

#define MAXN (4u << 20)

static void synth_fill(uint8_t *x, uint32_t n, const char *kind) {
    uint32_t c = 512, r = n / c; if (r < 1) r = 1;
    if (strcmp(kind, "noise") == 0) {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; x[i] = (uint8_t)s; }
    } else if (strcmp(kind, "smooth") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(((i / c) * 255u / r + (i % c) * 255u / c) / 2);
    } else if (strcmp(kind, "sine2d") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(128 + 120 * sin((double)(i % c) * 0.1) * cos((double)(i / c) * 0.05));
    } else {
        for (uint32_t i = 0; i < n; i++) x[i] = (uint8_t)(i * 255u / n);
    }
}

static uint32_t next_pow2(uint32_t v) {
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* หนึ่ง granularity: stream x[n] เป็น chunk CHUNK → raw vs delta → read กลับ
   sample ถูก clamp ให้ nchunks ≤ GHOST_LOG_MAX (4096 — log limit จริงของระบบ) */
static void run_chunk(const char *name, const uint8_t *x, uint32_t n, uint32_t CHUNK) {
    uint32_t max_bytes = (uint32_t)GHOST_LOG_MAX * CHUNK;
    if (n > max_bytes) n = max_bytes;      /* วัดบน prefix ที่ log รับได้ */
    uint32_t nchunks = (n + CHUNK - 1) / CHUNK;
    uint32_t cap = next_pow2(nchunks + 1);
    if (cap < 64) cap = 64;

    GhostLog logA; ghost_log_init(&logA);
    GhostLog logB; ghost_log_init(&logB);
    ResidualSpace rsA, rsB;
    memset(&rsA, 0, sizeof(rsA)); memset(&rsB, 0, sizeof(rsB));
    rs_init(&rsA, cap); rs_init(&rsB, cap);
    GhostPairTable pA, pB;
    memset(&pA, 0, sizeof(pA)); memset(&pB, 0, sizeof(pB));
    ghost_pair_attach(&logA, &pA);
    ghost_pair_attach(&logB, &pB);

    uint64_t raw_bytes = 0, delta_bytes = 0;
    uint32_t delta_wins = 0, delta_loses = 0, delta_entries = 0;
    int all_ok = 1;

    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = i * CHUNK;
        uint32_t sz = n - off; if (sz > CHUNK) sz = CHUNK;
        uint8_t to = cap_rule_scale(i);
        /* raw space */
        if (ghost_lift(&logA, &rsA, (uint16_t)i, 0, to, x + off, sz)
            == RS_BOND_KEY_RESERVED) all_ok = 0;
        raw_bytes += sz;
        /* delta space */
        if (ghost_lift_delta(&logB, &rsB, (uint16_t)i, 0, to, x + off, sz)
            == RS_BOND_KEY_RESERVED) all_ok = 0;
        /* per-entry: entry เป็น delta หรือไม่ + ขนาดจริง */
        int idx = ghost_log_find(&logB, (uint16_t)i, 0, to);
        if (idx >= 0 && (logB.entries[idx].flags & GHOST_FLAG_DELTA)) {
            delta_entries++;
            uint32_t got = 0;
            PoglsPiece p = ghost_piece((uint16_t)i, 0, to);
            const void *blob = rs_thaw(&rsB, pogls_bond_key(&p), &got);
            uint32_t blen = ghost_delta_size((const uint8_t *)blob, got);
            delta_bytes += blen;
            if (blen < sz) delta_wins++; else delta_loses++;
        } else {
            delta_bytes += sz;
            delta_loses++;        /* raw fallback (delta ไม่ชนะ) */
        }
    }

    /* read กลับทั้ง 2 ช่อง — materialize (กฎเดียว) → lossless? */
    uint8_t *out = (uint8_t *)malloc(n);
    uint32_t badA = 0xFFFFFFFF, badB = 0xFFFFFFFF;
    for (uint32_t i = 0; i < nchunks && all_ok; i++) {
        uint32_t off = i * CHUNK;
        uint32_t sz = n - off; if (sz > CHUNK) sz = CHUNK;
        uint32_t got = 0;
        if (ghost_read_rule_materialize(&logA, &rsA, (uint16_t)i, 0,
                                        out + off, sz, &got) != 0 || got != sz
            || memcmp(out + off, x + off, sz) != 0) { badA = i; all_ok = 0; break; }
        if (ghost_read_rule_materialize(&logB, &rsB, (uint16_t)i, 0,
                                        out + off, sz, &got) != 0 || got != sz
            || memcmp(out + off, x + off, sz) != 0) { badB = i; all_ok = 0; break; }
    }
    if (badA != 0xFFFFFFFF) {
        uint32_t got = 0;
        int r = ghost_read_rule_materialize(&logA, &rsA, (uint16_t)badA, 0,
                                            out + badA * CHUNK, CHUNK, &got);
        printf("  ✗ raw space fail chunk %u: r=%d got=%u (rs.count=%u cap=%u)\n",
               badA, r, got, rsA.count, rsA.capacity);
    }
    if (badB != 0xFFFFFFFF) {
        uint32_t got = 0;
        int r = ghost_read_rule_materialize(&logB, &rsB, (uint16_t)badB, 0,
                                            out + badB * CHUNK, CHUNK, &got);
        printf("  ✗ delta space fail chunk %u: r=%d got=%u (rs.count=%u cap=%u)\n",
               badB, r, got, rsB.count, rsB.capacity);
    }

    double raw_bpc = (double)raw_bytes / n;
    double del_bpc = (double)delta_bytes / n;
    printf("%-34s chunk=%-6u n=%u lossless=%s\n", name, CHUNK, n,
           all_ok ? "OK ✓" : "FAIL ✗");
    printf("  raw   : %llu B (%.4f B/cell)\n", (unsigned long long)raw_bytes, raw_bpc);
    printf("  delta : %llu B (%.4f B/cell)  = %.3f× raw\n",
           (unsigned long long)delta_bytes, del_bpc, del_bpc / raw_bpc);
    printf("  delta entries: %u/%u · ชนะ %u · แพ้ %u (fallback raw)\n",
           delta_entries, nchunks, delta_wins, delta_loses);
    printf("  ──\n");

    free(out);
    ghost_pair_free(&pA); ghost_pair_free(&pB);
    rs_free(&rsA); rs_free(&rsB);
}

static void run(const char *name, uint8_t *x, uint32_t n) {
    printf("════ %s (%u B) ════\n", name, n);
    static const uint32_t chunks[] = { 64u, 1024u, 16384u };
    for (int c = 0; c < 3; c++) run_chunk(name, x, n, chunks[c]);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage:\n  %s --file <path>\n  %s --syn <kind> <n>\n  %s --gguf <model> <idx>\n",
               argv[0], argv[0], argv[0]);
        return 1;
    }
    uint8_t *x = (uint8_t *)malloc(MAXN);
    if (!x) return 1;

    if (strcmp(argv[1], "--syn") == 0 && argc >= 4) {
        uint32_t n = (uint32_t)strtoul(argv[3], NULL, 10);
        if (n > MAXN) n = MAXN;
        synth_fill(x, n, argv[2]);
        run(argv[2], x, n);
        return 0;
    }
    if (strcmp(argv[1], "--file") == 0 && argc >= 3) {
        FILE *fp = fopen(argv[2], "rb");
        if (!fp) { printf("cannot open %s\n", argv[2]); return 1; }
        uint32_t n = (uint32_t)fread(x, 1, MAXN, fp);
        fclose(fp);
        run(argv[2], x, n);
        return 0;
    }
    if (strcmp(argv[1], "--gguf") == 0 && argc >= 4) {
        const char *path = argv[2];
        uint32_t idx = (uint32_t)strtoul(argv[3], NULL, 10);
        GgufReader r;
        if (gguf_open(path, &r) != 0) { printf("cannot open %s\n", path); return 1; }
        if (idx >= r.n_tensors) { printf("idx %u out of range (%u)\n", idx, r.n_tensors); return 1; }
        uint64_t sz = r.sizes[idx];
        if (sz > MAXN) sz = MAXN;
        uint8_t *t = (uint8_t *)malloc((size_t)sz);
        if (!t) return 1;
        if (gguf_read_tensor(path, &r, idx, t, (uint32_t)sz) != 0) { printf("read fail\n"); return 1; }
        char nm[96];
        snprintf(nm, sizeof(nm), "%s[%u] %s", path, idx, r.names[idx]);
        run(nm, t, (uint32_t)sz);
        free(t);
        return 0;
    }
    printf("unknown mode\n");
    return 1;
}
