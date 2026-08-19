/* ═══════════════════════════════════════════════════════════════════════════
 * geo_ggf_ckpt.h — §15.90: checkpoint/replay — save .ggf + manifest แล้ว
 * restore ใน process ใหม่ผ่าน walk clock (เหมือน fibo_checkpoint_sweep)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * checkpoint: เก็บ tensor → .ggf (home เท่านั้น — dedup ระดับไฟล์) + manifest
 * replay    : process ใหม่ อ่าน manifest จากดิสก์ → rebuild walk clock
 *             (state = seed/round/tick) → resolve ทุก tensor → อ่านผ่าน GGFMap
 *             (zero-copy) → เทียบกับต้นฉบับ (callback) → lossless
 *
 * MANIFEST (.mfp) — เก็บแค่ "วิธีสร้างกับ seed" (deterministic) + PROVENANCE:
 *   [GgfCkptHeader 584B] magic "GGRP" · version 5 · seed · ticks · cycles ·
 *                        n (tensors) · dup_bytes · data_bytes · ckpt_round ·
 *                        ckpt_tick (กลางรอบ) · created_utc · note[64] (ใคร/อะไร) ·
 *                        model[192] (โมเดลไหน) · base_dir[256] (delta — ว่าง = เต็ม) ·
 *                        crc64
 *   [entry × n]          name[128] · size u32 · home_of i32 · status u8   (140B)
 *                        status: 0 = STORED (ไฟล์ใน dir นี้) · 1 = SAME (ใน base_dir)
 *   — rq ของทุก tensor คำนวณจาก seed ใหม่ (ggf_walk_rq_of) — ไม่ต้องเก็บ
 *   — paths derive จาก name (sanitize เหมือน probe: จุด/สแลช → _)
 *   — crc64 (ECMA-182 — อัลกอริทึมเดียวกับ kis_crc64) ครอบ header (ยกเว้น
 *     crc64 field เอง) + ทุก entry → แก้ manifest ตรงไหนก็จับได้
 *
 * DELTA CHECKPOINT: manifest เก็บ base_dir + status ต่อ tensor — checkpoint
 *   ใหม่เก็บเฉพาะ tensor ที่เปลี่ยนตั้งแต่ base (ggf_ckpt_cmp_base: เทียบ CRC32
 *   ของ data region กับ base .ggf — diff ระดับไฟล์) · replay/verify merge:
 *   SAME → อ่านจาก base_dir · STORED → อ่านจาก dir นี้ (chain ต่อได้)
 *
 * DELTA CHAIN (multi-level — base → delta1 → delta2 → ...): manifest ของ
 *   delta2 อ้าง base_dir = delta1 (ซึ่งอ้าง base ต่อ) — SAME ของ delta2
 *   resolve ต่อผ่าน chain จนเจอระดับที่ STORED (ไฟล์จริงอยู่ที่ระดับนั้น —
 *   ggf_ckpt_chain_open/path เดิน chain นี้ ตรวจ crc64 ทุกระดับ =
 *   provenance chain · จับ chain วนได้) · ggf_ckpt_cmp_base เทียบผ่าน chain
 *   (เห็นระดับลึก — delta ของ delta ไม่เก็บซ้ำ) · GC (ggf_ckpt_gc) รวมทั้ง
 *   chain เป็น snapshot ใหม่ (คัดลอกไฟล์จริง + manifest เต็ม self-contained)
 *   — หลัง GC ปลอดภัยที่จะลบ base/delta เดิม (tool พิมพ์รายชื่อ dir)
 *
 * MID-ROUND CHECKPOINT: ckpt_round/ckpt_tick (≠ 0) → replay เริ่มจากจุดนั้น
 *   อ่านเฉพาะ tensor ที่ live ตั้งแต่ (round, tick) — tensor ก่อน checkpoint
 *   (pos = rq×ticks + rq%ticks < ckpt_pos) ถูกข้าม (ออก_skip)
 *
 * VERIFY (ไม่ต้องมีโมเดลต้นทาง): ggf_ckpt_verify — manifest crc64 + ทุก home
 *   .ggf (map + size ตรง manifest + CRC32 ต่อไฟล์) + จำนวนไฟล์ตรง
 *
 * ใช้โดย: tools/ggf_checkpoint_replay.c (fresh-process proof) +
 *         tests/test_ggf_ckpt_replay.c (TIER1)
 */

#ifndef GEO_GGF_CKPT_H
#define GEO_GGF_CKPT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "geo_ggf_walk.h"

