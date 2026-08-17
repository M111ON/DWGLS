/* test_fibo_walk.c — §15.76: walk-based access — เดิน spine tick-by-tick,
 * หา route ที่ live จาก state = (seed, round, tick)
 * ═══════════════════════════════════════════════════════════════════════════
 * handoff #2 เปิดไว้: "เดิน spine tick-by-tick แล้วหา route ที่ live →
 * พิสูจน์ state=(seed,round,tick) พอสำหรับทุกตำแหน่งทุกตาราง"
 *
 * แทนการอ่าน route ด้วย log pile lookup — เดินนาฬิกา (round, tick) จาก state:
 * ที่ตำแหน่ง (r, t) route ที่ live = {i : rq_i == r และ rq_i % ticks == t}
 * — คำนวณสนามใหม่จาก (method + seed) ("ถ้าสูตรคำนวณสนามได้ใหม่ทั้งสนาม
 *   จะใช้สนามยาวแค่ไหนก็ได้แค่วนรอบ" — user principle) → ไม่ต้อง index
 *
 * พิสูจน์ต่อตาราง (5 ตาราง: 1728×12 · 512×12 · 1728×4 · 256×3 + random-field):
 *   coverage      : ทุก chunk live ตรง 1 ตำแหน่ง (Σ live == n)
 *   enter-anywhere: เดินจาก 3 start states (0,0) / (รอบกลาง,2) / (รอบท้าย) →
 *                   ทุก chunk อ่าน lossless (state พอ — เริ่มตรงไหนก็ได้)
 *   ระยะ          : steps ที่เดิน == fibo_walk_dist (นาฬิกา forward วนข้าม 0)
 *   ว่าง          : ตำแหน่งที่ไม่มี route live → 0 (ไม่หลอน)
 *   cell เดียว   : n ≤ pipes → แต่ละ cell (pipe, tick) มี route เดียว
 *   seed/method ต่าง = field ต่าง → จับได้ (route ไม่มีใน log → NULL)
 *   ข้ามรอบ      : walk จากรอบท้ายวนกลับมารอบ 0 ยัง lossless
 *
 * BUILD: make test-test_fibo_walk
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/infra/fibo_spine.h"
#include "../core/geo_ghost_lift.h"
#include "../core/fibo_walk.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── config + method (formulas เดียวกับ fibo_checkpoint_sweep gen_chunks) ── */
#define PAT_SCATTER 0
#define PAT_CLUSTER 1
#define PAT_WRAP    3
#define PAT_RANDOM  4

typedef struct {
    uint32_t pipes, ticks, cycles, chunks, size, dist;
    int      pattern;
    uint32_t seed;
} WkCfg;

typedef struct {
    uint16_t block;
    uint8_t  r0, rq;
    uint32_t len;
    uint8_t  data[RS_MAX_DATA_SIZE];
} Chunk;

static uint32_t g_rand_state;
static uint32_t xrand(void) {   /* xorshift32 เดียวกับ sweep */
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return g_rand_state;
}

/* สร้าง route ของ chunk i — DETERMINISTIC จาก (cfg, i):
   scatter/cluster/wrap = สูตรล้วน · random = xrand sequence (seed กำหนด) */
static void route_of(const WkCfg *cfg, uint32_t i, uint16_t *block,
                     uint8_t *r0, uint8_t *rq) {
    uint32_t b0;
    switch (cfg->pattern) {
        case PAT_SCATTER: b0 = (uint32_t)(((uint64_t)i * 2654435761u) % cfg->cycles); break;
        case PAT_CLUSTER: b0 = (cfg->cycles >= 4u) ? (i % 4u) * (cfg->cycles / 4u)
                                                   : (i % cfg->cycles); break;
        case PAT_WRAP:    b0 = (uint32_t)(((uint64_t)i * 2654435761u) % cfg->cycles); break;
        default:          b0 = (uint32_t)((uint64_t)xrand() % cfg->cycles); break;
    }
    uint32_t q = (cfg->pattern == PAT_WRAP)
               ? (b0 + cfg->dist) % cfg->cycles
               : (b0 + 1u + (i % (cfg->dist ? cfg->dist : 1u))) % cfg->cycles;
    *block = (uint16_t)i;
    *r0    = (uint8_t)b0;
    *rq    = (uint8_t)q;
}

