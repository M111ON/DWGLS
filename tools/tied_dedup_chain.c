/* tied_dedup_chain.c — §15.75: tied-embedding dedup wired เข้า chain จริง
 * ═══════════════════════════════════════════════════════════════════════════
 * โจทย์จาก handoff 2026-08-17 (#1 เปิดไว้): tensor byte-identical (tied
 * embeddings — output.weight == token_embd.weight) → registry {id→home} →
 * freeze ครั้งเดียว → ตัวที่สองไม่กิน field ไม่ freeze (route ชี้ home)
 *
 * พิสูจน์บน GGUF จริง (dual pass — ตัวเลขเทียบกันได้โดยตรง):
 *   pass ON  (dedup) : home_of[] จาก tied_dedup_scan — dup ข้าม placement
 *   pass OFF (baseline): ทุก tensor วางเหมือนเดิม (dedup ปิด)
 *   ทั้งสอง pass verify byte-for-byte (home thaw จาก bond · dup ผ่าน route)
 *
 * placement = กฎ trained CAP_RULE_* (115,115,3.0,1,256K §15.71) — rank = chunk
 * ใน tensor (model เดียวกับ field_trainer) · freeze = sub-piece 64KB bond จาก
 * ghost_piece(gid, sub, w) (model เดียวกับ cap_chain_scan)
 *
 * BUILD: make tied_dedup
 * RUN  : ./build/tied_dedup_chain <model.gguf> [--dedup|--no-dedup|--both] [--no-walk]
 *
 * §15.77: walk-based access เหนือ dedup field — เดินนาฬิกา (round=w, tick=w%12)
 * tick-by-tick จาก state ใด → หา chunk ที่ live ที่ตำแหน่ง (scale w) → อ่านผ่าน
 * bond (dup tensor อ่านผ่าน registry → home bond เดียว) — พิสูจน์ lossless
 * ทุก tensor ทุกตำแหน่งบน field ที่ dedup แล้ว
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"
#include "../core/tied_dedup.h"
#include "../core/fibo_walk.h"

static uint32_t next_pow2(uint64_t v) {
    uint32_t p = 64;
    while ((uint64_t)p < v) p <<= 1;
    return p;
}

/* ═══════════════════════════════════════════════════════════════════════
   §15.77 WALK-BASED ACCESS เหนือ DEDUP FIELD
   ═══════════════════════════════════════════════════════════════════════
   แนวคิด (ต่อ §15.76): state = (seed, round, tick) พอ — เดินนาฬิกา tick-by-tick
   จาก state ใดก็ได้ → ที่ตำแหน่ง (r, t) route ที่ live = {chunk : w == r และ
   w % 12 == t} โดย w = cap_rule_scale(rank) (scale axis 144 = รอบของสนาม)

   chunk index = สนามที่คำนวณใหม่จาก (method + seed) — deterministic: tensor
   (ขนาด/ลำดับ = seed) × chunk rank r → w = (stride·r+offset)%144 · gid = home's
   gid (registry: dup chunk ชี้ home — block เดียวกับที่ freeze)

   อ่าน chunk ที่ live = thaw bond (ghost_piece(gid, sub, w)) — dup อ่านผ่าน
   home bond เดียว (telescope: bond ไม่ผูก route) → memcmp กับต้นฉบับ
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t *tensor;   /* tensor id ของ chunk         */
    uint32_t *rank;     /* chunk rank ใน tensor        */
    uint8_t  *w;        /* to_scale = live round (0..143) */
    uint64_t *gid;      /* home gid สำหรับ bond (dup → home) */
    uint64_t *off;      /* byte offset ใน tensor       */
    uint32_t  n;        /* จำนวน chunk ทั้งหมด (home + dup) */
    uint32_t  lifted;   /* chunks w > kmax (frozen)    */
    uint32_t  dup_chunks; /* chunks ของ tensor ซ้ำ      */
    uint32_t  dup_lifted; /* dup chunks ที่ frozen (ผ่าน home) */
} ChunkIndex;

