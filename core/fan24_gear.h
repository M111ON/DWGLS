/*
 * fan24_gear.h — FAN24 Gear Event Format (KIS × Hyperbolic sync wheel)
 * ════════════════════════════════════════════════════════════════════════
 *
 * WIRED (2026-08-26) จาก probes ที่พิสูจน์แล้ว (docs/FAN24-GEAR-SYNC.md):
 *   tools/fan24_gear_probe.c       — mesh identity G1–G7 (12/12)
 *   tools/fan24_gear_sync_probe.c  — gear delta log M1–M7 (10/10)
 *
 * ring-24 = ขอบเฟืองร่วมสองฝั่ง:
 *   ล้อ KIS        = cube wheel, 8 ฟัน   (s mod 8)
 *   ล้อ HYPERBOLIC = axis wheel, 3 ฟัน   (s mod 3)
 *   24 = 8·3, gcd(8,3)=1 → CRT bijection  s ↦ (s%8, s%3) บน Z24 (lossless)
 *   split อื่น (6,4)/(12,2): lcm=12 < 24 = fence (ห้ามใช้ addressing)
 *
 * EVENT FORMAT (แทน {from:u8,to:u8} = 16b ของ passive scale log เดิม):
 *   Δ = (to − from + 144) % 144 ; Δ = 24q + r ; r ≡ (dc mod 8, dx mod 3)
 *   FREE event = { q:3b | dc:3b | dx:2b } = 8 บิต/event
 *   RIM event  = { q:3b }                 = 3 บิต/event
 *     RIM mode = ทุก event ใน log เป็นฟันเดินตรง (r == 0, Δ ≡ 0 mod 24)
 *     header ประกาศโหมด 1 บิต — โปร่งใสทั้งสองฝั่ง
 *
 * ENTER ANYWHERE (doctrine KIS field):
 *   log เก็บ Δ ล้วน — ไม่มี absolute w แม้แต่ seed · reader ถือ current_w
 *   ของตัวเอง (จุดที่ตัวเองยืน) แล้ว fan24_gear_reconstruct() เดินย้อน
 *   (backward walk) ไปหา append scale เอง → lossless จากทุก entry ใด ๆ
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_tess_scale_log_gear \
 *          tests/test_tess_scale_log_gear.c -lm
 */
#ifndef FAN24_GEAR_H
#define FAN24_GEAR_H

#include <stdint.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────────────── */
#define FG_MAGIC      0x46474E32u   /* "FGN2" FanGear v2 */
#define FG_MAGIC_FULL 0x46474632u   /* "FGF2" FanGear-Full (20736) v2 */
#define FG_VERSION    1u
#define FG_RING       24u           /* teeth: 8 cubes × 3 axes            */
#define FG_WHEEL_KIS  8u            /* KIS cube wheel                     */
#define FG_WHEEL_HYP  3u            /* hyperbolic axis wheel              */
#define FG_LOCAL      144u          /* scale positions W ∈ [0,144)        */

/* ── Gear event ────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t q;   /* Δ / 24        : full turns of the rim     (3 bits local,
                     10 bits full-field — see FG_FULL section below)      */
    uint8_t dc;   /* (Δ%24) % 8    : KIS cube-wheel tooth      (3 bits) */
    uint8_t dx;   /* (Δ%24) % 3    : hyper axis-wheel tooth    (2 bits) */
} FGGearEv;

/* ── Log container (packed wire format) ────────────────────────────────── */
#define FG_LOG_CAP    256u          /* max events                        */

typedef struct {
    uint32_t magic;        /* FG_MAGIC */
    uint32_t version;      /* FG_VERSION */
    uint16_t n;            /* events stored                              */
    uint8_t  rim_mode;     /* 1 = all events are pure-rim (dc==dx==0)    */
    uint8_t  reserved;
} FGLogHeader;             /* 12 bytes                                   */

typedef struct {
    FGLogHeader hdr;
    FGGearEv   ev[FG_LOG_CAP];
} FGLog;

/* ── CRT inverse — closed form ─────────────────────────────────────────── */
/* s ≡ dc (mod 8), s ≡ dx (mod 3):  s = dc + 8·k, k ≡ (dx−dc)·8⁻¹ (mod 3);
 * 8 ≡ 2 (mod 3), 2⁻¹ ≡ 2 (mod 3). Verified against brute force in tests. */
static inline uint8_t fg_crt(uint8_t dc, uint8_t dx) {
    uint8_t k = (uint8_t)(((dx + 3u - (dc % 3u)) % 3u) * 2u % 3u);
    return (uint8_t)(dc + 8u * k);          /* ∈ [0,24) */
}

