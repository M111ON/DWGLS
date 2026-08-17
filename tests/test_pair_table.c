/* test_pair_table.c — dense pair table: (block, from) → pile slot, O(1)
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "ต่อยอด route check เป็น O(1): dense pair table (block,from)
 *        → pile slot แบบ direct (memory trade) — วัด footprint จริง
 *        บน 4 โมเดล GGUF ว่าแพงแค่ไหน"
 *
 * พิสูจน์:
 *   A. correctness: ghost_pair_find/route_count ตรง brute force 100%
 *      (บน log หลายพัน routes — เทียบกับ ghost_log_find/ghost_route_count)
 *   B. O(1): วัด cycles pair_find vs binary search find — ตารางเร็วกว่า
 *   C. staleness: lift หลัง build → pair ไม่ fresh (ต้อง rebuild);
 *      rebuild แล้วกลับมาตรงอีกครั้ง — ไม่มี stale read
 *   D. footprint จริงบน 4 GGUF: tensor → block, scale_w → from —
 *      ตารางเท่าไหร่ เทียบ log (5B/route) — ตัดสินใจ memory trade
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Icore -Itests \
 *        -o build/test_pair_table tests/test_pair_table.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_ghost_lift.h"
#include "../core/gguf_box.h"

#define WIN 20736u

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* scale position of rank r — placement formula (เดียวกับ cap_tune) */
static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

/* brute force — สแกนทั้ง log (ground truth) */
static int bf_find(const GhostLog *log, uint16_t b, uint8_t f, uint8_t t) {
    for (uint32_t i = 0; i < log->count; i++) {
        const GhostLogEntry *e = &log->entries[i];
        if (e->block_id != b || e->from_scale != f) continue;
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if (e->to_scale == t) return (int)i;
    }
    return -1;
}
static uint32_t bf_route_count(const GhostLog *log, uint16_t b) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if (log->entries[i].block_id == b) n++;
    return n;
}

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 37u) % 251u);
}

/* สร้าง log จำลองแบบ real chain: block = tensor, from = scale_w(rank),
 * หลาย routes ต่อ block (to ต่างกัน) + block ว่างคั่น */
static uint32_t build_log(GhostLog *log, ResidualSpace *rs, uint32_t n_blocks,
                          uint32_t routes_per) {
    ghost_log_init(log);
    rs_init(rs, 8192);
    uint8_t d[16];
    fill_pattern(d, sizeof(d), 7);
    uint32_t made = 0;
    for (uint32_t b = 0; b < n_blocks; b++) {
        uint16_t blk = (uint16_t)(b * 3u);          /* block ว่างคั่น (2/3 เต็ม) */
        uint8_t f = scale_w(b * 5u + 1u);
        for (uint32_t r = 0; r < routes_per; r++) {
            uint8_t t = (uint8_t)((f + 1u + r * 7u) % 144u);
            if (ghost_lift(log, rs, blk, f, t, d, sizeof(d)) != RS_BOND_KEY_RESERVED)
                made++;
        }
    }
    return made;
}

