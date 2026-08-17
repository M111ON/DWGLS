/* tools/rdh_bench.c — micro-benchmark: RDH encode vs FNV-1a (rdtsc cycle-accurate)
 * ═══════════════════════════════════════════════════════════════════════════
 * ยืนยันคำกล่าว "RDH ใช้ไม่เกิน 6 cpu cycle" ด้วยการวัดจริงบนชื่อ tensor ของ
 * GGUF จริง 4 โมเดล (ชุดเดียวกับ silk_screen_scan):
 *
 *   FNV-1a (ทางเก่า)   — เดินทุก byte ของชื่อ (O(len)) — pogls_fibo_addr pass 1
 *   rdh_addr           — ring×256+wedge                      (1 mul + 1 add)
 *   rdh_bond_key       — addr ^ (addr<<24)                   (interleave)
 *   rdh_key 5-param    — ((ring×W+w)×M+m)×U+u  dims 144×144×2×256
 *
 * วิธีวัด: rdtsc + lfence, min-of-trials (best case = instruction cost จริง)
 * ring/wedge มาจาก index (ระบบมี (block_id, from_scale) เป็น int อยู่แล้ว —
 * วัด cost ของ encode ล้วน ไม่ใช่ name parsing)
 *
 * Speedup curve: bucket ชื่อตามความยาว → FNV ควรโตตาม len, RDH แบน (O(1))
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format \
 *        -I. -Icore -Icore/infra -o build/rdh_bench tools/rdh_bench.c -lm
 * RUN:   ./build/rdh_bench [model.gguf ...]   (default = 4 โมเดล I:/model)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"
#include "../core/geo_rdh_addr.h"

#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME  UINT64_C(1099511628211)

/* FNV-1a 64-bit — เดินทุก byte ของชื่อ (ทางเก่า, O(len)) */
static uint64_t fnv1a(const char *s) {
    uint64_t h = FNV_OFFSET;
    while (*s) { h ^= (uint8_t)*s++; h *= FNV_PRIME; }
    return h;
}

