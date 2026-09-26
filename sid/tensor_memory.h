/*
 * tensor_memory.h — ZoneCardSID-indexed tensor store with delta compression
 *
 * On-disk format (.tmem):
 *   [Header 32B]
 *   Record[0..N]:
 *     ZoneCardSID(42B) + compression(1B) + data_size(4B) +
 *     stored_size(4B) + name_len(1B) + name(name_len B) + data(stored_size B)
 *
 * Compression:
 *   TMEM_NONE:  data = raw bytes
 *   TMEM_DELTA: data = RLE delta (zero_run(2B) + non_zero_run(2B) + data(non_zero_run B))*
 *
 * Query: linear scan ZoneCardSID headers (42B each) — no decompress needed.
 *
 * No malloc. Caller provides buffer.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "zone_card_sid.h" /* vendored: flattened from ../zone_card_sid.h */

#define TMEM_MAGIC     0x544D454Du
#define TMEM_VERSION   1u
#define TMEM_NAME_MAX  128u

#define TMEM_NONE      0u
#define TMEM_DELTA     1u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_records;
    uint32_t tick_base;
    uint8_t  pad[16];
} TensorMemHeader;

typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   used;
    uint32_t n_records;
    uint32_t tick_base;
} TensorMemStore;

#define TMEM_FILTER_NODE_ID     (1u << 0)
#define TMEM_FILTER_PENTAGON    (1u << 1)
#define TMEM_FILTER_LAYER       (1u << 2)
#define TMEM_FILTER_CLOCK_TICK  (1u << 3)
#define TMEM_FILTER_ENTROPY     (1u << 4)
#define TMEM_FILTER_CARD_TYPE   (1u << 5)
#define TMEM_FILTER_TICK        (1u << 6)
#define TMEM_FILTER_FLAGS       (1u << 7)

typedef struct {
    uint32_t mask;          /* bitmask of active filters (TMEM_FILTER_*) */
    uint32_t node_id;
    uint8_t  pentagon;
    uint8_t  layer;
    uint16_t clock_tick;
    uint8_t  entropy_min;
    uint8_t  entropy_max;
    uint8_t  card_type;
    uint16_t tick_min;
    uint16_t tick_max;
    uint8_t  flags_and;
} TensorMemQuery;
#pragma pack(pop)

_Static_assert(sizeof(TensorMemHeader) == 32, "TensorMemHeader must be 32B");

/* ── Init ──────────────────────────────────────────────────────── */
static inline void tmem_init(TensorMemStore *s, void *buf, size_t capacity, uint32_t tick_base)
{
    s->buf        = (uint8_t*)buf;
    s->capacity   = capacity;
    s->used       = sizeof(TensorMemHeader);
    s->n_records  = 0;
    s->tick_base  = tick_base;
}

/* ── RLE delta encode: data xor baseline ────────────────── */
static inline uint32_t _tmem_rle_encode(const uint8_t *data, const uint8_t *baseline,
                                         size_t n, uint8_t *out, size_t out_max)
{
    size_t out_pos = 0;
    size_t i = 0;
    while (i < n) {
        uint16_t zr = 0;
        while (i < n && data[i] == baseline[i] && zr < 0xFFFF) { zr++; i++; }
        uint16_t nr = 0;
        size_t nr_start = i;
        while (i < n && data[i] != baseline[i] && nr < 0xFFFF) { nr++; i++; }
        if (out_pos + 4 + nr > out_max) return 0;
        memcpy(out + out_pos, &zr, 2); out_pos += 2;
        memcpy(out + out_pos, &nr, 2); out_pos += 2;
        for (uint32_t j = 0; j < nr; j++)
            out[out_pos++] = (uint8_t)(data[nr_start + j] ^ baseline[nr_start + j]);
        if (i >= n) break;
    }
    return (uint32_t)out_pos;
}

/* ── RLE delta decode ──────────────────────────────────────── */
static inline uint32_t _tmem_rle_decode(const uint8_t *rle, uint32_t rle_size,
                                         const uint8_t *baseline, uint8_t *out, size_t n)
{
    if (!baseline) return 0;
    memcpy(out, baseline, n);
    size_t rp = 0, wp = 0;
    while (rp + 4 <= rle_size && wp < n) {
        uint16_t zr, nr;
        memcpy(&zr, rle + rp, 2); rp += 2;
        memcpy(&nr, rle + rp, 2); rp += 2;
        wp += zr;
        if (wp + nr > n) nr = (uint16_t)(n - wp);
        for (uint16_t j = 0; j < nr && rp < rle_size; j++, wp++)
            out[wp] = (uint8_t)(baseline[wp] ^ rle[rp++]);
    }
    return (uint32_t)wp;
}