/* ── A. correctness vs brute force ── */
static void test_correct(void) {
    printf("\nA. correctness — pair table ตรง brute force 100%%\n");
    GhostLog log; ResidualSpace rs;
    uint32_t made = build_log(&log, &rs, 400, 3);   /* 400 blocks × ~3 routes */
    CHECK(1, "log มีหลายพัน routes (made=%u)", made > 1000);

    GhostPairTable t;
    memset(&t, 0, sizeof(t));
    /* section A = correctness ของตาราง → force build (ข้าม signal §15.59
       ซึ่งตัดสินใจที่ refresh; ตรงนี้เราอยากเทสต์ตัวตารางโดยตรง) */
    ghost_pair_attach(&log, &t);
    ghost_pair_build(&log, &t);
    CHECK(1, "attach + build ตรง (ตารางพร้อมใช้)", ghost_pair_fresh(&log));

    /* find: ทุก entry ที่ lift → pair_find เจอ */
    int ok = 1;
    for (uint32_t i = 0; i < log.count; i++) {
        const GhostLogEntry *e = &log.entries[i];
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        int bf = bf_find(&log, e->block_id, e->from_scale, e->to_scale);
        int pf = ghost_pair_find(&log, e->block_id, e->from_scale, e->to_scale);
        if (bf < 0 || pf < 0 || log.entries[pf].to_scale != e->to_scale) ok = 0;
    }
    CHECK(2, "find ตรง brute force ทุก route (%u routes)", ok && made > 1000);

    /* find ที่ไม่มี (block, from, to ผิด) → -1 ทั้งคู่ */
    int miss_ok = 1;
    for (uint32_t b = 0; b < 400; b++) {
        uint16_t blk = (uint16_t)(b * 3u + 1u);     /* block ว่าง */
        if (bf_find(&log, blk, 3, 9) != -1) miss_ok = 0;
        if (ghost_pair_find(&log, blk, 3, 9) != -1) miss_ok = 0;
        uint16_t blk2 = (uint16_t)(b * 3u);         /* block มี data — to ผิด */
        uint8_t f = scale_w(b * 5u + 1u);
        if (bf_find(&log, blk2, f, 200) != -1) miss_ok = 0;
        if (ghost_pair_find(&log, blk2, f, 200) != -1) miss_ok = 0;
    }
    CHECK(2, "miss (block ว่าง / to ผิด) → -1 ทั้งคู่", miss_ok);

    /* route_count: ทุก block เทียบ brute force */
    int rc_ok = 1;
    for (uint16_t b = 0; b < 1200; b++) {
        if (ghost_pair_route_count(&log, b) != bf_route_count(&log, b))
            rc_ok = 0;
    }
    CHECK(3, "route_count ตรง brute force ครบทุก block (รวม block ว่าง)", rc_ok);

    /* เทียบ ghost_log_find/ghost_route_count (binary search) ด้วย */
    int bs_ok = 1;
    for (uint32_t i = 0; i < log.count; i++) {
        const GhostLogEntry *e = &log.entries[i];
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if ((ghost_log_find(&log, e->block_id, e->from_scale, e->to_scale) >= 0)
            != (ghost_pair_find(&log, e->block_id, e->from_scale, e->to_scale) >= 0))
            bs_ok = 0;
    }
    for (uint16_t b = 0; b < 1200; b++)
        if (ghost_route_count(&log, b) != ghost_pair_route_count(&log, b))
            bs_ok = 0;
    CHECK(3, "เทียบ binary search: find/route_count ตรงกันทุกจุด", bs_ok);

    ghost_pair_detach(&log);
    ghost_pair_free(&t);
    rs_free(&rs);
}

/* ── B. cycles: pair table vs binary search ── */
static void test_cycles(void) {
    printf("\nB. cost — pair table vs binary search (min-of-9, cyc/op)\n");
    GhostLog log; ResidualSpace rs;
    uint32_t made = build_log(&log, &rs, 900, 4);   /* ~3600 routes — เท่า ghost_direct */
    GhostPairTable t;
    memset(&t, 0, sizeof(t));
    ghost_pair_attach(&log, &t);
    ghost_pair_build(&log, &t);   /* section B = วัดความเร็วตารางล้วน (ข้าม signal) */

    const uint32_t N = 200000;
    uint64_t t0, t1;
    volatile int sink = 0;

    /* binary search find — ทำซ้ำ route เดียว (cache warm) */
    uint16_t blk = 6; uint8_t f = scale_w(31), tt = (uint8_t)((f + 1u) % 144u);
    t0 = __rdtsc();
    for (uint32_t i = 0; i < N; i++) sink += ghost_log_find(&log, blk, f, tt);
    t1 = __rdtsc();
    uint64_t bin_cyc = (t1 - t0) / N;

    /* pair find (fresh — ไม่ rebuild ระหว่าง loop) */
    t0 = __rdtsc();
    for (uint32_t i = 0; i < N; i++) sink += ghost_pair_find(&log, blk, f, tt);
    t1 = __rdtsc();
    uint64_t pair_cyc = (t1 - t0) / N;

    /* pair route_count vs binary route_count */
    t0 = __rdtsc();
    for (uint32_t i = 0; i < N; i++) sink += ghost_route_count(&log, blk);
    t1 = __rdtsc();
    uint64_t bin_rc = (t1 - t0) / N;
    t0 = __rdtsc();
    for (uint32_t i = 0; i < N; i++) sink += ghost_pair_route_count(&log, blk);
    t1 = __rdtsc();
    uint64_t pair_rc = (t1 - t0) / N;

    printf("  find:        binary %I64u cyc | pair %I64u cyc (%.1f×)\n",
           (unsigned long long)bin_cyc, (unsigned long long)pair_cyc,
           (double)bin_cyc / (double)(pair_cyc ? pair_cyc : 1));
    printf("  route_count: binary %I64u cyc | pair %I64u cyc (%.1f×)\n",
           (unsigned long long)bin_rc, (unsigned long long)pair_rc,
           (double)bin_rc / (double)(pair_rc ? pair_rc : 1));
    printf("  (sink=%d — routes=%u)\n", sink, made);

    CHECK(4, "pair find ไม่ช้ากว่า binary (O(1) direct index)", pair_cyc <= bin_cyc);
    CHECK(4, "pair route_count ไม่ช้ากว่า binary", pair_rc <= bin_rc);
    CHECK(4, "pair find เป็น O(1) จริง (< 100 cyc)", pair_cyc < 100);

    ghost_pair_detach(&log);
    ghost_pair_free(&t);
    rs_free(&rs);
}

