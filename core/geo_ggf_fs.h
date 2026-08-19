/* ═══════════════════════════════════════════════════════════════════════════
 * geo_ggf_fs.h — §15.96: GGFS — .ggf checkpoint directory เป็น geometric FS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "GeoFS mount จริง" ของ checkpoint (.ggf + manifest): เปิด directory เดียว
 * (chain ได้ — base → delta → ... → head) แล้วอ่าน tensor ด้วย STATE
 * (seed, round, tick) — เหมือน GeosVolume.walk แต่ไฟล์จริงคือ .ggf:
 *
 *   mount  : manifest + chain → walk clock (seed/ticks/cycles) + dedup
 *            registry (tied_dedup) + paths (chain resolve → ไฟล์จริง)
 *   read   : state (round, tick) ใดก็ได้ (enter-anywhere) → เดินนาฬิกาไป
 *            ตำแหน่ง live ของ tensor → เปิด home .ggf (zero-copy mmap,
 *            lazy per-home) → CRC32 ตรวจครั้งแรก (ไฟล์พังจับทันที) → memcpy
 *   stat   : name/size/rq/tick/home/dup/status/level (ระดับ chain ที่ไฟล์อยู่)
 *
 * ตรงหลัก: coordinate = address (state → ตำแหน่ง → ไฟล์) · deterministic +
 * replay ได้ · dedup ระดับไฟล์ (dup → อ่านจาก home) · chain resolve
 * (SAME → เดินลงลึกจนเจอ STORED) · mid-round (tensor ก่อน checkpoint =
 * pending — ยังไม่ live)
 *
 * ใช้โดย: tools/ggf_fs_probe.c (พิสูจน์ไฟล์จริง) + tests/test_ggf_fs.c (TIER1)
 */

#ifndef GEO_GGF_FS_H
#define GEO_GGF_FS_H

#include "geo_ggf_ckpt.h"

/* ── stat ต่อ tensor (มองผ่าน "หน้าต่าง" ของ FS) ── */
typedef struct {
    char     name[GGF_CKPT_NAME_LEN];
    uint32_t size;
    int32_t  home;            /* registry {idx → home} · -1 = ว่าง          */
    uint8_t  status;          /* GGF_CKPT_STORED / GGF_CKPT_SAME (head)     */
    uint8_t  dup;             /* 1 = dup — อ่านผ่าน home                    */
    uint8_t  pending;         /* 1 = อยู่ก่อน checkpoint กลางรอบ (ยังไม่ live) */
    uint32_t rq;              /* rq = ggf_walk_rq_of(seed, idx) — deterministic */
    uint32_t tick;            /* rq % ticks                                 */
    uint32_t level;           /* ระดับใน chain ที่ไฟล์จริงอยู่ (0 = head)     */
    char     file_dir[GGF_CKPT_DIR_LEN];   /* dir ที่ไฟล์จริงอยู่ (chain resolve) */
} GgfsStat;

/* ── mount: checkpoint dir → FS (manifest + chain + walk + maps) ── */
typedef struct {
    GgfCkptChain chain;                     /* manifest chain (head → base)  */
    GgfWalkTable walk;                      /* walk clock + registry + paths */
    GGFMap maps[GGF_WALK_MAX_TENSORS];      /* zero-copy map cache (lazy)    */
    uint8_t vfy[(GGF_WALK_MAX_TENSORS + 7) / 8]; /* home CRC verified bit    */
    uint32_t rq[GGF_WALK_MAX_TENSORS];      /* คำนวณจาก seed (scratch ของ walk) */
    uint32_t sizes[GGF_WALK_MAX_TENSORS];   /* n_bytes ต่อ tensor (walk อ้าง)  */
    int32_t  home_of[GGF_WALK_MAX_TENSORS]; /* registry (walk อ้าง)           */
    char path_buf[GGF_WALK_MAX_TENSORS][256];  /* paths (chain resolve)      */
    uint32_t n, seed, ticks, cycles;
    uint32_t ckpt_round, ckpt_tick;         /* checkpoint กลางรอบ (0 = จากเริ่ม) */
    uint64_t walk_steps;                    /* สะสม steps ที่เดิน (หลักฐาน walk) */
} GgfsMount;

/* ── mount — เปิด directory (manifest + chain) ──
 * returns 0 = ok · -1 = manifest/chain อ่านไม่ได้ · -2 = n เกินขีด */