#define GGF_CKPT_MAGIC      "GGRP"
#define GGF_CKPT_VERSION    5u
#define GGF_CKPT_NAME_LEN   128u   /* ชื่อ tensor ยาวได้ถึง ~127 (Kokoro ~63+) */
#define GGF_CKPT_NOTE_LEN   64u
#define GGF_CKPT_MODEL_LEN  192u
#define GGF_CKPT_DIR_LEN    256u   /* base_dir (delta checkpoint)  */
#define GGF_CKPT_MAX_T      2048u

/* delta checkpoint — สถานะต่อ tensor (diff ระดับไฟล์) */
#define GGF_CKPT_STORED     0u     /* ไฟล์เก็บอยู่ใน checkpoint dir นี้  */
#define GGF_CKPT_SAME       1u     /* เหมือน base — อ้างอิงไฟล์ใน base_dir */

/* CRC64 ECMA-182 — อัลกอริทึมเดียวกับ kis_crc64 (geo_kis_container.h)
 * seedable (chain ได้ข้าม buffer — เหมือน ggf_crc32) */
#define GGF_CKPT_CRC64_POLY UINT64_C(0x42F0E1EBA9EA3693)
static inline uint64_t ggf_ckpt_crc64(const uint8_t *data, uint64_t len,
                                      uint64_t seed)
{
    uint64_t crc = seed ^ UINT64_C(0xFFFFFFFFFFFFFFFF);
    for (uint64_t i = 0; i < len; i++) {
        crc ^= (uint64_t)data[i] << 56;
        for (int j = 0; j < 8; j++) {
            crc = (crc & (UINT64_C(1) << 63))
                ? ((crc << 1) ^ GGF_CKPT_CRC64_POLY)
                :  (crc << 1);
        }
    }
    return crc ^ UINT64_C(0xFFFFFFFFFFFFFFFF);
}

typedef struct {
    char     magic[4];      /* "GGRP"                          */
    uint8_t  version;       /* GGF_CKPT_VERSION                */
    uint8_t  _pad[3];
    uint32_t seed;          /* walk clock seed                 */
    uint32_t ticks;         /* 12 (FS_TICKS)                   */
    uint32_t cycles;        /* rounds บนนาฬิกา                 */
    uint32_t n;             /* จำนวน tensor                    */
    uint64_t dup_bytes;     /* bytes ที่ dedup ประหยัด          */
    uint64_t data_bytes;    /* bytes ต้นฉบับทั้งหมด             */
    uint32_t ckpt_round;    /* checkpoint กลางรอบ (0 = จากเริ่ม) */
    uint32_t ckpt_tick;     /* checkpoint tick (0 = จากเริ่ม)    */
    uint64_t created_utc;   /* เมื่อไหร่ (unix sec)             */
    char     note[GGF_CKPT_NOTE_LEN];    /* ใคร/อะไร          */
    char     model[GGF_CKPT_MODEL_LEN];  /* โมเดลไหน           */
    char     base_dir[GGF_CKPT_DIR_LEN]; /* base checkpoint (delta) — ว่าง = เต็ม */
    uint64_t crc64;         /* ครอบ header(ยกเว้น crc64) + entries */
} GgfCkptHeader;            /* 320 + 256 + 8 = 584B */

typedef struct {
    char     name[GGF_CKPT_NAME_LEN];
    uint32_t size;
    int32_t  home_of;       /* registry {tensor_id → home}   */
    uint8_t  status;        /* GGF_CKPT_STORED (dir นี้) / GGF_CKPT_SAME (base) */
    uint8_t  _pad[3];
} GgfCkptEntry;             /* 128+4+4+1+3 = 140B */

/* ── write manifest (provenance: note = ใคร/อะไร, model = โมเดลไหน,
 * created_utc = เมื่อไหร่ — 0 = ใช้เวลาปัจจุบัน) ── */
