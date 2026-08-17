/* test_fibo_checkpoint.c — fibo clock checkpoint-replay: state = (seed, round, tick)
 * ═══════════════════════════════════════════════════════════════════════════
 * แนวคิด (user): "สนาม deterministic + checkpoint + step tick ของการเดิน
 *   ถ้าสูตรคำนวณสนามได้ใหม่ทั้งสนามแน่นอน จะใช้สนามยาวแค่ไหนก็ได้แค่วนรอบ
 *   เราเก็บแค่วิธีการสร้างกับ seed ในการสร้างพอ"
 *
 * การแมปกับ engine ปัจจุบัน:
 *   - สนาม     = deterministic: ghost_origin_seed(block, round) → fibo_addr
 *   - การเดิน   = FiboSpine: round = walk_pos / FS_SLOTS, tick = walk_pos % 12
 *   - round     = from_scale (birth round — ส่วนของ bond, เสาเข็มห้ามขยับ)
 *   - route     = (from → to) 5B/event — telescope: 1 entry ครอบ ANY distance
 *   - checkpoint = serialize(residual_space + ghost_log) + (round, tick, seed)
 *   - jump+replay = reload เข้า space/log ใหม่ → อ่านต่อที่ round ใดก็ได้
 *
 * พิสูจน์:
 *   A. spine wrap mechanics (tick 11 → jet bridge → re-entry, รอบถัดไป)
 *   B. address identity: round อยู่ใน bond, to_scale ไม่อยู่ (telescope ได้)
 *   C. checkpoint กลางรอบ → restart → reload → อ่านข้อมูลก่อน checkpoint
 *      lossless + เดินต่อหลัง checkpoint (วางข้อมูลรอบใหม่) + อ่านครบทั้ง 64
 *   D. telescope: from=3→to=140 = 137 steps ใน 1 entry + ข้ามรอบ (140→3)
 *   E. overhead จริง: checkpoint image vs data bytes — ราคาของ "jump anywhere"
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-fibo_checkpoint tests/test_fibo_checkpoint.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/infra/fibo_spine.h"
#include "../core/geo_ghost_lift.h"
#include "../core/ckpt_wang.h"

#define N_CHUNKS    64u
#define CHUNK_SZ    4096u
#define CKPT_ROUND  72u            /* checkpoint ที่รอบกลาง (RC_N_CYCLES/2) */
#define CKPT_HEADER 28u            /* seed(8) + round(8) + tick(4) + ver/res(8) */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static uint64_t bond_of(uint16_t block, uint8_t from, uint8_t to) {
    PoglsPiece p = ghost_piece(block, from, to);
    return pogls_bond_key(&p);
}

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 131u) ^ (i >> 3) ^ (seed >> 7));
}

/* ── chunk ledger: birth round + requested round + payload ── */
typedef struct {
    uint16_t block;      /* chunk id                          */
    uint8_t  r0;         /* birth round = from_scale          */
    uint8_t  rq;         /* requested round = to_scale        */
    uint32_t len;
    uint8_t  data[CHUNK_SZ];
} Chunk;

/* birth round แบบ deterministic: (i*37)%144 — 37 coprime กับ 144 → รอบไม่ซ้ำ */
static void make_chunks(Chunk *c, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        c[i].block = (uint16_t)i;
        c[i].r0    = (uint8_t)((i * 37u) % RC_N_CYCLES);
        c[i].rq    = (uint8_t)((c[i].r0 + 5u + i % 13u) % RC_N_CYCLES);
        c[i].len   = CHUNK_SZ;
        fill_pattern(c[i].data, CHUNK_SZ, 1000u + i);
    }
}

/* sort by birth round — placement เดินตามรอบจากน้อยไปมาก */
static int cmp_r0(const void *a, const void *b) {
    const Chunk *x = (const Chunk *)a, *y = (const Chunk *)b;
    return (x->r0 > y->r0) - (x->r0 < y->r0);
}

