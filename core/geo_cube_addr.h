/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cube_addr.h — Generation-Indexed Cube Addresser
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * FOUNDATION LAYER: every weight position is addressable via pure formula.
 * No permutation arrays, no lookup tables beyond verified constants.
 *
 * Address = (generation n, face 0-5, slot within face)
 *   └─ like floating-point = (exponent, mantissa)
 *
 * w(time) = temporal position indicator
 *   └─ NOT f(time) from old project (avoid name collision)
 *   └─ "like water level tells position in river"
 *   └─ combined with gen_scale(n) for temporal modulation
 *
 * Depends: geo_cube_in_dodeca.h (vertex tables, φ, cell types)
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_CUBE_ADDR_H
#define GEO_CUBE_ADDR_H

#include "geo_cube_in_dodeca.h"

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define CUBE_ADDR_FACES     6u    /* 6 half-axes = 6 DiamondBlock faces */
#define CUBE_ADDR_SLOTS_MAX 128u  /* max slots per face (conservative) */
#define CUBE_ADDR_GEN_MAX   31u   /* max generation before float concern */

/* ═══════════════════════════════════════════════════════════════
   GEO CUBE ADDRESS STRUCT
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  generation;  /* n — which φ-layer (0..CUBE_ADDR_GEN_MAX) */
    uint8_t  face;        /* 0-5 — which DiamondBlock half-axis face */
    uint16_t slot;        /* position within face (0..slots_in_gen-1) */
    uint8_t  cell_type;   /* 0-7 from 3-bit parity (auto-computed) */
    double   w_time;      /* temporal position (default 1.0 = no shift) */
} GeoCubeAddr;

/* ═══════════════════════════════════════════════════════════════
   w(time) — TEMPORAL POSITION INDICATOR
   ═══════════════════════════════════════════════════════════════
   w(time) is NOT time itself — it IS time.
   Like water level tells position in river.
   w=1.0 = neutral (no temporal shift).
   w>1.0 = expanded (future/zoomed out).
   w<1.0 = contracted (past/zoomed in).
   
   Combined address scaling:
     effective_scale = gen_scale(n) * w(time)
   
   This means w(time) MODULATES the generation scale:
   - Same generation, different w → different spatial footprint
   - Same w, different generation → standard φ-scaling
   ═══════════════════════════════════════════════════════════════ */

/* Default w = 1.0 (no temporal shift) */
#define W_TIME_DEFAULT 1.0

/* w(time) scaling: modulate gen_scale by temporal position */
static inline double w_scale(uint32_t n, double w_time) {
    return gen_scale(n) * w_time;
}

/* w(time) from real time: map [0,1] → [0.1, 10.0] (log scale) */
static inline double w_from_time(double t) {
    /* log scale: t=0 → w=0.1, t=0.5 → w=1.0, t=1.0 → w=10.0 */
    return pow(10.0, 2.0 * t - 1.0);
}

/* ═══════════════════════════════════════════════════════════════
   SLOTS PER GENERATION
   ═══════════════════════════════════════════════════════════════
   Slot count = 6 faces × φⁿ cells per face
   But we use a simpler model: slots grow by φ each generation.
   
   gen 0: 6 faces × 1 slot  = 6 total
   gen 1: 6 faces × 2 slots = 12 total
   gen 2: 6 faces × 3 slots = 18 total
   gen 3: 6 faces × 5 slots = 30 total
   ...
   
   Fibonacci-like: slots_per_face(n) = round(φⁿ)
   ═══════════════════════════════════════════════════════════════ */

static inline uint16_t slots_per_face(uint32_t n) {
    if (n == 0) return 1;
    double s = gen_scale(n);
    return (uint16_t)(s + 0.5);  /* round to nearest integer */
}

static inline uint32_t total_slots(uint32_t n) {
    return (uint32_t)slots_per_face(n) * CUBE_ADDR_FACES;
}

/* ═══════════════════════════════════════════════════════════════
   CORE ADDRESSING FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* Create address: (generation, face, slot) → GeoCubeAddr
 * cell_type auto-computed from parity of generation across faces */