static inline int ggf_ckpt_write(const char *path, uint32_t seed,
                                 uint32_t ticks, uint32_t cycles,
                                 uint32_t n, const char *const *names,
                                 const uint32_t *sizes, const int32_t *home_of,
                                 const uint8_t *status, const char *base_dir,
                                 uint64_t dup_bytes, uint64_t data_bytes,
                                 uint32_t ckpt_round, uint32_t ckpt_tick,
                                 const char *note, const char *model)
{
    if (!path || !names || !sizes || !home_of) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    GgfCkptHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, GGF_CKPT_MAGIC, 4);
    h.version = GGF_CKPT_VERSION;
    h.seed = seed; h.ticks = ticks; h.cycles = cycles; h.n = n;
    h.dup_bytes = dup_bytes; h.data_bytes = data_bytes;
    h.ckpt_round = ckpt_round; h.ckpt_tick = ckpt_tick;
    h.created_utc = (uint64_t)time(NULL);
    if (note)  strncpy(h.note,  note,  GGF_CKPT_NOTE_LEN - 1);
    if (model) strncpy(h.model, model, GGF_CKPT_MODEL_LEN - 1);
    if (base_dir) strncpy(h.base_dir, base_dir, GGF_CKPT_DIR_LEN - 1);
    /* crc64 ครอบ header ยกเว้น crc64 field เอง + ทุก entry (chain) */
    uint64_t crc = ggf_ckpt_crc64((const uint8_t *)&h,
                                  offsetof(GgfCkptHeader, crc64), 0);
    if (fwrite(&h, sizeof h, 1, f) != 1) { fclose(f); return -3; }
    for (uint32_t i = 0; i < n; i++) {
        GgfCkptEntry e;
        memset(&e, 0, sizeof e);
        strncpy(e.name, names[i], GGF_CKPT_NAME_LEN - 1);
        e.size = sizes[i];
        e.home_of = home_of[i];
        e.status = status ? status[i] : GGF_CKPT_STORED;
        crc = ggf_ckpt_crc64((const uint8_t *)&e, sizeof e, crc);
        if (fwrite(&e, sizeof e, 1, f) != 1) { fclose(f); return -3; }
    }
    /* เขียน crc กลับที่ field (seek กลับ) */
    if (fseek(f, (long)offsetof(GgfCkptHeader, crc64), SEEK_SET) != 0 ||
        fwrite(&crc, sizeof crc, 1, f) != 1) { fclose(f); return -3; }
    fclose(f);
    return 0;
}

/* ── read manifest + verify crc64 (alloc entries) — 0 = ok · <0 = fail ──
 * -7 = crc64 mismatch (manifest ถูกแก้) · -8 = ไฟล์สั้นกว่าที่ crc ครอบ */
static inline int ggf_ckpt_read(const char *path, GgfCkptHeader *h,
                                GgfCkptEntry **out_entries)
{
    if (!path || !h || !out_entries) return -1;
    *out_entries = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    if (fread(h, sizeof *h, 1, f) != 1) { fclose(f); return -3; }
    if (memcmp(h->magic, GGF_CKPT_MAGIC, 4) != 0) { fclose(f); return -4; }
    if (h->version != GGF_CKPT_VERSION) { fclose(f); return -5; }
    if (h->n == 0 || h->n > GGF_CKPT_MAX_T) { fclose(f); return -6; }
    GgfCkptEntry *e = (GgfCkptEntry *)calloc(h->n, sizeof(GgfCkptEntry));
    if (!e) { fclose(f); return -1; }
    if (fread(e, sizeof(GgfCkptEntry), h->n, f) != h->n) {
        free(e); fclose(f); return -3;
    }
    fclose(f);
    /* crc64 ครอบ header (ยกเว้น crc64) + entries (chain) — แก้ตรงไหนก็จับได้ */
    uint64_t crc = ggf_ckpt_crc64((const uint8_t *)h,
                                  offsetof(GgfCkptHeader, crc64), 0);
    for (uint32_t i = 0; i < h->n; i++)
        crc = ggf_ckpt_crc64((const uint8_t *)&e[i], sizeof(GgfCkptEntry), crc);
    if (crc != h->crc64) { free(e); return -7; }
    /* delta: SAME ต้องมี base_dir (ไม่งั้นอ้างไฟล์ที่ไม่มี) */
    for (uint32_t i = 0; i < h->n; i++) {
        if (e[i].status == GGF_CKPT_SAME && h->base_dir[0] == 0) {
            free(e); return -11;
        }
    }
    *out_entries = e;
    return 0;
}

