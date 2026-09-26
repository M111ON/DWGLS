/* geo_vol6_spill.h — disk backend for the vol6 spill/fill hooks.
 * Single spill file: header + N fixed slots {valid,key,tombs,data}.
 * Linear scan (N<=64): spill is rare (evict-only), fill is cold-only.
 */
#ifndef GEO_VOL6_SPILL_H
#define GEO_VOL6_SPILL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "geo_vol6_res.h"

#define V6SPILL_MAGIC  0x5653504Cu   /* "VSPL" */
#define V6SPILL_MAX    64u
#define V6SPILL_SLOT   (1u + 4u + 4u + V6RES_SLOTS)

typedef struct {
    FILE *f;
    char path[256];
    uint32_t nslots;
} V6Spill;

static inline long v6spill_off(uint32_t i) {
    return 8L + (long)i * (long)V6SPILL_SLOT;
}

static inline int v6spill_open(V6Spill *s, const char *path, uint32_t nslots) {
    if (!s || !path || !nslots || nslots > V6SPILL_MAX) return -1;
    memset(s, 0, sizeof(*s));
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->nslots = nslots;
    s->f = fopen(path, "r+b");
    if (s->f) {
        /* validate layout; stale/damaged files are rebuilt, never trusted. */
        uint32_t magic = 0, have = 0;
        if (fread(&magic, 4, 1, s->f) != 1 || fread(&have, 4, 1, s->f) != 1 ||
            magic != V6SPILL_MAGIC || have != nslots) {
            fclose(s->f); s->f = NULL;
            remove(path);
        }
    }
    if (!s->f) {
        s->f = fopen(path, "w+b");
        if (!s->f) return -1;
        uint32_t magic = V6SPILL_MAGIC;
        fwrite(&magic, 4, 1, s->f);
        fwrite(&nslots, 4, 1, s->f);
        /* pre-size: every slot readable (zeros), no sparse holes. */
        fseek(s->f, v6spill_off(nslots - 1) + V6SPILL_SLOT - 1, SEEK_SET);
        uint8_t zero = 0;
        fwrite(&zero, 1, 1, s->f);
        fflush(s->f);
    }
    return 0;
}

static inline void v6spill_close(V6Spill *s) {
    if (s && s->f) { fclose(s->f); s->f = NULL; }
}

static inline int v6spill_slot_read(V6Spill *s, uint32_t i,
                                    uint8_t *valid, uint8_t key[4],
                                    uint8_t *data /*20736 or NULL*/) {
    if (fseek(s->f, v6spill_off(i), SEEK_SET) != 0) return -1;
    if (fread(valid, 1, 1, s->f) != 1) return -1;
    if (fread(key, 1, 4, s->f) != 4) return -1;
    uint32_t tombs = 0;
    if (fread(&tombs, 4, 1, s->f) != 1) return -1;
    if (data && fread(data, 1, V6RES_SLOTS, s->f) != V6RES_SLOTS) return -1;
    return 0;
}

static inline int v6spill_slot_write(V6Spill *s, uint32_t i,
                                     const uint8_t key[4],
                                     const uint8_t *data, uint32_t tombs) {
    if (fseek(s->f, v6spill_off(i), SEEK_SET) != 0) return -1;
    uint8_t valid = 1;
    if (fwrite(&valid, 1, 1, s->f) != 1) return -1;
    if (fwrite(key, 1, 4, s->f) != 4) return -1;
    if (fwrite(&tombs, 4, 1, s->f) != 1) return -1;
    if (fwrite(data, 1, V6RES_SLOTS, s->f) != V6RES_SLOTS) return -1;
    fflush(s->f);
    return 0;
}

/* hook bodies: register s as context via these wrappers' global. */
static V6Spill *v6spill_active = NULL;

static inline int v6spill_spill(const uint8_t key[4],
                                const uint8_t data[V6RES_SLOTS],
                                uint32_t tombs) {
    V6Spill *s = v6spill_active;
    if (!s || !s->f) return 1;
    uint8_t valid, k[4];
    uint32_t free = s->nslots;
    for (uint32_t i = 0; i < s->nslots; i++) {
        if (v6spill_slot_read(s, i, &valid, k, NULL) != 0) return 1;
        if (valid && memcmp(k, key, 4) == 0)
            return v6spill_slot_write(s, i, key, data, tombs);
        if (!valid && free == s->nslots) free = i;
    }
    if (free == s->nslots) return 1;   /* full = veto */
    return v6spill_slot_write(s, free, key, data, tombs);
}

static inline int v6spill_fill(const uint8_t key[4], uint8_t data[V6RES_SLOTS]) {
    V6Spill *s = v6spill_active;
    if (!s || !s->f) return 1;
    uint8_t valid, k[4];
    for (uint32_t i = 0; i < s->nslots; i++) {
        if (v6spill_slot_read(s, i, &valid, k, NULL) != 0) return -1;
        if (valid && memcmp(k, key, 4) == 0)
            return v6spill_slot_read(s, i, &valid, k, data) == 0 ? 0 : -1;
    }
    return 1;
}

#endif /* GEO_VOL6_SPILL_H */
