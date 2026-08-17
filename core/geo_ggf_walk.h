/* ═══════════════════════════════════════════════════════════════════════════
 * geo_ggf_walk.h — §15.87: single read path — walk clock + dedup registry +
 * GGFReader (เปิด .ggf แล้ว resolve tensor ด้วย state (seed, round, tick))
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * รวม 3 ชิ้นที่พิสูจน์แล้วเข้า read path เดียว:
 *   1. walk clock (fibo_walk.h §15.76) — state = (seed, round, tick):
 *        tensor t live ที่ตำแหน่ง (rq_t, rq_t % ticks) — rq_t คำนวณจาก
 *        (seed, t) ตรงๆ (เก็บแค่วิธีการสร้างกับ seed — ไม่ต้อง index)
 *   2. dedup registry (tied_dedup.h §15.75) — {tensor_id → home}:
 *        dup tensor → resolve route → home → อ่านจากไฟล์ของ home
 *        (dedup ระดับไฟล์: เก็บ 1 .ggf ต่อ home — dup ไม่มีไฟล์ของตัวเอง)
 *   3. GGFReader (geo_goldberg_file.h §15.86) — lazy read:
 *        เปิด .ggf เมื่อต้องการ (open-on-demand) + seek ต่อ node —
 *        RAM คงที่ ไม่โหลดทั้งไฟล์
 *
 * READ PATH เดียว: state (seed, round, tick) → ตำแหน่งนาฬิกา → live tensor
 * → resolve ผ่าน registry → GGFReader อ่านจาก home .ggf → bytes
 *
 * Registry semantics (เหมือน tied_dedup_scan):
 *   home_of[t] ==  t  → t เป็น home — ไฟล์ = paths[t]
 *   home_of[t] ==  h  → t เป็น dup — อ่านจาก paths[h] (size เท่ากันเสมอ)
 *   home_of[t] == -1  → ไม่มี data — ข้าม
 */

#ifndef GEO_GGF_WALK_H
#define GEO_GGF_WALK_H

#include <stdint.h>
#include <string.h>
#include "fibo_walk.h"
#include "geo_goldberg_file.h"

#define GGF_WALK_MAX_TENSORS 2048u

/* ── ตารางเดิน: seed + วิธีสร้าง → rq ของทุก tensor (deterministic) ──── */
typedef struct {
    uint32_t        seed;
    uint32_t        ticks;        /* 12 (FS_TICKS)                          */
    uint32_t        cycles;       /* rounds บนนาฬิกา (rq ∈ [0, cycles))      */
    uint32_t        n;            /* จำนวน tensor                           */
    const char *const *paths;     /* [n] — .ggf path ของ tensor t           */
    const uint32_t     *sizes;    /* [n] — n_bytes ของ tensor t             */
    const int32_t      *home_of;  /* [n] — registry {tensor_id → home}      */
    uint32_t           *rq_of;    /* [n] — scratch: คำนวณจาก (seed, t)     */
} GgfWalkTable;

/* rq ของ tensor t — deterministic จาก (seed, t): ทดรอบด้วย stride ใหญ่ */
static inline uint32_t ggf_walk_rq_of(uint32_t seed, uint32_t idx, uint32_t cycles)
{
    uint64_t x = (uint64_t)seed + (uint64_t)idx * UINT64_C(2654435761);
    return (uint32_t)(x % cycles);
}

static inline void ggf_walk_init(GgfWalkTable *t, uint32_t seed, uint32_t ticks,
                                 uint32_t cycles, uint32_t n,
                                 const char *const *paths,
                                 const uint32_t *sizes, const int32_t *home_of,
                                 uint32_t *rq_scratch)
{
    t->seed    = seed;
    t->ticks   = ticks;
    t->cycles  = cycles;
    t->n       = n;
    t->paths   = paths;
    t->sizes   = sizes;
    t->home_of = home_of;
    t->rq_of   = rq_scratch;
    for (uint32_t i = 0; i < n; i++)
        t->rq_of[i] = ggf_walk_rq_of(seed, i, cycles);
}

/* ตำแหน่งนาฬิกาที่ tensor t live — (round = rq, tick = rq % ticks) */
static inline void ggf_walk_pos(const GgfWalkTable *t, uint32_t idx,
                                FiboWalkPos *out)
{
    uint32_t rq = t->rq_of[idx];
    out->round = rq;
    out->tick  = rq % t->ticks;
    out->steps = 0;
}

/* live tensors ที่ตำแหน่ง (round, tick) — scan นาฬิกา (เหมือน fibo_walk_live)
 * returns จำนวน live (≤ cap) · -1 = position เกินขอบ */
static inline int32_t ggf_walk_live(const GgfWalkTable *t, const FiboWalkPos *pos,
                                    uint32_t *live, uint32_t cap)
{
    if (pos->round >= t->cycles || pos->tick >= t->ticks) return -1;
    uint32_t c = 0;
    for (uint32_t i = 0; i < t->n && c < cap; i++) {
        uint32_t rq = t->rq_of[i];
        if (rq == pos->round && (rq % t->ticks) == pos->tick)
            live[c++] = i;
    }
    return (int32_t)c;
}

