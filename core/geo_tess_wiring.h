/* ═══════════════════════════════════════════════════════════════════════════
 * geo_tess_wiring.h — Wiring: Rescope (Layout) ↔ Fast Access (Physical)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * CONNECTION MAP:
 *
 *   Freebuff Rescope (logical)          Our System (physical)
 *   ─────────────────────────           ─────────────────────────
 *   slot = cube×144 + w                 flat = RDH address
 *   cube 0 = index frame (144)          anchor × 128 + hilbert
 *   cube 1..7 = data (1008)             pointer table[20736]
 *   scale w ∈ [0,144)                   GearLock tick
 *   passive log (hyperbolic)            DRamTile coordinates
 *
 *   These map 1:1 through wiring functions below.
 *
 * SACRED CONSTANTS:
 *   TESS_SLOTS_PER_TESSERACT = 1152 (8 × 144)
 *   TESS_CUBES = 8
 *   TESS_CELLS = 144
 *   TESS_TOTAL = 20736 = 18 × 1152
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_TESS_WIRING_H
#define GEO_TESS_WIRING_H

#include <stdint.h>
#include <string.h>
#include "geo_unified.h"

/* ═══════════════ TESSERACT CONSTANTS ═══════════════ */
#define TESS_CUBES              8u
#define TESS_CELLS              144u
#define TESS_SLOTS_PER_CUBE     TESS_CELLS         /* 144 */
#define TESS_SLOTS_PER_TESS     (TESS_CUBES * TESS_CELLS)  /* 1152 */
#define TESS_N_TESS             18u                 /* 18 tesseracts */
#define TESS_TOTAL              (TESS_N_TESS * TESS_SLOTS_PER_TESS)  /* 20736 */

/* ═══════════════ ADDRESS MAPPING ═══════════════ */

/* Rescope → flat: cube + local → flat address in volume */
static inline uint32_t tess_to_flat(uint32_t tess, uint32_t cube, uint32_t local) {
    uint32_t tess_start = tess * TESS_SLOTS_PER_TESS;
    uint32_t cube_start = cube * TESS_CELLS;
    return (tess_start + cube_start + local) % TESS_TOTAL;
}

/* Flat → rescope: flat address → cube + local */
static inline void flat_to_tess(uint32_t flat, uint32_t *tess, uint32_t *cube, uint32_t *local) {
    flat = flat % TESS_TOTAL;
    *tess = flat / TESS_SLOTS_PER_TESS;
    uint32_t within_tess = flat % TESS_SLOTS_PER_TESS;
    *cube = within_tess / TESS_CELLS;
    *local = within_tess % TESS_CELLS;
}

/* Rescope → scale position: local → w (scale position in [0,144)) */
static inline uint32_t tess_local_to_w(uint32_t local) {
    return local % TESS_CELLS;
}

/* Scale position → flat via seek stride-37: w → scaled flat */
static inline uint32_t tess_seek_flat(uint32_t base_flat, uint32_t w) {
    /* w = scale position; frame_seek uses stride-37 */
    uint32_t scaled = (base_flat + w * 37u) % TESS_TOTAL;
    return scaled;
}

/* ═══════════════ INDEX FRAME ═══════════════ */

/* Index frame layout (cube 0, 144 slots):
 *   slot 0..7:   base[8]   — base address of each cube
 *   slot 8..15:  len[8]    — length of each cube
 *   slot 16..23: stride[8] — stride/checksum per cube
 *   slot 24..143: reserved */
#define TESS_INDEX_BASE_OFF     0u
#define TESS_INDEX_LEN_OFF      8u
#define TESS_INDEX_STRIDE_OFF   16u

/* Index entry size: base(2) + len(2) + stride(1) = 5 bytes per cube */
#define TESS_INDEX_ENTRY_SZ     5u

/* Build index frame for a tesseract */
static inline void tess_build_index(uint8_t *index_frame, uint32_t tess,
                                     const uint8_t **cube_ptrs, const uint32_t *cube_sizes) {
    if (!index_frame) return;
    memset(index_frame, 0, TESS_CELLS * 64);
    
    for (uint32_t c = 0; c < TESS_CUBES; c++) {
        /* base: flat address of cube c data start */
        uint32_t base = tess_to_flat(tess, c, 0);
        uint32_t size = cube_sizes ? cube_sizes[c] : 0;
        
        /* pack base (2 bytes) + len (2 bytes) + stride (1 byte) */
        uint32_t off = c * TESS_INDEX_ENTRY_SZ;
        index_frame[off + 0] = (uint8_t)(base & 0xFF);
        index_frame[off + 1] = (uint8_t)((base >> 8) & 0xFF);
        index_frame[off + 2] = (uint8_t)(size & 0xFF);
        index_frame[off + 3] = (uint8_t)((size >> 8) & 0xFF);
        index_frame[off + 4] = (uint8_t)(37u);  /* stride = 37 (seek constant) */
    }
}

/* Read index frame: get base/len for a cube */
static inline void tess_read_index(const uint8_t *index_frame, uint32_t cube,
                                    uint32_t *base, uint32_t *len, uint32_t *stride) {
    if (!index_frame || cube >= TESS_CUBES) {
        if (base) *base = 0;
        if (len) *len = 0;
        if (stride) *stride = 0;
        return;
    }
    
    uint32_t off = cube * TESS_INDEX_ENTRY_SZ;
    if (base) *base = (uint32_t)index_frame[off + 0] | ((uint32_t)index_frame[off + 1] << 8);
    if (len) *len = (uint32_t)index_frame[off + 2] | ((uint32_t)index_frame[off + 3] << 8);
    if (stride) *stride = index_frame[off + 4];
}

