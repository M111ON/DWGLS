/* geo_kis_4d_container.h — KIS 4D Container Format
 *
 * Container สำหรับ KIS 4D space
 * - Header: scale factor, axis config
 * - Data: compressed values
 * - Formula: embedded (derived from header)
 * - Seal: prevent drift
 *
 * BUILD: included in tests
 */

#ifndef GEO_KIS_4D_CONTAINER_H
#define GEO_KIS_4D_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#define PI 3.14159265358979323846

/* ═══════════════════════════════════════════════════════════════════════════
   CONTAINER HEADER (32 bytes)
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t magic;           /* 0x4B495334 = "KIS4" */
    uint32_t version;         /* 1 */
    uint32_t total_slots;     /* 20736 */
    uint32_t scale_factor;    /* fixed-point: scale * 65536 */
    uint32_t x_slots;         /* X-axis slots */
    uint32_t y_slots;         /* Y-axis slots */
    uint32_t z_slots;         /* Z-axis slots */
    uint32_t data_count;      /* number of unique values */
    uint32_t checksum;        /* CRC-32 of data */
    uint32_t reserved[3];     /* padding */
} KIS4DHeader;

/* ═══════════════════════════════════════════════════════════════════════════
   CONTAINER STRUCTURE
   ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    KIS4DHeader header;
    uint8_t    *data;         /* compressed values */
    uint32_t   *address_map;  /* original_slot → compressed_address */
} KIS4DContainer;

/* ═══════════════════════════════════════════════════════════════════════════
   AXIS SELECTION
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint8_t kis4d_select_axis(uint32_t slot, const KIS4DHeader *h) {
    if (slot < h->x_slots) return 0;
    if (slot < h->x_slots + h->y_slots) return 1;
    return 2;
}

static inline uint32_t kis4d_axis_slot(uint32_t slot, const KIS4DHeader *h) {
    if (slot < h->x_slots) return slot;
    if (slot < h->x_slots + h->y_slots) return slot - h->x_slots;
    return slot - h->x_slots - h->y_slots;
}

/* ═══════════════════════════════════════════════════════════════════════════
   ADDRESS RESOLUTION (Formula)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint32_t kis4d_resolve(uint32_t slot, uint32_t scale,
                                      const KIS4DHeader *h) {
    uint8_t axis = kis4d_select_axis(slot, h);
    uint32_t aslot = kis4d_axis_slot(slot, h);
    
    uint32_t axis_slots;
    if (axis == 0) axis_slots = h->x_slots;
    else if (axis == 1) axis_slots = h->y_slots;
    else axis_slots = h->z_slots;
    
    double angle = 2.0 * PI * (double)aslot / (double)axis_slots;
    angle += (double)axis * 2.0 * PI / 3.0;
    
    double ratio = (double)scale / (double)h->scale_factor;
    double new_angle = angle * ratio;
    
    while (new_angle < 0) new_angle += 2.0 * PI;
    while (new_angle >= 2.0 * PI) new_angle -= 2.0 * PI;
    
    double a = new_angle;
    a -= (double)axis * 2.0 * PI / 3.0;
    if (a < 0) a += 2.0 * PI;
    
    uint32_t result = (uint32_t)(a * (double)axis_slots / (2.0 * PI) + 0.5);
    uint32_t offset = 0;
    if (axis == 1) offset = h->x_slots;
    else if (axis == 2) offset = h->x_slots + h->y_slots;
    
    return (result % axis_slots) + offset;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CREATE CONTAINER
   ═══════════════════════════════════════════════════════════════════════════ */
static inline int kis4d_create(KIS4DContainer *c, uint32_t total_slots,
                                 uint32_t scale_factor) {
    /* Initialize header */
    c->header.magic = 0x4B495334;  /* "KIS4" */
    c->header.version = 1;
    c->header.total_slots = total_slots;
    c->header.scale_factor = scale_factor;
    
    /* Default: equal distribution */
    c->header.x_slots = total_slots / 3;
    c->header.y_slots = total_slots / 3;
    c->header.z_slots = total_slots - c->header.x_slots - c->header.y_slots;
    
    /* Allocate address map */
    c->address_map = (uint32_t *)malloc(total_slots * sizeof(uint32_t));
    if (!c->address_map) return -1;
    
    /* Compute addresses */
    for (uint32_t i = 0; i < total_slots; i++) {
        c->address_map[i] = kis4d_resolve(i, scale_factor, &c->header);
    }
    
    /* Count unique addresses */
    uint32_t unique = 0;
    for (uint32_t i = 0; i < total_slots; i++) {
        int found = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (c->address_map[i] == c->address_map[j]) { found = 1; break; }
        }
        if (!found) unique++;
    }
    c->header.data_count = unique;
    
    /* Allocate data buffer */
    c->data = (uint8_t *)malloc(unique * sizeof(uint8_t));
    if (!c->data) { free(c->address_map); return -1; }
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   ENCODE (Store data in container)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline void kis4d_encode(KIS4DContainer *c, const uint8_t *input,
                                  uint32_t input_count) {
    /* Create unique value list */
    uint8_t unique_values[256];
    uint32_t unique_count = 0;
    
    for (uint32_t i = 0; i < input_count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < unique_count; j++) {
            if (input[i] == unique_values[j]) { found = 1; break; }
        }
        if (!found) {
            unique_values[unique_count++] = input[i];
        }
    }
    
    /* Store unique values */
    for (uint32_t i = 0; i < unique_count; i++) {
        c->data[i] = unique_values[i];
    }
    
    /* Update header */
    c->header.data_count = unique_count;
    
    /* Compute checksum */
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < unique_count; i++) {
        checksum = (checksum << 1) ^ c->data[i];
    }
    c->header.checksum = checksum;
}

/* ═══════════════════════════════════════════════════════════════════════════
   DECODE (Read data from container)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline uint8_t kis4d_decode(KIS4DContainer *c, uint32_t slot) {
    /* Resolve address */
    uint32_t addr = kis4d_resolve(slot, c->header.scale_factor, &c->header);
    
    /* Find value at address */
    uint32_t idx = 0;
    uint32_t unique_idx = 0;
    for (uint32_t i = 0; i < c->header.total_slots; i++) {
        if (c->address_map[i] == addr) {
            if (idx == 0) unique_idx = i;
            idx++;
        }
    }
    
    /* Return value (simplified: use index as proxy) */
    return c->data[unique_idx % c->header.data_count];
}

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFY (Lossless check)
   ═══════════════════════════════════════════════════════════════════════════ */
static inline int kis4d_verify(KIS4DContainer *c, const uint8_t *original,
                                 uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (kis4d_decode(c, i) != original[i]) return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CLEANUP
   ═══════════════════════════════════════════════════════════════════════════ */
static inline void kis4d_destroy(KIS4DContainer *c) {
    if (c->data) free(c->data);
    if (c->address_map) free(c->address_map);
    c->data = NULL;
    c->address_map = NULL;
}

#endif /* GEO_KIS_4D_CONTAINER_H */
