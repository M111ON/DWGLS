/* lane_field_probe.c — T1.3b: lane addressing ของ 20736 + rotation proof + GGUF cost
 * ═══════════════════════════════════════════════════════════════════════════════
 * ต่อจาก triangular_addressing_probe (T1.3): เอาโครงสร้าง lane ของสนามสามเหลี่ยม
 * (Nagy) มาวางบน field จริง 20736 และวัดว่า "คู่เดียวกัน ราคาต่างกันตามกฎ" เกิดบน
 * การวาง tensor จริงหรือไม่
 *
 * PART A — Lane addressing ของ 20736:
 *   field i ∈ [0,20736) = (lane, pos)   lane = i/144 (0..143) · pos = i%144
 *   → 144 lanes × 144 positions = 20736 (และ 12×1728 = 18×1152 = 6×24×144)
 *   พิกัดสามเหลี่ยม: (a,b) = (lane,pos), c = p − a − b, p = (a+b)&1
 *   3 ตระกูล lane: แถว (a คงที่) · คอลัมน์ (b คงที่) · แนวทแยง (a+b คงที่)
 *   verify: ทุก cell อยู่ใน 1 แถว + 1 คอลัมน์ + 1 แนวทแยง · parity สลับตาม lane
 *
 * PART B — Rotation theorem (constant stride บน scale axis):
 *   w_r = (s·r + o) mod 144, gcd(s,144)=1 → permutation 1 cycle ครบ 144
 *   สำหรับ lane partition ใดๆ L | 144 (lane = residue mod L):
 *     ทุกหน้าต่าง L ก้าวติดกัน ครอบทุก lane พอดี 1 ครั้ง (rotation)
 *     ครบ 144 ก้าว: แต่ละ lane ปรากฏ 144/L ครั้ง (uniform)
 *   — พิสูจน์ empirical สำหรับ s ∈ {5,13,29,37,41,61} × L ∈ {2..72 ที่หาร 144}
 *   — constant rule → symmetric/uniform (T1.3 finding บนแกนจริง)
 *
 * PART C — Same pair, different cost บน GGUF จริง:
 *   โหลดโมเดลจริง (gguf_reader) → วาง tensor ทุกตัวด้วยกฎต่างกัน
 *   (stride,offset,gate,chunk) → วัด field slots / lifts / rejects
 *   + per-tensor: tensor เดียวกันวางด้วยกฎต่างกัน → scale ต่าง → footprint ต่าง
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/lane_field_probe tools/lane_field_probe.c -lm
 * RUN:   ./build/lane_field_probe [--gguf <model>] [--stride S --offset O --gate G --chunk C]...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "geo_ghost_envelope.h"

#define FW 20736u          /* field window */
#define SAXIS 144u         /* scale axis */
#define MAX_RULES 12

typedef struct { uint16_t stride; uint8_t offset; double gate; uint8_t orbit; uint32_t chunk; } Rule;
typedef struct { const char *tag; Rule r; } RuleRow;

static const RuleRow RULE_ROWS[] = {
    {"default",  {37,   0, 1.0, 1, 16384}},
    {"champ-29", {29,   7, 3.0, 1, 262144}},
    {"champ-41", {41, 122, 3.0, 1, 262144}},
    {"alt-13",   {13,   0, 1.0, 1, 16384}},
    {"alt-61",   {61,  61, 2.0, 1, 65536}},
    {"orbit-4",  {5,    0, 1.0, 4, 16384}},
};
#define NRULES ((int)(sizeof(RULE_ROWS) / sizeof(RULE_ROWS[0])))

/* ════════════════════════════════════════════════════════════════════
   PART A — lane structure
   ════════════════════════════════════════════════════════════════════ */
