#ifndef GEO_JUMP_H
#define GEO_JUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER) && defined(GEO_JUMP_DLL)
#  define GEO_JUMP_API __declspec(dllexport)
#elif defined(__GNUC__) && defined(GEO_JUMP_DLL)
#  define GEO_JUMP_API __attribute__((visibility("default")))
#else
#  define GEO_JUMP_API
#endif

#ifdef GEO_JUMP_INLINE
#  define GEO_JUMP_DEF static inline
#else
#  define GEO_JUMP_DEF extern GEO_JUMP_API
#endif

#define GEO_METATRON_COLS     4u
#define GEO_METATRON_ROWS     4u
#define GEO_METATRON_FLOORS   3u

#define GEO_METATRON_CELLS    (GEO_METATRON_COLS * GEO_METATRON_ROWS)
#define GEO_BLOCK             (GEO_METATRON_CELLS * GEO_METATRON_FLOORS)
#define GEO_TOWER             (GEO_BLOCK * GEO_METATRON_FLOORS)
#define GEO_FULL              (GEO_TOWER * GEO_TOWER)

#define GEO_PENTAGONS        12u
#define GEO_PENT_RING        10u
#define GEO_FIBO_CLOCK      1440u
#define GEO_SHELL_TICK       12u
#define GEO_MOD_PRIME       162u   /* tower-grid identity: 128×162 = 20736 */
#define GEO_MOD_STRIDE        5u   /* MOD walk on 20736: order 1728 = MAX (lcm(64,54)) = 1 pentagon; 12 orbits = full space */

#define GEO_WRAP(x)    ((uint32_t)(x) % GEO_FULL)

#define GEO_CLIMATE_TROPICAL    0u
#define GEO_CLIMATE_TEMPERATE   1u
#define GEO_CLIMATE_BOREAL      2u
#define GEO_CLIMATE_TUNDRA      3u

#define GEO_ZONE_INNER_R       24u            /* incircle: d < 24   */
#define GEO_ZONE_OUTER_R       GEO_TOWER      /* mid:      d < 144  */
#define GEO_ZONE_FAR_R         (GEO_TOWER * 3u) /* between: d < 432 */

#define GEO_INCIRCLE    0u
#define GEO_MIDDLE      1u
#define GEO_BETWEEN     2u
#define GEO_OUTSIDE     3u

typedef struct {
    uint32_t node;
    uint32_t zone;
    uint32_t climate;
    uint32_t dist;
    uint32_t centroid;
} GeoFieldClimate;

typedef enum {
    JUMP_HILBERT  = 0,
    JUMP_PEANO    = 1,
    JUMP_PENTAGON = 2,
    JUMP_MOD      = 3,
    JUMP_INVERT   = 4,
    JUMP_GROUND   = 5,
    JUMP_CAPO     = 6
} GeoJumpType;

typedef struct {
    GeoJumpType type;
    uint32_t    param;    // col / pent_id / mult / tower_off
    uint32_t    param2;   // row (hilbert/peano/ground)
    uint32_t    param3;   // floor (hilbert/peano)
} GeoJumpRouter;

#ifndef GEO_JUMP_INLINE
GEO_JUMP_DEF uint32_t geo_jump(uint32_t node_id, GeoJumpType type, uint32_t param);
GEO_JUMP_DEF uint32_t geo_jump_r(uint32_t node_id, const GeoJumpRouter *r);
GEO_JUMP_DEF void     geo_jump_batch(const uint32_t *nodes, uint32_t n, GeoJumpType type, uint32_t param, uint32_t *out);
GEO_JUMP_DEF void     geo_jump_batch_r(const uint32_t *nodes, uint32_t n, const GeoJumpRouter *r, uint32_t *out);
GEO_JUMP_DEF uint32_t geo_clock_tick(uint32_t node_id);
GEO_JUMP_DEF uint32_t geo_pentagon_id(uint32_t node_id);
GEO_JUMP_DEF uint32_t geo_shell_level(uint32_t node_id);
#endif

#define GEO_WALK_MAX 1024u

