#pragma once
#ifndef GEO_FRAME_SEEK_WANG_H
#define GEO_FRAME_SEEK_WANG_H

/*
 * geo_frame_seek_wang.h — Wang Tile Layer for geo_frame_seek
 *
 * 1440 frames → 120 windows × 12 frames (1 phase cycle per window)
 * Wang edge = Fibonacci 2&7 chord on 9-point clock
 *
 * Edge derivation (per enc):
 *   edge_A = (enc × 2) % 9   — World A (binary multiplier)
 *   edge_B = (enc × 7) % 9   — World B (complement)
 *   invariant: edge_A + edge_B == 9  (enc%9 != 0), both 0 if enc%9==0
 *   → tamper detect: sum != 9 → corrupt
 *
 * 369 self-reference: enc%9 ∈ {0,3,6} → Tesla loop = skip boundary
 *
 * Tamper: edge_bot[w] == edge_top[w+1]
 * Parity: XOR of enc per window → reconstruct 1 missing
 *
 * FIX (2026-08-17): edge_bot เดิมใช้ frame สุดท้ายใน window (t = w*12+11)
 *   → edge continuity พังทุก window (fwang_verify = -2, gate เปิดแค่ 8/1440
 *   เฟรม) เพราะ enc เพิ่ม 37/step → chord_a เลื่อน 2 ทุก boundary
 *   แก้เป็น frame ที่ BOUNDARY ของ window (t = (w+1)*12 = frame แรกของ
 *   window ถัดไป) → edge_bot[w] == edge_top[w+1] จริง 119/119 + wrap ✓
 *   (Wang semantics: ขอบล่างของ tile = ค่าที่แบ่งกับ tile ถัดไป)
 *   หมายเหตุ: FGLS_new ยังเป็นต้นฉบับที่พัง — แก้เฉพาะ DWGLS นี้
 */

#include <stdbool.h>
#include "geo_frame_seek.h"

#define WANG_WIN_SIZE    12u
#define WANG_WIN_COUNT   120u   /* 1440 / 12 */

/* ── Fib 2&7 chord functions ── */

static inline uint8_t _fwang_chord_a(uint16_t enc) {
    return (uint8_t)((enc * 2u) % 9u);
}

static inline uint8_t _fwang_chord_b(uint16_t enc) {
    return (uint8_t)((enc * 7u) % 9u);
}

/* 369 loop: marks skip boundary */
static inline bool _fwang_is_369(uint16_t enc) {
    uint8_t d = (uint8_t)(enc % 9u);
    return d == 0u || d == 3u || d == 6u;
}

/* tamper invariant: chord_a + chord_b == 9 (or both 0 if enc%9==0) */
static inline bool _fwang_chord_valid(uint16_t enc) {
    uint8_t a = _fwang_chord_a(enc);
    uint8_t b = _fwang_chord_b(enc);
    return (enc % 9u == 0u) ? (a == 0u && b == 0u) : ((a + b) == 9u);
}

/* ── Window struct ── */

typedef struct {
    uint16_t xor_enc;       /* XOR of all enc in window */
    uint8_t  edge_top;      /* chord_A of first frame enc */
    uint8_t  edge_bot;      /* chord_A of last frame enc */
    uint8_t  edge_top_b;    /* chord_B complement — tamper check */
    uint8_t  edge_bot_b;
    uint8_t  tile_id;       /* face of first frame (0..11) */
    uint16_t skip_mask;     /* bitmask: which of 12 frames is_skip */
    uint16_t corrupt_mask;  /* bitmask: suspect frames */
    bool     valid;
} FrameWangWindow;

typedef struct {
    FrameWangWindow wins[WANG_WIN_COUNT];
    uint32_t        dirty_mask[4];  /* 120 bits */
} FrameWangLayer;

/* ── dirty tracking ── */
static inline void _fwang_set_dirty(FrameWangLayer *wl, uint16_t win) {
    wl->dirty_mask[win >> 5] |= (1u << (win & 31u));
}
static inline bool _fwang_is_dirty(const FrameWangLayer *wl, uint16_t win) {
    return (wl->dirty_mask[win >> 5] >> (win & 31u)) & 1u;
}
static inline void _fwang_clear_dirty(FrameWangLayer *wl, uint16_t win) {
    wl->dirty_mask[win >> 5] &= ~(1u << (win & 31u));
}

