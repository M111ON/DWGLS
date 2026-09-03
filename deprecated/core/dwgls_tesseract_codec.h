/*
 * dwgls_tesseract_codec.h — Tesseract × Dynamic Codec Integration
 * ════════════════════════════════════════════════════════════════════
 *
 * CONNECTION: Tesseract = WHERE (address), Dynamic Codec = HOW (encode)
 *
 * 20736 = 144 cubes × 144 slots = 18 tesseracts × 8 cubes
 *
 * Each cube independently chooses its encoding strategy:
 *   - Empty cube → SPARSE (just count zeros)
 *   - Repeated cube → CODEBOOK (histogram)
 *   - Random cube → RAW (no compression)
 *
 * Tesseract provides:
 *   - Cube addressing: which tess, which cube within tess
 *   - Z-axis bridge: cross-tesseract access via mirror_z
 *   - 12³ internal structure: each cube = 12×12×12
 *
 * Container format:
 *   [GlobalHeader 32B][CubeEntry × 144][CRC32 4B]
 *   CubeEntry = [strategy:1][payload_size:2][payload:variable]
 *
 * PRINCIPLE: MAP not COMPRESS — coordinate = address
 * SACRED: 20736, 1728, 144, 12, 18
 *
 * BUILD: gcc -O2 -Wall -Icore -o test_tess_codec tests/test_tess_codec.c -lm
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_TESSERACT_CODEC_H
#define DWGLS_TESSERACT_CODEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "dwgls_dynamic_codec.h"

/* ════════════════════════════════════════════════════════════════
   CONSTANTS — 6ico compound structure
   ════════════════════════════════════════════════════════════════ */

#define TCODEC_MAGIC        0x54434F44u  /* "TCOD" */
#define TCODEC_VERSION      1u
#define TCODEC_TOTAL_SLOTS  20736u
#define TCODEC_CUBES        144u
#define TCODEC_SLOTS_CUBE   144u        /* 12³ per cube */
#define TCODEC_TESSERACTS   18u
#define TCODEC_CUBES_TESS   8u          /* 144 / 18 */
#define TCODEC_CUBE_DIM     12u         /* 12³ internal structure */
#define TCODEC_HEADER_SZ    32u
#define TCODEC_CRC_POLY     0xEDB88320u

/* ════════════════════════════════════════════════════════════════
   GLOBAL HEADER (32 bytes)
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;           /* TCODEC_MAGIC                      */
    uint32_t version;         /* TCODEC_VERSION                    */
    uint32_t n_cubes;         /* 144                               */
    uint32_t total_slots;     /* 20736                             */
    uint32_t total_payload;   /* bytes of cube payloads combined   */
    uint32_t n_sparse;        /* count of cubes using SPARSE       */
    uint32_t n_codebook;      /* count of cubes using CODEBOOK     */
    uint32_t n_raw;           /* count of cubes using RAW          */
} TCodecHeader;

/* ════════════════════════════════════════════════════════════════
   PER-CUBE ENTRY
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  strategy;        /* DYN_STRAT_*                       */
    uint16_t payload_size;    /* bytes of encoded cube data        */
    uint8_t  payload[512];    /* encoded cube (max ~384B for 144 slots) */
} TCodecCubeEntry;

/* ════════════════════════════════════════════════════════════════
   FULL CONTAINER
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    TCodecHeader    header;
    TCodecCubeEntry cubes[TCODEC_CUBES];
    uint32_t        checksum;    /* CRC32 of all cube payloads */
} TCodecContainer;

/* ════════════════════════════════════════════════════════════════
   TESSERACT ADDRESSING
   ════════════════════════════════════════════════════════════════ */

/* Which tesseract does cube belong to? */
static inline uint32_t tcodec_cube_to_tess(uint32_t cube_idx) {
    return cube_idx / TCODEC_CUBES_TESS;
}

/* Base cube of a tesseract */
static inline uint32_t tcodec_tess_to_base(uint32_t tess_idx) {
    return tess_idx * TCODEC_CUBES_TESS;
}

/* Slot → (cube, slot_in_cube) */
static inline void tcodec_slot_to_cube(uint32_t slot, uint32_t *cube, uint32_t *s) {
    *cube = slot / TCODEC_SLOTS_CUBE;
    *s = slot % TCODEC_SLOTS_CUBE;
}

/* (cube, slot_in_cube) → slot */
static inline uint32_t tcodec_cube_to_slot(uint32_t cube, uint32_t s) {
    return cube * TCODEC_SLOTS_CUBE + s;
}

/* Internal cube address: (x, y, z) within 12³ → slot_in_cube */
static inline uint32_t tcodec_xyz_to_local(uint32_t x, uint32_t y, uint32_t z) {
    return x + y * TCODEC_CUBE_DIM + z * TCODEC_CUBE_DIM * TCODEC_CUBE_DIM;
}