/* ── Encode/decode one hop ─────────────────────────────────────────────── */
static inline FGGearEv fg_enc(uint32_t from, uint32_t to) {
    uint32_t d = (to + FG_LOCAL - from) % FG_LOCAL;   /* 0..143 */
    FGGearEv e;
    e.q  = (uint8_t)(d / FG_RING);
    e.dc = (uint8_t)((d % FG_RING) % FG_WHEEL_KIS);
    e.dx = (uint8_t)((d % FG_RING) % FG_WHEEL_HYP);
    return e;
}

static inline uint32_t fg_dec(uint32_t from, FGGearEv e) {
    return (from + (uint32_t)e.q * FG_RING + fg_crt(e.dc, e.dx)) % FG_LOCAL;
}

/* ── Wheel positions (each side reads ONLY its own remainder) ──────────── */
static inline uint32_t fg_wheel_kis(uint32_t w)  { return w % FG_WHEEL_KIS; }
static inline uint32_t fg_wheel_hyp(uint32_t w)  { return w % FG_WHEEL_HYP; }

/* ── Log ops ───────────────────────────────────────────────────────────── */
static inline void fg_log_init(FGLog *g) {
    memset(g, 0, sizeof(*g));
    g->hdr.magic   = FG_MAGIC;
    g->hdr.version = FG_VERSION;
}

/* returns 0 ok, -1 overflow, -2 home tooth (Δ==0 emits nothing by design) */
static inline int fg_log_push(FGLog *g, uint32_t from, uint32_t to) {
    if (from % FG_LOCAL == to % FG_LOCAL) return -2;
    if (g->hdr.n >= FG_LOG_CAP) return -1;
    g->ev[g->hdr.n++] = fg_enc(from, to);
    return 0;
}

/* is this log pure-rim? (every Δ multiple of 24 → RIM mode eligible) */
static inline int fg_log_is_rim(const FGLog *g) {
    for (uint16_t i = 0; i < g->hdr.n; i++)
        if (!(g->ev[i].dc == 0 && g->ev[i].dx == 0)) return 0;
    return 1;
}

/* declare mode (encoder side) — refuses RIM when any tooth is off-rim */
static inline int fg_log_set_mode(FGLog *g, uint8_t rim) {
    if (rim && !fg_log_is_rim(g)) return -1;
    g->hdr.rim_mode = rim ? 1u : 0u;
    return 0;
}

/*
 * fg_reconstruct — backward walk from the reader's own position.
 *
 *   cur_w : the scale the READER currently stands at (its own state,
 *           no absolute seed inside the log)
 *   out_w : filled with the chain of scales ending at cur_w:
 *           out_w[0] = append scale … out_w[n] = cur_w
 *   returns number of hops (≥1 if n>0), or 0 on empty log.
 *
 * Lossless at ANY entry point: replaying forward from out_w[0]
 * reproduces every intermediate scale exactly.
 */
static inline uint32_t fg_reconstruct(const FGLog *g, uint32_t cur_w,
                                      uint32_t *out_w, uint32_t cap) {
    if (g->hdr.n == 0 || cap < g->hdr.n + 1u) return 0;
    uint32_t w[FG_LOG_CAP + 1];
    w[g->hdr.n] = cur_w % FG_LOCAL;
    for (uint16_t i = g->hdr.n; i > 0; i--)
        w[i - 1] = (w[i] + FG_LOCAL -
                    ((uint32_t)g->ev[i - 1].q * FG_RING +
                     fg_crt(g->ev[i - 1].dc, g->ev[i - 1].dx)) % FG_LOCAL) % FG_LOCAL;
    memcpy(out_w, w, (g->hdr.n + 1u) * sizeof(uint32_t));
    return g->hdr.n + 1u;
}

/* ════════════════════════════════════════════════════════════════════════
 * FULL FIELD — rim เต็ม 20736 = 144²  (2026-08-26)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Window [0,144) → [0,20736):  W ≡ q·24 + s,   s ∈ [0,24)  (tooth, CRT),
 * q ∈ [0,864)  (full turns of the 24-tooth rim; 20736/24 = 864).
 *
 * โครงเดิมคงเดิมทุกอย่าง — ฟันเฟือง {dc mod 8, dx mod 3} ไม่เปลี่ยน
 * สิ่งเดียวที่ขยายคือ q: 6 บิต → 10 บิต. FREE event = {q:10b|dc:3b|dx:2b}
 * = 15 บิต/event vs baseline {from:14b,to:14b} = 28 บิต → 53.6%.
 * RIM event = {q:10b} = 10 บิต/event (35.7%).
 *
 * ที่มาของ 144²: window เดิม [0,144) = ระนาบ single-frame;
 * 20736 = 144·144 = 18tes field เต็ม (protagonist GEO_COMPOUND_144).
 * W_full = frame·144 + w_local — frame เก่ากลายเป็น q สูงสุด 6 บิต,
 * แถวใหม่อีก 143 frames วางต่อใน q ที่เหลือ (q < 864).
 *
 * HOME tooth Δ=0 ยังถูกปฏิเสธเหมือนเดิม (encoder emits nothing).
 */
