/* ckpt_wang.h — Wang checkpoint validation: จับ corrupted checkpoint ก่อน decode
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "เอา wang gate ไปใช้กับ checkpoint image จริง (fibo checkpoint):
 *        ก่อน replay ตรวจ wang edges ทั้ง log — จับ corrupted checkpoint
 *        ได้ก่อน decode"
 *
 * wang-flavored digest ของ ghost log (ใช้กลไกเดียวกับ geo_frame_seek_wang:
 * chord 2&7 บน 9-clock, window = 12, 369 Tesla-loop markers):
 *   - window = 12 entries (สมมาตร wang WANG_WIN_SIZE)
 *   - edge_top  = chord_a(enc ของ entry แรกของ window)
 *   - edge_bot  = chord_a(enc ของ boundary = entry แรกของ window ถัดไป)
 *     (บทเรียน §15.50: edge_bot ต้องเป็นค่าที่ boundary ไม่ใช่ entry สุดท้าย)
 *   - parity    = XOR ของ enc ทั้ง window — reconstruct 1 missing
 *   - n369      = จำนวน entry ที่ enc%9 ∈ {0,3,6} (Tesla loop)
 *
 * flow:  serialize → ckpt_wang_digest เก็บต่อจาก log ใน checkpoint image
 *        load    → ckpt_wang_verify  recompute + เทียบ → ต่าง = corrupted
 *                  (reject ก่อน decode — ยังไม่ thaw อะไร)
 *        scan    → ckpt_wang_scan  hyp_gate ทุก entry (timeline ต้องเปิด)
 */

#ifndef CKPT_WANG_H
#define CKPT_WANG_H

#include <stdint.h>
#include <string.h>
#include "geo_ghost_lift.h"

#define CKPT_WANG_WIN 12u   /* สมมาตร wang WANG_WIN_SIZE */

/* 8B ต่อ window */
typedef struct __attribute__((packed)) {
    uint8_t  edge_top;    /* chord_a(enc ของ entry แรกของ window)      */
    uint8_t  edge_top_b;  /* chord_b — complement (tamper pair)        */
    uint8_t  edge_bot;    /* chord_a(enc ของ boundary = แรกของถัดไป)    */
    uint8_t  edge_bot_b;
    uint16_t parity;      /* XOR ของ enc ทั้ง window                    */
    uint8_t  n369;        /* จำนวน Tesla-loop markers (enc%9 ∈ {0,3,6}) */
    uint8_t  _pad;
} CkptWangWin;

/* enc ของ entry = ตำแหน่ง timeline ของ route (เหมือน ghost_read) */
static inline uint16_t ckpt_entry_enc(const GhostLogEntry *e) {
    return (uint16_t)((ghost_origin_seed(e->block_id, e->from_scale)
                      + e->to_scale) % FRAME_CYCLE);
}

static inline uint32_t ckpt_wang_digest_size(uint32_t count) {
    return ((count + CKPT_WANG_WIN - 1) / CKPT_WANG_WIN) * sizeof(CkptWangWin);
}

/* digest — wang edges + parity + 369 ต่อ window (เก็บใน checkpoint image) */
static inline void ckpt_wang_digest(const GhostLog *log, void *out) {
    uint32_t nw = (log->count + CKPT_WANG_WIN - 1) / CKPT_WANG_WIN;
    uint8_t *o = (uint8_t *)out;
    for (uint32_t w = 0; w < nw; w++) {
        uint32_t base = w * CKPT_WANG_WIN;
        uint32_t n = (base + CKPT_WANG_WIN <= log->count)
                   ? CKPT_WANG_WIN : (log->count - base);
        uint16_t enc_first = ckpt_entry_enc(&log->entries[base]);
        uint16_t enc_bot = (base + CKPT_WANG_WIN < log->count)
                         ? ckpt_entry_enc(&log->entries[base + CKPT_WANG_WIN])
                         : ckpt_entry_enc(&log->entries[base + n - 1]);
        CkptWangWin wv;
        wv.edge_top   = _fwang_chord_a(enc_first);
        wv.edge_top_b = _fwang_chord_b(enc_first);
        wv.edge_bot   = _fwang_chord_a(enc_bot);
        wv.edge_bot_b = _fwang_chord_b(enc_bot);
        uint16_t par = 0; uint8_t n369 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t enc = ckpt_entry_enc(&log->entries[base + i]);
            par ^= enc;
            if (enc % 9u == 0u || enc % 9u == 3u || enc % 9u == 6u) n369++;
        }
        wv.parity = par; wv.n369 = n369; wv._pad = 0;
        memcpy(o + w * sizeof(CkptWangWin), &wv, sizeof(CkptWangWin));
    }
}