/* slot_in_cube → (x, y, z) within 12³ */
static inline void tcodec_local_to_xyz(uint32_t s, uint32_t *x, uint32_t *y, uint32_t *z) {
    *x = s % TCODEC_CUBE_DIM;
    *y = (s / TCODEC_CUBE_DIM) % TCODEC_CUBE_DIM;
    *z = s / (TCODEC_CUBE_DIM * TCODEC_CUBE_DIM);
}

/* ════════════════════════════════════════════════════════════════
   Z-AXIS BRIDGE — cross-tesseract access
   ════════════════════════════════════════════════════════════════ */

/* Mirror Z within cube: z → (11-z)
 * In 6ico compound, this CROSSES tesseract boundaries.
 * cube 0 → cube 11 → cube 22 → cube 33 ... */
static inline uint32_t tcodec_mirror_z(uint32_t slot) {
    uint32_t cube, s;
    tcodec_slot_to_cube(slot, &cube, &s);
    uint32_t x, y, z;
    tcodec_local_to_xyz(s, &x, &y, &z);
    uint32_t z_mirror = (TCODEC_CUBE_DIM - 1) - z;
    return tcodec_cube_to_slot(cube, tcodec_xyz_to_local(x, y, z_mirror));
}

/* Bridge: access slot in adjacent tesseract via Z-mirror */
static inline uint32_t tcodec_bridge_z(uint32_t slot, uint32_t *dst_tess) {
    uint32_t mirrored = tcodec_mirror_z(slot);
    uint32_t cube;
    uint32_t dummy_s; tcodec_slot_to_cube(mirrored, &cube, &dummy_s);
    *dst_tess = tcodec_cube_to_tess(cube);
    return mirrored;
}

/* ════════════════════════════════════════════════════════════════
   CRC32
   ════════════════════════════════════════════════════════════════ */

static inline uint32_t tcodec_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (TCODEC_CRC_POLY & (-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ════════════════════════════════════════════════════════════════
   PER-CUBE CLASSIFY + ENCODE
   ════════════════════════════════════════════════════════════════ */

/* Classify one cube (144 int8_t values) and fill entry */
static inline int tcodec_encode_cube(TCodecCubeEntry *entry,
                                      const int8_t *cube_data, uint32_t n)
{
    if (!entry || !cube_data || n == 0) return -1;

    DynProfile prof;
    dyn_classify(cube_data, n, &prof);

    entry->strategy = prof.strategy;

    /* Encode using the selected strategy */
    uint32_t written = 0;
    switch (prof.strategy) {
        case DYN_STRAT_SPARSE:
            written = dyn_encode_sparse(cube_data, n, entry->payload, sizeof(entry->payload));
            break;
        case DYN_STRAT_CODEBOOK:
            written = dyn_encode_codebook(cube_data, n, entry->payload, sizeof(entry->payload));
            break;
        case DYN_STRAT_DELTA:
            written = dyn_encode_delta(cube_data, n, entry->payload, sizeof(entry->payload));
            break;
        case DYN_STRAT_BITPACK:
            written = dyn_encode_bitpack(cube_data, n, entry->payload, sizeof(entry->payload));
            break;
        default:
            written = dyn_encode_raw(cube_data, n, entry->payload, sizeof(entry->payload));
            entry->strategy = DYN_STRAT_RAW;
            break;
    }

    entry->payload_size = (uint16_t)written;
    /* Fallback: if chosen strategy failed, try RAW */
    if (written == 0 && entry->strategy != DYN_STRAT_RAW) {
        written = dyn_encode_raw(cube_data, n, entry->payload, sizeof(entry->payload));
        entry->strategy = DYN_STRAT_RAW;
        entry->payload_size = (uint16_t)written;
    }
    return (written > 0) ? 0 : -2;
}

/* Decode one cube */
static inline int tcodec_decode_cube(const TCodecCubeEntry *entry,
                                      int8_t *cube_data, uint32_t n)
{
    if (!entry || !cube_data || n == 0) return -1;

    switch (entry->strategy) {
        case DYN_STRAT_RAW:
            return dyn_decode_raw(entry->payload, entry->payload_size, cube_data, n);
        case DYN_STRAT_SPARSE:
            return dyn_decode_sparse(entry->payload, entry->payload_size, cube_data, n);
        case DYN_STRAT_CODEBOOK:
            return dyn_decode_codebook(entry->payload, entry->payload_size, cube_data, n);
        case DYN_STRAT_DELTA:
            return dyn_decode_delta(entry->payload, entry->payload_size, cube_data, n);
        case DYN_STRAT_BITPACK:
            return dyn_decode_bitpack(entry->payload, entry->payload_size, cube_data, n);
        default:
            return -3;
    }
}

/* ════════════════════════════════════════════════════════════════
   FULL ENCODE: 20736 slots → 144 cubes → container
   ════════════════════════════════════════════════════════════════ */

static inline void tcodec_init(TCodecContainer *tc) {
    if (!tc) return;
    memset(tc, 0, sizeof(*tc));
    tc->header.magic = TCODEC_MAGIC;
    tc->header.version = TCODEC_VERSION;
    tc->header.n_cubes = TCODEC_CUBES;
    tc->header.total_slots = TCODEC_TOTAL_SLOTS;
}

static inline int tcodec_encode(TCodecContainer *tc,
                                 const int8_t *data, uint32_t n)
{
    if (!tc || !data || n != TCODEC_TOTAL_SLOTS) return -1;

    tcodec_init(tc);
    tc->header.total_payload = 0;
    tc->header.n_sparse = 0;
    tc->header.n_codebook = 0;
    tc->header.n_raw = 0;

    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        uint32_t offset = c * TCODEC_SLOTS_CUBE;
        int rc = tcodec_encode_cube(&tc->cubes[c], data + offset, TCODEC_SLOTS_CUBE);
        if (rc != 0) return -2;

        tc->header.total_payload += tc->cubes[c].payload_size;

        /* Count strategies */
        switch (tc->cubes[c].strategy) {
            case DYN_STRAT_SPARSE:   tc->header.n_sparse++;   break;
            case DYN_STRAT_CODEBOOK: tc->header.n_codebook++; break;
            default:                 tc->header.n_raw++;      break;
        }
    }

    /* CRC over all cube payloads */
    uint32_t total_crc = 0;
    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        total_crc ^= tcodec_crc32(tc->cubes[c].payload, tc->cubes[c].payload_size);
    }
    tc->checksum = total_crc;

    return 0;
}