static inline GeoCubeAddr geo_cube_addr(uint8_t generation, uint8_t face, uint16_t slot) {
    GeoCubeAddr addr;
    addr.generation = generation;
    addr.face = face % CUBE_ADDR_FACES;
    addr.slot = slot % slots_per_face(generation);
    addr.w_time = W_TIME_DEFAULT;
    
    /* Cell type from parity of (generation, face, slot)
     * This maps to the 8 cell types: III, IID, IDI, IDD, DII, DID, DDI, DDD */
    addr.cell_type = ((generation & 1) << 2) | ((face & 1) << 1) | (slot & 1);
    
    return addr;
}

/* Apply w(time) to address — temporal modulation */
static inline GeoCubeAddr geo_cube_addr_w(GeoCubeAddr addr, double w_time) {
    addr.w_time = w_time;
    return addr;
}

/* ═══════════════════════════════════════════════════════════════
   ADDRESS → 3D POSITION
   ═══════════════════════════════════════════════════════════════
   Uses cube-in-dodeca half-axis mapping:
   face 0 = X+, face 1 = X-
   face 2 = Y+, face 3 = Y-
   face 4 = Z+, face 5 = Z-
   
   Scale = gen_scale(n) * w(time)
   ═══════════════════════════════════════════════════════════════ */

static inline Vec3D geo_cube_addr_to_xyz(GeoCubeAddr addr) {
    uint8_t axis = addr.face / 2;      /* 0=X, 1=Y, 2=Z */
    uint8_t sign = addr.face % 2;      /* 0=+, 1=- */
    
    /* Base position from half-axis center */
    Vec3D center = half_axis_center(axis, sign, addr.generation);
    
    /* Apply w(time) modulation */
    double w_factor = addr.w_time;
    center.x *= w_factor;
    center.y *= w_factor;
    center.z *= w_factor;
    
    /* Add slot offset within face (spread along face normal) */
    double slot_offset = (double)addr.slot * 0.1;  /* small offset per slot */
    double normal[3] = {0.0, 0.0, 0.0};
    normal[axis] = (sign == 0) ? 1.0 : -1.0;
    
    center.x += normal[0] * slot_offset;
    center.y += normal[1] * slot_offset;
    center.z += normal[2] * slot_offset;
    
    return center;
}

/* ═══════════════════════════════════════════════════════════════
   ADDRESS → FLAT INDEX (for 20736 grid)
   ═══════════════════════════════════════════════════════════════
   Maps (generation, face, slot) → flat index in [0, 20736)
   Used for grid-based storage (DiamondBlock, shell, etc.)
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t geo_cube_addr_to_flat(GeoCubeAddr addr) {
    /* Accumulate total slots from generation 0 to addr.generation-1 */
    uint32_t base = 0;
    for (uint32_t g = 0; g < addr.generation; g++) {
        base += total_slots(g);
    }
    
    /* Add face × slots_per_gen + slot offset */
    base += (uint32_t)addr.face * slots_per_face(addr.generation);
    base += addr.slot;
    
    return base % 20736u;  /* wrap to grid space */
}

/* ═══════════════════════════════════════════════════════════════
   FLAT INDEX → ADDRESS (reverse mapping)
   ═══════════════════════════════════════════════════════════════ */

static inline GeoCubeAddr geo_flat_to_addr(uint32_t flat) {
    flat %= 20736u;
    
    /* Find generation by accumulating slots */
    uint32_t accumulated = 0;
    uint8_t gen = 0;
    uint32_t slots_this_gen;
    
    while (gen < CUBE_ADDR_GEN_MAX) {
        slots_this_gen = total_slots(gen);
        if (accumulated + slots_this_gen > flat) {
            break;
        }
        accumulated += slots_this_gen;
        gen++;
    }
    
    /* Within this generation: face and slot */
    uint32_t within_gen = flat - accumulated;
    uint16_t spf = slots_per_face(gen);
    uint8_t face = (uint8_t)(within_gen / spf);
    uint16_t slot = (uint16_t)(within_gen % spf);
    
    return geo_cube_addr(gen, face, slot);
}