typedef struct {
    uint32_t start;
    uint32_t step;
    uint32_t path[GEO_WALK_MAX];
    GeoJumpRouter router;
} GeoJumpWalk;

#ifndef GEO_JUMP_INLINE
GEO_JUMP_DEF void     geo_walk_init(GeoJumpWalk *w, uint32_t start, const GeoJumpRouter *r);
GEO_JUMP_DEF uint32_t geo_walk_step(GeoJumpWalk *w);
GEO_JUMP_DEF uint32_t geo_walk_peak(const GeoJumpWalk *w, uint32_t lookahead);
#endif

#define GEO_DNA_MAX_STEPS 64u

typedef struct {
    uint32_t         head;
    uint32_t         tail;
    uint32_t         seed_key;
    uint32_t         fibo_round;
    uint32_t         length;
    GeoJumpRouter    router;
} GeoDna;

#ifndef GEO_JUMP_INLINE
GEO_JUMP_DEF void     geo_dna_from_walk(GeoDna *d, const GeoJumpWalk *w, uint32_t fibo_round);
GEO_JUMP_DEF uint32_t geo_dna_at(const GeoDna *d, uint32_t step);
GEO_JUMP_DEF uint32_t geo_dna_timeline(const GeoDna *d, uint32_t layer);
GEO_JUMP_DEF void     geo_dna_timeline_all(const GeoDna *d, uint32_t out[12]);
GEO_JUMP_DEF uint32_t geo_capo(uint32_t node, uint32_t key);
GEO_JUMP_DEF GeoFieldClimate geo_field_climate(uint32_t node, uint32_t anchor_id);
#endif