static inline int ggfs_mount(const char *dir, GgfsMount *fs)
{
    if (!dir || !fs) return -1;
    memset(fs, 0, sizeof *fs);
    if (ggf_ckpt_chain_open(dir, &fs->chain) != 0) return -1;
    const GgfCkptHeader *h = &fs->chain.links[0].h;
    const GgfCkptEntry  *e = fs->chain.links[0].e;
    uint32_t n = fs->chain.n;
    if (n > GGF_WALK_MAX_TENSORS) { ggf_ckpt_chain_close(&fs->chain); return -2; }
    fs->n = n;
    fs->seed   = h->seed;
    fs->ticks  = h->ticks;
    fs->cycles = h->cycles;
    fs->ckpt_round = h->ckpt_round;
    fs->ckpt_tick  = h->ckpt_tick;

    /* paths/registry: เก็บไว้ใน struct (walk อ้าง pointer — ต้องอยู่ถาวร) */
    const char *paths[GGF_WALK_MAX_TENSORS];
    for (uint32_t i = 0; i < n; i++) {
        paths[i] = fs->path_buf[i];
        ggf_ckpt_chain_path(&fs->chain, i, e[i].name, fs->path_buf[i], 256);
        fs->sizes[i]   = e[i].size;
        fs->home_of[i] = e[i].home_of;
    }
    ggf_walk_init(&fs->walk, fs->seed, fs->ticks, fs->cycles, n,
                  paths, fs->sizes, fs->home_of, fs->rq);
    return 0;
}

static inline void ggfs_unmount(GgfsMount *fs)
{
    if (!fs) return;
    for (uint32_t i = 0; i < fs->n; i++) ggf_unmap(&fs->maps[i]);
    ggf_ckpt_chain_close(&fs->chain);
    memset(fs, 0, sizeof *fs);
}

/* ── จำนวน tensor (files) ── */
static inline uint32_t ggfs_count(const GgfsMount *fs)
{
    return fs ? fs->n : 0;
}

/* ── find tensor by name · returns idx หรือ -1 ── */
static inline int32_t ggfs_find(const GgfsMount *fs, const char *name)
{
    if (!fs || !name) return -1;
    for (uint32_t i = 0; i < fs->n; i++)
        if (strcmp(fs->chain.links[0].e[i].name, name) == 0)
            return (int32_t)i;
    return -1;
}

/* ── stat — มอง tensor ผ่านหน้าต่าง FS (rq/tick/home/dup/status/level) ──
 * returns 0 = ok · -1 = idx เกิน / arg ผิด */
static inline int ggfs_stat(const GgfsMount *fs, uint32_t idx, GgfsStat *st)
{
    if (!fs || !st || idx >= fs->n) return -1;
    const GgfCkptEntry *e = fs->chain.links[0].e;
    memset(st, 0, sizeof *st);
    memcpy(st->name, e[idx].name, sizeof st->name);        /* สอง buffer ขนาดเท่ากัน */
    st->size   = e[idx].size;
    st->home   = e[idx].home_of;
    st->status = e[idx].status;
    st->dup    = (e[idx].home_of >= 0 && e[idx].home_of != (int32_t)idx) ? 1 : 0;
    st->rq     = fs->rq[idx];
    st->tick   = st->rq % fs->ticks;
    /* level: ระดับ chain ที่ไฟล์จริงอยู่ (เดินจาก head ลงลึกจนเจอ STORED) */
    for (int d = 0; d < fs->chain.depth; d++) {
        if (fs->chain.links[d].e[idx].status == GGF_CKPT_STORED) {
            st->level = (uint32_t)d;
            memcpy(st->file_dir, fs->chain.links[d].dir, sizeof st->file_dir);
            break;
        }
    }
    /* pending: อยู่ก่อน checkpoint กลางรอบ → ยังไม่ live */
    if (fs->ckpt_round || fs->ckpt_tick) {
        uint64_t ckpt_pos = (uint64_t)fs->ckpt_round * fs->ticks + fs->ckpt_tick;
        uint64_t pos = (uint64_t)st->rq * fs->ticks + st->tick;
        if (pos < ckpt_pos) st->pending = 1;
    }
    return 0;
}

