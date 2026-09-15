/* geo_jump_container.h — geo_jump AS the container, placed on BreathingFS.
 *
 * STRUCTURAL IDENTITY (no translation, no copy of geometry):
 *   GEO_TOWER(144)  = BFS_SLOTS_BLOCK(144)  → tower offset = block slot
 *   GEO_FULL(20736) = BFS_TOTAL_SLOTS(20736) → node = flat position
 *   144 towers      = BFS_BLOCKS(144)        → tower = block
 *   GeoDna(head+router) = index (persisted as "tag/dna")
 *   GeoJumpRouter = the key: same (head,router) re-walks f(step) → gather
 *   inverts scatter. Payload bytes are NEVER transformed (waveform lesson:
 *   value-domain codecs die on odd halves; a permutation is exact always).
 *   ROLE LAW (proven tests/test_geo_jump_roles.c): HILBERT/PEANO/GROUND/
 *   PENTAGON are WALLS (idempotent projections 20736/20736 — never placement);
 *   only bijective jumps (MOD coprime) place. Pure 4^a3^b cuts only.
 *
 * LAYOUT in fs (2 files): "tag/dna" (28B GJCIndex) + "tag/dat" (n bytes in
 * node-sorted order). New file beside old; breathing_fs.h untouched.
 *
 * BUILD: gcc -O2 -Wall -Icore tests/test_gjc_place.c -o /tmp/gjc_place
 */
#ifndef GEO_JUMP_CONTAINER_H
#define GEO_JUMP_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "breathing_fs.h"

#define GEO_JUMP_INLINE
#include "../../FGLS_new/collection/geo_jump_module/include/geo_jump.h"

#define GJC_MAGIC  0x474A4331u  /* "GJC1" */
#define GJC_TOWERS 144u
#define GJC_SLOTS  144u

typedef struct {
    uint32_t magic;      /* GJC_MAGIC */
    uint32_t head;       /* walk start node */
    uint32_t n;          /* payload bytes */
    GeoJumpRouter router;/* type/param/param2/param3 = the key */
} GJCIndex;              /* 28 bytes */

/* static scratch (ponytail: single-session proof adapter; per-fs mutex if threaded) */
static uint8_t _gjc_seen[GEO_FULL];

/* placement f(step): nodes[i] = jump((head+i) % FULL).
 * Single application (NOT iteration): JUMP_MOD with coprime mult is a full
 * bijection, so all n nodes are distinct. Iterated node*=m cycles with period
 * ord_m (small) — that is a walk, not a placement; v6's slot(i)=i*37 is the
 * placement form. Non-bijective routers (HILBERT/PENTAGON collapse a tower to
 * one offset) fail the gate honestly with -1. 0=ok, -1=collision/unfit. */
static inline int gjc_walk(uint32_t head, const GeoJumpRouter *r, uint32_t n, uint32_t *nodes) {
    if (!r || !nodes || n == 0 || n > GEO_FULL) return -1;
    memset(_gjc_seen, 0, sizeof(_gjc_seen));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t node = geo_jump_r((head + i) % GEO_FULL, r);
        if (node >= GEO_FULL || _gjc_seen[node]) return -1;
        _gjc_seen[node] = 1;
        nodes[i] = node;
    }
    return 0;
}

/* node-sorted permutation: file[j] = byte living at j-th smallest node.
 * Tower/offset are VIRTUAL (computed, never materialized) — coordinate =
 * address. Sort is the placement; DNA re-derives it. Needs nodes[] filled. */
static uint32_t *_gjc_snodes;
static int _gjc_cmp(const void *a, const void *b) {
    uint32_t ia = *(const uint32_t *)a, ib = *(const uint32_t *)b;
    return (_gjc_snodes[ia] > _gjc_snodes[ib]) - (_gjc_snodes[ia] < _gjc_snodes[ib]);
}

/* place container into the system: 2 files — "tag/dna" (28B) + "tag/dat"
 * (n bytes in node-sorted order). 0=ok, -1=walk invalid, -2=fs write fail */
static inline int gjc_store(BreathingFS *fs, const char *tag,
                            uint32_t head, const GeoJumpRouter *r,
                            const int8_t *data, uint32_t n) {
    if (!fs || !tag || !r || !data || n == 0 || n > GEO_FULL) return -1;
    static uint32_t nodes[GEO_FULL];
    static uint32_t order[GEO_FULL];
    if (gjc_walk(head, r, n, nodes) != 0) return -1;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    _gjc_snodes = nodes;
    qsort(order, n, sizeof(uint32_t), _gjc_cmp);

    static int8_t flat[GEO_FULL];
    for (uint32_t j = 0; j < n; j++) flat[j] = data[order[j]];
    char name[BFS_MAX_NAME];
    snprintf(name, sizeof(name), "%s/dat", tag);
    if (bfs_write(fs, name, flat, n) != 0) return -2;
    GJCIndex idx = { GJC_MAGIC, head, n, *r };
    snprintf(name, sizeof(name), "%s/dna", tag);
    if (bfs_write(fs, name, (const int8_t *)&idx, sizeof(idx)) != 0) return -2;
    return 0;
}

/* open index (cross-session resume: read DNA back). 0=ok */
static inline int gjc_open(const BreathingFS *fs, const char *tag, GJCIndex *idx) {
    if (!fs || !tag || !idx) return -1;
    char name[BFS_MAX_NAME];
    snprintf(name, sizeof(name), "%s/dna", tag);
    uint32_t actual = 0;
    int8_t buf[sizeof(GJCIndex)];
    if (bfs_read(fs, name, buf, sizeof(buf), &actual) != 0) return -1;
    if (actual != sizeof(GJCIndex)) return -1;
    memcpy(idx, buf, sizeof(GJCIndex));
    return (idx->magic == GJC_MAGIC) ? 0 : -1;
}

/* load: re-walk (head,router) → re-sort → invert permutation. 0=ok */
static inline int gjc_load(const BreathingFS *fs, const char *tag,
                           uint32_t head, const GeoJumpRouter *r,
                           int8_t *out, uint32_t n) {
    if (!fs || !tag || !r || !out || n == 0 || n > GEO_FULL) return -1;
    GJCIndex idx;
    if (gjc_open(fs, tag, &idx) != 0) return -1;
    if (idx.head != head || idx.n != n) return -1;
    if (memcmp(&idx.router, r, sizeof(*r)) != 0) return -3; /* wrong key */

    char name[BFS_MAX_NAME];
    snprintf(name, sizeof(name), "%s/dat", tag);
    static int8_t flat[GEO_FULL];
    uint32_t actual = 0;
    if (bfs_read(fs, name, flat, sizeof(flat), &actual) != 0) return -2;
    if (actual != n) return -2;

    static uint32_t nodes[GEO_FULL];
    static uint32_t order[GEO_FULL];
    if (gjc_walk(head, r, n, nodes) != 0) return -1;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    _gjc_snodes = nodes;
    qsort(order, n, sizeof(uint32_t), _gjc_cmp);
    for (uint32_t j = 0; j < n; j++) out[order[j]] = flat[j];
    return 0;
}

#endif /* GEO_JUMP_CONTAINER_H */
