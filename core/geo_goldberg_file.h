/*
 * geo_goldberg_file.h — Goldberg sphere persistence (.ggf file)
 * ═══════════════════════════════════════════════════════════════
 *
 * T1.2i — serialize sphere ลงไฟล์จริง (ต่อจาก T1.2h — ggs_store ใน RAM)
 *
 * ggs_save: data → decagram-Goldberg spheres → verify ภายใน (memcmp) →
 *           เขียนลงไฟล์ .ggf (streaming — 1 sphere ใน RAM ตอนใดตอนหนึ่ง)
 * ggs_load: อ่าน .ggf → reconstruct กลับเป็น data ตามลำดับ chunk เดิม →
 *           CRC32 ยืนยันความถูกต้อง (corruption detect)
 * ggf_open/chunk/read/verify: LAZY read — seek ต่อ node โดยตรง ไม่โหลดทั้งไฟล์
 *           (build index ของ sphere offsets ตอน open เท่านั้น = O(n_spheres) reads)
 * ggf_map/unmap/map_node/map_chunk/map_read/map_verify: MMAP read —
 *           map ทั้งไฟล์เข้าหน้าเพจ → อ่านตรงจากเพจ (zero-copy pointer)
 *           — ไม่มี fseek/fread เลย (เร็วสุดสำหรับ random/sequential)
 * ggf_save_map: เขียน .ggf ผ่าน mmap VIEW (แทน fwrite) — ไฟล์ถูกสร้าง +
 *           map แล้วเขียนลง view ตรงๆ + verify จาก view เองก่อน flush —
 *           อ่านด้วย GGFMap ได้ทันที (ไม่ต้อง reopen/flush dance)
 *
 * FILE LAYOUT (.ggf):
 *   [GGFHeader 64B]  magic "GGF0" · version · level · n_spheres ·
 *                    n_chunks (total 64B nodes) · n_bytes (original) ·
 *                    crc32 (CRC32 ของ data ทั้งหมด, padded 64B/chunk)
 *   [sphere 0]       [count u32] แล้ว count × [tick u32][data 64B]
 *   [sphere 1..n]    ...
 *
 *   node tick = gp_addr_to_tick({tile_id, dim}) — self-describing
 *   (ถ้า tick พัง → record ไปผิด tile → reconstruct ผิด → CRC จับได้)
 *
 * Address mapping (decagram, เหมือน ggs_store):
 *   hex tile_id = 12 + (k mod hex_total) · dim = k / hex_total
 *   k = chunk index ใน sphere (0..count−1)
 *
 * Depends: geo_goldberg_store.h (GGS_CHUNK etc) + geo_goldberg_sphere.h + tring
 */

#ifndef GEO_GOLDBERG_FILE_H
#define GEO_GOLDBERG_FILE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "geo_goldberg_store.h"
#include "geo_goldberg_sphere.h"
#include "infra/tring.h"

#define GGF_MAGIC        "GGF0"
#define GGF_VERSION      1u
#define GGF_NOTE_LEN     28u

typedef struct {
    char     magic[4];      /* "GGF0"                          */
    uint8_t  version;       /* GGF_VERSION                     */
    uint8_t  level;         /* gp level 2..8 (1 = no hex tiles) */
    uint8_t  _pad[2];
    uint64_t n_chunks;      /* total 64B nodes                 */
    uint64_t n_bytes;       /* original data length (unpadded) */
    uint32_t n_spheres;     /* spheres in this file            */
    uint32_t crc32;         /* CRC32 over all padded chunk data */
    uint32_t _pad2;
    char     note[GGF_NOTE_LEN];  /* provenance label          */
} GGFHeader;                /* 4+1+1+2+8+8+4+4+4+28 = 64B */

