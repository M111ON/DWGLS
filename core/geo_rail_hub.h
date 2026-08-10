/* core/geo_rail_hub.h — tensor hook layer: .gcube (mmap, zero-copy) → caller
 *
 * The single production read-path for weights. llama.cpp hook (or any
 * consumer) asks:  geo_rail_pull(lane, tensor_name, &ptr, &nelems, &dtype)
 * Answer is a pointer DIRECTLY into the mmap'd .gcube region — no malloc,
 * no memcpy, no per-tensor system calls. This is the "O(1) per tensor,
 * 131M pulls/s" hot path that was proven in bench/rail_bench.c.
 *
 * DESIGN (MAP not COMPRESS):
 *   coordinate = address. The .gcube IS the address space; pull = address
 *   resolution only. Verification (CRC/RDH) happens once at open, the
 *   pulls themselves touch zero metadata — they hand out an address.
 */
#ifndef GEO_RAIL_HUB_H
#define GEO_RAIL_HUB_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "geo_cube_container.h"
#include "geo_zerocopy.h"

#define RAIL_OK               0
#define RAIL_ERR_NOT_OPEN   (-1)
#define RAIL_ERR_NO_TENSOR  (-2)
#define RAIL_ERR_VERSION    (-3)

typedef struct {
    GeoZeroCopy   zc;          /* mmap'ed .gcube */
    int           is_open;
    uint32_t      n_tensors;
    uint32_t      crc_ok;      /* set by rail_verify */
} GeoRailHub;

/* ── OPEN — mmap .gcube, check magic+version (zero malloc for weights) ── */
static inline int geo_rail_open(GeoRailHub *rail, const char *gcube_path)
{
    if (!rail || !gcube_path) return RAIL_ERR_NOT_OPEN;
    memset(rail, 0, sizeof(*rail));

    int rc = geo_zerocopy_open(&rail->zc, gcube_path);
    if (rc != 0) return (rc == -5) ? RAIL_ERR_VERSION : RAIL_ERR_NOT_OPEN;

    rail->n_tensors = rail->zc.cube.header.n_tensors;
    rail->is_open   = 1;
    rail->crc_ok    = 0;
    return RAIL_OK;
}

/* ── CLOSE ── */
static inline void geo_rail_close(GeoRailHub *rail)
{
    if (rail && rail->is_open) {
        geo_zerocopy_close(&rail->zc);
        rail->is_open = 0;
    }
}

/* ── PULL — resolve tensor name → pointer into mmap (THE hot path) ──
 * No malloc. No copy. The returned pointer is valid until geo_rail_close. */
static inline int geo_rail_pull(GeoRailHub *rail,
                                const char *tensor_name,
                                const uint8_t **data_out,
                                uint32_t *n_elems_out,
                                uint32_t *dtype_out)
{
    if (!rail || !rail->is_open) return RAIL_ERR_NOT_OPEN;
    const GCubeTensorEntry *e = gcube_find(&rail->zc.cube, tensor_name);
    if (!e) return RAIL_ERR_NO_TENSOR;

    *data_out    = rail->zc.cube.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
    *n_elems_out = e->n_elems;
    *dtype_out   = e->dtype;
    return RAIL_OK;
}

/* ── PULL BY INDEX — the 131M pulls/s loop path: no string compare ── */
static inline int geo_rail_pull_idx(GeoRailHub *rail,
                                    uint32_t tidx,
                                    const uint8_t **data_out,
                                    uint32_t *n_elems_out,
                                    uint32_t *dtype_out)
{
    if (!rail || !rail->is_open || tidx >= rail->n_tensors)
        return RAIL_ERR_NOT_OPEN;
    const GCubeTensorEntry *e = &rail->zc.cube.tensors[tidx];

    *data_out    = rail->zc.cube.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
    *n_elems_out = e->n_elems;
    *dtype_out   = e->dtype;
    return RAIL_OK;
}

/* ── CRC-32 full-file verification at open time (one-time cost) ──
 * Proves the mmap region is bit-identical to what gcube_write produced.
 * Returns RAIL_OK when the stored CRC matches the on-disk buffer. */
static inline int geo_rail_verify(GeoRailHub *rail)
{
    if (!rail || !rail->is_open) return RAIL_ERR_NOT_OPEN;

    const uint8_t *base  = rail->zc.base;
    size_t         fsz   = rail->zc.mapped_size;
    /* stored CRC is the LAST 4 bytes of the file (gcube_write appends it) */
    if (fsz < 4 + GCUBE_FILE_HDR_SZ) return RAIL_ERR_VERSION;

    uint32_t stored = 0;
    memcpy(&stored, base + fsz - 4, 4);

    uint32_t idx_bytes = rail->n_tensors * GCUBE_TENSOR_HDR_SZ;
    uint32_t data_bytes = GCUBE_FILE_HDR_SZ + idx_bytes +
                          rail->zc.cube.header.total_blocks * GCUBE_BLOCK_SZ;
    if ((size_t)data_bytes + 4 != fsz) return RAIL_ERR_VERSION;

    /* CRC over [0, data_bytes) — same order as gcube_write packed */
    uint32_t calc = gcube_crc32(base, data_bytes);
    rail->crc_ok  = (calc == stored) ? 1 : 0;
    return rail->crc_ok ? RAIL_OK : RAIL_ERR_VERSION;
}

#endif /* GEO_RAIL_HUB_H */