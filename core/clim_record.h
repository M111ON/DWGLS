/* clim_record.h — CLIM climate-offset record (window slide bookkeeping).
 *
 * Climate change = the 20736 window slides over content (capo slide:
 * [k,k+20736)). Payload bytes NEVER move (RELABEL); only this record
 * mutates. Resume = verify cksum → apply offset → replay.
 *
 * Scale rule (CONSTRAINTS #6121): ONE shared scale — W on the KIS ring
 * [0,144), same as breathing_fs/scale_bridge. CLIM carries spatial slide
 * (offset k, u64 unbounded) + W. No second scale, ever.
 *
 * File layout: TESS_SectionHdr{type='CLIM', size=32} + ClimRec (32B).
 * Header-only, std-only. No separate build.
 */
#ifndef CLIM_RECORD_H
#define CLIM_RECORD_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLIM_MAGIC    0x4D494C43u /* "CLIM" */
#define CLIM_VERSION  1u
#define CLIM_SIZE     32u
#define CLIM_FLAG_SLIDING 0x01u
#define CLIM_W_MAX    144u        /* shared KIS-ring scale, cf. SBR_RING */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      /* CLIM_MAGIC */
    uint16_t ver;        /* CLIM_VERSION */
    uint16_t flags;      /* bit0 = sliding active */
    uint64_t offset;     /* window base k: window = [k, k+20736) */
    uint16_t w;          /* shared scale W in [0,144) */
    uint16_t _rsv;       /* reserved, zero */
    uint32_t head;       /* walk start (cf. GJCIndex DNA) */
    uint32_t router;     /* type/param pack */
    uint32_t cksum;      /* FNV-1a over the 28 bytes above */
} ClimRec;               /* 32 bytes */
#pragma pack(pop)

static inline uint32_t clim_fnv(const ClimRec *r) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)r;
    for (uint32_t i = 0; i < 28; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* init at home (offset 0, given W). Returns 0=ok, -1=bad W. */
static inline int clim_init(ClimRec *r, uint16_t w) {
    if (!r || w >= CLIM_W_MAX) return -1;
    memset(r, 0, sizeof(*r));
    r->magic = CLIM_MAGIC;
    r->ver = CLIM_VERSION;
    r->w = w;
    r->cksum = clim_fnv(r);
    return 0;
}

/* slide: offset += delta, set sliding flag, reseal. Always ok. */
static inline void clim_slide(ClimRec *r, uint64_t delta) {
    if (!r) return;
    r->offset += delta;
    if (delta) r->flags |= CLIM_FLAG_SLIDING;
    r->cksum = clim_fnv(r);
}

/* verify: 0=ok, -1=bad magic/ver, -2=corrupt (cksum), -3=bad W. */
static inline int clim_verify(const ClimRec *r) {
    if (!r || r->magic != CLIM_MAGIC || r->ver != CLIM_VERSION) return -1;
    if (r->w >= CLIM_W_MAX) return -3;
    if (r->cksum != clim_fnv(r)) return -2;
    return 0;
}

/* logical position of stored slot s: p = s + offset (u64, unbounded). */
static inline uint64_t clim_apply(const ClimRec *r, uint32_t slot) {
    return r->offset + slot;
}

/* inverse: slot of logical position p. 0=ok, -1=outside window. */
static inline int clim_invert(const ClimRec *r, uint64_t pos, uint32_t *slot_out) {
    if (!r || pos < r->offset || pos - r->offset >= 20736u) return -1;
    if (slot_out) *slot_out = (uint32_t)(pos - r->offset);
    return 0;
}

/* persist with section header. 0=ok, -1=io. */
static inline int clim_save(const char *path, const ClimRec *r) {
    if (!path || !r) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t hdr[2] = { CLIM_MAGIC, CLIM_SIZE };
    int ok = fwrite(hdr, 8, 1, f) == 1 && fwrite(r, CLIM_SIZE, 1, f) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

/* load + verify. 0=ok, -1=io/shape, else clim_verify code. */
static inline int clim_load(const char *path, ClimRec *r) {
    if (!path || !r) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t hdr[2] = { 0, 0 };
    int rc = -1;
    if (fread(hdr, 8, 1, f) == 1 && hdr[0] == CLIM_MAGIC && hdr[1] == CLIM_SIZE &&
        fread(r, CLIM_SIZE, 1, f) == 1)
        rc = clim_verify(r);
    fclose(f);
    return rc;
}

#endif /* CLIM_RECORD_H */