/* เดินจาก start state ใดก็ได้ ไปตำแหน่งของ tensor t (enter-anywhere) —
 * เขียนตำแหน่งสุดท้ายลง out · returns 1 ถ้าเดินถึง (วนครบ cycles×ticks เสมอ) */
static inline int ggf_walk_to(const GgfWalkTable *t, FiboWalkPos start,
                              uint32_t idx, FiboWalkPos *out)
{
    FiboWalkPos target;
    ggf_walk_pos(t, idx, &target);
    return fibo_walk_to(NULL, NULL, t->ticks, t->cycles, start,
                        target.round, target.tick, out);
}

/* resolve tensor t → home (ผ่าน registry) · -1 = ข้าม (ไม่มี data) */
static inline int32_t ggf_walk_home(const GgfWalkTable *t, uint32_t idx)
{
    if (idx >= t->n) return -1;
    int32_t h = t->home_of[idx];
    if (h < 0) return -1;                       /* ข้าม */
    return (h == (int32_t)idx) ? (int32_t)idx : h;
}

/* อ่าน tensor t (ผ่าน home .ggf, lazy GGFReader):
 *   cache: array GGFReader ขนาด n (zero-init) — เปิดครั้งแรกที่ใช้ (on-demand)
 *   out: buffer ≥ sizes[t] · out_n = bytes จริง
 * returns: 0 = read lossless · -1 = ไม่มี data · <0 = read/verify fail */
static inline int ggf_walk_read(const GgfWalkTable *t, uint32_t idx,
                                GGFReader *cache, uint8_t *out,
                                uint64_t out_cap, uint64_t *out_n)
{
    int32_t home = ggf_walk_home(t, idx);
    if (home < 0) return -1;
    uint64_t sz = t->sizes[idx];
    if (sz > out_cap) return -2;

    GGFReader *r = &cache[home];
    if (!r->f) {                                 /* lazy open — ครั้งแรกเท่านั้น */
        int rc = ggf_open(t->paths[home], r);
        if (rc != 0) return rc;
    }
    int rc = ggf_read(r, 0, out, sz);
    if (rc != 0) return rc;
    if (out_n) *out_n = sz;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * MMAP READ PATH — เหมือนด้านบน แต่ใช้ GGFMap (zero-copy) แทน GGFReader
 * ═══════════════════════════════════════════════════════════════════
 * map_cache: array GGFMap ขนาด n (zero-init) — เปิด mapping ครั้งแรกที่ใช้
 * (open-on-demand เหมือน GGFReader cache) — หลังใช้ ggf_unmap ทุกตัว
 */

/* เปิด mapping ของ home tensor idx (lazy — ครั้งแรกเท่านั้น) · NULL = fail */
static inline GGFMap *ggf_walk_map_open(const GgfWalkTable *t, uint32_t idx,
                                        GGFMap *map_cache)
{
    int32_t home = ggf_walk_home(t, idx);
    if (home < 0) return NULL;
    GGFMap *m = &map_cache[home];
    if (!m->base) {
        if (ggf_map(t->paths[home], m) != 0) return NULL;
    }
    return m;
}

/* read ทั้ง tensor ผ่าน mmap (memcpy จากเพจ — drop-in แทน ggf_walk_read) */
static inline int ggf_walk_read_map(const GgfWalkTable *t, uint32_t idx,
                                    GGFMap *map_cache, uint8_t *out,
                                    uint64_t out_cap, uint64_t *out_n)
{
    int32_t home = ggf_walk_home(t, idx);
    if (home < 0) return -1;
    uint64_t sz = t->sizes[idx];
    if (sz > out_cap) return -2;
    GGFMap *m = ggf_walk_map_open(t, idx, map_cache);
    if (!m) return -3;
    int rc = ggf_map_read(m, 0, out, sz);
    if (rc != 0) return rc;
    if (out_n) *out_n = sz;
    return 0;
}

/* ZERO-COPY: chunk k ของ tensor idx → pointer ตรงเข้า mapping ของ home
 * (chunk k ของ dup == chunk k ของ home — size เท่ากันเสมอ) · NULL = fail */
static inline const uint8_t *ggf_walk_node_map(const GgfWalkTable *t,
                                               uint32_t idx, uint64_t k,
                                               GGFMap *map_cache)
{
    GGFMap *m = ggf_walk_map_open(t, idx, map_cache);
    if (!m) return NULL;
    if (k >= m->h.n_chunks) return NULL;
    return ggf_map_node(m, k, NULL);
}

/* bytes ที่ dedup ประหยัด (dup tensor ไม่มีไฟล์ของตัวเอง — อ่านจาก home) */
static inline uint64_t ggf_walk_dedup_bytes(const GgfWalkTable *t)
{
    uint64_t saved = 0;
    for (uint32_t i = 0; i < t->n; i++)
        if (t->home_of[i] >= 0 && t->home_of[i] != (int32_t)i)
            saved += t->sizes[i];
    return saved;
}

/* coverage: นับ live ต่อตำแหน่ง (เหมือน fibo_walk_coverage) — Σ == n เสมอ */
static inline uint64_t ggf_walk_coverage(const GgfWalkTable *t,
                                         uint32_t *counts /* cycles*ticks */)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        uint32_t rq = t->rq_of[i];
        counts[rq * t->ticks + (rq % t->ticks)]++;
        total++;
    }
    return total;
}

#endif /* GEO_GGF_WALK_H */