/* ── C. signal-before-compute — ประเมินราคา rebuild จาก history ── */
static void test_signal(void) {
    printf("\nC. signal-before-compute — ราคา rebuild รู้ก่อนจ่าย (history: reads_served)\n");
    GhostLog log; ResidualSpace rs;
    ghost_log_init(&log); rs_init(&rs, 1024);
    uint8_t d[8];
    fill_pattern(d, sizeof(d), 99);
    GhostPairTable t;
    memset(&t, 0, sizeof(t));
    ghost_pair_attach(&log, &t);

    /* log เล็ก (2 entries, max_block 4) → pred 4×514=2KB << 4×2400
       → build ได้ (ถูก) — find ทำงานผ่านตาราง */
    ghost_lift(&log, &rs, 3, 1, 5, d, sizeof(d));
    ghost_lift(&log, &rs, 3, 1, 9, d, sizeof(d));
    CHECK(5, "attach → dirty (ยังไม่ build)", t.dirty == 1 && !ghost_pair_fresh(&log));
    CHECK(5, "log เล็ก → build (ราคา 2KB ต่ำกว่า amortization)",
          ghost_pair_refresh(&log) == 0 && t.pile && ghost_pair_fresh(&log));
    CHECK(5, "find/route_count ผ่านตาราง — ถูกต้อง",
          ghost_pair_find(&log, 3, 1, 5) >= 0 &&
          ghost_pair_route_count(&log, 3) == 2 &&
          ghost_read(&log, &rs, 3, 1, 5, NULL) != NULL);

    /* จำลอง chunk-index โต (ไฟล์ใหญ่): lift block 5000 → pred 2.5MB
       แต่ history ยังต่ำ (ไม่กี่ reads ต่อ rebuild) → SKIP → binary */
    ghost_lift(&log, &rs, 5000, 1, 3, d, sizeof(d));
    CHECK(6, "block โต (pred 2.5MB) + history ต่ำ → signal skip rebuild",
          t.dirty == 1 && ghost_pair_refresh(&log) == 1 && !ghost_pair_fresh(&log));
    CHECK(6, "skip แล้ว find fallback binary — ยังถูกต้อง (ไม่ใช่ stale)",
          ghost_pair_find(&log, 5000, 1, 3) >= 0 &&
          ghost_pair_find(&log, 3, 1, 5) >= 0 &&
          ghost_pair_route_count(&log, 5000) == 1);

    /* read-heavy ต่อ: pred 2.5MB ÷ 2400 ≈ 1071 reads → rebuild คุ้ม
       (600 ครั้ง × 2 find = 1200 reads > 1071) — history เรียนรู้:
       ตาราง rebuild เองกลาง loop เมื่อ served ข้าม threshold แล้ว
       กลับมาใช้ตารางต่อ (dirty ถูกล้าง — ไม่ต้องรอ refresh นอก) */
    for (uint32_t i = 0; i < 600; i++) {
        ghost_pair_find(&log, 3, 1, 5);
        ghost_pair_find(&log, 5000, 1, 3);
    }
    CHECK(7, "read-heavy → history เรียนรู้: ตาราง rebuild + fresh เองกลาง loop",
          t.pile && !t.dirty && ghost_pair_fresh(&log));
    CHECK(7, "หลัง auto-rebuild: route ใหม่เจอ (ไม่มี stale read)",
          ghost_pair_find(&log, 5000, 1, 3) >= 0 &&
          ghost_pair_route_count(&log, 5000) == 1);

    /* expire → dirty → read ปิดเส้นทาง */
    uint32_t ex = ghost_expire(&log, &rs, 3, 1);
    CHECK(8, "expire → dirty + route ตาย (auto-refresh, ไม่มี stale)",
          ex == 2 && t.dirty == 1 &&
          ghost_pair_find(&log, 3, 1, 5) == -1 &&
          ghost_pair_route_count(&log, 3) == 2 &&
          ghost_read(&log, &rs, 3, 1, 5, NULL) == NULL);

    /* detach → ghost_read กลับ binary (fallback) */
    ghost_pair_detach(&log);
    ghost_lift(&log, &rs, 3, 1, 5, d, sizeof(d));
    CHECK(9, "detach → ghost_read ยังถูก (fallback binary — ไม่พึ่งตาราง)",
          ghost_read(&log, &rs, 3, 1, 5, NULL) != NULL && !ghost_pair_fresh(&log));

    ghost_pair_free(&t);
    rs_free(&rs);
}