/* ── CRC32 (zlib poly 0xEDB88320, seedable — stream ต่อเนื่องได้) ── */
static inline uint32_t ggf_crc32(const uint8_t *p, uint64_t n, uint32_t seed)
{
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (uint64_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

/*
 * ggs_save — เก็บ data ลงไฟล์ .ggf (streaming multi-sphere)
 *   ทุก sphere: write → verify (memcmp) → เขียน node ลงไฟล์ → destroy
 *   → ไม่มีทางเขียน data เสียลงไฟล์ (verify ก่อน persist)
 *   returns: 0 = saved lossless · <0 = fail
 *   หมายเหตุ: ต้องการ level ≥ 2 (level 1 ไม่มี hex tile — degenerate)
 */
static inline int ggs_save(const uint8_t *data, uint64_t n_bytes,
                           uint8_t level, const char *path)
{
    if (!path) return -1;
    if (!data && n_bytes > 0) return -1;
    if (level < 2) return -9;                    /* no hex tiles @ L1 */

    FILE *f = fopen(path, "wb");
    if (!f) return -2;

    /* ── header ── */
    GGFHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, GGF_MAGIC, 4);
    h.version    = GGF_VERSION;
    h.level      = level;
    h.n_bytes    = n_bytes;

    uint64_t n_chunks = (n_bytes + GGS_CHUNK - 1) / GGS_CHUNK;
    uint64_t per_sphere = (uint64_t)ggd_hex_total(level) * GP_MAX_DIM;
    h.n_spheres  = (uint32_t)((n_chunks + per_sphere - 1) / per_sphere);
    if (n_chunks == 0) h.n_spheres = 0;
    h.n_chunks   = n_chunks;

    if (fwrite(&h, sizeof h, 1, f) != 1) { fclose(f); return -3; }

    /* ── spheres ── */
    uint32_t crc = 0;
    for (uint32_t sp = 0; sp < h.n_spheres; sp++) {
        Tring tring;
        if (tring_init(&tring, (uint32_t)(ggd_face_count(level) << 8)) != 0) {
            fclose(f); return -4;
        }
        GpSphere sphere;
        gp_sphere_init(&sphere, &tring, level);

        uint64_t k0 = (uint64_t)sp * per_sphere;
        uint64_t k1 = k0 + per_sphere;
        if (k1 > n_chunks) k1 = n_chunks;
        uint32_t count = (uint32_t)(k1 - k0);

        /* write */
        for (uint64_t k = k0; k < k1; k++) {
            const uint8_t *src = data + k * GGS_CHUNK;
            uint8_t chunk[GGS_CHUNK];
            uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                         ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
            memset(chunk, 0, sizeof chunk);
            memcpy(chunk, src, n);
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(level, kk);
            uint8_t  dim  = ggs_dim(level, kk);
            if (gp_lens_write(&sphere, tile, dim, chunk) == UINT32_MAX) {
                tring_destroy(&tring); fclose(f); return -5;
            }
        }
        /* verify ภายใน (lossless ก่อน persist) */
        for (uint64_t k = k0; k < k1; k++) {
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(level, kk);
            uint8_t  dim  = ggs_dim(level, kk);
            const uint8_t *rd = gp_lens_read(&sphere, tile, dim);
            uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                         ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
            if (!rd || memcmp(rd, data + k * GGS_CHUNK, n) != 0) {
                tring_destroy(&tring); fclose(f); return -6;
            }
        }
        /* เขียน node ลงไฟล์: [count u32] + count × [tick u32][data 64B] */
        if (fwrite(&count, sizeof count, 1, f) != 1) {
            tring_destroy(&tring); fclose(f); return -3;
        }
        for (uint64_t k = k0; k < k1; k++) {
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(level, kk);
            uint8_t  dim  = ggs_dim(level, kk);
            const uint8_t *rd = gp_lens_read(&sphere, tile, dim);
            uint32_t tick = gp_addr_to_tick((GpAddr){ tile, dim });
            if (!rd || fwrite(&tick, sizeof tick, 1, f) != 1 ||
                fwrite(rd, GGS_CHUNK, 1, f) != 1) {
                tring_destroy(&tring); fclose(f); return -7;
            }
            crc = ggf_crc32(rd, GGS_CHUNK, crc);
        }
        tring_destroy(&tring);
    }

    /* เขียน crc กลับที่ header (seek กลับ) */
    {
        long pos = (long)offsetof(GGFHeader, crc32);
        if (fseek(f, pos, SEEK_SET) != 0 ||
            fwrite(&crc, sizeof crc, 1, f) != 1) {
            fclose(f); return -8;
        }
    }

    fclose(f);
    return 0;
}

/*
 * ggs_load — อ่าน .ggf → reconstruct data ตามลำดับ chunk เดิม
 *   out_buf: buffer ขนาด ≥ ceil(n_bytes/64)×64 (padded) — คืน n_bytes จริง
 *   ตรวจ CRC32 → จับ corruption (tick พัง / data พัง / count พัง)
 *   returns: 0 = loaded + CRC ผ่าน · -2 = CRC mismatch (corrupt)
 *            <0 = ไฟล์เสีย / read fail
 */
static inline int ggs_load(const char *path, uint8_t *out_buf,
                           uint64_t buf_cap, uint64_t *out_n_bytes)
{
    if (!path || !out_buf || !out_n_bytes) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -2;

    GGFHeader h;
    if (fread(&h, sizeof h, 1, f) != 1) { fclose(f); return -3; }
    if (memcmp(h.magic, GGF_MAGIC, 4) != 0) { fclose(f); return -4; }
    if (h.version != GGF_VERSION) { fclose(f); return -5; }
    if (h.level < 2 || h.level > GP_MAX_LEVEL) { fclose(f); return -6; }
    if (h.n_bytes == 0) {                          /* empty file — valid */
        *out_n_bytes = 0;
        fclose(f);
        return 0;
    }

    uint64_t n_chunks = h.n_chunks;
    uint64_t need = n_chunks * GGS_CHUNK;          /* padded size */
    if (buf_cap < need) { fclose(f); return -7; }

    uint64_t per_sphere = (uint64_t)ggd_hex_total(h.level) * GP_MAX_DIM;
    if (per_sphere == 0) { fclose(f); return -8; }

    uint64_t written = 0;
    for (uint32_t sp = 0; sp < h.n_spheres; sp++) {
        uint32_t count = 0;
        if (fread(&count, sizeof count, 1, f) != 1) { fclose(f); return -3; }
        if ((uint64_t)sp * per_sphere + count > n_chunks) {
            fclose(f); return -9;                  /* count เกิน — corrupt */
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t tick;
            uint8_t  chunk[GGS_CHUNK];
            if (fread(&tick, sizeof tick, 1, f) != 1 ||
                fread(chunk, GGS_CHUNK, 1, f) != 1) {
                fclose(f); return -3;
            }
            GpAddr a = gp_tick_to_addr(tick);
            if (a.tile_id >= ggd_face_count(h.level) || a.dim >= GP_MAX_DIM) {
                fclose(f); return -9;              /* tick นอกขอบ — corrupt */
            }
            /* self-describing: tick ต้องตรงกับตำแหน่ง chunk (kk = i)
             * → tick พัง / node เรียงผิด / node ซ้ำ → จับได้ */
            uint32_t exp_tick = gp_addr_to_tick(
                (GpAddr){ ggs_tile(h.level, i), ggs_dim(h.level, i) });
            if (tick != exp_tick) { fclose(f); return -9; }
            /* reconstruct ตามลำดับ chunk เดิม (kk = ตำแหน่งใน sphere) */
            memcpy(out_buf + (written * GGS_CHUNK), chunk, GGS_CHUNK);
            written++;
        }
    }
    if (written != n_chunks) { fclose(f); return -9; }

    /* CRC check — เหนือ data (padded 64B/chunk) เหมือนตอน save */
    uint32_t crc = ggf_crc32(out_buf, need, 0);
    if (crc != h.crc32) { fclose(f); return -10; } /* corrupt */

    *out_n_bytes = h.n_bytes;
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * SAVE VIA MMAP VIEW — เขียน .ggf ผ่าน mapping (mmap-friendly write)
 * ═══════════════════════════════════════════════════════════════════
 *
 * เหมือน ggs_save (layout/CRC/verify ภายในเดียวกัน) แต่เขียนลง mmap VIEW
 * แทน fwrite:
 *   - สร้างไฟล์ → map (Windows PAGE_READWRITE / POSIX MAP_SHARED)
 *   - เขียน header + nodes ลง view ตรงๆ (ไม่มี syscall ระหว่างเขียน)
 *   - verify จาก view เอง (memcmp กับต้นฉบับ ก่อน flush) — เขียน data เสีย
 *     ลงไฟล์ไม่ได้ (หลักเดียวกับ ggs_save)
 *   - flush + unmap + close — หลังกลับ อ่านด้วย ggf_map ได้ทันที
 *
 * deterministic: ไฟล์ที่ได้ byte-for-byte เท่ากับ ggs_save (layout เดียวกัน)
 */
static inline int ggf_save_map(const uint8_t *data, uint64_t n_bytes,
                               uint8_t level, const char *path)
{
    if (!path) return -1;
    if (!data && n_bytes > 0) return -1;
    if (level < 2) return -9;

    uint64_t n_chunks = (n_bytes + GGS_CHUNK - 1) / GGS_CHUNK;
    uint64_t per_sphere = (uint64_t)ggd_hex_total(level) * GP_MAX_DIM;
    uint32_t n_spheres = (n_chunks + per_sphere - 1) / per_sphere;
    if (n_chunks == 0) n_spheres = 0;

    /* ขนาดไฟล์ = header + Σ sphere (4 + count×68) */
    uint64_t size = sizeof(GGFHeader);
    for (uint32_t sp = 0; sp < n_spheres; sp++) {
        uint64_t k0 = (uint64_t)sp * per_sphere;
        uint64_t k1 = k0 + per_sphere;
        if (k1 > n_chunks) k1 = n_chunks;
        size += 4 + (k1 - k0) * (4 + GGS_CHUNK);
    }

    /* ── create + map ── */
#ifdef _WIN32
    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return -2;
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READWRITE,
                                   (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!mh) { CloseHandle(fh); return -3; }
    uint8_t *view = (uint8_t *)MapViewOfFile(mh, FILE_MAP_WRITE, 0, 0, 0);
    if (!view) { CloseHandle(mh); CloseHandle(fh); return -3; }
#else
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -2;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -3; }
    uint8_t *view = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) { close(fd); return -3; }
#endif

    /* ── เขียน header ── */
    GGFHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, GGF_MAGIC, 4);
    h.version = GGF_VERSION;
    h.level   = level;
    h.n_bytes = n_bytes;
    h.n_spheres = n_spheres;
    h.n_chunks  = n_chunks;
    memcpy(view, &h, sizeof h);

    /* ── เขียน nodes ── */
    uint32_t crc = 0;
    uint8_t *p = view + sizeof(GGFHeader);
    uint64_t written = 0;
    for (uint32_t sp = 0; sp < n_spheres; sp++) {
        uint64_t k0 = (uint64_t)sp * per_sphere;
        uint64_t k1 = k0 + per_sphere;
        if (k1 > n_chunks) k1 = n_chunks;
        uint32_t count = (uint32_t)(k1 - k0);
        memcpy(p, &count, sizeof count);
        p += 4;
        for (uint64_t k = k0; k < k1; k++) {
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(level, kk);
            uint8_t  dim  = ggs_dim(level, kk);
            uint32_t tick = gp_addr_to_tick((GpAddr){ tile, dim });
            memcpy(p, &tick, sizeof tick);
            p += 4;
            uint8_t chunk[GGS_CHUNK];
            memset(chunk, 0, sizeof chunk);
            uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                         ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
            memcpy(chunk, data + k * GGS_CHUNK, n);
            memcpy(p, chunk, GGS_CHUNK);
            p += GGS_CHUNK;
            crc = ggf_crc32(chunk, GGS_CHUNK, crc);
            written++;
        }
    }

    /* ── verify จาก view เอง (ก่อน flush) — เทียบกับต้นฉบับ ────── */
    {
        const uint8_t *q = view + sizeof(GGFHeader);
        uint64_t ok = 1;
        for (uint32_t sp = 0; sp < n_spheres && ok; sp++) {
            uint64_t k0 = (uint64_t)sp * per_sphere;
            uint64_t k1 = k0 + per_sphere;
            if (k1 > n_chunks) k1 = n_chunks;
            uint32_t count;
            memcpy(&count, q, sizeof count);
            if (count != (uint32_t)(k1 - k0)) { ok = 0; break; }
            q += 4;
            for (uint64_t k = k0; k < k1 && ok; k++) {
                uint64_t kk = k - k0;
                uint32_t tick;
                memcpy(&tick, q, sizeof tick);
                uint32_t exp = gp_addr_to_tick((GpAddr){
                    ggs_tile(level, kk), ggs_dim(level, kk) });
                if (tick != exp) { ok = 0; break; }
                q += 4;
                uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
                if (memcmp(q, data + k * GGS_CHUNK, n) != 0) { ok = 0; break; }
                q += GGS_CHUNK;
            }
        }
        if (!ok || written != n_chunks) {           /* เขียนไม่ถูก — ไม่ flush */
#ifdef _WIN32
            UnmapViewOfFile(view); CloseHandle(mh); CloseHandle(fh);
#else
            munmap(view, size); close(fd);
#endif
            return -6;
        }
    }

    /* ── เขียน crc ลง header ── */
    memcpy(view + offsetof(GGFHeader, crc32), &crc, sizeof crc);

    /* ── flush + unmap + close ── */