/* always-available helpers (not dependent on GEO_JUMP_INLINE) */
static inline uint32_t _hilbert_idx(uint32_t x, uint32_t y, uint32_t n) {
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | (((uint32_t)(3u * rx)) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

static inline uint32_t _peano_idx(uint32_t x, uint32_t y, uint32_t cols __attribute__((unused)), uint32_t rows) {
    if (x & 1u)
        return x * rows + (rows - 1u - y);
    return x * rows + y;
}

#ifdef GEO_JUMP_INLINE

/* forward decls — needed by _jump_pentagon and geo_field_climate below */
static inline uint32_t geo_pentagon_id(uint32_t node_id);
static inline uint32_t geo_shell_level(uint32_t node_id);
static inline uint32_t geo_clock_tick(uint32_t node_id);

static inline uint32_t _jump_hilbert(uint32_t node, uint32_t col, uint32_t row, uint32_t floor) {
    if (col < 1 || col > GEO_METATRON_COLS) col = 1;
    if (row < 1 || row > GEO_METATRON_ROWS) row = 1;
    if (floor < 1 || floor > GEO_METATRON_FLOORS) floor = 1;
    uint32_t tower = node / GEO_TOWER;
    uint32_t cell  = _hilbert_idx(col - 1, row - 1, GEO_METATRON_COLS);
    uint32_t offset = (floor - 1) * GEO_METATRON_CELLS + cell;
    return GEO_WRAP(tower * GEO_TOWER + offset);
}

static inline uint32_t _jump_peano(uint32_t node, uint32_t col, uint32_t row, uint32_t floor) {
    if (col < 1 || col > GEO_METATRON_COLS) col = 1;
    if (row < 1 || row > GEO_METATRON_ROWS) row = 1;
    if (floor < 1 || floor > GEO_METATRON_FLOORS) floor = 1;
    uint32_t tower = node / GEO_TOWER;
    uint32_t cell  = _peano_idx(col - 1, row - 1, GEO_METATRON_COLS, GEO_METATRON_ROWS);
    uint32_t offset = (floor - 1) * GEO_METATRON_CELLS + cell;
    return GEO_WRAP(tower * GEO_TOWER + offset);
}

static inline uint32_t _jump_ground(uint32_t node, uint32_t col, uint32_t row) {
    if (col < 1 || col > GEO_METATRON_FLOORS) col = 1;
    if (row < 1 || row > GEO_BLOCK) row = 1;
    uint32_t tower = node / GEO_TOWER;
    uint32_t g_idx;
    if (col % 2 == 0)
        g_idx = GEO_BLOCK * (col - 1) + (GEO_BLOCK - row);
    else
        g_idx = GEO_BLOCK * (col - 1) + (row - 1);
    // project to current tower
    return GEO_WRAP(tower * GEO_TOWER + g_idx);
}

static inline uint32_t _jump_pentagon(uint32_t node, uint32_t layer) {
    uint32_t face = geo_pentagon_id(node) - 1;
    if (layer >= GEO_SHELL_TICK) layer = 0;
    uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
    return GEO_WRAP(face * face_stride + layer * GEO_TOWER);
}

static inline uint32_t _jump_mod(uint32_t node, uint32_t mult) {
    if (mult == 0) mult = GEO_MOD_STRIDE;  /* 5: order 1728 = MAX on 20736 */
    return GEO_WRAP((uint64_t)node * mult);
}

static inline uint32_t _jump_invert(uint32_t node, uint32_t tower_off) {
    uint32_t tower  = (node / GEO_BLOCK) % GEO_METATRON_FLOORS;
    uint32_t next_t = (tower + tower_off + 1) % GEO_METATRON_FLOORS;
    uint32_t local  = node % GEO_BLOCK;
    uint32_t mirror = GEO_BLOCK - 1 - local;
    return GEO_WRAP(next_t * GEO_BLOCK + mirror);
}

uint32_t geo_jump(uint32_t node_id, GeoJumpType type, uint32_t param) {
    switch (type) {
        case JUMP_HILBERT:  return _jump_hilbert(node_id, param ? param : 1, 1, 1);
        case JUMP_PEANO:    return _jump_peano  (node_id, param ? param : 1, 1, 1);
        case JUMP_PENTAGON: return _jump_pentagon(node_id, param);
        case JUMP_MOD:      return _jump_mod    (node_id, param);
        case JUMP_INVERT:   return _jump_invert (node_id, param);
        case JUMP_GROUND:   return _jump_ground (node_id, param ? param : 1, 1);
        default:            return GEO_WRAP(node_id + 1);
    }
}

uint32_t geo_jump_r(uint32_t node_id, const GeoJumpRouter *r) {
    if (!r) return GEO_WRAP(node_id + 1);
    switch (r->type) {
        case JUMP_HILBERT: return _jump_hilbert(node_id, r->param ? r->param : 1, r->param2 ? r->param2 : 1, r->param3 ? r->param3 : 1);
        case JUMP_PEANO:   return _jump_peano  (node_id, r->param ? r->param : 1, r->param2 ? r->param2 : 1, r->param3 ? r->param3 : 1);
        case JUMP_PENTAGON: {
            uint32_t face = geo_pentagon_id(node_id) - 1;
            uint32_t layer = r->param;
            if (r->param2) face = r->param2 - 1;
            if (layer >= GEO_SHELL_TICK) layer = 0;
            uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
            return GEO_WRAP(face * face_stride + layer * GEO_TOWER);
        }
        case JUMP_GROUND:  return _jump_ground (node_id, r->param ? r->param : 1, r->param2 ? r->param2 : 1);
        default:           return geo_jump(node_id, r->type, r->param);
    }
}

void geo_jump_batch(const uint32_t *nodes, uint32_t n, GeoJumpType type, uint32_t param, uint32_t *out) {
    for (uint32_t i = 0; i < n; i++) out[i] = geo_jump(nodes[i], type, param);
}

void geo_jump_batch_r(const uint32_t *nodes, uint32_t n, const GeoJumpRouter *r, uint32_t *out) {
    for (uint32_t i = 0; i < n; i++) out[i] = geo_jump_r(nodes[i], r);
}

void geo_walk_init(GeoJumpWalk *w, uint32_t start, const GeoJumpRouter *r) {
    w->start = start;
    w->step  = 0;
    w->path[0] = start;
    if (r) w->router = *r;
}

uint32_t geo_walk_step(GeoJumpWalk *w) {
    if (w->step >= GEO_WALK_MAX - 1) return w->path[w->step];
    uint32_t next = geo_jump_r(w->path[w->step], &w->router);
    w->step++;
    w->path[w->step] = next;
    return next;
}

uint32_t geo_walk_peak(const GeoJumpWalk *w, uint32_t lookahead) {
    if (!w || w->step >= GEO_WALK_MAX) return 0;
    uint32_t node = w->path[w->step];
    uint32_t max_look = GEO_WALK_MAX - 1u - w->step;
    for (uint32_t i = 0; i < lookahead && i < max_look; i++)
        node = geo_jump_r(node, &w->router);
    return node;
}

void geo_dna_from_walk(GeoDna *d, const GeoJumpWalk *w, uint32_t fibo_round) {
    if (!d || !w) return;
    d->head       = w->start;
    d->tail       = w->path[w->step];
    d->seed_key   = w->router.param ^ (w->router.param2 << 8) ^ (w->router.param3 << 16) ^ (w->router.type << 24);
    d->fibo_round = fibo_round;
    d->length     = w->step + 1;
    d->router     = w->router;
}

uint32_t geo_dna_at(const GeoDna *d, uint32_t step) {
    if (!d || step >= d->length) return d ? d->tail : 0;
    uint32_t node = d->head;
    for (uint32_t i = 0; i < step; i++)
        node = geo_jump_r(node, &d->router);
    return node;
}

uint32_t geo_dna_timeline(const GeoDna *d, uint32_t layer) {
    if (!d || layer >= GEO_SHELL_TICK) return d ? d->tail : 0;
    uint32_t face  = d->head / (GEO_FULL / GEO_PENTAGONS);
    uint32_t base  = face * (GEO_FULL / GEO_PENTAGONS);
    uint32_t cell  = (d->head - base) % GEO_TOWER;
    return base + layer * GEO_TOWER + cell;
}

void geo_dna_timeline_all(const GeoDna *d, uint32_t out[12]) {
    if (!d || !out) return;
    uint32_t face  = d->head / (GEO_FULL / GEO_PENTAGONS);
    uint32_t base  = face * (GEO_FULL / GEO_PENTAGONS);
    uint32_t local = d->head - base;
    uint32_t cell  = local % GEO_TOWER;
    for (uint32_t i = 0; i < 12; i++)
        out[i] = base + i * GEO_TOWER + cell;
}

uint32_t geo_clock_tick(uint32_t node_id) {
    return (node_id * GEO_FIBO_CLOCK) / GEO_FULL;
}

uint32_t geo_pentagon_id(uint32_t node_id) {
    return (node_id / (GEO_FULL / GEO_PENTAGONS)) + 1;
}

uint32_t geo_shell_level(uint32_t node_id) {
    return (node_id / (GEO_FULL / (GEO_PENTAGONS * GEO_SHELL_TICK))) % GEO_SHELL_TICK;
}

uint32_t geo_capo(uint32_t node, uint32_t key) {
    return GEO_WRAP(node + key * GEO_TOWER);
}

GeoFieldClimate geo_field_climate(uint32_t node, uint32_t anchor_id) {
    GeoFieldClimate c;
    c.node     = node;
    anchor_id %= 24u;
    c.centroid = anchor_id * (GEO_FULL / 24u) + (GEO_FULL / 48u);
    int32_t d  = (int32_t)(c.centroid - node);
    if (d < 0) d = -d;
    c.dist     = (uint32_t)d;
    if (d < GEO_ZONE_INNER_R)      { c.zone = GEO_INCIRCLE;  c.climate = GEO_CLIMATE_TROPICAL; }
    else if (d < GEO_ZONE_OUTER_R) { c.zone = GEO_MIDDLE;    c.climate = GEO_CLIMATE_TEMPERATE; }
    else if (d < GEO_ZONE_FAR_R)   { c.zone = GEO_BETWEEN;   c.climate = GEO_CLIMATE_BOREAL; }
    else                            { c.zone = GEO_OUTSIDE;   c.climate = GEO_CLIMATE_TUNDRA; }
    return c;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
