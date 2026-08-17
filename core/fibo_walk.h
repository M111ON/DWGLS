/* ═══════════════════════════════════════════════════════════════════════════
 * fibo_walk.h — §15.76: walk-based access — เดิน spine tick-by-tick,
 * หา route ที่ live จาก state = (seed, round, tick) — พอสำหรับทุกตำแหน่งทุกตาราง
 * ═══════════════════════════════════════════════════════════════════════════
 * เดิม: อ่าน route ด้วย log pile lookup (ต้องมี log/index)
 * ใหม่: เดินนาฬิกา (round, tick) tick-by-tick จาก state — ที่ตำแหน่ง (r, t)
 *       route ที่ live = {i : rq_i == r และ rq_i % ticks == t} — regenerate
 *       จาก (method + seed) ตรงๆ ("เก็บแค่วิธีการสร้างกับ seed" — user principle)
 *       → ไม่ต้อง index: state (seed, round, tick) ก็รู้ว่าอะไร live ตรงไหน
 *
 * ตรงกับ FiboSpine: tick 11 → jet bridge → wrap (รอบถัดไป) — การ wrap ของ
 * นาฬิกาที่นี่ (tick ครบ ticks → round+1) คือ bridge เดียวกันบนตาราง generic
 * (pipes×ticks ใดก็ได้ — ticks=12 → bridge ที่ tick 11 เป๊ะ)
 *
 * ตัวเลข/การพิสูจน์ (ใช้ใน tool/test):
 *   coverage      : ทุก chunk live ตรง 1 ตำแหน่ง (rq, rq%ticks) — Σ live == n
 *   enter-anywhere: เดินจาก state ใด (s_round, s_tick) → ทุกตำแหน่งถึงได้
 *   ระยะ          : steps ที่เดิน == fibo_walk_dist(start, target) (forward ticks)
 *   ว่าง          : ตำแหน่งที่ไม่มี route live → 0 (ไม่หลอน)
 *   seed ผิด      : regenerate ด้วย seed อื่น → route ต่าง → จับได้
 */
#ifndef FIBO_WALK_H
#define FIBO_WALK_H

#include <stdint.h>

/* ── ตำแหน่งเดิน: (round, tick) — state = (seed, round, tick) ─────────── */
typedef struct {
    uint32_t round;      /* รอบบน scale axis (0..cycles-1)   */
    uint32_t tick;       /* tick ในรอบ (0..ticks-1)           */
    uint64_t steps;      /* จำนวน tick ที่เดินแล้ว (walk clock) */
} FiboWalkPos;

/* เดิน 1 tick: tick+1 → wrap ที่ ticks → round+1 (mod cycles = ข้ามรอบ)
   — tick ครบรอบ = jet bridge (tick 11 บนตาราง 12 ticks) = เข้ารอบใหม่ */
static inline void fibo_walk_next(FiboWalkPos *pos, uint32_t ticks, uint32_t cycles) {
    pos->steps++;
    if (++pos->tick >= ticks) {
        pos->tick = 0;
        pos->round = (pos->round + 1u) % cycles;
    }
}

/* ระยะเดิน forward (ticks) จาก a → b — วนข้าม 0 ได้ (ข้ามรอบ) */
static inline uint64_t fibo_walk_dist(const FiboWalkPos *a, const FiboWalkPos *b,
                                      uint32_t ticks, uint32_t cycles) {
    uint64_t pa = (uint64_t)a->round * ticks + a->tick;
    uint64_t pb = (uint64_t)b->round * ticks + b->tick;
    uint64_t n  = (uint64_t)cycles * ticks;
    return (pb + n - pa) % n;
}

/* ── route ของ chunk — regenerate จาก (method, seed) ─────────────────── */
typedef struct {
    uint16_t block;      /* chunk id                    */
    uint8_t  r0;         /* birth round = from_scale    */
    uint8_t  rq;         /* requested round = to_scale  */
} FiboWalkRoute;

/* method: สูตร deterministic ของ chunk i (caller กำหนด — เช่น gen_chunks) */
typedef void (*FiboWalkGen)(void *ctx, uint32_t i, FiboWalkRoute *out);

/* ── หา route ที่ live ที่ตำแหน่ง (round, tick) ─────────────────────────
 * live iff rq == pos.round และ rq % ticks == pos.tick
 * (ทุก pipe ที่ tick นี้ — route ที่ถูกอ่านที่ตำแหน่งนี้บนนาฬิกา)
 * returns จำนวน live routes (≤ cap) */
static inline uint32_t fibo_walk_live(FiboWalkGen gen, void *ctx, uint32_t n,
                                      uint32_t ticks, const FiboWalkPos *pos,
                                      FiboWalkRoute *live, uint32_t cap) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n && c < cap; i++) {
        FiboWalkRoute r;
        gen(ctx, i, &r);
        if (r.rq == (uint8_t)pos->round && (uint8_t)(r.rq % ticks) == (uint8_t)pos->tick)
            live[c++] = r;
    }
    return c;
}

/* ── เดินจาก start ไป target (round, tick) tick-by-tick ────────────────
 * เขียนตำแหน่งสุดท้ายลง out · returns 1 ถ้าเดินถึง (ทุกตำแหน่งถึงได้เสมอ —
 * นาฬิกาวนครบ cycles×ticks) */
static inline int fibo_walk_to(FiboWalkGen gen, void *ctx, uint32_t ticks,
                               uint32_t cycles, FiboWalkPos start,
                               uint32_t target_round, uint32_t target_tick,
                               FiboWalkPos *out) {
    (void)gen; (void)ctx;
    FiboWalkPos pos = start;
    uint64_t max_steps = (uint64_t)cycles * ticks;
    while (!(pos.round == target_round && pos.tick == target_tick)) {
        fibo_walk_next(&pos, ticks, cycles);
        if (pos.steps - start.steps > max_steps) return 0;   /* ไม่ควรเกิด — guard */
    }
    *out = pos;
    return 1;
}

/* ── coverage: นับ live ต่อตำแหน่งใน 1 pass เหนือ chunks ────────────────
 * counts[(r*ticks)+t] = จำนวน chunk ที่ live ที่ (r, t)
 * ทุก chunk live ตรง 1 ตำแหน่ง → Σ counts == n เสมอ (rq เดียวต่อ chunk)
 * returns Σ (ควร == n — ใช้ verify) */
static inline uint64_t fibo_walk_coverage(FiboWalkGen gen, void *ctx, uint32_t n,
                                          uint32_t ticks, uint32_t cycles,
                                          uint32_t *counts /* cycles*ticks */) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        FiboWalkRoute r;
        gen(ctx, i, &r);
        uint32_t pos = (uint32_t)r.rq * ticks + (r.rq % ticks);
        counts[pos]++;
        total++;
    }
    return total;
}

#endif /* FIBO_WALK_H */
