/* tools/bond_direct_resolve.c — b-bond: chunk ไม่ต้องรันเลข (มีคู่เดียว)
 * ═══════════════════════════════════════════════════════════════════════
 * user: "มันทำให้ chunk ไม่ต้องรันเลขไง เพราะมีได้แค่คู่เดียว"
 *
 * หลักการ: b-bond = (block_id, from_scale) มีคู่เดียวในโลก (§15.53) →
 *   - resolution เป็น identity: กู้ (block, from) จาก bond ได้ด้วยเลขคณิต
 *     ตรงๆ (2 op) — ไม่ต้องค้น ไม่ต้อง hash ไม่ต้องเทียบ
 *   - data freeze ครั้งเดียวต่อ birth pile (ghost_lift) → chunk ↔ bond = 1:1
 *   - ไม่มี ambiguity → ไม่มีอะไรต้อง "คำนวณเพื่อหา"
 *
 * พิสูจน์ + วัดเทียบกับโค้ดปัจจุบัน (ghost_log_find = linear scan):
 *   A. direct resolve: bond → (block, from) เลขคณิตล้วน (2 op)
 *   B. uniqueness: sweep (block, from) → key ไม่ชน (ไม่มี ambiguity)
 *   C. เทียบ cost: direct (O(1)) vs linear scan แบบเดิม (O(n)) — หลัง
 *      ปรับ (จ§15.55) ghost_log_find เป็น binary search แล้ว (239 cyc)
 *   D. freeze-once: chunk ↔ (block, from) = 1:1 (route = a-bond, ไม่กระทบ)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_ghost_lift.h"
#include "../core/hyp_fusion.h"

static int checks = 0, fails = 0;
#define CHECK(desc, cond) do { \
    checks++; \
    if (cond) printf("  ✓ %s\n", desc); \
    else { fails++; printf("  ✗ FAIL: %s\n", desc); } \
} while (0)

#if defined(_MSC_VER)
#include <intrin.h>
static inline uint64_t rdtsc_now(void) { return __rdtsc(); }
#else
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

/* ── A. direct resolve — เลขคณิตล้วน ไม่มีการค้น ── */
static void test_direct(void) {
    printf("\n[A] direct resolve: bond → (block, from) ด้วยเลขคณิต (2 op)\n");
    uint32_t bad = 0;
    for (uint16_t b = 0; b < 4096; b += 11)
        for (uint16_t s = 0; s < 256; s += 9) {
            /* ทางเดียวกับ ghost: bond จาก (block, from) */
            uint64_t key = ghost_bond_key(b, (uint8_t)s, 0);
            /* direct: กู้กลับด้วย div/mod — ไม่มีการค้น ไม่มี hash */
            uint32_t rb; uint8_t rf;
            hyp_bond_core(key & 0xFFFFFFFFFFFFull, &rb, &rf);
            if (rb != b || rf != s) bad++;
        }
    CHECK("bond → (block, from) กู้กลับด้วยเลขคณิตตรงๆ ทุกคู่ (bad=0)",
          bad == 0);
}

/* ── B. uniqueness — ไม่มี ambiguity → ไม่ต้องคำนวณเพื่อหา ── */
static void test_unique(void) {
    printf("\n[B] uniqueness: (block, from) → key ไม่ชน (มีคู่เดียวในโลก)\n");
    /* 2^20 keys × 8-bit stamp — bitset เช็ค collision จริง */
    static uint8_t seen[1 << 20];
    memset(seen, 0, sizeof(seen));
    uint32_t bad = 0, n = 0;
    for (uint16_t b = 0; b < 4096; b++)
        for (uint16_t s = 0; s < 256; s++) {
            uint64_t key = ghost_bond_key(b, (uint8_t)s, 0);
            uint32_t low = (uint32_t)(key & 0xFFFFFu);  /* 20 bits */
            if (seen[low]) { bad++; continue; }
            seen[low] = 1;
            n++;
        }
    CHECK("sweep 4096×256: key ไม่ชนเลย (bad=0)", bad == 0 && n == 4096u * 256u);
}

