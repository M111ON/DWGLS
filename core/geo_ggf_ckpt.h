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
 *   [GgfCkptHeader 312B] magic "GGRP" · version 3 · seed · ticks · cycles ·
 *                        n (tensors) · dup_bytes · data_bytes · created_utc ·
 *                        note[64] (ใคร/อะไร) · model[192] (โมเดลไหน) · crc64
 *   [entry × n]          name[128] · size u32 · home_of i32   (136B each)
 *   — rq ของทุก tensor คำนวณจาก seed ใหม่ (ggf_walk_rq_of) — ไม่ต้องเก็บ
 *   — paths derive จาก name (sanitize เหมือน probe: จุด/สแลช → _)
 *   — crc64 (ECMA-182 — อัลกอริทึมเดียวกับ kis_crc64) ครอบ header (ยกเว้น
 *     crc64 field เอง) + ทุก entry → แก้ manifest ตรงไหนก็จับได้
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
#include "geo_ggf_walk.h"

#define GGF_CKPT_MAGIC      "GGRP"
#define GGF_CKPT_VERSION    3u
#define GGF_CKPT_NAME_LEN   128u   /* ชื่อ tensor ยาวได้ถึง ~127 (Kokoro ~63+) */
#define GGF_CKPT_NOTE_LEN   64u
#define GGF_CKPT_MODEL_LEN  192u
#define GGF_CKPT_MAX_T      2048u

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
    uint64_t created_utc;   /* เมื่อไหร่ (unix sec)             */
    char     note[GGF_CKPT_NOTE_LEN];    /* ใคร/อะไร          */
    char     model[GGF_CKPT_MODEL_LEN];  /* โมเดลไหน           */
    uint64_t crc64;         /* ครอบ header(ยกเว้น crc64) + entries */
} GgfCkptHeader;            /* 4+1+3+16+16+8+64+192+8 = 312B */

typedef struct {
    char     name[GGF_CKPT_NAME_LEN];
    uint32_t size;
    int32_t  home_of;       /* registry {tensor_id → home}   */
} GgfCkptEntry;             /* 128+4+4 = 136B */

/* ── write manifest (provenance: note = ใคร/อะไร, model = โมเดลไหน,
 * created_utc = เมื่อไหร่ — 0 = ใช้เวลาปัจจุบัน) ── */
static inline int ggf_ckpt_write(const char *path, uint32_t seed,
                                 uint32_t ticks, uint32_t cycles,
                                 uint32_t n, const char *const *names,
                                 const uint32_t *sizes, const int32_t *home_of,
                                 uint64_t dup_bytes, uint64_t data_bytes,
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
    h.created_utc = (uint64_t)time(NULL);
    if (note)  strncpy(h.note,  note,  GGF_CKPT_NOTE_LEN - 1);
    if (model) strncpy(h.model, model, GGF_CKPT_MODEL_LEN - 1);
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

/* ── replay callback: เทียบ bytes ที่อ่านได้กับต้นฉบับ ──
 * returns 1 = ตรง · 0 = ไม่ตรง */
typedef int (*GgfCkptCmp)(void *ctx, uint32_t idx,
                          const uint8_t *got, uint64_t got_n);

/* ── replay: rebuild walk clock จาก manifest → resolve ทุก tensor → อ่านผ่าน
 * GGFMap → cmp กับต้นฉบับ (ใน process ใหม่ — state ทั้งหมดมาจาก manifest) ──
 * out_ok/out_fail/out_bytes = สถิติ · returns 0 = ทุก tensor lossless */
static inline int ggf_ckpt_replay(const GgfCkptHeader *h,
                                  const GgfCkptEntry *e,
                                  const char *const *paths,
                                  GgfCkptCmp cmp, void *ctx,
                                  uint64_t *out_bytes,
                                  uint32_t *out_ok, uint32_t *out_fail)
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

    GgfWalkTable tbl;
    ggf_walk_init(&tbl, h->seed, h->ticks, h->cycles, h->n,
                  paths, sizes, home_of, rq);

    uint32_t ok_cnt = 0, fail_cnt = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < h->n; i++) {
        if (home_of[i] < 0) continue;            /* ว่าง — ข้าม */
        FiboWalkPos start = { 7, 3, 0 };         /* enter-anywhere */
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
    if (out_bytes) *out_bytes = bytes;
    if (out_ok) *out_ok = ok_cnt;
    if (out_fail) *out_fail = fail_cnt;
    return (fail_cnt == 0) ? 0 : 1;
}

#endif /* GEO_GGF_CKPT_H */