/* ── compute window ── */
static inline void fwang_compute_win(FrameWangLayer *wl, uint16_t win)
{
    uint32_t base_t = (uint32_t)win * WANG_WIN_SIZE;
    uint16_t xor_acc = 0u;
    /* FIX: ต้องเป็น uint16_t — skip อยู่ที่ตำแหน่ง 9..11 ใน window
     * เสมอ (enc%12 >= 9) → uint8_t truncate bits ≥ 8 → mask = 0 ทุก
     * window → fwang_verify = -5 (popcount != 3) */
    uint16_t skip_mask = 0u;
    uint16_t first_enc = 0u;
    uint8_t  tile_id = 0u;

    for (uint8_t i = 0u; i < WANG_WIN_SIZE; i++) {
        DualFrame f = frame_seek(base_t + i);
        xor_acc ^= f.enc;
        if (f.h.is_skip) skip_mask |= (uint16_t)(1u << i);
        if (i == 0u) { first_enc = f.enc; tile_id = f.face; }
    }

    FrameWangWindow *w = &wl->wins[win];
    w->xor_enc    = xor_acc;
    w->edge_top   = _fwang_chord_a(first_enc);
    w->edge_top_b = _fwang_chord_b(first_enc);
    /* FIX: ขอบล่าง = frame ที่ boundary (t = base+12 = frame แรกของ
     * window ถัดไป) — ไม่ใช่ frame สุดท้ายใน window (last_enc)
     * เพราะ enc เพิ่ม 37/step → chord_a ของ last ต่างจาก first ของ
     * window ถัดไป 2 เสมอ → continuity พังทุก window */
    w->edge_bot   = _fwang_chord_a(
        frame_seek(base_t + WANG_WIN_SIZE).enc);
    w->edge_bot_b = _fwang_chord_b(
        frame_seek(base_t + WANG_WIN_SIZE).enc);
    w->tile_id    = tile_id;
    w->skip_mask  = skip_mask;
    w->corrupt_mask = 0u;
    w->valid      = true;
    _fwang_clear_dirty(wl, win);
}

/* ── init ── */
static inline void fwang_init(FrameWangLayer *wl) {
    __builtin_memset(wl, 0, sizeof(*wl));
    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        fwang_compute_win(wl, w);
}

static inline void fwang_flush_dirty(FrameWangLayer *wl) {
    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        if (_fwang_is_dirty(wl, w))
            fwang_compute_win(wl, w);
}

/* ── edge validation ── */

static inline bool fwang_edge_valid(const FrameWangLayer *wl, uint16_t win) {
    if (!wl->wins[win].valid) return false;
    if (win == 0u) return true;
    return wl->wins[win - 1u].edge_bot == wl->wins[win].edge_top;
}

static inline bool fwang_edge_valid_wrap(const FrameWangLayer *wl, uint16_t win) {
    uint16_t prev = (win + WANG_WIN_COUNT - 1u) % WANG_WIN_COUNT;
    return wl->wins[prev].edge_bot == wl->wins[win].edge_top;
}

/* tamper: chord_a + chord_b must == 9 per window boundary */
static inline bool fwang_tamper_check(const FrameWangWindow *w) {
    bool top_ok = (w->edge_top == 0u && w->edge_top_b == 0u)
                || (w->edge_top + w->edge_top_b == 9u);
    bool bot_ok = (w->edge_bot == 0u && w->edge_bot_b == 0u)
                || (w->edge_bot + w->edge_bot_b == 9u);
    return top_ok && bot_ok;
}

/* ── seek gate ── */
typedef enum {
    FWANG_SEEK_OK,
    FWANG_SEEK_MISMATCH,   /* edge broken */
    FWANG_SEEK_369,        /* Tesla loop boundary — cpair candidate */
    FWANG_SEEK_TAMPER,     /* chord invariant broken */
} FrameWangDecision;

static inline FrameWangDecision fwang_seek_gate(FrameWangLayer *wl,
                                                  uint16_t enc)
{
    uint16_t win = (enc / WANG_WIN_SIZE) % WANG_WIN_COUNT;
    if (_fwang_is_dirty(wl, win)) fwang_compute_win(wl, win);

    /* FIX: _fwang_chord_valid(enc) เป็น identity เสมอ (2e+7e=9e≡0 mod 9)
     * → TAMPER ไม่มีทาง trigger จาก enc — tamper จริงต้องตรวจชั้นเก็บ
     * (edge_top/_b, edge_bot/_b ที่ window) → wire fwang_tamper_check
     * เข้า gate (เดิมเขียนไว้แต่ไม่ถูกเรียก — dead code) */
    if (!fwang_tamper_check(&wl->wins[win])) return FWANG_SEEK_TAMPER;
    if (_fwang_is_369(enc))             return FWANG_SEEK_369;
    if (!fwang_edge_valid(wl, win))     return FWANG_SEEK_MISMATCH;
    return FWANG_SEEK_OK;
}

/* ── XOR reconstruct (1 missing) ── */
static inline bool fwang_reconstruct_enc(const FrameWangWindow *w,
                                          uint16_t known_xor,
                                          uint16_t *out_enc)
{
    if (__builtin_popcount(w->corrupt_mask) != 1) return false;
    *out_enc = w->xor_enc ^ known_xor;
    return true;
}

/* ── verify ── */
static inline int fwang_verify(const FrameWangLayer *wl)
{
    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        if (!wl->wins[w].valid) return -1;

    for (uint16_t w = 1u; w < WANG_WIN_COUNT; w++)
        if (!fwang_edge_valid(wl, w)) return -2;

    if (!fwang_edge_valid_wrap(wl, 0u)) return -3;

    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        if (wl->wins[w].tile_id > 11u) return -4;

    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        if (__builtin_popcount(wl->wins[w].skip_mask) != 3u) return -5;

    /* [W6] chord tamper invariant holds for all windows */
    for (uint16_t w = 0u; w < WANG_WIN_COUNT; w++)
        if (!fwang_tamper_check(&wl->wins[w])) return -6;

    return 0;
}

#endif /* GEO_FRAME_SEEK_WANG_H */