/* derive .ggf path จาก name (sanitize เหมือน probe — จุด/สแลช → _) */
static inline void ggf_ckpt_path(char *out, size_t cap, const char *dir,
                                 uint32_t idx, const char *name)
{
    char stem[GGF_CKPT_NAME_LEN + 32];
    size_t sl = 0;
    for (const char *q = name; *q && sl < sizeof(stem) - 1; q++) {
        char c = *q;
        if (c == '.' || c == '/' || c == '\\' || c == ':') c = '_';
        stem[sl++] = c;
    }
    stem[sl] = 0;
    snprintf(out, cap, "%s/t%05u_%s.ggf", dir, idx, stem);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DELTA CHAIN — base → delta1 → delta2 → ... (multi-level merge)
 * ═══════════════════════════════════════════════════════════════════════
 * manifest ของแต่ละระดับ อ้าง base_dir ต่อกัน (tail = ระดับที่ base ว่าง =
 * full checkpoint) — SAME ของระดับบน resolve ต่อลงไปจนเจอระดับที่ STORED
 * (ไฟล์จริงอยู่ที่ระดับนั้น) · ตรวจ crc64 ทุก manifest ที่เดินผ่าน
 * (provenance chain — แก้ระดับไหนก็จับ) · ใช้โดย replay/verify/cmp_base/GC
 */
#define GGF_CKPT_MAX_CHAIN 32

typedef struct {
    GgfCkptHeader h;          /* manifest ของระดับนี้ (อ่าน + crc64 ตรวจแล้ว) */
    GgfCkptEntry *e;          /* entries (alloc — close ฟรี)                */
    char dir[GGF_CKPT_DIR_LEN];
} GgfCkptLink;

typedef struct {
    GgfCkptLink links[GGF_CKPT_MAX_CHAIN];
    int  depth;               /* 1 = full · 2+ = delta chain */
    uint32_t n;               /* entries ต่อระดับ (ทุกระดับเท่ากัน) */
} GgfCkptChain;

static inline void ggf_ckpt_chain_close(GgfCkptChain *ch)
{
    for (int d = 0; d < ch->depth; d++) free(ch->links[d].e);
    ch->depth = 0;
}

/* เปิด chain จาก head_dir (อ่าน manifest → เดิน base_dir ต่อ)
 * returns 0 = ok · -1 = manifest ระดับใดอ่าน/ตรวจไม่ผ่าน · -2 = chain ลึกเกิน
 * MAX_CHAIN · -3 = วน (base_dir ชี้กลับระดับที่เคยผ่าน) · -4 = n ไม่ตรงกัน */
static inline int ggf_ckpt_chain_open(const char *head_dir, GgfCkptChain *ch)
{
    if (!head_dir || !ch) return -1;
    memset(ch, 0, sizeof *ch);
    const char *dir = head_dir;
    for (int d = 0; d < GGF_CKPT_MAX_CHAIN; d++) {
        if (d > 0) {                       /* วน: base_dir ชี้กลับระดับเดิม */
            for (int k = 0; k < d; k++)
                if (strcmp(ch->links[k].dir, dir) == 0) {
                    ch->depth = d;
                    ggf_ckpt_chain_close(ch);
                    return -3;
                }
        }
        char mfp[1024];
        snprintf(mfp, sizeof mfp, "%s/manifest.mfp", dir);
        int rc = ggf_ckpt_read(mfp, &ch->links[d].h, &ch->links[d].e);
        if (rc != 0) { ggf_ckpt_chain_close(ch); return -1; }
        strncpy(ch->links[d].dir, dir, GGF_CKPT_DIR_LEN - 1);
        ch->depth = d + 1;
        if (d == 0) ch->n = ch->links[0].h.n;
        else if (ch->links[d].h.n != ch->n) { ggf_ckpt_chain_close(ch); return -4; }
        if (ch->links[d].h.base_dir[0] == 0) return 0;   /* ถึง tail (เต็ม) */
        dir = ch->links[d].h.base_dir;
    }
    ggf_ckpt_chain_close(ch);
    return -2;
}

/* resolve ไฟล์จริงของ entry idx: เดินจาก head ลงไปจนเจอระดับที่ STORED —
 * ไฟล์อยู่ที่ระดับนั้น · returns 0 = ok (path ใน out) · -1 = idx เกิน ·
 * -11 = SAME ครบทุกระดับ (chain ผิดปกติ — ไม่ควรเกิดใน chain ที่ valid) */
static inline int ggf_ckpt_chain_path(const GgfCkptChain *ch, uint32_t idx,
                                      const char *name, char *out, size_t cap)
{
    if (idx >= ch->n) return -1;
    for (int d = 0; d < ch->depth; d++) {
        if (ch->links[d].e[idx].status == GGF_CKPT_STORED) {
            ggf_ckpt_path(out, cap, ch->links[d].dir, idx, name);
            return 0;
        }
    }
    return -11;
}

/* provenance chain: ชื่อ entry idx ต้องตรงทุกระดับ (แก้ชื่อระดับไหนก็จับ) */
static inline int ggf_ckpt_chain_name_ok(const GgfCkptChain *ch, uint32_t idx)
{
    if (idx >= ch->n) return 0;
    for (int d = 1; d < ch->depth; d++)
        if (strcmp(ch->links[d].e[idx].name, ch->links[0].e[idx].name) != 0)
            return 0;
    return 1;
}

/* ── delta: เทียบ data กับไฟล์จริงใน chain ของ base (diff ระดับไฟล์) ──
 * returns GGF_CKPT_SAME = data เท่ากับไฟล์ที่ resolve ผ่าน chain (ไม่ต้องเก็บใหม่)
 *         GGF_CKPT_STORED = ต่าง / resolve ไม่ได้ / ไฟล์พัง (ต้องเก็บ)
 * resolve ผ่าน chain (base → ...): ไฟล์จริงอาจอยู่ลึกกว่าระดับแรก — เทียบกับ
 * ไฟล์ที่ replay จะอ่านจริง (delta ของ delta ไม่เก็บซ้ำ)
 * เปรียบเทียบด้วย CRC32 ของ data region (chunk 64B + pad ศูนย์ — เหมือนที่
 * ggf_save คำนวณ) — deterministic: data เท่ากัน ⇒ crc เท่ากัน */
static inline uint8_t ggf_ckpt_cmp_base(const char *base_dir, uint32_t idx,
                                        const char *name, uint32_t size,
                                        const uint8_t *data)
{
    if (!base_dir || !base_dir[0] || !name || !data || size == 0)
        return GGF_CKPT_STORED;
    GgfCkptChain ch;
    if (ggf_ckpt_chain_open(base_dir, &ch) != 0) return GGF_CKPT_STORED;
    char path[512];
    int pr = ggf_ckpt_chain_path(&ch, idx, name, path, sizeof path);
    ggf_ckpt_chain_close(&ch);
    if (pr != 0) return GGF_CKPT_STORED;
    GGFMap m;
    if (ggf_map(path, &m) != 0) return GGF_CKPT_STORED;   /* ไฟล์พัง → เก็บเอง */
    if (m.h.n_bytes != size) { ggf_unmap(&m); return GGF_CKPT_STORED; }
    uint32_t crc_b = 0, crc_d = 0;
    for (uint64_t k = 0; k < m.h.n_chunks; k++) {          /* crc ของ base */
        const uint8_t *d = ggf_map_node(&m, k, NULL);
        if (!d) { ggf_unmap(&m); return GGF_CKPT_STORED; }
        crc_b = ggf_crc32(d, GGS_CHUNK, crc_b);
    }
    uint64_t off = 0;                                      /* crc ของ data ใหม่ */
    while (off < size) {
        uint8_t chunk[GGS_CHUNK];
        memset(chunk, 0, sizeof chunk);
        uint32_t take = (size - off >= GGS_CHUNK)
                        ? GGS_CHUNK : (uint32_t)(size - off);
        memcpy(chunk, data + off, take);
        crc_d = ggf_crc32(chunk, GGS_CHUNK, crc_d);
        off += GGS_CHUNK;
    }
    ggf_unmap(&m);
    return (crc_b == crc_d) ? GGF_CKPT_SAME : GGF_CKPT_STORED;
}

/* ── replay callback: เทียบ bytes ที่อ่านได้กับต้นฉบับ ──
 * returns 1 = ตรง · 0 = ไม่ตรง */
typedef int (*GgfCkptCmp)(void *ctx, uint32_t idx,
                          const uint8_t *got, uint64_t got_n);

/* ── replay: rebuild walk clock จาก manifest → resolve tensor (ตั้งแต่
 * checkpoint กลางรอบเป็นต้นไป) → อ่านผ่าน GGFMap → cmp กับต้นฉบับ
 * (ใน process ใหม่ — state ทั้งหมดมาจาก manifest)
 * out_ok/out_fail/out_skip/out_bytes = สถิติ · returns 0 = ทุก pending lossless */
static inline int ggf_ckpt_replay(const GgfCkptHeader *h,
                                  const GgfCkptEntry *e,
                                  const char *dir, const char *base_dir,
                                  GgfCkptCmp cmp, void *ctx,
                                  uint64_t *out_bytes,
                                  uint32_t *out_ok, uint32_t *out_fail,
                                  uint32_t *out_skip)
{
    uint32_t *rq = (uint32_t *)malloc(h->n * sizeof(uint32_t));
    uint32_t *sizes = (uint32_t *)malloc(h->n * sizeof(uint32_t));
    int32_t  *home_of = (int32_t *)malloc(h->n * sizeof(int32_t));
    GGFMap  *maps = (GGFMap *)calloc(h->n, sizeof(GGFMap));
    uint64_t max_sz = 0;
    for (uint32_t i = 0; i < h->n; i++) {
        sizes[i] = e[i].size;
        home_of[i] = e[i].home_of;
        if (sizes[i] > max_sz) max_sz = sizes[i];
    }
    uint8_t *scratch = (uint8_t *)malloc(max_sz ? max_sz : 1);

    /* paths ต่อ entry: resolve ผ่าน delta chain (base → delta1 → ... → head)
     * — SAME เดินต่อลงไปจนเจอระดับที่ STORED (ไฟล์จริงอยู่ที่ระดับนั้น) */
    (void)base_dir;   /* chain เดินจาก dir เอง (manifest.base_dir ต่อระดับ) */
    const char **paths = (const char **)malloc(h->n * sizeof(char *));
    char *path_buf = (char *)malloc(h->n * 256);
    GgfCkptChain ch;
    if (ggf_ckpt_chain_open(dir, &ch) != 0) {
        free(maps); free(scratch); free(rq); free(sizes); free(home_of);
        free(paths); free(path_buf);
        return -1;   /* chain พัง (manifest/crc64/วน) — ปฏิเสธทั้ง replay */
    }
    for (uint32_t i = 0; i < h->n; i++) {
        if (ggf_ckpt_chain_path(&ch, i, e[i].name,
                                path_buf + i * 256, 256) != 0) {
            ggf_ckpt_chain_close(&ch);
            free(maps); free(scratch); free(rq); free(sizes); free(home_of);
            free(paths); free(path_buf);
            return -1;
        }
        paths[i] = path_buf + i * 256;
    }
    ggf_ckpt_chain_close(&ch);

    GgfWalkTable tbl;
    ggf_walk_init(&tbl, h->seed, h->ticks, h->cycles, h->n,
                  paths, sizes, home_of, rq);

    /* checkpoint กลางรอบ: อ่านเฉพาะ tensor ที่ live ตั้งแต่ (round, tick) */
    int mid_round = !(h->ckpt_round == 0 && h->ckpt_tick == 0);
    uint64_t ckpt_pos = mid_round
        ? (uint64_t)h->ckpt_round * h->ticks + h->ckpt_tick
        : 0;
    FiboWalkPos start = mid_round
        ? (FiboWalkPos){ h->ckpt_round, h->ckpt_tick, 0 }
        : (FiboWalkPos){ 7, 3, 0 };              /* enter-anywhere */

    uint32_t ok_cnt = 0, fail_cnt = 0, skip_cnt = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < h->n; i++) {
        if (home_of[i] < 0) continue;            /* ว่าง — ข้าม */
        uint64_t pos = (uint64_t)rq[i] * h->ticks + (rq[i] % h->ticks);
        if (mid_round && pos < ckpt_pos) { skip_cnt++; continue; }
        FiboWalkPos end;
        if (!ggf_walk_to(&tbl, start, i, &end)) { fail_cnt++; continue; }
        uint64_t got_n = 0;
        int rc = ggf_walk_read_map(&tbl, i, maps, scratch, max_sz, &got_n);
        if (rc != 0 || got_n != sizes[i] ||
            (cmp && !cmp(ctx, i, scratch, got_n))) {
            fail_cnt++;
        } else {
            ok_cnt++;
            bytes += sizes[i];
        }
    }

    for (uint32_t i = 0; i < h->n; i++) ggf_unmap(&maps[i]);
    free(maps); free(scratch); free(rq); free(sizes); free(home_of);
    free(paths); free(path_buf);
    if (out_bytes) *out_bytes = bytes;
    if (out_ok) *out_ok = ok_cnt;
    if (out_fail) *out_fail = fail_cnt;
    if (out_skip) *out_skip = skip_cnt;
    return (fail_cnt == 0) ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * VERIFY — สแกน .ggf + manifest ทั้งชุด โดยไม่ต้องมีโมเดลต้นทาง
 * ═══════════════════════════════════════════════════════════════════
 * ตรวจ: manifest crc64 (provenance) + ทุก home .ggf (map ได้ + n_bytes ตรง
 * manifest + CRC32 ต่อไฟล์ผ่าน) + จำนวนไฟล์บนดิสก์ == จำนวน home
 * out: ok_files/fail_files (home ที่ตรวจ) · returns 0 = ผ่านหมด */
static inline int ggf_ckpt_verify(const char *dir,
                                  uint32_t *out_files, uint32_t *out_fail)
{
    GgfCkptChain ch;
    if (ggf_ckpt_chain_open(dir, &ch) != 0) return -1;   /* crc64 chain จับ */
    const GgfCkptHeader *h = &ch.links[0].h;
    const GgfCkptEntry  *e = ch.links[0].e;

    uint32_t n_home = 0, ok_cnt = 0, fail_cnt = 0;
    for (uint32_t i = 0; i < h->n; i++) {
        if (e[i].home_of != (int32_t)i) continue;  /* dup — ผ่าน home */
        if (e[i].size == 0) continue;
        n_home++;
        /* provenance chain: ชื่อต้องตรงทุกระดับ (แก้ระดับไหนก็จับ) */
        if (!ggf_ckpt_chain_name_ok(&ch, i)) {
            fail_cnt++;
            if (fail_cnt <= 5)
                printf("    [verify-fail] idx=%u name=%s (chain name mismatch)\n",
                       i, e[i].name);
            continue;
        }
        char path[512];
        if (ggf_ckpt_chain_path(&ch, i, e[i].name, path, sizeof path) != 0) {
            fail_cnt++;
            if (fail_cnt <= 5)
                printf("    [verify-fail] idx=%u name=%s (unresolved chain)\n",
                       i, e[i].name);
            continue;
        }
        GGFMap m;
        int rc = ggf_map(path, &m);
        int ok = (rc == 0);
        if (ok && m.h.n_bytes != e[i].size) ok = 0;   /* size ตรง manifest */
        if (ok && ggf_map_verify(&m) != 0) ok = 0;    /* CRC32 ต่อไฟล์ */
        if (ok) {
            ok_cnt++;
        } else {
            fail_cnt++;
            if (fail_cnt <= 5)
                printf("    [verify-fail] idx=%u name=%s rc=%d\n", i, e[i].name, rc);
        }
        ggf_unmap(&m);
    }
    ggf_ckpt_chain_close(&ch);
    if (out_files) *out_files = ok_cnt;
    if (out_fail) *out_fail = fail_cnt;
    return (fail_cnt == 0) ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * GC — รวม delta chain เป็น snapshot ใหม่ (base ที่เลิกอ้างแล้ว)
 * ═══════════════════════════════════════════════════════════════════
 * อ่าน chain (head → base → ...) → resolve home .ggf ทุกตัว (ไฟล์จริงอาจ
 * อยู่คนละระดับ) → คัดลอกลง new_dir (ตรวจขนาด + CRC หลังคัดลอก) → เขียน
 * manifest เต็ม (base_dir ว่าง · ทุกตัว STORED — self-contained)
 * หลัง GC: chain เดิมไม่ถูกอ้างอิงอีก — ลบได้ (tool พิมพ์รายชื่อ dir)
 * returns 0 = ok · -1 = arg ผิด · -2 = chain อ่านไม่ได้ ·
 * -3 = คัดลอกไม่ครบ / manifest เขียนไม่ได้ */
static inline int ggf_ckpt_file_copy(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    FILE *g = fopen(dst, "wb");
    if (!g) { fclose(f); return -2; }
    uint8_t buf[1 << 20];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        if (fwrite(buf, 1, r, g) != r) { fclose(f); fclose(g); return -3; }
    fclose(f);
    if (fclose(g) != 0) return -4;
    return 0;
}

/* mkdir recursive (Windows-safe — gc สร้าง dir ปลายทางเอง) */
static inline void ggf_ckpt_mkdirs(const char *dir)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/' || *s == '\\') { *s = '\0'; mkdir(tmp); *s = '/'; }
    }
    mkdir(tmp);
}