typedef struct {
    uint64_t coverage, located, lossless_read, pointer_home;
    uint32_t max_steps;
    uint32_t chunks_total;    /* จำนวน chunk ทั้งหมด (home + dup)  */
    uint32_t dup_chunks_total;/* จำนวน chunk ของ tensor ซ้ำ        */
    uint32_t dup_read_ok;    /* dup chunks อ่านผ่าน home bond ได้   */
    uint32_t rule_diff;      /* chunk ที่ตำแหน่งต่างเมื่อใช้กฎอื่น   */
    int      empty_ok;
} WalkStat;

/* สร้าง chunk index — สนามที่คำนวณใหม่ (deterministic จาก tensor metadata) */
static uint32_t build_chunk_index(const uint32_t *sizes, uint32_t n,
                                  const int32_t *home_of, const uint64_t *base_gid,
                                  ChunkIndex *idx) {
    uint32_t k_max = ght_envelope_depth(CAP_RULE_GATE);
    uint64_t total = 0;
    for (uint32_t t = 0; t < n; t++)
        if (home_of[t] >= 0)
            total += (sizes[t] + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;
    if (total == 0 || total > 65000) return 0;

    idx->tensor = (uint32_t *)calloc((size_t)total, sizeof(uint32_t));
    idx->rank   = (uint32_t *)calloc((size_t)total, sizeof(uint32_t));
    idx->w      = (uint8_t  *)calloc((size_t)total, sizeof(uint8_t));
    idx->gid    = (uint64_t *)calloc((size_t)total, sizeof(uint64_t));
    idx->off    = (uint64_t *)calloc((size_t)total, sizeof(uint64_t));
    if (!idx->tensor || !idx->rank || !idx->w || !idx->gid || !idx->off) return 0;

    uint32_t c = 0;
    for (uint32_t t = 0; t < n; t++) {
        if (home_of[t] < 0) continue;
        int32_t src = (home_of[t] == (int32_t)t) ? (int32_t)t : home_of[t];
        int is_dup = (home_of[t] != (int32_t)t);
        uint64_t nchunks = (sizes[t] + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;
        for (uint64_t r = 0; r < nchunks; r++) {
            uint8_t w = (uint8_t)(((uint64_t)CAP_RULE_STRIDE * r + CAP_RULE_OFFSET) % 144u);
            idx->tensor[c] = t;
            idx->rank[c]   = (uint32_t)r;
            idx->w[c]      = w;
            idx->gid[c]    = base_gid[src] + r;      /* registry: dup → home gid */
            idx->off[c]    = r * CAP_RULE_CHUNK;
            if (w > k_max) idx->lifted++;
            if (is_dup) {
                idx->dup_chunks++;
                if (w > k_max) idx->dup_lifted++;
            }
            c++;
        }
    }
    idx->n = c;
    return c;
}

static void chunk_index_free(ChunkIndex *idx) {
    free(idx->tensor); free(idx->rank); free(idx->w);
    free(idx->gid); free(idx->off);
    memset(idx, 0, sizeof(*idx));
}

/* gen callback สำหรับ fibo_walk — chunk c → route (live round = w) */
static void dedup_chunk_route(void *ctx, uint32_t c, FiboWalkRoute *out) {
    const ChunkIndex *idx = (const ChunkIndex *)ctx;
    out->block = (uint16_t)c;
    out->r0    = (uint8_t)(idx->rank[c] & 0xFFu);
    out->rq    = idx->w[c];
}

/* อ่าน chunk c ผ่าน bond (sub-piece 64KB) → เทียบกับ source → 1 = lossless */
static int read_chunk_via_bond(ResidualSpace *rs, const uint8_t *const *data,
                               const uint32_t *sizes, const ChunkIndex *idx,
                               uint32_t c, uint8_t w) {
    uint32_t t = idx->tensor[c];
    uint32_t len = sizes[t] - (uint32_t)idx->off[c];
    if (len > CAP_RULE_CHUNK) len = CAP_RULE_CHUNK;
    const uint8_t *src = data[t] + idx->off[c];
    for (uint32_t s = 0; s * RS_MAX_DATA_SIZE < len; s++) {
        uint32_t sl = len - s * RS_MAX_DATA_SIZE;
        if (sl > RS_MAX_DATA_SIZE) sl = RS_MAX_DATA_SIZE;
        PoglsPiece p = ghost_piece((uint16_t)idx->gid[c], (uint8_t)s, w);
        uint32_t got = 0;
        const uint8_t *chunk = (const uint8_t *)rs_thaw(rs, pogls_bond_key(&p), &got);
        if (!chunk || got != sl) return 0;
        if (memcmp(chunk, src + s * RS_MAX_DATA_SIZE, sl) != 0) return 0;
    }
    return 1;
}

/* พิสูจน์ walk-based access เหนือ field ที่วางแล้ว (ON/OFF ใช้ฟังก์ชันเดียว) */
static int walk_proof_dedup(const uint8_t *const *data, const uint32_t *sizes,
                            uint32_t n, const int32_t *home_of,
                            const uint64_t *base_gid, ResidualSpace *rs,
                            WalkStat *st) {
    ChunkIndex idx; memset(&idx, 0, sizeof(idx));
    uint32_t total = build_chunk_index(sizes, n, home_of, base_gid, &idx);
    if (total == 0) return 0;
    uint32_t k_max = ght_envelope_depth(CAP_RULE_GATE);
    memset(st, 0, sizeof(*st));

    /* coverage: ทุก chunk live ตรง 1 ตำแหน่ง (w, w%12) — Σ == total */
    uint32_t *counts = (uint32_t *)calloc(144u * 12u, sizeof(uint32_t));
    uint64_t total_live = fibo_walk_coverage(dedup_chunk_route, &idx, total, 12u, 144u, counts);
    st->coverage = total_live;
    st->chunks_total = total;
    st->dup_chunks_total = idx.dup_chunks;
    int ok = (total_live == total);

    /* enter-anywhere: 3 start states → เดิน tick-by-tick ถึง (w, w%12) → อ่าน lossless */
    FiboWalkPos starts[3] = { { 0u, 0u, 0 }, { 72u, 2u, 0 }, { 143u, 11u, 0 } };
    for (int s = 0; s < 3; s++) {
        uint64_t located = 0, lossless = 0, ph = 0;
        for (uint32_t c = 0; c < total; c++) {
            FiboWalkPos target = { idx.w[c], (uint32_t)(idx.w[c] % 12u), 0 };
            FiboWalkPos pos;
            if (!fibo_walk_to(NULL, NULL, 12u, 144u, starts[s],
                              target.round, target.tick, &pos)) { ok = 0; continue; }
            uint64_t expect = fibo_walk_dist(&starts[s], &target, 12u, 144u);
            if (pos.steps - starts[s].steps != expect) { ok = 0; continue; }
            uint32_t d = (uint32_t)(pos.steps - starts[s].steps);
            if (d > st->max_steps) st->max_steps = d;
            located++;
            if (idx.w[c] > k_max) {
                int r = read_chunk_via_bond(rs, data, sizes, &idx, c, idx.w[c]);
                if (r) {
                    lossless++;
                    if (s == 0 && home_of[idx.tensor[c]] != (int32_t)idx.tensor[c])
                        st->dup_read_ok++;   /* dup ผ่าน registry → home bond */
                } else ok = 0;
            } else {
                ph++;   /* admit/reject → pointer-home (data อยู่ใน source) */
            }
        }
        if (s == 0) { st->located = located; st->lossless_read = lossless; st->pointer_home = ph; }
        if (located != total || lossless != idx.lifted) ok = 0;
    }

    /* ตำแหน่งว่าง: tick ≠ w%12 → ไม่มี route live */
    FiboWalkPos pe = { 143u, 0u, 0 };   /* 143%12 = 11 → tick 0 ว่าง */
    FiboWalkRoute live[64];
    uint32_t lc = fibo_walk_live(dedup_chunk_route, &idx, total, 12u, &pe, live, 64);
    st->empty_ok = (lc == 0);
    ok = ok && st->empty_ok;

    /* rule-other: กฎอื่น (37,0) → chunk อยู่ตำแหน่งต่าง → field ต่างจริง */
    st->rule_diff = 0;
    for (uint32_t c = 0; c < total; c++) {
        uint8_t w2 = (uint8_t)(((uint64_t)37u * idx.rank[c] + 0u) % 144u);
        if (w2 != idx.w[c]) st->rule_diff++;
    }
    ok = ok && st->rule_diff >= 1;

    free(counts);
    chunk_index_free(&idx);
    return ok;
}


int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = NULL;
    int mode = 2;   /* 0 = --no-dedup · 1 = --dedup · 2 = --both */
    int walk = 1;   /* --no-walk ปิด walk-based access proof */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dedup") == 0) mode = 1;
        else if (strcmp(argv[i], "--no-dedup") == 0) mode = 0;
        else if (strcmp(argv[i], "--both") == 0) mode = 2;
        else if (strcmp(argv[i], "--walk") == 0) walk = 1;
        else if (strcmp(argv[i], "--no-walk") == 0) walk = 0;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) { printf("usage: tied_dedup_chain <model.gguf> [--dedup|--no-dedup|--both] [--no-walk]\n"); return 2; }

    GGUFBox box;
    if (gguf_box_open(&box, path) != 0) { printf("cannot open %s\n", path); return 1; }
    uint32_t n = box.n_tensors;
    if (n > TIED_MAX_TENSORS) {
        printf("✗ %u tensors > TIED_MAX_TENSORS %u — ข้าม\n", n, (unsigned)TIED_MAX_TENSORS);
        gguf_box_close(&box);
        return 1;
    }

    const uint8_t **data = (const uint8_t **)calloc(n, sizeof(uint8_t *));
    uint32_t *sizes = (uint32_t *)calloc(n, sizeof(uint32_t));
    int32_t  *home_of = (int32_t *)calloc(n, sizeof(int32_t));
    int32_t  *home_off = (int32_t *)calloc(n, sizeof(int32_t));
    uint64_t *base_gid = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!data || !sizes || !home_of || !home_off || !base_gid) return 1;

    uint64_t data_bytes = 0, max_tensor = 0;
    uint32_t n_placed = 0;
    for (uint32_t i = 0; i < n; i++) {
        const GGUFBoxEntry *e = &box.entries[i];
        data[i] = e->data;
        sizes[i] = e->size;
        if (e->data && e->size > 0) { data_bytes += e->size; n_placed++; }
        if (e->size > max_tensor) max_tensor = e->size;
    }
    printf("═══ TIED-EMBEDDING DEDUP → CHAIN (§15.75) ═══\n");
    printf("model : %s (%u tensors · data %llu MB · %u placed)\n", path, n,
           (unsigned long long)(data_bytes >> 20), n_placed);
    printf("rule  : stride %u · offset %u · gate %.1f (kmax %u) · orbit %u · chunk %u\n",
           CAP_RULE_STRIDE, CAP_RULE_OFFSET, (double)CAP_RULE_GATE,
           (unsigned)ght_envelope_depth(CAP_RULE_GATE), CAP_RULE_ORBIT, CAP_RULE_CHUNK);

    /* ── registry scan: byte-identical → home ── */
    uint64_t dup_bytes = tied_dedup_scan(data, sizes, n, home_of);
    uint32_t groups = 0;
    for (uint32_t i = 0; i < n; i++) if (home_of[i] >= 0 && home_of[i] != (int32_t)i) groups++;
    printf("\n── registry {id → home} (identity = memcmp — FNV แค่ตัวกรอง) ──\n");
    if (groups == 0) {
        printf("  (ไม่พบ tensor byte-identical — dedup 0 MB)\n");
    } else {
        for (uint32_t j = 0; j < n; j++) {
            if (home_of[j] < 0 || home_of[j] == (int32_t)j) continue;
            printf("  ▶ %s == %s  (%u MB — เก็บ 1 copy, %s ชี้ route ไป %s)\n",
                   box.entries[j].name, box.entries[home_of[j]].name,
                   sizes[j] >> 20, box.entries[j].name, box.entries[home_of[j]].name);
        }
        printf("  dedup: %u tensor(s), %llu MB = %.1f%% ของ data section — registry 0 extra bytes\n",
               groups, (unsigned long long)(dup_bytes >> 20),
               data_bytes ? 100.0 * (double)dup_bytes / (double)data_bytes : 0.0);
    }

    /* ── rs sizing (ไม่ evict: load ≤ 0.5 — next_pow2(2×sub-pieces)) ── */
    uint64_t home_bytes = 0, total_chunks = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (home_of[i] != (int32_t)i) continue;         /* dup ไม่นับ (ข้าม placement) */
        home_bytes += sizes[i];
        total_chunks += (sizes[i] + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;
    }
    if (total_chunks > 65000) {
        printf("✗ total chunks %llu > uint16 block_id — model ใหญ่เกิน probe นี้\n",
               (unsigned long long)total_chunks);
        return 1;
    }
    uint64_t need = (home_bytes + RS_MAX_DATA_SIZE - 1) / RS_MAX_DATA_SIZE;
    uint32_t rs_cap = next_pow2(need * 2);
    uint8_t *scratch = (uint8_t *)malloc(max_tensor ? max_tensor : 1);
    if (!scratch) return 1;

    TiedChainStats st_on, st_off;
    int lossless_on = 1, lossless_off = 1;

    /* ── pass 1: dedup ON ── */
    if (mode >= 1) {
        ResidualSpace rs;
        if (rs_init(&rs, rs_cap) != 0) { printf("rs_init fail\n"); return 1; }
        if (tied_place(&rs, data, sizes, n, home_of, base_gid, &st_on) != 0) {
            printf("✗ place (dedup) fail\n"); return 1;
        }
        lossless_on = (tied_verify(&rs, data, sizes, n, home_of, base_gid,
                                   scratch, (uint32_t)max_tensor) == 0);
        printf("\n── pass DEDUP ON ──\n");
        printf("  field %llu slots · lift %llu chunks · rej %llu · frozen %llu MB (%u entries)\n",
               (unsigned long long)st_on.field_slots, (unsigned long long)st_on.lifts,
               (unsigned long long)st_on.rejects,
               (unsigned long long)(st_on.frozen_bytes >> 20), st_on.rs_count);
        printf("  lossless byte-for-byte: %s\n", lossless_on ? "OK ✓ (ทุก tensor — dup ผ่าน route → home)" : "FAIL ✗");
        if (walk) {
            WalkStat wst;
            int wok = walk_proof_dedup(data, sizes, n, home_of, base_gid, &rs, &wst);
            printf("  §15.77 walk (dedup field): coverage %llu/%u · enter-anywhere 3/3 · "
                   "lossless %llu/%u lifted (%llu pointer-home) · max %u steps%s\n",
                   (unsigned long long)wst.coverage, wst.located ? (unsigned)wst.located : 0u,
                   (unsigned long long)wst.lossless_read, wst.lossless_read + wst.pointer_home,
                   (unsigned long long)wst.pointer_home, wst.max_steps,
                   wok ? "  ✓" : "  ✗");
            printf("       registry: dup %u/%u chunks อ่านผ่าน home bond เดียว · "
                   "rule-other (37,0) %u/%u ตำแหน่งต่าง · empty position %s\n",
                   wst.dup_read_ok, wst.dup_chunks_total,
                   wst.rule_diff, wst.chunks_total,
                   wst.empty_ok ? "✓" : "✗");
            lossless_on = lossless_on && wok;
        }
        rs_free(&rs);
    }

    /* ── pass 2: dedup OFF (baseline) ── */
    if (mode == 0 || mode == 2) {
        for (uint32_t i = 0; i < n; i++)
            home_off[i] = (data[i] && sizes[i] > 0) ? (int32_t)i : -1;
        ResidualSpace rs;
        if (rs_init(&rs, rs_cap) != 0) { printf("rs_init fail\n"); return 1; }
        if (tied_place(&rs, data, sizes, n, home_off, base_gid, &st_off) != 0) {
            printf("✗ place (baseline) fail\n"); return 1;
        }
        lossless_off = (tied_verify(&rs, data, sizes, n, home_off, base_gid,
                                    scratch, (uint32_t)max_tensor) == 0);
        printf("\n── pass DEDUP OFF (baseline) ──\n");
        printf("  field %llu slots · lift %llu chunks · rej %llu · frozen %llu MB (%u entries)\n",
               (unsigned long long)st_off.field_slots, (unsigned long long)st_off.lifts,
               (unsigned long long)st_off.rejects,
               (unsigned long long)(st_off.frozen_bytes >> 20), st_off.rs_count);
        printf("  lossless byte-for-byte: %s\n", lossless_off ? "OK ✓" : "FAIL ✗");
        if (walk) {
            WalkStat wst;
            int wok = walk_proof_dedup(data, sizes, n, home_off, base_gid, &rs, &wst);
            printf("  §15.77 walk (baseline field): coverage %llu/%u · enter-anywhere 3/3 · "
                   "lossless %llu/%u lifted (%llu pointer-home) · max %u steps%s\n",
                   (unsigned long long)wst.coverage, wst.located ? (unsigned)wst.located : 0u,
                   (unsigned long long)wst.lossless_read, wst.lossless_read + wst.pointer_home,
                   (unsigned long long)wst.pointer_home, wst.max_steps,
                   wok ? "  ✓" : "  ✗");
            lossless_off = lossless_off && wok;
        }
        rs_free(&rs);
    }

    /* ── verdict ── */
    printf("\n════════════════════════════════════════════════════════════\n");
    if (mode == 2) {
        uint64_t saved = st_off.frozen_bytes - st_on.frozen_bytes;
        printf("  frozen (physical): OFF %llu MB → ON %llu MB  (↓ %llu MB = dup ที่ไม่ freeze)\n",
               (unsigned long long)(st_off.frozen_bytes >> 20),
               (unsigned long long)(st_on.frozen_bytes >> 20),
               (unsigned long long)(saved >> 20));
        printf("  field: OFF %llu → ON %llu slots · lift: OFF %llu → ON %llu\n",
               (unsigned long long)st_off.field_slots, (unsigned long long)st_on.field_slots,
               (unsigned long long)st_off.lifts, (unsigned long long)st_on.lifts);
        printf("  registry {id→home}: %u route(s) — 0 bytes (coordinate = address)\n", groups);
    }
    int ok = (mode == 1) ? lossless_on : ((mode == 0) ? lossless_off
                                          : (lossless_on && lossless_off));
    printf("═══ VERDICT: %s ═══\n", ok ? "LOSSESS + DEDUP + WALK ✓ (freeze ครั้งเดียว — walk หา route จาก state, dup อ่านผ่าน home bond)"
                                        : "FAIL — ดูด้านบน");
    printf("rs capacity %u · dup bytes %llu MB\n", rs_cap,
           (unsigned long long)(dup_bytes >> 20));

    free(scratch); free(base_gid); free(home_off); free(home_of);
    free(sizes); free(data);
    gguf_box_close(&box);
    return ok ? 0 : 1;
}