/* ── Append record (raw/NONE) ────────────────────────────── */
static inline int tmem_append_raw(TensorMemStore *s,
                                   const ZoneCardSID *zcsid,
                                   const char *name,
                                   const uint8_t *data, size_t data_size)
{
    uint8_t  nl = name ? (uint8_t)strlen(name) : 0;
    if (nl > TMEM_NAME_MAX) nl = TMEM_NAME_MAX;
    size_t rec_size = sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl + data_size;
    if (s->used + rec_size > s->capacity) return -1;
    uint8_t *p = s->buf + s->used;
    memcpy(p, zcsid, sizeof(ZoneCardSID)); p += sizeof(ZoneCardSID);
    *p++ = TMEM_NONE;
    memcpy(p, &data_size, 4); p += 4;
    memcpy(p, &data_size, 4); p += 4;
    *p++ = nl;
    if (nl) { memcpy(p, name, nl); p += nl; }
    memcpy(p, data, data_size);
    s->used += rec_size;
    s->n_records++;
    return 0;
}

/* ── Append record (delta compressed) ───────────────────── */
static inline int tmem_append_delta(TensorMemStore *s,
                                     const ZoneCardSID *zcsid,
                                     const char *name,
                                     const uint8_t *data, size_t data_size,
                                     const uint8_t *baseline)
{
    uint8_t  nl = name ? (uint8_t)strlen(name) : 0;
    if (nl > TMEM_NAME_MAX) nl = TMEM_NAME_MAX;
    size_t max_rle = data_size + 4;
    size_t rec_overhead = sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl;
    if (s->used + rec_overhead + max_rle > s->capacity) return -1;
    uint8_t *rle_buf = s->buf + s->used + rec_overhead;
    uint32_t rle_size = _tmem_rle_encode(data, baseline, data_size, rle_buf, max_rle);
    if (rle_size == 0) return tmem_append_raw(s, zcsid, name, data, data_size);
    uint8_t *p = s->buf + s->used;
    memcpy(p, zcsid, sizeof(ZoneCardSID)); p += sizeof(ZoneCardSID);
    *p++ = TMEM_DELTA;
    memcpy(p, &data_size, 4); p += 4;
    memcpy(p, &rle_size, 4); p += 4;
    *p++ = nl;
    if (nl) { memcpy(p, name, nl); p += nl; }
    s->used = (size_t)(p - s->buf);
    s->used += rle_size;
    s->n_records++;
    return 0;
}

/* ── Read record (decompress into caller buffer) ──────────── */
static inline int tmem_read_record(const TensorMemStore *s, uint32_t idx,
                                    uint8_t *data_out, size_t out_max,
                                    ZoneCardSID *zcsid_out,
                                    const uint8_t *baseline)
{
    if (idx >= s->n_records) return -1;
    uint8_t *p = s->buf + sizeof(TensorMemHeader);
    for (uint32_t i = 0; i < idx; i++) {
        if ((size_t)(p - s->buf) >= s->used) return -1;
        uint8_t comp = *(p + sizeof(ZoneCardSID));
        uint32_t dsize, ssize;
        memcpy(&dsize, p + sizeof(ZoneCardSID) + 1, 4);
        memcpy(&ssize, p + sizeof(ZoneCardSID) + 1 + 4, 4);
        uint8_t  nl = *(p + sizeof(ZoneCardSID) + 1 + 4 + 4);
        size_t skip = sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl + ssize;
        p += skip;
    }
    if (zcsid_out) memcpy(zcsid_out, p, sizeof(ZoneCardSID));
    uint8_t  comp  = *(p + sizeof(ZoneCardSID));
    uint32_t dsize, ssize;
    memcpy(&dsize, p + sizeof(ZoneCardSID) + 1, 4);
    memcpy(&ssize, p + sizeof(ZoneCardSID) + 1 + 4, 4);
    uint8_t  nl    = *(p + sizeof(ZoneCardSID) + 1 + 4 + 4);
    uint8_t *cdata = p + sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl;
    if (data_out && dsize <= out_max) {
        if (comp == TMEM_NONE) {
            memcpy(data_out, cdata, dsize);
        } else if (comp == TMEM_DELTA) {
            if (!baseline) return -1;
            _tmem_rle_decode(cdata, ssize, baseline, data_out, dsize);
        } else {
            return -1;
        }
    }
    return (int)dsize;
}

/* ── Get record ZoneCardSID without decompressing data ───── */
static inline const ZoneCardSID* tmem_record_zcsid(const TensorMemStore *s, uint32_t idx)
{
    if (idx >= s->n_records) return NULL;
    uint8_t *p = s->buf + sizeof(TensorMemHeader);
    for (uint32_t i = 0; i < idx; i++) {
        if ((size_t)(p - s->buf) >= s->used) return NULL;
        uint32_t dsize, ssize;
        memcpy(&dsize, p + sizeof(ZoneCardSID) + 1, 4);
        memcpy(&ssize, p + sizeof(ZoneCardSID) + 1 + 4, 4);
        uint8_t nl = *(p + sizeof(ZoneCardSID) + 1 + 4 + 4);
        p += sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl + ssize;
    }
    return (const ZoneCardSID*)p;
}