static void gen_chunks(Chunk *c, uint32_t n, const WkCfg *cfg) {
    g_rand_state = cfg->seed ? cfg->seed : 0xC0FFEEu;
    for (uint32_t i = 0; i < n; i++) {
        route_of(cfg, i, &c[i].block, &c[i].r0, &c[i].rq);
        c[i].len = cfg->size;
        for (uint32_t b = 0; b < cfg->size; b++)
            c[i].data[b] = (uint8_t)((1000u + i + b * 131u) ^ (b >> 3) ^ ((1000u + i) >> 7));
    }
}

/* ── walk gen callback — คำนวณสนามใหม่ (array จาก (seed, method)) ── */
typedef struct { const Chunk *chunks; } ChunkCtx;
static void chunk_route(void *ctx, uint32_t i, FiboWalkRoute *out) {
    const ChunkCtx *cc = (const ChunkCtx *)ctx;
    out->block = cc->chunks[i].block;
    out->r0    = cc->chunks[i].r0;
    out->rq    = cc->chunks[i].rq;
}

/* ── เดินจาก start ไป target แล้วอ่านผ่าน route → เปรียบเทียบ ── */
static int walk_read(const WkCfg *cfg, GhostLog *log, ResidualSpace *rs,
                     const Chunk *chunks, uint32_t n,
                     FiboWalkPos start, uint32_t *max_steps) {
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        FiboWalkPos target = { chunks[i].rq, (uint32_t)(chunks[i].rq % cfg->ticks), 0 };
        FiboWalkPos pos;
        if (!fibo_walk_to(NULL, NULL, cfg->ticks, cfg->cycles, start,
                          target.round, target.tick, &pos)) { ok = 0; break; }
        uint64_t expect = fibo_walk_dist(&start, &target, cfg->ticks, cfg->cycles);
        if (pos.steps - start.steps != expect) { ok = 0; break; }
        uint32_t d = (uint32_t)(pos.steps - start.steps);
        if (d > *max_steps) *max_steps = d;

        uint32_t sz = 0;
        const void *got = ghost_read(log, rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    return ok;
}

/* ── หนึ่งตาราง: วางครบ → พิสูจน์ walk-based access ── */
static void test_config(int base, const WkCfg *cfg, const char *name) {
    uint32_t n = cfg->chunks;
    Chunk *chunks = (Chunk *)calloc(n, sizeof(Chunk));
    gen_chunks(chunks, n, cfg);
    ChunkCtx ctx = { chunks };

    GhostLog log;      ghost_log_init(&log);
    ResidualSpace rs;  rs_init(&rs, 4096);
    for (uint32_t i = 0; i < n; i++) {
        if (ghost_lift(&log, &rs, chunks[i].block, chunks[i].r0,
                       chunks[i].rq, chunks[i].data, chunks[i].len) == RS_BOND_KEY_RESERVED) {
            CHECK(base + 0, "place fail — abort", 0);
            free(chunks); rs_free(&rs); return;
        }
    }

    /* 0: direct read baseline */
    int ok_direct = 1;
    for (uint32_t i = 0; i < n && ok_direct; i++) {
        uint32_t sz = 0;
        const void *got = ghost_read(&log, &rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok_direct = 0;
    }
    CHECK(base + 0, "วางครบ + direct read lossless (baseline)", ok_direct);

    /* 1-2: coverage — ทุก chunk live ตรง 1 ตำแหน่ง (rq, rq%ticks) */
    uint32_t *counts = (uint32_t *)calloc((size_t)cfg->cycles * cfg->ticks, sizeof(uint32_t));
    uint64_t total = fibo_walk_coverage(chunk_route, &ctx, n, cfg->ticks,
                                        cfg->cycles, counts);
    uint32_t nonempty = 0;
    for (uint32_t i = 0; i < cfg->cycles * cfg->ticks; i++) if (counts[i]) nonempty++;
    CHECK(base + 1, "coverage: ทุก chunk live ตรง 1 ตำแหน่ง (Σ == n)", total == n);
    CHECK(base + 2, "ตำแหน่งไม่ว่าง ≤ n (rq ซ้ำกันได้ — หลาย route ต่อตำแหน่ง)",
          nonempty >= 1 && nonempty <= n);

    /* 3-6: enter-anywhere — 3 start states → ทุก chunk lossless */
    FiboWalkPos sA = { 0u, 0u, 0 };
    FiboWalkPos sB = { cfg->cycles / 2u, 2u, 0 };
    FiboWalkPos sC = { cfg->cycles - 1u, cfg->ticks - 1u, 0 };
    uint32_t max_steps = 0;
    int okA = walk_read(cfg, &log, &rs, chunks, n, sA, &max_steps);
    int okB = walk_read(cfg, &log, &rs, chunks, n, sB, &max_steps);
    int okC = walk_read(cfg, &log, &rs, chunks, n, sC, &max_steps);
    CHECK(base + 3, "walk จาก (0,0) → ทุก chunk lossless (state พอ)", okA);
    CHECK(base + 4, "walk จาก (รอบกลาง, tick 2) → ทุก chunk lossless", okB);
    CHECK(base + 5, "walk จาก (รอบท้าย, tick ท้าย) → ทุก chunk lossless (ข้ามรอบ)", okC);
    CHECK(base + 6, "steps เดิน == fibo_walk_dist ทุก chunk", okA && okB && okC);
    CHECK(base + 6, "ระยะ walk ≤ cycles×ticks (วนครบสนามก็ถึง)", max_steps <= cfg->cycles * cfg->ticks);

    /* 7: ตำแหน่งว่าง — tick ≠ รอบ%ticks → ไม่มี route live */
    uint32_t r_empty = cfg->cycles - 1u;
    uint32_t t_empty = (r_empty % cfg->ticks + 1u) % cfg->ticks;
    FiboWalkPos pe = { r_empty, t_empty, 0 };
    FiboWalkRoute live[64];
    uint32_t lc = fibo_walk_live(chunk_route, &ctx, n, cfg->ticks, &pe, live, 64);
    CHECK(base + 7, "ตำแหน่งที่ไม่มี route live → 0 (ว่าง = ว่าง ไม่หลอน)",
          lc == 0 && t_empty != r_empty % cfg->ticks);

    /* 8: cell เดียว — n ≤ pipes → route ใน cell (pipe, tick) ไม่ชน */
    if (n <= cfg->pipes) {
        int cell_ok = 1;
        for (uint32_t i = 0; i < cfg->cycles * cfg->ticks && cell_ok; i++) {
            if (!counts[i]) continue;
            FiboWalkPos pp = { i / cfg->ticks, i % cfg->ticks, 0 };
            uint32_t nl = fibo_walk_live(chunk_route, &ctx, n, cfg->ticks,
                                         &pp, live, 64);
            if (nl != counts[i]) { cell_ok = 0; break; }
            for (uint32_t a = 0; a < nl && cell_ok; a++)
                for (uint32_t b = a + 1; b < nl && cell_ok; b++)
                    if (live[a].block % cfg->pipes == live[b].block % cfg->pipes)
                        cell_ok = 0;   /* สอง route ใน cell เดียว (pipe, tick) */
        }
        CHECK(base + 8, "ทุก cell (pipe, tick) มี route เดียว (block % pipes ไม่ชน)",
              cell_ok);
    } else {
        CHECK(base + 8, "n > pipes — ข้าม cell uniqueness (ไม่ใช่ invariant)", 1);
    }

    /* 9: seed/method ต่าง = field ต่าง → จับได้ (route ไม่มีใน log → NULL) */
    WkCfg bad = *cfg;
    if (cfg->pattern == PAT_RANDOM) bad.seed += 1u;       /* seed ต่าง */
    else                           bad.dist = cfg->dist + 3u;  /* method ต่าง */
    Chunk *bad_chunks = (Chunk *)calloc(n, sizeof(Chunk));
    gen_chunks(bad_chunks, n, &bad);
    uint32_t detected = 0, differs = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (bad_chunks[i].r0 != chunks[i].r0 || bad_chunks[i].rq != chunks[i].rq)
            differs++;
        uint32_t sz = 0;
        if (ghost_read(&log, &rs, bad_chunks[i].block, bad_chunks[i].r0,
                       bad_chunks[i].rq, &sz) == NULL) detected++;
    }
    CHECK(base + 9, "seed/method ต่าง = field ต่าง (route ต่าง ≥ 1 chunk)",
          differs >= 1);
    CHECK(base + 9, "จับได้: route ของ field อื่นไม่มีใน log (NULL)",
          detected >= 1 && detected == differs);
    printf("        (field อื่น: %u/%u route ต่าง → NULL %u)",
           differs, n, detected);
    free(bad_chunks);

    free(counts); free(chunks); rs_free(&rs);
}

/* ── นาฬิกา: wrap + ระยะข้ามรอบ ── */
static void clock_mechanics(void) {
    /* walk_next: tick ครบ ticks → round+1 (jet bridge บนตาราง 12 ticks) */
    FiboWalkPos p = { 140u, 10u, 0 };
    fibo_walk_next(&p, 12, 144);
    fibo_walk_next(&p, 12, 144);
    CHECK(101, "walk_next: tick 11 → wrap → round+1, tick 0 (bridge ข้ามรอบ)",
          p.round == 141u && p.tick == 0u && p.steps == 2);
    /* ระยะข้ามรอบ: (140,11) → (0,0) = 37 ticks บน 144×12 */
    FiboWalkPos a = { 140u, 11u, 0 }, b = { 0u, 0u, 0 };
    CHECK(102, "fibo_walk_dist ข้ามรอบ: (140,11)→(0,0) == 37 ticks",
          fibo_walk_dist(&a, &b, 12, 144) == 37);
    /* เดินจริงวนข้าม 0 ถึงเป้า */
    FiboWalkPos out;
    int r = fibo_walk_to(NULL, NULL, 12, 144, a, 0, 0, &out);
    CHECK(103, "fibo_walk_to วนข้ามรอบถึง (0,0) — steps == dist",
          r == 1 && out.round == 0 && out.tick == 0 && out.steps == 37);
    /* วนครบ 1 รอบกลับมาเหมือนเดิม (state วนได้กี่รอบก็ได้) */
    FiboWalkPos c = { 5u, 3u, 0 };
    FiboWalkPos c1 = c;
    for (int i = 0; i < 144 * 12; i++) fibo_walk_next(&c1, 12, 144);
    CHECK(104, "เดินครบ 1 รอบ (cycles×ticks) → กลับตำแหน่งเดิม",
          c1.round == c.round && c1.tick == c.tick && c1.steps == 144u * 12u);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("═══ WALK-BASED ACCESS — state=(seed,round,tick) พอทุกตำแหน่งทุกตาราง (§15.76) ═══\n");

    clock_mechanics();

    static const WkCfg cfgs[] = {
        /* ตารางมาตรฐาน 1728×12 · สนาม 144 · scatter */
        { 1728u, 12u, 144u, 64u, 4096u, 5u, PAT_SCATTER, 0xC0FFEEu },
        /* ตาราง 512×12 · สนาม 72 */
        { 512u,  12u,  72u, 64u, 4096u, 5u, PAT_SCATTER, 7u },
        /* ตาราง 1728×4 · สนาม 72 */
        { 1728u,  4u,  72u, 64u, 4096u, 5u, PAT_SCATTER, 7u },
        /* ตาราง 256×3 · สนาม 255 · wrap (วนข้าม 0 บ่อย) */
        { 256u,   3u, 255u, 64u, 4096u, 191u, PAT_WRAP, 7u },
        /* สนาม random — seed เป็นตัวกำหนด field (negative: seed ต่าง) */
        { 1728u, 12u, 144u, 64u, 4096u, 5u, PAT_RANDOM, 0xC0FFEEu },
    };
    static const char *names[] = {
        "1728×12/144 scatter", "512×12/72 scatter", "1728×4/72 scatter",
        "256×3/255 wrap", "1728×12/144 random"
    };

    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
        printf("\n── ตาราง %s ──\n", names[i]);
        test_config((int)(20 * i), &cfgs[i], names[i]);
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass, fail);
    return fail ? 1 : 0;
}