#ifdef _WIN32
    FlushViewOfFile(view, 0);
    UnmapViewOfFile(view);
    CloseHandle(mh);
    CloseHandle(fh);
#else
    msync(view, size, MS_SYNC);
    munmap(view, size);
    close(fd);
#endif
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * LAZY READ — เปิด .ggf แล้วอ่านเฉพาะ node ที่ต้องการ (seek ต่อ sphere)
 * ═══════════════════════════════════════════════════════════════════
 *
 * ggf_open:  อ่าน header + scan sphere counts (n_spheres × 4B) → สร้าง
 *            index sphere_off[] — ไม่ต้องโหลด data ลง RAM เลย
 * ggf_chunk: อ่าน node k (64B) → seek O(1) + ตรวจ tick ตรงตำแหน่ง
 * ggf_read:  อ่าน byte range [off, off+n) ตามออฟเซ็ตต้นฉบับ (ข้าม chunk ได้)
 * ggf_verify: verify CRC แบบ lazy (อ่านทั้งไฟล์ แต่ RAM คงที่ 64B — ไม่จอง
 *            buffer ขนาด data) — ใช้แทน ggs_load ได้เมื่อไม่ต้องการ buffer
 */

typedef struct {
    FILE        *f;
    GGFHeader    h;
    uint64_t     per_sphere;   /* hex_total × GP_MAX_DIM chunks/sphere */
    uint32_t     n_spheres;
    long        *sphere_off;   /* byte offset ของ count field ต่อ sphere  */
    uint32_t    *sphere_cnt;   /* nodes ต่อ sphere (cached)              */
} GGFReader;