/* ── Get record name ────────────────────────────────────── */
static inline const char* tmem_record_name(const TensorMemStore *s, uint32_t idx)
{
    if (idx >= s->n_records) return NULL;
    uint8_t *p = s->buf + sizeof(TensorMemHeader);
    for (uint32_t i = 0; i < idx; i++) {
        if ((size_t)(p - s->buf) >= s->used) return NULL;
        uint32_t dsize, ssize;
        memcpy(&dsize, p + sizeof(ZoneCardSID) + 1, 4);
        memcpy(&ssize, p + sizeof(ZoneCardSID) + 1 + 4, 4);
        uint8_t nl = *(p + sizeof(ZoneCardSID) + 1 + 4 + 4);
        p += sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + nl + ssize;
    }
    uint8_t nl = *(p + sizeof(ZoneCardSID) + 1 + 4 + 4);
    if (nl == 0) return NULL;
    uint8_t *name_p = p + sizeof(ZoneCardSID) + 1 + 4 + 4 + 1;
    static char _name_buf[TMEM_NAME_MAX + 1];
    memcpy(_name_buf, name_p, nl);
    _name_buf[nl] = '\0';
    return _name_buf;
}

/* ── Query: scan headers, return matches ──────────────────── */
static inline int tmem_query(const TensorMemStore *s, const TensorMemQuery *q,
                              uint32_t *out_indices, uint32_t max_out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->n_records && n < max_out; i++) {
        const ZoneCardSID *z = tmem_record_zcsid(s, i);
        if (!z) break;
        if ((q->mask & TMEM_FILTER_NODE_ID) && z->node_id != q->node_id) continue;
        if ((q->mask & TMEM_FILTER_PENTAGON) && geo_pentagon_id(z->node_id) != q->pentagon) continue;
        if ((q->mask & TMEM_FILTER_LAYER) && z->inf.layer != q->layer) continue;
        if ((q->mask & TMEM_FILTER_CLOCK_TICK) && z->inf.clock_tick != q->clock_tick) continue;
        if ((q->mask & TMEM_FILTER_ENTROPY) &&
            (z->inf.logit_entropy < q->entropy_min || z->inf.logit_entropy > q->entropy_max)) continue;
        if ((q->mask & TMEM_FILTER_CARD_TYPE) && z->card.card_type != q->card_type) continue;
        if ((q->mask & TMEM_FILTER_TICK) &&
            (z->inf.tick < q->tick_min || z->inf.tick > q->tick_max)) continue;
        if ((q->mask & TMEM_FILTER_FLAGS) && (z->inf.flags & q->flags_and) == 0) continue;
        out_indices[n++] = i;
    }
    return (int)n;
}

/* ── Iterate all records (callback returns 0 to continue) ── */
static inline int tmem_foreach(const TensorMemStore *s,
                                int (*cb)(const ZoneCardSID *zcsid,
                                          const char *name,
                                          uint32_t idx,
                                          void *ctx),
                                void *ctx)
{
    for (uint32_t i = 0; i < s->n_records; i++) {
        const ZoneCardSID *z = tmem_record_zcsid(s, i);
        if (!z) break;
        const char *name = tmem_record_name(s, i);
        int r = cb(z, name, i, ctx);
        if (r != 0) return r;
    }
    return 0;
}

/* ── Save to file ────────────────────────────────────────── */
static inline int tmem_save(const TensorMemStore *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    TensorMemHeader hdr;
    hdr.magic     = TMEM_MAGIC;
    hdr.version   = TMEM_VERSION;
    hdr.n_records = s->n_records;
    hdr.tick_base = s->tick_base;
    memset(hdr.pad, 0, 16);
    fwrite(&hdr, sizeof(hdr), 1, f);
    if (s->used > sizeof(hdr))
        fwrite(s->buf + sizeof(TensorMemHeader), s->used - sizeof(TensorMemHeader), 1, f);
    fclose(f);
    return 0;
}

/* ── Load from file (reads entire file into caller buffer) ─ */
static inline int tmem_load(TensorMemStore *s, void *buf, size_t buf_size, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if ((size_t)fsize > buf_size) { fclose(f); return -2; }
    fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    TensorMemHeader *hdr = (TensorMemHeader*)buf;
    if (hdr->magic != TMEM_MAGIC || hdr->version != TMEM_VERSION) return -3;
    s->buf       = (uint8_t*)buf;
    s->capacity  = buf_size;
    s->used      = (size_t)fsize;
    s->n_records = hdr->n_records;
    s->tick_base = hdr->tick_base;
    return 0;
}

/* ── Estimate capacity: how many records can fit for given avg data size ─ */
static inline uint32_t tmem_capacity(size_t buf_size, size_t avg_data_size, uint8_t name_len)
{
    size_t rec = sizeof(ZoneCardSID) + 1 + 4 + 4 + 1 + name_len + avg_data_size;
    if (rec == 0 || buf_size <= sizeof(TensorMemHeader)) return 0;
    return (uint32_t)((buf_size - sizeof(TensorMemHeader)) / rec);
}