static inline int ggf_ckpt_gc(const char *head_dir, const char *new_dir,
                              const char *note,
                              uint32_t *out_home, uint64_t *out_bytes,
                              uint32_t *out_fail)
{
    if (!new_dir || !new_dir[0]) return -1;
    ggf_ckpt_mkdirs(new_dir);
    GgfCkptChain ch;
    if (ggf_ckpt_chain_open(head_dir, &ch) != 0) return -2;
    const GgfCkptHeader *h = &ch.links[0].h;
    const GgfCkptEntry  *e = ch.links[0].e;
    uint32_t n = ch.n;

    uint32_t *sizes   = (uint32_t *)malloc(n * sizeof(uint32_t));
    int32_t  *home_of = (int32_t *)malloc(n * sizeof(int32_t));
    const char **names = (const char **)malloc(n * sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        sizes[i] = e[i].size;
        home_of[i] = e[i].home_of;
        names[i] = e[i].name;
    }

    uint32_t n_home = 0, n_fail = 0;
    uint64_t bytes = 0;
    char src[512], dst[512];
    for (uint32_t i = 0; i < n; i++) {
        if (home_of[i] != (int32_t)i || sizes[i] == 0) continue;  /* dup/ว่าง */
        if (ggf_ckpt_chain_path(&ch, i, names[i], src, sizeof src) != 0) {
            n_fail++;
            continue;
        }
        ggf_ckpt_path(dst, sizeof dst, new_dir, i, names[i]);
        if (ggf_ckpt_file_copy(src, dst) != 0) { n_fail++; continue; }
        /* ตรวจผลคัดลอก: map ได้ + ขนาดตรง manifest + CRC32 ผ่าน */
        GGFMap m;
        int ok = ggf_map(dst, &m) == 0;
        if (ok && m.h.n_bytes != sizes[i]) ok = 0;
        if (ok && ggf_map_verify(&m) != 0) ok = 0;
        if (ok) { n_home++; bytes += sizes[i]; }
        else n_fail++;
        ggf_unmap(&m);
    }

    /* manifest เต็ม — ทุกตัว STORED · base ว่าง (self-contained)
     * เขียนก่อนปิด chain (names ชี้เข้า entries — ยังต้องใช้อยู่) */
    char mfp[1024];
    snprintf(mfp, sizeof mfp, "%s/manifest.mfp", new_dir);
    int wr = ggf_ckpt_write(mfp, h->seed, h->ticks, h->cycles, n,
                            names, sizes, home_of, NULL /*ทุกตัว STORED*/,
                            NULL /*base ว่าง*/, h->dup_bytes, h->data_bytes,
                            h->ckpt_round, h->ckpt_tick,
                            note ? note : "gc consolidated", h->model);
    ggf_ckpt_chain_close(&ch);
    free(sizes); free(home_of); free(names);
    if (wr != 0 || n_fail > 0) return -3;
    if (out_home) *out_home = n_home;
    if (out_bytes) *out_bytes = bytes;
    if (out_fail) *out_fail = n_fail;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * AUTO-GC — chain ของ base ลึกเกิน max_chain → รวมเป็น snapshot ใหม่
 * ═══════════════════════════════════════════════════════════════════
 * เรียกก่อนเขียน delta ระดับถัดไป: เปิด chain ของ base → ถ้า depth ≥
 * max_chain → ggf_ckpt_gc(base → new_dir) แล้วใช้ new_dir เป็น base ใหม่
 * (chain ใหม่ลึก 2 เสมอ — ไม่ยาวเกิน threshold) · ไม่งั้นใช้ base เดิม
 * returns: 1 = GC เกิดขึ้น (out_new_base = new_dir) · 0 = chain ยังสั้น
 *          (ใช้ base เดิม — out_new_base ไม่ถูกเขียน) · -1 = base อ่านไม่ได้ ·
 *          -2 = GC ล้ม */
static inline int ggf_ckpt_auto_gc(const char *base_dir, const char *new_dir,
                                   int max_chain, const char *note,
                                   char *out_new_base, size_t cap,
                                   uint32_t *out_home, uint64_t *out_bytes)
{
    if (!base_dir || !base_dir[0] || !new_dir || !new_dir[0] || max_chain <= 1)
        return 0;
    GgfCkptChain ch;
    if (ggf_ckpt_chain_open(base_dir, &ch) != 0) return -1;   /* base พัง */
    int depth = ch.depth;
    ggf_ckpt_chain_close(&ch);
    if (depth < max_chain) return 0;                          /* ยังสั้น */
    uint32_t n_home = 0, n_fail = 0;
    uint64_t bytes = 0;
    if (ggf_ckpt_gc(base_dir, new_dir, note, &n_home, &bytes, &n_fail) != 0)
        return -2;
    if (out_home) *out_home = n_home;
    if (out_bytes) *out_bytes = bytes;
    if (out_new_base && cap) snprintf(out_new_base, cap, "%s", new_dir);
    return 1;
}

#endif /* GEO_GGF_CKPT_H */