static inline void ggf_close(GGFReader *r)
{
    if (!r) return;
    if (r->f) fclose(r->f);
    free(r->sphere_off);
    free(r->sphere_cnt);
    memset(r, 0, sizeof *r);
}

/* เปิดไฟล์ + สร้าง sphere index (ไม่โหลด data) — 0 = ok, <0 = fail */
static inline int ggf_open(const char *path, GGFReader *r)
{
    if (!path || !r) return -1;
    memset(r, 0, sizeof *r);

    r->f = fopen(path, "rb");
    if (!r->f) return -2;
    if (fread(&r->h, sizeof r->h, 1, r->f) != 1) { ggf_close(r); return -3; }
    if (memcmp(r->h.magic, GGF_MAGIC, 4) != 0)   { ggf_close(r); return -4; }
    if (r->h.version != GGF_VERSION)             { ggf_close(r); return -5; }
    if (r->h.level < 2 || r->h.level > GP_MAX_LEVEL) { ggf_close(r); return -6; }

    r->per_sphere = (uint64_t)ggd_hex_total(r->h.level) * GP_MAX_DIM;
    r->n_spheres  = r->h.n_spheres;

    if (r->h.n_chunks > 0) {
        uint64_t expect = (r->h.n_chunks + r->per_sphere - 1) / r->per_sphere;
        if (expect != r->n_spheres) { ggf_close(r); return -9; }
    }
    if (r->n_spheres == 0) return 0;             /* empty file */

    r->sphere_off = (long *)malloc(r->n_spheres * sizeof(long));
    r->sphere_cnt = (uint32_t *)malloc(r->n_spheres * sizeof(uint32_t));
    if (!r->sphere_off || !r->sphere_cnt) { ggf_close(r); return -1; }

    uint64_t total = 0;
    for (uint32_t s = 0; s < r->n_spheres; s++) {
        long pos = ftello(r->f);
        if (pos < 0) { ggf_close(r); return -3; }
        r->sphere_off[s] = pos;
        uint32_t count;
        if (fread(&count, sizeof count, 1, r->f) != 1) { ggf_close(r); return -3; }
        if ((uint64_t)count > r->per_sphere) { ggf_close(r); return -9; }
        r->sphere_cnt[s] = count;
        total += count;
        /* node s:i = 4 + i×(4+64) — ข้ามทั้ง section */
        if (fseeko(r->f, (off_t)count * (4 + GGS_CHUNK), SEEK_CUR) != 0) {
            ggf_close(r); return -3;
        }
    }
    if (total != r->h.n_chunks) { ggf_close(r); return -9; }
    return 0;
}

