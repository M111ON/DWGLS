/* ghost_delta.h — Delta-mode ghost payload (pred+ent, §15.74)
 * ═══════════════════════════════════════════════════════════════════════
 * ต่อจาก T1.1d (HyperDeltaEnt): เก็บ residual แบบ scale-predict + Huffman
 * ลงใน ghost entry แทน payload ดิบ — read materialize ผ่าน delta แทน thaw ตรง.
 *
 *   pred[i]  = base[i>>1]              — subsample-2 = "coarse view byte"
 *                                        (เดียวกับ hdent_pred: (kis>>20)&0xFF)
 *   residual = (orig − pred) & 0xFF    — กระจุกใกล้ 0 บนข้อมูล smooth/structured
 *   huffman  = canonical (huff_codec.h) — lens[256] + coded (deterministic)
 *   recover  = base[i>>1] + residual[i] — lossless
 *
 * Blob format (self-contained — อ่านได้โดยไม่ต้องมีข้อมูลอื่น):
 *   [0]      u8  mode      = GHOST_DELTA_MODE_HDENT
 *   [1]      u8  base_kind = 0 (subsample-2)
 *   [2..3]   u16 base_n    = base bytes ((n+1)/2)
 *   [4..5]   u16 orig_n    = original byte count (decode target)
 *   [6..9]   u32 data_len  = lens[256] + coded residual bytes
 *   [10..]   base[base_n]  → lens[256] → coded
 *
 * GHOST_DELTA_MODE_RAW (0) = payload ดิบ (fallback เมื่อ delta ไม่ชนะ)
 * — ตัวเลือก adaptive อยู่ที่ caller: encode → เทียบ size → เลือกเล็กกว่า.
 *
 * จำกัด n ≤ GHOST_DELTA_MAX_N (16 KB) — เหนือนั้น fallback raw.
 * ทั้งหมด header-only, integer-only (huff_codec.h).
 */

#ifndef GEO_GHOST_DELTA_H
#define GEO_GHOST_DELTA_H

#include <stdint.h>
#include <string.h>
#include "huff_codec.h"

#define GHOST_DELTA_MODE_RAW    0u
#define GHOST_DELTA_MODE_HDENT  1u
#define GHOST_DELTA_MAX_N       16384u   /* chunk ≤ 16 KB — เหนือนั้น raw */
#define GHOST_DELTA_HDR_SZ      10u

typedef struct __attribute__((packed)) {
    uint8_t  mode;       /* GHOST_DELTA_MODE_*       */
    uint8_t  base_kind;  /* 0 = subsample-2          */
    uint16_t base_n;     /* base bytes               */
    uint16_t orig_n;     /* original size (decode)   */
    uint32_t data_len;   /* lens[256] + coded        */
} GhostDeltaHdr;         /* packed = 10 B — ห้าม pad!
                            (pad 12B → base data ทับ high bytes
                             ของ data_len — blob[10..11] ถูกทับเป็น 0) */

/* ── encode: orig[n] → blob (คืนความยาว blob, 0 = ไม่ fit/ไม่ encode ได้) ──
   cap ต้อง ≥ 10 + (n+1)/2 + 256 + n + 4  (worst-case coded ≤ n + 4) */
static inline uint32_t ghost_delta_encode(const uint8_t *orig, uint32_t n,
                                          uint8_t *blob, uint32_t cap) {
    if (!orig || !blob || n == 0 || n > GHOST_DELTA_MAX_N)
        return 0;
    uint32_t base_n = (n + 1u) >> 1;
    uint32_t need = GHOST_DELTA_HDR_SZ + base_n + 256u + n + 4u;
    if (cap < need) return 0;

    uint8_t residual[GHOST_DELTA_MAX_N];
    uint64_t freq[256] = {0};
    for (uint32_t i = 0; i < n; i++) {
        /* pred ต้องตรงกับ decode: base[i>>1] = orig[(i>>1)<<1] (even sample)
           — subsample-2 center: pred ของทั้งคู่ (2k, 2k+1) = orig[2k] */
        uint8_t b = orig[(i >> 1) << 1];
        uint8_t r = (uint8_t)((orig[i] - b) & 0xFFu);
        residual[i] = r;
        freq[r]++;
    }

    HuffModel m;
    huff_build(&m, freq);

    uint8_t *p = blob + GHOST_DELTA_HDR_SZ;
    for (uint32_t i = 0; i < base_n; i++) p[i] = orig[i << 1];
    p += base_n;
    memcpy(p, m.lens, 256);
    p += 256;
    uint32_t coded = huff_encode(&m, residual, n, p, cap - (uint32_t)(p - blob));
    if (coded == 0) return 0;                     /* pathological — raw */

    GhostDeltaHdr *h = (GhostDeltaHdr *)blob;
    h->mode      = GHOST_DELTA_MODE_HDENT;
    h->base_kind = 0;
    h->base_n    = (uint16_t)base_n;
    h->orig_n    = (uint16_t)n;
    h->data_len  = 256u + coded;
    return GHOST_DELTA_HDR_SZ + base_n + 256u + coded;
}

/* ── decode: blob → out[orig_n] (คืน 0 = OK, −1 = ผิด format) ── */
static inline int ghost_delta_decode(const uint8_t *blob, uint32_t blob_len,
                                     uint8_t *out, uint32_t cap) {
    if (!blob || !out || blob_len < GHOST_DELTA_HDR_SZ) return -1;
    const GhostDeltaHdr *h = (const GhostDeltaHdr *)blob;
    if (h->mode != GHOST_DELTA_MODE_HDENT) return -1;
    uint32_t n = h->orig_n;
    if (n == 0 || n > GHOST_DELTA_MAX_N || cap < n) return -1;
    if (h->data_len < 256u) return -1;
    if (blob_len != GHOST_DELTA_HDR_SZ + h->base_n + h->data_len) return -1;

    const uint8_t *base  = blob + GHOST_DELTA_HDR_SZ;
    const uint8_t *lens  = base + h->base_n;
    const uint8_t *coded = lens + 256u;

    HuffModel m;
    huff_rebuild(&m, lens);
    uint8_t residual[GHOST_DELTA_MAX_N];
    if (huff_decode(&m, coded, h->data_len - 256u, residual, n) != 0)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (uint8_t)((base[i >> 1] + residual[i]) & 0xFFu);
    return 0;
}

/* ── size probes ── */
static inline uint32_t ghost_delta_size(const uint8_t *blob, uint32_t blob_len) {
    if (!blob || blob_len < GHOST_DELTA_HDR_SZ) return blob_len;
    const GhostDeltaHdr *h = (const GhostDeltaHdr *)blob;
    return GHOST_DELTA_HDR_SZ + h->base_n + h->data_len;
}

static inline uint32_t ghost_delta_orig_n(const uint8_t *blob, uint32_t blob_len) {
    if (!blob || blob_len < GHOST_DELTA_HDR_SZ) return 0;
    return ((const GhostDeltaHdr *)blob)->orig_n;
}

#endif /* GEO_GHOST_DELTA_H */