/* ── D. footprint จริงบน 4 GGUF ── */
static void test_footprint(void) {
    printf("\nD. footprint จริงบน 4 GGUF — dense pair table vs log\n");
    const char *paths[4] = {
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Qwen3-0.6B-Q8_0.gguf",
        "I:/model/LFM2.5-2.6B-Q8_0.gguf",
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
    };
    const char *labels[4] = { "SmolLM2-360M", "Qwen3-0.6B", "LFM2.5-2.6B", "Qwen2.5-0.5B" };

    uint64_t tot_pile = 0, tot_log = 0;
    int n_present = 0;
    for (uint32_t m = 0; m < 4; m++) {
        GGUFBox box;
        if (gguf_box_open(&box, paths[m]) != 0) {
            printf("  %-12s (cannot open — skip)\n", labels[m]);
            continue;
        }
        uint32_t N = box.n_tensors;
        /* tensor → block (rank), from = scale_w(rank), 1 route/tensor
           (placement เดียวกับ cap_tune — envelope gate ตัดสิน lift) */
        uint32_t max_block = N > 0 ? N : 0;          /* block = tensor index */
        uint64_t pile_bytes = (uint64_t)max_block * 256u * 2u
                            + ((uint64_t)max_block + 1u) * 2u;
        uint64_t log_bytes  = (uint64_t)N * 5u + 12u;
        double ratio = (double)pile_bytes / (double)(log_bytes ? log_bytes : 1);
        printf("  %-12s %5u tensors | table %7I64u B (max_block %u) | log %6I64u B | %.0f× log | %.2f MB per field-window\n",
               labels[m], N,
               (unsigned long long)pile_bytes, max_block,
               (unsigned long long)log_bytes, ratio,
               (double)pile_bytes / (double)(WIN * 4096.0));
        tot_pile += pile_bytes; tot_log += log_bytes; n_present++;
    }
    if (n_present >= 2) {
        double agg = (double)tot_pile / (double)tot_log;
        printf("  ── aggregate (%d models): table %.1f× log size — dense pair table\n", n_present, agg);
    }
    CHECK(7, "≥2 real models measured", n_present >= 2);
    CHECK(7, "table เล็กกว่า 1/10 ของ field window (1 window = 20736×4KB)",
          n_present >= 2 && tot_pile < (uint64_t)WIN * 4096u / 10u);
    printf("  → memory trade: ~%I64u KB ทั้ง 4 โมเดล เพื่อ find/route_count O(1)\n",
           (unsigned long long)(tot_pile / 1024ull));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("dense pair table — (block, from) → pile slot, O(1) route check\n");
    printf("══════════════════════════════════════════════════════════════\n");
    test_correct();
    test_cycles();
    test_signal();
    test_footprint();
    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