/* อ่าน node k (64B) — ตรวจ tick ตรงตำแหน่ง · 0 = ok · -1 = k เกิน · <0 = fail */
static inline int ggf_chunk(GGFReader *r, uint64_t k, uint8_t out[GGS_CHUNK])
{
    if (!r || !r->f) return -1;
    if (k >= r->h.n_chunks) return -1;
    uint64_t s = k / r->per_sphere;
    uint64_t i = k % r->per_sphere;
    if (s >= r->n_spheres || i >= r->sphere_cnt[s]) return -1;

    long pos = r->sphere_off[s] + 4 + (long)i * (4 + GGS_CHUNK);
    if (fseeko(r->f, (off_t)pos, SEEK_SET) != 0) return -3;

    uint32_t tick;
    if (fread(&tick, sizeof tick, 1, r->f) != 1 ||
        fread(out, GGS_CHUNK, 1, r->f) != 1) return -3;

    /* self-describing: tick ต้องตรงกับตำแหน่ง i ใน sphere (เหมือน ggs_load) */
    uint32_t exp = gp_addr_to_tick((GpAddr){
        ggs_tile(r->h.level, i), ggs_dim(r->h.level, i) });
    if (tick != exp) return -9;                  /* node ที่ k พัง */
    return 0;
}

/* อ่าน byte range ตามออฟเซ็ตต้นฉบับ (ข้าม chunk เอง) — 0 = ok · <0 = fail */
static inline int ggf_read(GGFReader *r, uint64_t off, uint8_t *out, uint64_t n)
{
    if (!r || !out) return -1;
    if (off + n > r->h.n_bytes) return -1;
    while (n > 0) {
        uint8_t chunk[GGS_CHUNK];
        int rc = ggf_chunk(r, off / GGS_CHUNK, chunk);
        if (rc != 0) return rc;
        uint64_t co  = off % GGS_CHUNK;
        uint64_t take = GGS_CHUNK - co;
        if (take > n) take = n;
        memcpy(out, chunk + co, (size_t)take);
        out += take;
        off += take;
        n   -= take;
    }
    return 0;
}