#define FG_FULL        20736u       /* full field: 144 × 144              */
#define FG_FULL_TURNS  864u         /* full turns of the rim (20736/24)   */
#define FG_FULL_QBITS  10u          /* q needs 10 bits (864 ≤ 2¹⁰=1024)   */

/* ── Full-field encode/decode (mirror ของ fg_enc/fg_dec บน [0,20736)) ──── */
static inline FGGearEv fgx_enc(uint32_t from, uint32_t to) {
    uint32_t d = (to + FG_FULL - from) % FG_FULL;     /* 0..20735        */
    FGGearEv e;
    e.q  = (uint16_t)(d / FG_RING);                   /* 0..863          */
    e.dc = (uint8_t)((d % FG_RING) % FG_WHEEL_KIS);
    e.dx = (uint8_t)((d % FG_RING) % FG_WHEEL_HYP);
    return e;
}

static inline uint32_t fgx_dec(uint32_t from, FGGearEv e) {
    return (from + (uint32_t)e.q * FG_RING + fg_crt(e.dc, e.dx)) % FG_FULL;
}

/* Full-field log — packed wire: FREE 15b/event (2 B + 7 spare bits),
 * RIM 10b/event. Same header layout as FGLog, magic distinguishes. */
typedef struct {
    FGLogHeader hdr;                  /* magic = FG_MAGIC_FULL           */
    FGGearEv    ev[FG_LOG_CAP];
} FGXLog;

static inline void fgx_log_init(FGXLog *g) {
    memset(g, 0, sizeof(*g));
    g->hdr.magic   = FG_MAGIC_FULL;
    g->hdr.version = FG_VERSION;
}

/* returns 0 ok, -1 overflow, -2 home tooth */
static inline int fgx_log_push(FGXLog *g, uint32_t from, uint32_t to) {
    if (from % FG_FULL == to % FG_FULL) return -2;
    if (g->hdr.n >= FG_LOG_CAP) return -1;
    g->ev[g->hdr.n++] = fgx_enc(from, to);
    return 0;
}

static inline int fgx_log_is_rim(const FGXLog *g) {
    for (uint16_t i = 0; i < g->hdr.n; i++)
        if (!(g->ev[i].dc == 0 && g->ev[i].dx == 0)) return 0;
    return 1;
}

/* Backward walk on the full field — ENTER ANYWHERE unchanged semantics. */
static inline uint32_t fgx_reconstruct(const FGXLog *g, uint32_t cur_w,
                                       uint32_t *out_w, uint32_t cap) {
    if (g->hdr.n == 0 || cap < g->hdr.n + 1u) return 0;
    uint32_t w[FG_LOG_CAP + 1];
    w[g->hdr.n] = cur_w % FG_FULL;
    for (uint16_t i = g->hdr.n; i > 0; i--)
        w[i - 1] = (w[i] + FG_FULL -
                    ((uint32_t)g->ev[i - 1].q * FG_RING +
                     fg_crt(g->ev[i - 1].dc, g->ev[i - 1].dx)) % FG_FULL) % FG_FULL;
    memcpy(out_w, w, (g->hdr.n + 1u) * sizeof(uint32_t));
    return g->hdr.n + 1u;
}

/* ── Bridge: local [0,144) ↔ full [0,20736) ──────────────────────────────
 * W_full = frame·144 + w_local · frame = W_full / 144 · local = W_full % 144.
 * Gear identity is FRAME-INVARIANT: tooth s depends only on D%24, and
 * 144 ≡ 0 (mod 24) → moving a whole frame turns the rim exactly 6 times,
 * teeth unchanged. Proven in test_tess_gear_full X4. */
static inline uint32_t fg_to_full(uint32_t frame, uint32_t w_local) {
    return (frame % (FG_FULL / FG_LOCAL)) * FG_LOCAL + (w_local % FG_LOCAL);
}
static inline uint32_t fg_from_full_frame(uint32_t w_full) {
    return (w_full % FG_FULL) / FG_LOCAL;
}
static inline uint32_t fg_from_full_local(uint32_t w_full) {
    return (w_full % FG_FULL) % FG_LOCAL;
}

#endif /* FAN24_GEAR_H */