/* ── A. spine wrap mechanics (เดินจริง ไม่กี่สิบ tick) ────── */
static void spine_mechanics(void) {
    printf("\nA. spine wrap — tick 11 → jet bridge → re-entry, รอบถัดไป\n");
    CHECK(1, "FS_SLOTS = 1728 pipes × 12 ticks = 20736 (1 รอบของ field)",
          FS_SLOTS == FS_PIPES * FS_TICKS_PER_CYCLE && FS_SLOTS == 20736u);

    FiboSpine fs;
    fibo_spine_init(&fs);
    uint32_t b1 = fibo_spine_tick_n(&fs, 12);      /* 1 cycle: tick 11 fires 1 bridge */
    CHECK(2, "12 ticks → 1 jet bridge (tick 11) + re-entry, global_tick = 2",
          b1 == 1 && fs.tick_count == 12 && fs.global_tick == 2 &&
          fs.bridge_state == JB_INACTIVE);

    uint32_t b2 = fibo_spine_tick_n(&fs, 13);      /* 2nd cycle crossing */
    CHECK(3, "25 ticks → 2 bridges (wrap ข้ามรอบได้) + tick ต่อเนื่อง",
          b2 == 1 && fs.tick_count == 25 && fs.global_tick == 5);
    /* 2nd bridge fires at tick 21 (gt=11 → reset to 1) → +4 ticks = 5 */

    /* round = walk_pos / FS_SLOTS — ใช้ tick_count เป็น walk clock */
    CHECK(4, "round ที่ checkpoint = 72 (กลาง 144 รอบ) — RC_N_CYCLES ครบ",
          RC_N_CYCLES == 144u && CKPT_ROUND < RC_N_CYCLES &&
          FS_JET_BRIDGE_TICK == 11u && FS_REENTRY_TICK == 13u);
}

/* ── B. address identity: round ใน bond, route ไม่แตะ bond ── */
static void address_identity(void) {
    printf("\nB. address identity — round อยู่ใน bond (เสาเข็ม), route ไม่แตะ\n");
    CHECK(5, "round ต่าง → bond ต่าง (birth round เป็นส่วนของ address)",
          bond_of(7, 3, 5) != bond_of(7, 9, 5));
    CHECK(6, "to_scale ต่าง → bond เท่าเดิม (telescope: 1 data, หลาย route)",
          bond_of(7, 3, 5) == bond_of(7, 3, 140));
    CHECK(7, "deterministic: (block, round, to) เดิม → bond เดิมเสมอ",
          bond_of(21, 100, 7) == bond_of(21, 100, 7) &&
          bond_of(21, 100, 7) != bond_of(21, 101, 7));
}