/* verify CRC แบบ lazy (RAM คงที่ 64B — ไม่จอง buffer ทั้งไฟล์)
 * 0 = ผ่าน · -10 = CRC mismatch · <0 = อ่าน fail */
static inline int ggf_verify(GGFReader *r)
{
    if (!r || !r->f) return -1;
    uint32_t crc = 0;
    uint8_t chunk[GGS_CHUNK];
    for (uint64_t k = 0; k < r->h.n_chunks; k++) {
        int rc = ggf_chunk(r, k, chunk);
        if (rc != 0) return rc;
        crc = ggf_crc32(chunk, GGS_CHUNK, crc);
    }
    return (crc == r->h.crc32) ? 0 : -10;
}

/* ═══════════════════════════════════════════════════════════════════
 * MMAP READ — map .ggf เข้าหน้าเพจ → อ่านตรงจากเพจ (zero-copy)
 * ═══════════════════════════════════════════════════════════════════
 *
 * ต่างจาก GGFReader (fseek+fread ต่อ node):
 *   - open: mmap ทั้งไฟล์ (Windows CreateFileMapping / POSIX mmap) — OS
 *     page-in on demand — ไม่มี syscall ตอนอ่านเลย
 *   - ggf_map_node: คืน POINTER ตรงเข้า data node ใน mapping (zero-copy)
 *     — ไม่มีการ copy 64B ให้ด้วยซ้ำ (อ่านตรงจากเพจ)
 *   - ggf_map_chunk/read: drop-in แทน ggf_chunk/ggf_read (memcpy จากเพจ)
 *   - ggf_map_verify: CRC เหนือ mapping (ไม่จอง buffer)
 *
 * index: scan sphere counts จาก mapping เอง (memory reads — ไม่มี seek)
 * สถิติเทียบ (bench, ไฟล์จริง): lazy seek ~27.5 MB/s vs mmap (ดู §15.88)
 */