/* verify — recompute ทีละ window เทียบ digest → ต่าง = corrupted (-1) */
static inline int ckpt_wang_verify(const GhostLog *log, const void *digest,
                                   uint32_t dsz) {
    if (!log || !digest) return -1;
    uint32_t nw = (log->count + CKPT_WANG_WIN - 1) / CKPT_WANG_WIN;
    if (dsz < nw * sizeof(CkptWangWin)) return -1;
    const uint8_t *ref = (const uint8_t *)digest;
    for (uint32_t w = 0; w < nw; w++) {
        uint32_t base = w * CKPT_WANG_WIN;
        uint32_t n = (base + CKPT_WANG_WIN <= log->count)
                   ? CKPT_WANG_WIN : (log->count - base);
        uint16_t enc_first = ckpt_entry_enc(&log->entries[base]);
        uint16_t enc_bot = (base + CKPT_WANG_WIN < log->count)
                         ? ckpt_entry_enc(&log->entries[base + CKPT_WANG_WIN])
                         : ckpt_entry_enc(&log->entries[base + n - 1]);
        CkptWangWin wv;
        wv.edge_top   = _fwang_chord_a(enc_first);
        wv.edge_top_b = _fwang_chord_b(enc_first);
        wv.edge_bot   = _fwang_chord_a(enc_bot);
        wv.edge_bot_b = _fwang_chord_b(enc_bot);
        uint16_t par = 0; uint8_t n369 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t enc = ckpt_entry_enc(&log->entries[base + i]);
            par ^= enc;
            if (enc % 9u == 0u || enc % 9u == 3u || enc % 9u == 6u) n369++;
        }
        wv.parity = par; wv.n369 = n369; wv._pad = 0;
        if (memcmp(&wv, ref + w * sizeof(CkptWangWin), sizeof(CkptWangWin)) != 0)
            return (int)w + 1;   /* window ที่ corrupt */
    }
    return 0;
}

/* scan — ตรวจ wang edges ทั้ง log: hyp_gate ทุก entry (timeline ต้องเปิด)
   คืน index แรกที่ปิด หรือ -1 ถ้าทั้งหมดเปิด */
static inline int ckpt_wang_scan(const GhostLog *log) {
    if (!log) return -1;
    for (uint32_t i = 0; i < log->count; i++) {
        uint16_t enc = ckpt_entry_enc(&log->entries[i]);
        HypSeek d = hyp_gate(&log->wang, enc,
                             (uint8_t)(_fwang_chord_a(enc) & 3u));
        if (d == HYP_SEEK_TAMPER || d == HYP_SEEK_CLOSED) return (int)i;
    }
    return -1;   /* ทั้งหมดเปิด — ผ่าน */
}

/* ตัวเดียวที่ต้องเรียกก่อน replay: verify digest แล้ว scan gate
   คืน 0 = ผ่าน · >0 = digest ต่าง (window ที่ corrupt) · <0 = scan ปิด */
static inline int ckpt_wang_check(const GhostLog *log, const void *digest,
                                  uint32_t dsz) {
    int r = ckpt_wang_verify(log, digest, dsz);
    if (r != 0) return r;      /* corrupted — reject ก่อน decode */
    int s = ckpt_wang_scan(log);
    return (s < 0) ? 0 : -(100 + s);   /* scan: -1 = เปิดหมด → 0 */
}

#endif /* CKPT_WANG_H */