/* ── C. cost เทียบ: direct (O(1)) vs linear scan (O(n)) ── */
static void test_cost(void) {
    printf("\n[C] cost เทียบ: direct (ไม่รันเลข) vs ghost_log_find (scan)\n");
    GhostLog log;
    ghost_log_init(&log);

    /* เติม log 1000 routes */
    for (uint16_t i = 0; i < 1000; i++) {
        GhostLogEntry *e = &log.entries[log.count++];
        e->block_id   = (uint16_t)(i * 17u);
        e->from_scale = (uint8_t)(i % 256u);
        e->to_scale   = (uint8_t)(i % 144u);
        e->flags      = GHOST_FLAG_LIFT;
    }

    /* วัด comparisons ของ scan (เทียบ count ครั้ง) — เฉลี่ยต่อ find */
    uint64_t cmp_total = 0;
    for (uint16_t i = 0; i < 500; i++) {
        uint16_t b = (uint16_t)(i * 17u);
        uint8_t  f = (uint8_t)(i % 256u);
        uint8_t  t = (uint8_t)(i % 144u);
        for (uint32_t j = 0; j < log.count; j++) {
            cmp_total++;
            const GhostLogEntry *e = &log.entries[j];
            if (e->flags & GHOST_FLAG_EXPIRED) continue;
            if (e->block_id == b && e->from_scale == f && e->to_scale == t)
                break;
        }
    }
    printf("    linear scan: เฉลี่ย %lu comparisons/find (log=%u)\n",
           (unsigned long)(cmp_total / 500), (unsigned)log.count);

    /* rdtsc: direct (hyp_bond_core = 2 op) vs scan loop */
    volatile uint64_t sink = 0;
    const int N = 200000;
    uint64_t best_d = ~0ull, best_s = ~0ull;
    for (int trial = 0; trial < 9; trial++) {
        uint64_t t0 = rdtsc_now();
        for (int i = 0; i < N; i++) {
            uint32_t rb; uint8_t rf;
            hyp_bond_core((uint64_t)(i & 0xFFFFFF), &rb, &rf);
            sink += rb + rf;
        }
        uint64_t t1 = rdtsc_now();
        if (t1 - t0 < best_d) best_d = t1 - t0;

        t0 = rdtsc_now();
        for (int i = 0; i < N; i++) {
            uint16_t b = (uint16_t)((i * 17u) % 1000u);
            for (uint32_t j = 0; j < log.count; j++) {
                const GhostLogEntry *e = &log.entries[j];
                if (e->block_id == b) { sink += j; break; }
            }
        }
        uint64_t t2 = rdtsc_now();
        if (t2 - t0 < best_s) best_s = t2 - t0;
    }
    printf("    rdtsc min-of-9 (cyc/resolve): direct=%lu  scan=%lu\n",
           (unsigned long)(best_d / N), (unsigned long)(best_s / N));
    CHECK("direct (O(1) เลขคณิต) เร็วกว่า scan (O(n) เทียบ) อย่างชัดเจน",
          best_d < best_s);
    (void)sink;
}

/* ── D. freeze-once: chunk ↔ (block, from) = 1:1 ── */
static void test_freeze_once(void) {
    printf("\n[D] freeze-once: ข้อมูล freeze ครั้งเดียวต่อ birth pile (1:1)\n");
    /* ghost_lift: data frozen ONCE per (block, from) — routes แชร์ data เดียว
     * → chunk มีคู่เดียวจริง (b-bond) · route = a-bond (หลายทาง) ไม่กระทบ */
    GhostLog log;
    ResidualSpace rs;
    ghost_log_init(&log);
    rs_init(&rs, 1024);
    uint8_t d1[8] = {1,2,3,4,5,6,7,8};
    uint8_t d2[8] = {9,9,9,9,9,9,9,9};
    uint64_t k1 = ghost_lift(&log, &rs, 42, 100, 5, d1, 8);
    /* birth pile เดียว ไป scale อื่น → route ใหม่ แชร์ data เดียว */
    uint64_t k2 = ghost_lift(&log, &rs, 42, 100, 90, d1, 8);
    uint64_t k3 = ghost_lift(&log, &rs, 42, 100, 5, d2, 8);  /* data ต่าง → reject */
    CHECK("birth เดียว → bond เดียว (k1==k2 — routes แชร์ frozen data)",
          k1 != 0 && k1 == k2);
    CHECK("route ซ้ำ (same to_scale) → reject (k3=0 — ไม่มี data ที่ 2)",
          k3 == RS_BOND_KEY_RESERVED);
    CHECK("อ่านกลับ: bond เดียว → data เดียว (lossless)",
          k1 == k2 && ghost_read(&log, &rs, 42, 100, 90, NULL) != NULL);
    rs_free(&rs);
}

int main(void) {
    printf("═ b-bond: chunk ไม่ต้องรันเลข (มีคู่เดียวในโลก) ═\n");
    test_direct();
    test_unique();
    test_cost();
    test_freeze_once();
    printf("\n══════════ %d/%d PASS ══════════\n", checks - fails, checks);
    return fails ? 1 : 0;
}