typedef struct {
    const uint8_t *base;       /* mapped region                      */
    uint64_t       size;       /* file size (bytes)                  */
    GGFHeader      h;
    uint64_t       per_sphere;
    uint32_t       n_spheres;
    uint64_t      *sphere_off; /* byte offset ของ count field ต่อ sphere */
    uint32_t      *sphere_cnt; /* nodes ต่อ sphere                        */
#ifdef _WIN32
    HANDLE _fh, _mh;
#else
    int _fd;
#endif
} GGFMap;

static inline void ggf_unmap(GGFMap *m)
{
    if (!m) return;
#ifdef _WIN32
    if (m->base) UnmapViewOfFile(m->base);
    if (m->_mh) CloseHandle(m->_mh);
    if (m->_fh) CloseHandle(m->_fh);
#else
    if (m->base && m->size) munmap((void *)m->base, m->size);
    if (m->_fd >= 0) close(m->_fd);
#endif
    free(m->sphere_off);
    free(m->sphere_cnt);
    memset(m, 0, sizeof *m);
}

/* map .ggf + สร้าง index จาก mapping — 0 = ok · <0 = fail (เหมือน ggf_open) */
static inline int ggf_map(const char *path, GGFMap *m)
{
    if (!path || !m) return -1;
    memset(m, 0, sizeof *m);
#ifndef _WIN32
    m->_fd = -1;
#endif

#ifdef _WIN32
    m->_fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->_fh == INVALID_HANDLE_VALUE) { ggf_unmap(m); return -2; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(m->_fh, &sz)) { ggf_unmap(m); return -3; }
    m->size = (uint64_t)sz.QuadPart;
    m->_mh = CreateFileMappingA(m->_fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m->_mh) { ggf_unmap(m); return -3; }
    m->base = (const uint8_t *)MapViewOfFile(m->_mh, FILE_MAP_READ, 0, 0, 0);
    if (!m->base) { ggf_unmap(m); return -3; }
#else
    m->_fd = open(path, O_RDONLY);
    if (m->_fd < 0) { ggf_unmap(m); return -2; }
    struct stat st;
    if (fstat(m->_fd, &st) != 0) { ggf_unmap(m); return -3; }
    m->size = (uint64_t)st.st_size;
    if (m->size == 0) { ggf_unmap(m); return -3; }
    m->base = (const uint8_t *)mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, m->_fd, 0);
    if (m->base == MAP_FAILED) { ggf_unmap(m); return -3; }