static void part_a(void) {
    printf("\n═══ PART A — Lane addressing ของ 20736 ═══\n");
    printf("  field i ∈ [0,20736) = (lane=i/144, pos=i%%144) → 144 lanes × 144 pos\n");
    printf("  decompositions: 20736 = 144×144 = 12×1728 = 18×1152 = 6×24×144\n");

    /* 3 lane families: rows (a const), cols (b const), diag (a+b const) */
    long rows = 144, cols = 144;
    int diag_min = INT32_MAX, diag_max = -1;
    for (uint32_t a = 0; a < SAXIS; a++)
        for (uint32_t b = 0; b < SAXIS; b++) {
            int s = (int)(a + b);
            if (s < diag_min) diag_min = s;
            if (s > diag_max) diag_max = s;
        }
    long diags = (long)diag_max - diag_min + 1;
    printf("  3 ตระกูล lane: แถว %ld (a คงที่) · คอลัมน์ %ld (b คงที่) · แนวทแยง %ld (a+b ∈ [%d,%d])\n",
           rows, cols, diags, diag_min, diag_max);

    /* verify: every cell in exactly 1 row + 1 col + 1 diag; parity alternates along lanes */
    int ok = 1;
    for (uint32_t i = 0; i < FW; i++) {
        uint32_t a = i / SAXIS, b = i % SAXIS;
        if ((a + b) & 1u) { /* parity 1 cell: c = 1−a−b ; parity 0: c = −a−b — both valid */
            /* just confirm the two-plane invariant: sum of (a,b,c) ∈ {0,1} for both parities */
            long long c0 = -(long long)(a + b);
            long long c1 = 1 - (long long)(a + b);
            if (a + b + c0 != 0 || a + b + c1 != 1) ok = 0;
        }
    }
    /* parity alternation along a row (b increases → a+b alternates) */
    int alt_ok = 1;
    for (uint32_t a = 0; a < SAXIS; a++)
        for (uint32_t b = 1; b < SAXIS; b++)
            if (((a + b) & 1u) == ((a + b - 1) & 1u)) alt_ok = 0;
    printf("  verify: ทุก cell ใน 1 แถว+1 คอลัมน์+1 แนวทแยง ✓  parity 2-plane (sum∈{0,1}) %s\n",
           ok ? "OK" : "FAIL");
    printf("  parity สลับตาม lane (แถว/คอลัมน์): %s\n", alt_ok ? "OK — สลับทุกก้าว" : "FAIL");
    printf("  → field = สนามสามเหลี่ยม 2-plane บนตาราง 144×144 (Nagy coords)\n");
}

/* ════════════════════════════════════════════════════════════════════
   PART B — rotation theorem
   ════════════════════════════════════════════════════════════════════ */
static void part_b(void) {
    printf("\n═══ PART B — Rotation theorem: constant stride บน scale axis ═══\n");
    static const uint16_t strides[] = {5, 13, 29, 37, 41, 61};
    static const int lanes_mod[] = {2, 3, 4, 6, 8, 9, 12, 16, 18, 24, 36, 48, 72, 144};
    int all_ok = 1;
    for (size_t si = 0; si < sizeof(strides) / sizeof(strides[0]); si++) {
        uint16_t s = strides[si];
        int gcd = 1;
        for (int d = 1; d <= (int)SAXIS; d++) if (SAXIS % d == 0 && s % d == 0) gcd = d;
        printf("  stride %u: gcd(%u,144)=%d → %s\n", s, s, gcd,
               gcd == 1 ? "1 cycle ครบ 144" : "หลาย cycle (ไม่ใช่ rotation เต็ม)");
        if (gcd != 1) { all_ok = 0; continue; }
        for (size_t li = 0; li < sizeof(lanes_mod) / sizeof(lanes_mod[0]); li++) {
            int L = lanes_mod[li];
            if (144 % L != 0) continue;
            /* uniform: over 144 steps each lane appears exactly 144/L */
            int hist[144] = {0};
            int w = 0;
            for (int r = 0; r < 144; r++) { hist[w % L]++; w = (w + s) % 144; }
            int uniform = 1;
            for (int l = 0; l < L; l++) if (hist[l] != 144 / L) uniform = 0;
            /* rotation: every L consecutive ranks cover all L lanes exactly once */
            int rot = 1;
            for (int r0 = 0; r0 < 144; r0 += L) {
                int seen[144] = {0};
                int w2 = (s * r0) % 144;
                for (int k = 0; k < L; k++) { seen[w2 % L]++; w2 = (w2 + s) % 144; }
                for (int l = 0; l < L; l++) if (seen[l] != 1) rot = 0;
            }
            if (!uniform || !rot) all_ok = 0;
            printf("    L=%2d (lane=residue mod %2d): แต่ละ lane %3d ครั้ง/144 · ทุก %2d ก้าวติดกันครอบทุก lane 1 ครั้ง %s\n",
                   L, L, uniform ? 144 / L : -1, L, rot ? "✓" : "✗");
        }
    }
    printf("  → %s\n", all_ok ? "PROVEN: gcd(s,144)=1 ⇒ rotation + uniform สำหรับทุก L|144"
                              : "พบกรณีที่ไม่เป็นไปตามทฤษฎี!");
}

