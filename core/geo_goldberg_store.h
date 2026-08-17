/*
 * geo_goldberg_store.h — Goldberg decagram storage API (ระบบ)
 * ═══════════════════════════════════════════════════════════════
 *
 * จัดเก็บ tensor/data ผ่าน decagram-Goldberg (T1.2f, §15.80-15.83):
 *   data → 64B chunks → gp_lens_write(tile, dim) → sphere(s) → verify
 *
 * ADDRESSING (decagram, geo_goldberg_decagram.h):
 *   hex tile_id = 12 + (k mod hex_total)         (hex_total = 10(n²−1))
 *   dim         = k / hex_total                  (0..GP_MAX_DIM−1)
 *   → 1 sphere เก็บ hex_total × GP_MAX_DIM chunks (streaming unit)
 *   → tensor ใหญ่ = หลาย sphere, เขียน→verify→destroy ทีละ sphere
 *     (RAM คงที่ 1 sphere — ไม่ขึ้นกับขนาด tensor, §15.83)
 *
 * สถานะของ store: สถิติสะสม (bytes/chunks) — ตัว data อยู่ที่ caller
 * (zero-copy จาก mmap) — ฟังก์ชัน verify เทียบกับต้นฉบับโดยตรง
 *
 * Depends: geo_goldberg_decagram.h + geo_goldberg_sphere.h + infra/tring.h
 */

#ifndef GEO_GOLDBERG_STORE_H
#define GEO_GOLDBERG_STORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_goldberg_decagram.h"
#include "geo_goldberg_sphere.h"
#include "infra/tring.h"

#define GGS_CHUNK        GP_CHUNK_SZ       /* 64B lens window */
#define GGS_DEFAULT_LEV  8u                /* GP(8,0) = 642 faces */

typedef struct {
    uint8_t  level;         /* gp level (1..8)                      */
    uint32_t faces;         /* ggd_face_count(level)                */
    uint32_t hex_total;     /* ggd_hex_total(level) = 10(n²−1)      */
    uint64_t per_sphere;    /* hex_total × GP_MAX_DIM chunks/sphere */
    uint64_t chunks_stored; /* cumulative (all tensors)             */
    uint64_t bytes_stored;  /* cumulative                           */
} GoldbergStore;

static inline void ggs_init(GoldbergStore *s, uint8_t level)
{
    s->level = (level < 1) ? 1 : (level > GP_MAX_LEVEL ? GP_MAX_LEVEL : level);
    s->faces     = ggd_face_count(s->level);
    s->hex_total = ggd_hex_total(s->level);
    s->per_sphere = (uint64_t)s->hex_total * GP_MAX_DIM;
    s->chunks_stored = 0;
    s->bytes_stored  = 0;
}

/* chunk k → (tile_id, dim) within a sphere (decagram addressing) */
static inline uint32_t ggs_tile(uint8_t level, uint64_t k)
{
    return GGD_PENTAGONS + (uint32_t)(k % ggd_hex_total(level));
}
static inline uint8_t ggs_dim(uint8_t level, uint64_t k)
{
    return (uint8_t)(k / ggd_hex_total(level));
}

/* number of spheres needed for a tensor of n_chunks */
static inline uint32_t ggs_spheres(const GoldbergStore *s, uint64_t n_chunks)
{
    if (n_chunks == 0) return 0;
    return (uint32_t)((n_chunks + s->per_sphere - 1) / s->per_sphere);
}

/*
 * ggs_store — เก็บ data (n_bytes) ผ่าน decagram-Goldberg + verify ระหว่างทาง
 *   write→verify→destroy ทีละ sphere (streaming, RAM ~1 sphere)
 *   ใช้ Tring ภายใน — ฟรีเองทุก sphere
 *   returns: 0 = ครบ lossless · <0 = fail (พร้อม sphere ที่พัง)
 */
static inline int ggs_store(GoldbergStore *s,
                            const uint8_t *data, uint64_t n_bytes)
{
    if (!data && n_bytes > 0) return -1;
    if (n_bytes == 0) return 0;

    uint64_t n_chunks = (n_bytes + GGS_CHUNK - 1) / GGS_CHUNK;
    uint32_t n_spheres = ggs_spheres(s, n_chunks);

    for (uint32_t sp = 0; sp < n_spheres; sp++) {
        Tring tring;
        if (tring_init(&tring, (uint32_t)(s->faces << 8)) != 0)
            return -2;
        GpSphere sphere;
        gp_sphere_init(&sphere, &tring, s->level);

        uint64_t k0 = (uint64_t)sp * s->per_sphere;
        uint64_t k1 = k0 + s->per_sphere;
        if (k1 > n_chunks) k1 = n_chunks;

        /* write */
        for (uint64_t k = k0; k < k1; k++) {
            const uint8_t *src = data + k * GGS_CHUNK;
            uint8_t chunk[GGS_CHUNK];
            uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                         ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
            memset(chunk, 0, sizeof chunk);
            memcpy(chunk, src, n);
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(s->level, kk);
            uint8_t  dim  = ggs_dim(s->level, kk);
            if (gp_lens_write(&sphere, tile, dim, chunk) == UINT32_MAX) {
                tring_destroy(&tring);
                return -3;
            }
        }
        /* verify within sphere */
        for (uint64_t k = k0; k < k1; k++) {
            uint64_t kk = k - k0;
            uint32_t tile = ggs_tile(s->level, kk);
            uint8_t  dim  = ggs_dim(s->level, kk);
            const uint8_t *rd = gp_lens_read(&sphere, tile, dim);
            uint32_t n = (n_bytes - k * GGS_CHUNK >= GGS_CHUNK)
                         ? GGS_CHUNK : (uint32_t)(n_bytes - k * GGS_CHUNK);
            if (!rd || memcmp(rd, data + k * GGS_CHUNK, n) != 0) {
                tring_destroy(&tring);
                return -4;
            }
        }
        tring_destroy(&tring);
        s->chunks_stored += (k1 - k0);
        s->bytes_stored  += (k1 - k0) * GGS_CHUNK;
    }
    return 0;
}

/*
 * ggs_store_verify — variant ที่คืน lossless flag (สะดวกเรียกจาก probe)
 *   0 = lossless · 1 = fail
 */
static inline int ggs_store_verify(GoldbergStore *s,
                                   const uint8_t *data, uint64_t n_bytes)
{
    return ggs_store(s, data, n_bytes) != 0;
}

#endif /* GEO_GOLDBERG_STORE_H */