/* ═══════════════════════════════════════════════════════════════
   CELL TYPE → NAME (for debug/display)
   ═══════════════════════════════════════════════════════════════ */

static inline const char* cell_type_name(uint8_t ct) {
    static const char* names[] = {
        "III", "IID", "IDI", "IDD",
        "DII", "DID", "DDI", "DDD"
    };
    return (ct < 8) ? names[ct] : "???";
}

/* ═══════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════ */

/* Verify roundtrip: addr → flat → addr preserves generation, face, slot */
static inline int verify_addr_roundtrip(uint32_t max_gen) {
    int pass = 0, fail = 0;
    
    for (uint32_t g = 0; g <= max_gen && g <= CUBE_ADDR_GEN_MAX; g++) {
        uint16_t spf = slots_per_face(g);
        for (uint8_t f = 0; f < CUBE_ADDR_FACES; f++) {
            for (uint16_t s = 0; s < spf; s++) {
                GeoCubeAddr original = geo_cube_addr(g, f, s);
                uint32_t flat = geo_cube_addr_to_flat(original);
                GeoCubeAddr restored = geo_flat_to_addr(flat);
                
                if (restored.generation == original.generation &&
                    restored.face == original.face &&
                    restored.slot == original.slot) {
                    pass++;
                } else {
                    fail++;
                    if (fail <= 3) {  /* only print first 3 failures */
                        printf("  FAIL: gen=%u face=%u slot=%u → flat=%u → gen=%u face=%u slot=%u\n",
                               original.generation, original.face, original.slot,
                               flat, restored.generation, restored.face, restored.slot);
                    }
                }
            }
        }
    }
    
    printf("  Roundtrip: %d PASS / %d FAIL\n", pass, fail);
    return fail == 0;
}

/* Verify w(time) modulation doesn't break addressing */
static inline int verify_w_time(void) {
    GeoCubeAddr a1 = geo_cube_addr(2, 0, 0);
    GeoCubeAddr a2 = geo_cube_addr_w(a1, 2.0);
    
    /* Same address fields, different w */
    int ok = (a1.generation == a2.generation &&
              a1.face == a2.face &&
              a1.slot == a2.slot &&
              a1.w_time == 1.0 &&
              a2.w_time == 2.0);
    
    /* Position should be different */
    Vec3D p1 = geo_cube_addr_to_xyz(a1);
    Vec3D p2 = geo_cube_addr_to_xyz(a2);
    double dist = vec3_distance(p1, p2);
    
    printf("  w(time) modulation: %s (dist=%.4f)\n", ok ? "PASS" : "FAIL", dist);
    return ok && dist > 0.01;
}

/* ═══════════════════════════════════════════════════════════════
   STATISTICS / PRINT
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cube_addr_stats(uint32_t max_gen) {
    printf("===============================================================\n");
    printf("  GeoCubeAddr — Generation-Indexed Cube Addresser\n");
    printf("---------------------------------------------------------------\n");
    printf("  Faces:                  %u (DiamondBlock half-axes)\n", CUBE_ADDR_FACES);
    printf("  Max generation:         %u\n", max_gen);
    printf("  w(time) default:        %.1f\n", W_TIME_DEFAULT);
    printf("---------------------------------------------------------------\n");
    printf("  Gen | Slots/Face | Total  | Scale (φⁿ)\n");
    printf("  ----|------------|--------|------------\n");
    
    uint32_t cumulative = 0;
    for (uint32_t g = 0; g <= max_gen && g <= CUBE_ADDR_GEN_MAX; g++) {
        uint16_t spf = slots_per_face(g);
        uint32_t tot = total_slots(g);
        cumulative += tot;
        printf("  %3u | %10u | %6u | %.4f\n", g, spf, tot, gen_scale(g));
    }
    
    printf("---------------------------------------------------------------\n");
    printf("  Cumulative total:      %u slots\n", cumulative);
    printf("  Grid capacity:         20736 (12⁴)\n");
    printf("===============================================================\n");
}

#endif /* GEO_CUBE_ADDR_H */