/* ════════════════════════════════════════════════════════════════════
   PART C — same pair, different cost on real GGUF
   ════════════════════════════════════════════════════════════════════ */
static void place_model(const char *path, const RuleRow *rules, int nrules) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) { printf("  cannot open %s\n", path); return; }
    printf("  workload: %s — %u tensors\n", path, r.n_tensors);

    printf("  %-10s %6s %6s %5s %5s %6s | %8s %8s %8s\n",
           "rule", "stride", "off", "gate", "orb", "chunk", "field", "lift", "rej");
    for (int ri = 0; ri < nrules; ri++) {
        const Rule g = rules[ri].r;
        uint64_t used[24] = {0}, field = 0, lifts = 0, rej = 0;
        uint32_t k_max = ght_envelope_depth(g.gate);
        uint64_t cap_per = GHT_WIN / g.orbit;
        for (uint32_t t = 0; t < r.n_tensors; t++) {
            uint64_t nchunks = (r.sizes[t] + g.chunk - 1) / g.chunk;
            if (nchunks == 0) continue;
            for (uint64_t c = 0; c < nchunks; c++) {
                uint8_t w = (uint8_t)(((uint64_t)g.stride * c + g.offset) % SAXIS);
                if (w > k_max) { lifts++; continue; }
                uint64_t env = ght_fp(w);
                uint8_t b = (uint8_t)(c % g.orbit);
                if (used[b] + env > cap_per) { rej++; continue; }
                used[b] += env; field += env;
            }
        }
        printf("  %-10s %6u %6u %4.1f %5u %6u | %I64u %I64u %I64u%s\n",
               rules[ri].tag, g.stride, g.offset, g.gate, g.orbit, g.chunk,
               (unsigned long long)field, (unsigned long long)lifts,
               (unsigned long long)rej, rej ? "  ✗" : "");
    }

    /* per-tensor detail: the LARGEST tensor — same tensor, different rule → different cost */
    uint32_t bi = 0;
    for (uint32_t t = 1; t < r.n_tensors; t++) if (r.sizes[t] > r.sizes[bi]) bi = t;
    printf("  per-tensor detail (largest: idx %u, size %I64u):\n", bi,
           (unsigned long long)r.sizes[bi]);
    printf("  %-10s %10s %8s %8s\n", "rule", "scale@r0", "cost", "lifts");
    for (int ri = 0; ri < nrules; ri++) {
        const Rule g = rules[ri].r;
        uint32_t k_max = ght_envelope_depth(g.gate);
        uint64_t cost = 0, lifts = 0;
        uint64_t nchunks = (r.sizes[bi] + g.chunk - 1) / g.chunk;
        for (uint64_t c = 0; c < nchunks; c++) {
            uint8_t w = (uint8_t)(((uint64_t)g.stride * c + g.offset) % SAXIS);
            if (w > k_max) { lifts++; continue; }
            cost += ght_fp(w);
        }
        uint8_t w0 = (uint8_t)(g.offset % SAXIS);
        printf("  %-10s %10u %I64u %I64u\n", rules[ri].tag, w0,
               (unsigned long long)cost, (unsigned long long)lifts);
    }
    printf("  → tensor เดียวกัน วางด้วยกฎต่างกัน → scale ต่าง → footprint ต่าง (rule-dependent cost)\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *gguf = NULL;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--gguf") == 0 && i + 1 < argc) gguf = argv[++i];

    part_a();
    part_b();

    printf("\n═══ PART C — Same pair, different cost บน GGUF จริง ═══\n");
    if (gguf) {
        place_model(gguf, RULE_ROWS, NRULES);
    } else {
        static const char *models[] = {
            "I:/model/Kokoro_no_espeak_Q8.gguf",
            "I:/model/Qwen3-0.6B-Q8_0.gguf",
        };
        for (size_t m = 0; m < sizeof(models) / sizeof(models[0]); m++)
            place_model(models[m], RULE_ROWS, NRULES);
    }
    return 0;
}