#endif
    if (m->size < sizeof(GGFHeader)) { ggf_unmap(m); return -3; }

    memcpy(&m->h, m->base, sizeof m->h);
    if (memcmp(m->h.magic, GGF_MAGIC, 4) != 0)  { ggf_unmap(m); return -4; }
    if (m->h.version != GGF_VERSION)            { ggf_unmap(m); return -5; }
    if (m->h.level < 2 || m->h.level > GP_MAX_LEVEL) { ggf_unmap(m); return -6; }

    m->per_sphere = (uint64_t)ggd_hex_total(m->h.level) * GP_MAX_DIM;
    m->n_spheres  = m->h.n_spheres;
    if (m->h.n_chunks > 0) {
        uint64_t expect = (m->h.n_chunks + m->per_sphere - 1) / m->per_sphere;
        if (expect != m->n_spheres) { ggf_unmap(m); return -9; }
    }
    if (m->n_spheres == 0) return 0;

    m->sphere_off = (uint64_t *)malloc(m->n_spheres * sizeof(uint64_t));
    m->sphere_cnt = (uint32_t *)malloc(m->n_spheres * sizeof(uint32_t));
    if (!m->sphere_off || !m->sphere_cnt) { ggf_unmap(m); return -1; }

    /* scan counts จาก mapping — memory reads ล้วน (ไม่มี seek) */
    const uint8_t *p = m->base + sizeof(GGFHeader);
    uint64_t total = 0;
    for (uint32_t s = 0; s < m->n_spheres; s++) {
        m->sphere_off[s] = (uint64_t)(p - m->base);
        uint32_t count;
        memcpy(&count, p, sizeof count);
        if ((uint64_t)count > m->per_sphere) { ggf_unmap(m); return -9; }
        m->sphere_cnt[s] = count;
        total += count;
        p += 4 + (uint64_t)count * (4 + GGS_CHUNK);
    }
    if (total != m->h.n_chunks) { ggf_unmap(m); return -9; }
    if ((uint64_t)(p - m->base) > m->size) { ggf_unmap(m); return -9; }
    return 0;
}

/* ── zero-copy: คืน pointer ตรงเข้า data node k ใน mapping (ไม่ copy) ──
 * ตรวจ tick ตรงตำแหน่งเหมือน ggf_chunk (rc -9 ถ้าพัง)
 * returns pointer หรือ NULL (เกิน/พัง) — out_tick = tick ที่อ่านได้ */
static inline const uint8_t *ggf_map_node(const GGFMap *m, uint64_t k,
                                          uint32_t *out_tick)
{
    if (!m || !m->base) return NULL;
    if (k >= m->h.n_chunks) return NULL;
    uint64_t s = k / m->per_sphere;
    uint64_t i = k % m->per_sphere;
    if (s >= m->n_spheres || i >= m->sphere_cnt[s]) return NULL;

    const uint8_t *node = m->base + m->sphere_off[s] + 4 + i * (4 + GGS_CHUNK);
    uint32_t tick;
    memcpy(&tick, node, sizeof tick);
    if (out_tick) *out_tick = tick;
    uint32_t exp = gp_addr_to_tick((GpAddr){
        ggs_tile(m->h.level, i), ggs_dim(m->h.level, i) });
    if (tick != exp) return NULL;               /* node k พัง */
    return node + 4;                            /* data 64B */
}

/* drop-in แทน ggf_chunk — copy 64B จากเพจลง out · 0 = ok · -1/-9 = fail */
static inline int ggf_map_chunk(const GGFMap *m, uint64_t k, uint8_t out[GGS_CHUNK])
{
    const uint8_t *d = ggf_map_node(m, k, NULL);
    if (!d) return (k < m->h.n_chunks) ? -9 : -1;
    memcpy(out, d, GGS_CHUNK);
    return 0;
}

/* drop-in แทน ggf_read — byte range ตามออฟเซ็ตต้นฉบับ (ข้าม chunk เอง) */
static inline int ggf_map_read(const GGFMap *m, uint64_t off, uint8_t *out, uint64_t n)
{
    if (!m || !out) return -1;
    if (off + n > m->h.n_bytes) return -1;
    while (n > 0) {
        uint64_t k = off / GGS_CHUNK;
        const uint8_t *d = ggf_map_node(m, k, NULL);
        if (!d) return -9;
        uint64_t co = off % GGS_CHUNK;
        uint64_t take = GGS_CHUNK - co;
        if (take > n) take = n;
        memcpy(out, d + co, (size_t)take);
        out += take;
        off += take;
        n   -= take;
    }
    return 0;
}

/* verify CRC เหนือ mapping (ไม่จอง buffer) · 0 = ผ่าน · -10 = mismatch */
static inline int ggf_map_verify(const GGFMap *m)
{
    if (!m || !m->base) return -1;
    uint32_t crc = 0;
    for (uint64_t k = 0; k < m->h.n_chunks; k++) {
        const uint8_t *d = ggf_map_node(m, k, NULL);
        if (!d) return -9;
        crc = ggf_crc32(d, GGS_CHUNK, crc);
    }
    return (crc == m->h.crc32) ? 0 : -10;
}

#endif /* GEO_GOLDBERG_FILE_H */