/* rdtsc + lfence (serialize) — MinGW-safe inline asm */
static inline uint64_t rdtsc_lf(void) {
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* full 5-param rdh_key (dims 144×144×2×256 — 3 mul + 3 add) */
static inline uint64_t rdh_key5(uint32_t ring, uint32_t wedge,
                                uint32_t mirror, uint32_t u) {
    return ((ring * 144u + wedge) * 2u + mirror) * 256u + u;
}

#define TRIALS     9u
#define ITERS      400000u

static volatile uint64_t g_sink;   /* กัน compiler optimize ทิ้ง */

typedef struct { char *name; uint32_t ring, wedge; } Item;

/* bench หนึ่งฟังก์ชันบน i=0..n-1 (ไม่มี % ใน hot path — กัน division ปน) */
static double bench_min(uint64_t (*fn)(uint32_t), uint32_t n) {
    double best = 1e18;
    for (uint32_t t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_lf();
        for (uint32_t r = 0; r < ITERS / n + 1u; r++)
            for (uint32_t i = 0; i < n; i++)
                g_sink ^= fn(i);
        uint64_t t1 = rdtsc_lf();
        double ops = (double)((ITERS / n + 1u) * n);
        double c = (double)(t1 - t0) / ops;
        if (c < best) best = c;
    }
    return best;
}

/* ── wrappers (รับ index → ค่า) ── */
static const Item *g_items;
static uint32_t g_n;

static uint64_t w_fnva(uint32_t i) { return fnv1a(g_items[i].name); }
static uint64_t w_addr(uint32_t i) { return rdh_addr(g_items[i].ring, g_items[i].wedge); }
static uint64_t w_bond(uint32_t i) { return rdh_bond_key(g_items[i].ring, g_items[i].wedge); }
static uint64_t w_key5(uint32_t i) {
    return rdh_key5(g_items[i].ring % 144u, g_items[i].wedge, i & 1u, i & 255u);
}
/* pure encode — ค่า derive จาก loop counter (register, ไม่มี array load) */
static uint64_t w_addr_pure(uint32_t i) { return rdh_addr(i & 65535u, (i >> 8) & 255u); }
static uint64_t w_bond_pure(uint32_t i) { return rdh_bond_key(i & 65535u, (i >> 8) & 255u); }
static uint64_t w_key5_pure(uint32_t i) {
    return rdh_key5(i & 143u, (i >> 8) & 143u, i & 1u, (i >> 9) & 255u);
}

/* bench ตามความยาวชื่อ (speedup curve) — FNV ต่อ bucket, RDH คงที่ */
static void bench_bucket(const char *label, uint32_t *idx, uint32_t cnt) {
    if (cnt == 0) return;
    double best = 1e18;
    for (uint32_t t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_lf();
        for (uint32_t r = 0; r < ITERS / cnt + 1u; r++)
            for (uint32_t k = 0; k < cnt; k++)
                g_sink ^= fnv1a(g_items[idx[k]].name);
        uint64_t t1 = rdtsc_lf();
        double ops = (double)((ITERS / cnt + 1u) * cnt);
        double c = (double)(t1 - t0) / ops;
        if (c < best) best = c;
    }
    double rdh = bench_min(w_addr, g_n);
    printf("  len %-6s n=%-5u  FNV=%6.1f cyc  RDH=%5.2f cyc  speedup %6.2fx\n",
           label, cnt, best, rdh, best / rdh);
}

static void run_model(const char *path, uint32_t *n_present) {
    GGUFBox box;
    printf("\n═ %s ═\n", path);
    if (gguf_box_open(&box, path) != 0) { printf("  (cannot open — skip)\n"); return; }
    (*n_present)++;

    /* เก็บชื่อทุก tensor + ring/wedge จาก index */
    uint32_t n = box.n_tensors;
    if (n == 0) { printf("  (0 tensors)\n"); return; }
    Item *items = (Item *)calloc(n, sizeof(Item));
    if (!items) { gguf_box_close(&box); return; }
    for (uint32_t i = 0; i < n; i++) {
        items[i].name  = strdup(box.entries[i].name);
        items[i].ring  = i;                 /* block_id (int มีอยู่แล้วในระบบ) */
        items[i].wedge = i % 256u;          /* from_scale */
    }
    g_items = items;
    g_n = n;

    /* ตรวจก่อน: รายชื่อจริง */
    uint32_t min_len = 999, max_len = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t L = (uint32_t)strlen(items[i].name);
        if (L < min_len) min_len = L;
        if (L > max_len) max_len = L;
    }
    printf("  tensors=%u  name_len=[%u..%u]\n", n, min_len, max_len);
    printf("  ── end-to-end (array loads — realistic) ──\n");
    printf("  %-6s %-6s %-6s %-6s %-6s\n", "fnv1a", "rdh_addr", "rdh_bond", "rdh_key5", "fnv/rdh");
    double c_fnva = bench_min(w_fnva, n);
    double c_addr = bench_min(w_addr, n);
    double c_bond = bench_min(w_bond, n);
    double c_key5 = bench_min(w_key5, n);
    printf("  %-6.1f %-6.2f %-6.2f %-6.2f %-6.2fx\n",
           c_fnva, c_addr, c_bond, c_key5, c_fnva / c_addr);
    printf("  ── pure encode (register derive — instruction cost ล้วน) ──\n");
    double p_addr = bench_min(w_addr_pure, n);
    double p_bond = bench_min(w_bond_pure, n);
    double p_key5 = bench_min(w_key5_pure, n);
    printf("  %-6s %-6s %-6s\n", "rdh_addr", "rdh_bond", "rdh_key5");
    printf("  %-6.2f %-6.2f %-6.2f\n", p_addr, p_bond, p_key5);
    printf("  verdict: rdh_addr %.2f cyc %s  |  rdh_bond %.2f cyc %s  |  rdh_key5 %.2f cyc %s\n",
           p_addr, p_addr <= 6.0 ? "≤6 ✓" : ">6 ✗",
           p_bond, p_bond <= 6.0 ? "≤6 ✓" : ">6 ✗",
           p_key5, p_key5 <= 6.0 ? "≤6 ✓" : ">6 ✗");

    /* speedup curve ตามความยาวชื่อ */
    printf("  ── speedup curve (FNV vs RDH) ตาม length bucket ──\n");
    uint32_t *idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    for (uint32_t pass = 0; pass < 4; pass++) {
        uint32_t lo = (pass == 0) ? 0 : 8 + pass * 8;   /* 0-7, 8-15, 16-23, 24-31 */
        uint32_t hi = (pass == 0) ? 7 : 15 + pass * 8;
        uint32_t cnt = 0;
        for (uint32_t i = 0; i < n; i++)
            if (strlen(items[i].name) >= lo && strlen(items[i].name) <= hi) idx[cnt++] = i;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%u-%u", lo, hi);
        bench_bucket(lbl, idx, cnt);
    }
    /* ชื่อยาวเกิน 31 */
    {
        uint32_t cnt = 0;
        for (uint32_t i = 0; i < n; i++)
            if (strlen(items[i].name) > 31) idx[cnt++] = i;
        bench_bucket("32+", idx, cnt);
    }
    free(idx);
    for (uint32_t i = 0; i < n; i++) free(items[i].name);
    free(items);
    gguf_box_close(&box);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("RDH vs FNV-1a micro-benchmark — rdtsc cycle-accurate, GGUF tensor names\n");
    printf("══════════════════════════════════════════════════════════════════════\n");

    const char *paths[4] = {
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Qwen3-0.6B-Q8_0.gguf",
        "I:/model/LFM2.5-2.6B-Q8_0.gguf",
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"
    };
    uint32_t n = (argc > 1) ? (uint32_t)argc - 1 : 4;
    uint32_t present = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *p = (argc > 1) ? argv[i + 1] : paths[i];
        run_model(p, &present);
    }
    printf("\n══════════════════════════════════════════════════════════════════════\n");
    printf("models scanned: %u/%u — min-of-%u trials × %u iters, lfence-serialized\n",
           present, n, TRIALS, ITERS);
    return 0;
}