/* ── C+D+E. checkpoint-replay กลางรอบ ───────────────────── */
static int checkpoint_replay(void) {
    printf("\nC. checkpoint @round %u → restart → reload → jump → lossless\n", CKPT_ROUND);

    Chunk chunks[N_CHUNKS];
    make_chunks(chunks, N_CHUNKS);
    qsort(chunks, N_CHUNKS, sizeof(Chunk), cmp_r0);

    uint64_t data_bytes = 0;
    for (uint32_t i = 0; i < N_CHUNKS; i++) data_bytes += chunks[i].len;

    /* ── phase 1: วาง chunk ที่ birth round <= CKPT_ROUND ── */
    GhostLog log;      ghost_log_init(&log);
    ResidualSpace rs;  rs_init(&rs, 4096);
    P5HRibcage rc;     p5h_ribcage_init(&rc, NULL);   /* ledger — bond+round */

    uint32_t pre = 0, placed = 0;
    for (uint32_t i = 0; i < N_CHUNKS; i++) {
        if (chunks[i].r0 > CKPT_ROUND) continue;
        uint64_t bk = ghost_lift(&log, &rs, chunks[i].block, chunks[i].r0,
                                 chunks[i].rq, chunks[i].data, chunks[i].len);
        if (bk == RS_BOND_KEY_RESERVED) {
            printf("  (place failed at chunk %u: block=%u r0=%u rq=%u)\n",
                   i, chunks[i].block, chunks[i].r0, chunks[i].rq);
            return -1;
        }
        /* ledger: tick = requested tick, pipe = block % 1728 */
        p5h_ribcage_step(&rc, (uint16_t)(chunks[i].block % FS_PIPES),
                         (uint8_t)(chunks[i].rq % FS_TICKS_PER_CYCLE), bk);
        placed++; if (chunks[i].r0 <= CKPT_ROUND) pre++;
    }
    printf("  placed %u/%u chunks ก่อน checkpoint (round 0..%u)\n", placed, N_CHUNKS, CKPT_ROUND);

    /* verify ก่อน serialize */
    int ok_pre = 1;
    for (uint32_t i = 0; i < N_CHUNKS && ok_pre; i++) {
        if (chunks[i].r0 > CKPT_ROUND) continue;
        uint32_t out_sz = 0;
        const void *got = ghost_read(&log, &rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &out_sz);
        if (!got || out_sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok_pre = 0;
    }
    CHECK(8, "pre-checkpoint: อ่านทุก chunk ที่วางแล้ว lossless (live space)", ok_pre);

    /* ── checkpoint image: seed(8) + round(8) + tick(4) + ver(4) + reserved(4)
          + ghost log + wang digest + residual space
          — "เก็บแค่วิธีการสร้างกับ seed" + wang gate ก่อน decode (§15.50/15.55) ── */
    uint64_t rsz = rs_serialize_size(&rs);
    uint64_t lsz = ghost_log_serialize_size(&log);
    uint32_t dsz = ckpt_wang_digest_size(log.count);
    uint64_t image_sz = CKPT_HEADER + lsz + dsz + rsz;
    uint8_t *img = (uint8_t *)malloc((size_t)image_sz);
    uint8_t *p = img;
    uint64_t seed = GHOST_SEED_MAGIC;
    memcpy(p, &seed, 8); p += 8;                    /* method + seed (field formula) */
    uint64_t ckpt_round = CKPT_ROUND;  memcpy(p, &ckpt_round, 8); p += 8;
    uint32_t ckpt_tick = 2;            memcpy(p, &ckpt_tick, 4);  p += 4;
    p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 0;          /* version + ver reserved */
    p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0; p += 8;  /* reserved (CKPT_HEADER=28) */
    CHECK(9, "checkpoint serialize (header + log + wang digest + space) ครบขนาด",
          ghost_log_serialize(&log, p, lsz) == lsz && (p += lsz) &&
          (ckpt_wang_digest(&log, p), (p += dsz)) &&
          rs_serialize(&rs, p, rsz) == rsz && p + rsz == img + image_sz);

    /* ── phase 2: restart (ทุกอย่างใหม่) + reload checkpoint ── */
    GhostLog log2;      ghost_log_init(&log2);
    ResidualSpace rs2;  rs_init(&rs2, 4096);
    P5HRibcage rc2;     p5h_ribcage_init(&rc2, NULL);
    const uint8_t *q = img + 8;
    uint64_t r2; memcpy(&r2, q, 8); q += 8;         /* round กู้คืนจาก checkpoint */
    q += 12;                                        /* tick(4) + ver/res(8) */
    /* ก่อน decode: load log → wang gate ตรวจ digest + edges ของทั้ง log
       (จับ corrupted checkpoint ก่อน thaw อะไร — user request) */
    int wang_ok = 0;
    if (ghost_log_load(&log2, q, lsz) == 0) {
        q += lsz;
        wang_ok = (ckpt_wang_check(&log2, q, dsz) == 0);
        q += dsz;
    }
    CHECK(10, "reload checkpoint (round 72 ถูกกู้คืน) + log + wang gate เปิด + space",
          r2 == CKPT_ROUND && wang_ok && rs_load(&rs2, q, rsz) == 0);
    CHECK(10, "wang digest จับ corrupted ก่อน decode (reject r≠0)",
          ({
              uint8_t *img3 = (uint8_t *)malloc((size_t)image_sz);
              memcpy(img3, img, (size_t)image_sz);
              /* log region เริ่มที่ img+28: GHST(4)+ver(2)+rsv(2)+count(4)
                 → entries เริ่มที่ img+40; byte 3 ของ entry 0 = to_scale */
              img3[40 + 3] ^= 0x08;
              GhostLog l3; ghost_log_init(&l3);
              const uint8_t *q3 = img3 + 8 + 8 + 12;
              int loaded = (ghost_log_load(&l3, q3, lsz) == 0);
              int rej = (ckpt_wang_check(&l3, q3 + lsz, dsz) != 0);
              free(img3);
              loaded && rej;
          }));
    CHECK(10, "count/bytes ตรงกับก่อน serialize",
          rs2.count == rs.count && rs2.total_bytes == rs.total_bytes &&
          log2.count == log.count);

    /* ledger ใหม่ rebuild จาก log (log = audit trail = checkpoint) */
    uint32_t rc_rebuild = 0;
    for (uint32_t i = 0; i < log2.count; i++) {
        const GhostLogEntry *e = &log2.entries[i];
        p5h_ribcage_step(&rc2, (uint16_t)(e->block_id % FS_PIPES),
                         (uint8_t)(e->to_scale % FS_TICKS_PER_CYCLE),
                         bond_of(e->block_id, e->from_scale, e->to_scale));
        rc_rebuild++;
    }
    CHECK(11, "ribcage ledger rebuild จาก log — จำนวน entry ตรง (log = ข้อมูล checkpoint)",
          rc2.entry_count == rc_rebuild && rc2.entry_count == log2.count);

    /* reconstruct ทุก chunk ก่อน checkpoint จาก state ที่ reload (jump ไปรอบใดก็ได้) */
    int ok_ckpt = 1;
    for (uint32_t i = 0; i < N_CHUNKS && ok_ckpt; i++) {
        if (chunks[i].r0 > CKPT_ROUND) continue;
        uint32_t out_sz = 0;
        const void *got = ghost_read(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &out_sz);
        if (!got || out_sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok_ckpt = 0;
    }
    CHECK(12, "หลัง restart: reconstruct ทุก chunk ก่อน checkpoint lossless (จาก checkpoint image)", ok_ckpt);

    /* ── phase 3: เดินต่อ (jump) วาง chunk ที่ birth round > CKPT_ROUND ── */
    uint32_t post = 0;
    for (uint32_t i = 0; i < N_CHUNKS; i++) {
        if (chunks[i].r0 <= CKPT_ROUND) continue;
        uint64_t bk = ghost_lift(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                 chunks[i].rq, chunks[i].data, chunks[i].len);
        if (bk == RS_BOND_KEY_RESERVED) {
            printf("  (post place failed at chunk %u)\n", i);
            return -1;
        }
        p5h_ribcage_step(&rc2, (uint16_t)(chunks[i].block % FS_PIPES),
                         (uint8_t)(chunks[i].rq % FS_TICKS_PER_CYCLE), bk);
        post++;
    }
    printf("  เดินต่อหลัง checkpoint: +%u chunks (round %u..143) — state ต่อเนื่อง\n", post, CKPT_ROUND + 1);

    /* อ่านครบทุก chunk (ก่อน + หลัง checkpoint) → lossless ข้ามรอบ */
    int ok_all = 1;
    for (uint32_t i = 0; i < N_CHUNKS && ok_all; i++) {
        uint32_t out_sz = 0;
        const void *got = ghost_read(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &out_sz);
        if (!got || out_sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok_all = 0;
    }
    CHECK(13, "อ่านครบ 64 chunks ข้ามรอบ (ก่อน+หลัง checkpoint) byte-for-byte lossless", ok_all);

    /* ความผิดพลาดของ round: เสาเข็มห้ามขยับ — round ผิด → bond แตก */
    uint8_t wrong_r0 = (uint8_t)((chunks[1].r0 + 1u) % RC_N_CYCLES);
    uint8_t wrong_rq = (uint8_t)((chunks[1].rq + 1u) % RC_N_CYCLES);
    uint32_t out_sz = 0;
    CHECK(14, "birth round ผิด → bond แตก (NULL) — ข้ามรอบไม่ได้โดยไม่ได้รับอนุญาต",
          ghost_read(&log2, &rs2, chunks[1].block, wrong_r0, chunks[1].rq, &out_sz) == NULL);
    CHECK(14, "requested round ผิด → route ไม่มี (NULL)",
          ghost_read(&log2, &rs2, chunks[1].block, chunks[1].r0, wrong_rq, &out_sz) == NULL);

    /* ── D. telescope: 1 route ครอบ 137 steps + ข้ามรอบ ── */
    printf("\nD. telescope — 1 entry (5B) ครอบ ANY distance บนแกน scale\n");
    uint8_t far[512], wrap[512];
    fill_pattern(far, sizeof(far), 777); fill_pattern(wrap, sizeof(wrap), 888);
    uint64_t b_far  = ghost_lift(&log2, &rs2, 200, 3, 140, far, sizeof(far));
    uint64_t b_wrap = ghost_lift(&log2, &rs2, 201, 140, 3, wrap, sizeof(wrap));
    CHECK(15, "lift from=3 → to=140 (137 steps) 1 entry + from=140 → to=3 (ข้ามรอบ) 1 entry",
          b_far != RS_BOND_KEY_RESERVED && b_wrap != RS_BOND_KEY_RESERVED);
    uint32_t fsz = 0, wsz = 0;
    const void *gf = ghost_read(&log2, &rs2, 200, 3, 140, &fsz);
    const void *gw = ghost_read(&log2, &rs2, 201, 140, 3, &wsz);
    CHECK(16, "อ่านที่ to=140 หลังขยับ 137 steps lossless (ท่องไปไกลใน 1 route)",
          gf && fsz == sizeof(far) && memcmp(gf, far, sizeof(far)) == 0);
    CHECK(16, "อ่านข้ามรอบ (140 → 3) lossless — วนรอบก็ได้ แค่ route เดียว",
          gw && wsz == sizeof(wrap) && memcmp(gw, wrap, sizeof(wrap)) == 0);
    CHECK(16, "แต่ละ route นับได้ 1 entry (5B) — cost ∝ events ไม่ใช่ distance",
          ghost_route_count(&log2, 200) == 1 && ghost_route_count(&log2, 201) == 1);

    /* ── E. overhead จริง ── */
    uint64_t final_rsz = rs_serialize_size(&rs2);
    uint64_t final_lsz = ghost_log_serialize_size(&log2);
    uint32_t final_dsz = ckpt_wang_digest_size(log2.count);
    uint64_t total_data = data_bytes + sizeof(far) + sizeof(wrap);
    uint64_t total_img  = CKPT_HEADER + final_rsz + final_lsz + final_dsz;
    double overhead_b = (double)(total_img - total_data);
    double overhead_pct = 100.0 * overhead_b / (double)total_data;
    printf("\nE. overhead ของ checkpoint จริง\n");
    printf("  data : %llu B (%u chunks × %u B + 2 telescope)\n",
           (unsigned long long)total_data, N_CHUNKS, CHUNK_SZ);
    printf("  image: %llu B (header %u + space %llu + log %llu + wang %u)\n",
           (unsigned long long)total_img, CKPT_HEADER,
           (unsigned long long)final_rsz, (unsigned long long)final_lsz,
           (unsigned)final_dsz);
    printf("  overhead = %+.2f%% (%.1f B/chunk) — log = %llu routes × 5B = %llu B ∝ events\n",
           overhead_pct, overhead_b / (double)(N_CHUNKS + 2),
           (unsigned long long)log2.count, (unsigned long long)final_lsz - 12);
    CHECK(17, "log เป็นสัดส่วน events: 5B/route, 66 routes = 330B — ไม่ขึ้นกับขนาดข้อมูล",
          (final_lsz - 12) == 5ull * log2.count);
    CHECK(17, "overhead < 2% ที่ chunk 4KB (16KB → < 0.5%) — ราคา 'jump anywhere + วนรอบ'",
          overhead_pct < 2.0);

    p5h_ribcage_free(&rc); p5h_ribcage_free(&rc2);
    free(img); rs_free(&rs); rs_free(&rs2);
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("fibo clock checkpoint-replay — state = (seed, round, tick), วนกี่รอบก็ได้\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    spine_mechanics();
    address_identity();
    if (checkpoint_replay() < 0) fail++;   /* สั้นวงจร = FAIL ไม่ใช่ silent pass */

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