/* ═══════════════ PASSIVE LOG (Hyperbolic Side) ═══════════════ */

/* Passive log entry: scale-change event */
typedef struct {
    uint16_t from_w;    /* scale position before */
    uint16_t to_w;      /* scale position after */
} TessLogEntry;

#define TESS_LOG_MAX 256u

typedef struct {
    TessLogEntry entries[TESS_LOG_MAX];
    uint32_t count;
} TessPassiveLog;

/* Append scale-change event */
static inline void tess_log_append(TessPassiveLog *log, uint16_t from_w, uint16_t to_w) {
    if (!log || log->count >= TESS_LOG_MAX) return;
    log->entries[log->count].from_w = from_w;
    log->entries[log->count].to_w = to_w;
    log->count++;
}

/* Replay log: compute final scale position */
static inline uint32_t tess_log_replay(const TessPassiveLog *log, uint32_t initial_w) {
    if (!log) return initial_w;
    
    uint32_t w = initial_w;
    for (uint32_t i = 0; i < log->count; i++) {
        /* each entry is a hop from → to */
        w = log->entries[i].to_w;
    }
    return w;
}

/* Collapse log: reduce to single entry {initial → final} */
static inline void tess_log_collapse(TessPassiveLog *log, uint32_t initial_w, uint32_t *final_w) {
    if (!log || !final_w) return;
    
    uint32_t w = initial_w;
    for (uint32_t i = 0; i < log->count; i++) {
        w = log->entries[i].to_w;
    }
    *final_w = w;
    
    /* replace all entries with single collapsed entry */
    log->count = 1;
    log->entries[0].from_w = (uint16_t)initial_w;
    log->entries[0].to_w = (uint16_t)w;
}

/* ═══════════════ WIRING: Rescope ↔ Unified ═══════════════ */

/* Read data through unified system using rescope addressing */
static inline int tess_unified_read(GeoUnifiedVolume *v, uint32_t tess, uint32_t cube,
                                     uint32_t local, void *out, uint32_t out_sz) {
    if (!v || !out) return -1;
    
    uint32_t flat = tess_to_flat(tess, cube, local);
    if (flat >= TESS_TOTAL) return -1;
    
    void *ptr = v->slot_ptrs[flat];
    if (!ptr) return -1;
    
    uint32_t to_copy = out_sz < 64 ? out_sz : 64;
    memcpy(out, ptr, to_copy);
    return 0;
}

/* Write data through unified system using rescope addressing */
static inline int tess_unified_write(GeoUnifiedVolume *v, uint32_t tess, uint32_t cube,
                                      uint32_t local, const void *data, uint32_t size) {
    if (!v || !data) return -1;
    
    uint32_t flat = tess_to_flat(tess, cube, local);
    if (flat >= TESS_TOTAL) return -1;
    
    void *ptr = v->slot_ptrs[flat];
    if (!ptr) return -1;
    
    uint32_t to_copy = size < 64 ? size : 64;
    memcpy(ptr, data, to_copy);
    return 0;
}

/* ═══════════════ MAGNIFY GLASS ═══════════════ */

#define TESS_GLASS_CENTER  72u     /* center of window */
#define TESS_GLASS_RADIUS  36u     /* 144 ÷ 4 = 36 */
#define TESS_GLASS_OFFSET  5u      /* small offset */

/* Check if flat address is inside magnify glass */
static inline int tess_in_glass(uint32_t flat) {
    uint32_t w = flat % TESS_CELLS;
    uint32_t lo = (TESS_GLASS_CENTER - TESS_GLASS_RADIUS + TESS_GLASS_OFFSET) % TESS_CELLS;
    uint32_t hi = (TESS_GLASS_CENTER + TESS_GLASS_RADIUS + TESS_GLASS_OFFSET) % TESS_CELLS;
    
    if (lo < hi) {
        return (w >= lo && w < hi);
    } else {
        return (w >= lo || w < hi);
    }
}

/* Get antipodal position (opposite side of glass) */
static inline uint32_t tess_antipode(uint32_t flat) {
    uint32_t tess, cube, local;
    flat_to_tess(flat, &tess, &cube, &local);
    
    uint32_t w = local % TESS_CELLS;
    uint32_t antipodal_w = (w + TESS_GLASS_CENTER) % TESS_CELLS;
    uint32_t antipodal_local = (local / TESS_CELLS) * TESS_CELLS + antipodal_w;
    
    return tess_to_flat(tess, cube, antipodal_local);
}

/* ═══════════════ VERIFICATION ═══════════════ */

/* Verify wiring: rescope ↔ unified */
static inline int tess_verify_wiring(GeoUnifiedVolume *v) {
    if (!v) return 0;
    
    /* verify all 20736 positions map correctly */
    for (uint32_t flat = 0; flat < TESS_TOTAL; flat++) {
        uint32_t tess, cube, local;
        flat_to_tess(flat, &tess, &cube, &local);
        
        /* verify round-trip */
        uint32_t roundtrip = tess_to_flat(tess, cube, local);
        if (roundtrip != flat) return 0;
        
        /* verify pointer exists */
        if (!v->slot_ptrs[flat]) return 0;
    }
    
    return 1;
}

#endif /* GEO_TESS_WIRING_H */