/* ════════════════════════════════════════════════════════════════
   FULL DECODE: container → 20736 slots
   ════════════════════════════════════════════════════════════════ */

static inline int tcodec_decode(const TCodecContainer *tc,
                                 int8_t *data, uint32_t n)
{
    if (!tc || !data || n != TCODEC_TOTAL_SLOTS) return -1;
    if (tc->header.magic != TCODEC_MAGIC) return -3;

    /* Verify CRC */
    uint32_t total_crc = 0;
    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        total_crc ^= tcodec_crc32(tc->cubes[c].payload, tc->cubes[c].payload_size);
    }
    if (total_crc != tc->checksum) return -4;

    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        uint32_t offset = c * TCODEC_SLOTS_CUBE;
        int rc = tcodec_decode_cube(&tc->cubes[c], data + offset, TCODEC_SLOTS_CUBE);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   VERIFY: roundtrip check
   ════════════════════════════════════════════════════════════════ */

static inline int tcodec_verify(const int8_t *original, uint32_t n,
                                 const TCodecContainer *tc)
{
    if (!original || !tc || n != TCODEC_TOTAL_SLOTS) return -1;

    int8_t *recon = (int8_t *)malloc(n);
    if (!recon) return -1;

    int rc = tcodec_decode(tc, recon, n);
    if (rc != 0) { free(recon); return rc; }

    int match = (memcmp(original, recon, n) == 0);
    free(recon);
    return match ? 0 : -5;
}

/* ════════════════════════════════════════════════════════════════
   UTILITY
   ════════════════════════════════════════════════════════════════ */

static inline float tcodec_ratio(const TCodecContainer *tc) {
    if (!tc) return 1.0f;
    uint32_t total = TCODEC_HEADER_SZ + tc->header.total_payload
                   + TCODEC_CUBES * 3 + 4;  /* per-cube overhead + CRC */
    return (float)total / (float)TCODEC_TOTAL_SLOTS;
}

static inline void tcodec_print_stats(const TCodecContainer *tc) {
    if (!tc) return;
    printf("  Cubes: %u | Sparse: %u | Codebook: %u | Raw: %u\n",
           tc->header.n_cubes, tc->header.n_sparse,
           tc->header.n_codebook, tc->header.n_raw);
    printf("  Total payload: %u bytes | Ratio: %.4f\n",
           tc->header.total_payload, tcodec_ratio(tc));
}

/* Per-cube strategy distribution */
static inline void tcodec_print_cube_strategies(const TCodecContainer *tc) {
    if (!tc) return;
    for (uint32_t c = 0; c < TCODEC_CUBES; c++) {
        if (c % 24 == 0) printf("\n  ");
        printf("%c", ".SCLRBitP"[tc->cubes[c].strategy]);
    }
    printf("\n  ( .=RAW S=SPARSE C=CODEBOOK L=DELTA R=unused B=BITPACK)\n");
}

#endif /* DWGLS_TESSERACT_CODEC_H */