/* ── read tensor ด้วย state (round, tick) ใดก็ได้ (enter-anywhere) ──
 * เดินนาฬิกาไปตำแหน่ง live → เปิด home .ggf (zero-copy) + CRC ตรวจครั้งแรก
 * → memcpy ลง buf · walk_steps สะสม (หลักฐาน: state ต่าง → เส้นทางต่าง)
 * returns 0 = ok · -1 = ว่าง (home < 0) · -2 = pending (ก่อน checkpoint) ·
 * -3 = เปิด home map ไม่ได้ · -4 = CRC ไฟล์พัง (verify-on-open จับ) ·
 * -5 = ขนาดไม่ตรง manifest · -6 = buf เล็กเกิน · -7 = idx เกิน · -8 = เดินไม่ถึง */
static inline int ggfs_read(GgfsMount *fs, uint32_t idx,
                            uint32_t round, uint32_t tick,
                            uint8_t *buf, uint64_t cap, uint64_t *got)
{
    if (!fs || idx >= fs->n) return -7;
    int32_t home = fs->walk.home_of[idx];
    if (home < 0) return -1;                       /* ว่าง */
    if (fs->ckpt_round || fs->ckpt_tick) {         /* mid-round gate */
        uint64_t ckpt_pos = (uint64_t)fs->ckpt_round * fs->ticks + fs->ckpt_tick;
        uint64_t pos = (uint64_t)fs->rq[idx] * fs->ticks + (fs->rq[idx] % fs->ticks);
        if (pos < ckpt_pos) return -2;             /* ยังไม่ live */
    }
    uint32_t sz = fs->walk.sizes[idx];
    if (sz > cap) return -6;

    /* enter-anywhere: เดินจาก state ที่ให้ → ตำแหน่ง live ของ tensor */
    FiboWalkPos start = { round, tick, 0 }, end;
    if (!ggf_walk_to(&fs->walk, start, idx, &end)) return -8;
    fs->walk_steps += end.steps - start.steps;

    /* เปิด home .ggf (zero-copy mmap — lazy ต่อ home) */
    GGFMap *m = ggf_walk_map_open(&fs->walk, idx, fs->maps);
    if (!m) return -3;
    uint32_t h = (uint32_t)home;
    if (!(fs->vfy[h >> 3] & (1u << (h & 7)))) {    /* CRC ตรวจครั้งแรก */
        if (ggf_map_verify(m) != 0) return -4;    /* ไฟล์พัง — จับทันที */
        fs->vfy[h >> 3] |= (1u << (h & 7));
    }
    if (m->h.n_bytes != sz) return -5;
    int rc = ggf_map_read(m, 0, buf, sz);
    if (rc != 0) return rc;
    if (got) *got = sz;
    return 0;
}

/* ── read by name (find + read) — สะดวกสำหรับ CLI ── */
static inline int ggfs_read_by_name(GgfsMount *fs, const char *name,
                                    uint32_t round, uint32_t tick,
                                    uint8_t *buf, uint64_t cap, uint64_t *got)
{
    int32_t idx = ggfs_find(fs, name);
    if (idx < 0) return -9;                        /* ไม่พบ */
    return ggfs_read(fs, (uint32_t)idx, round, tick, buf, cap, got);
}

/* ── zero-copy: chunk k ของ tensor idx → pointer ตรงเข้า mapping ของ home
 * (ไม่ copy — ใช้เทียบ/ตรวจได้โดยไม่จอง buffer) · NULL = fail */
static inline const uint8_t *ggfs_node(GgfsMount *fs, uint32_t idx,
                                       uint64_t k, uint32_t round, uint32_t tick)
{
    if (!fs || idx >= fs->n) return NULL;
    int32_t home = fs->walk.home_of[idx];
    if (home < 0) return NULL;
    /* เดิน + เปิด map (ไม่นับ steps ที่นี่ — read เป็นตัวนับ) */
    FiboWalkPos start = { round, tick, 0 }, end;
    if (!ggf_walk_to(&fs->walk, start, idx, &end)) return NULL;
    GGFMap *m = ggf_walk_map_open(&fs->walk, idx, fs->maps);
    if (!m) return NULL;
    uint32_t h = (uint32_t)home;
    if (!(fs->vfy[h >> 3] & (1u << (h & 7)))) {
        if (ggf_map_verify(m) != 0) return NULL;
        fs->vfy[h >> 3] |= (1u << (h & 7));
    }
    if (k >= m->h.n_chunks) return NULL;
    return ggf_map_node(m, k, NULL);
}

#endif /* GEO_GGF_FS_H */
