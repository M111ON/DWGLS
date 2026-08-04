/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cube_in_dodeca.h — Cube-in-Dodecahedron Mapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * CRITICAL INVARIANT:
 *   Cube vertices are DERIVED FROM dodecahedron vertices.
 *   Never create XYZ separately — must use existing dodeca structure.
 *
 * Geometry Facts:
 *   - Dodecahedron: 20 vertices
 *   - Cube: 8 vertices (subset of dodeca's 20)
 *   - 5 cubes compound inside dodecahedron
 *   - cube edge = φ × pentagon edge (1.618034...)
 *   - 3 axes × 2 signs = 6 half-axes = DiamondBlock 6 faces
 *
 * Addressing:
 *   - (n, k) where n=generation, k=face/vertex ID
 *   - 8 cell types from parity (2³)
 *   - Origin (0,0,0) = corner asymptote, unreachable
 *
 * Depends: math.h (for sqrt)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_CUBE_IN_DODECA_H
#define GEO_CUBE_IN_DODECA_H

#include <stdint.h>
#include <stdio.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define PHI  1.6180339887498948482  /* golden ratio φ = (1+√5)/2 */
#define PHI2 (PHI * PHI)            /* φ² ≈ 2.618 */
#define INV_PHI (1.0 / PHI)         /* 1/φ ≈ 0.618 */
#define INV_PHI2 (1.0 / PHI2)       /* 1/φ² ≈ 0.382 */

#define DODECA_VERTS  20u
#define CUBE_VERTS     8u
#define CUBE_AXES      3u
#define CUBE_HALF_AXES 6u  /* 3 axes × 2 signs */

/* ═══════════════════════════════════════════════════════════════
   DODECAHEDRON VERTICES (20 vertices)
   
   Standard construction:
   - 8 vertices from cube: (±1, ±1, ±1)
   - 12 vertices from golden rectangles: (0, ±1/φ, ±φ), etc.
   
   These are the canonical dodecahedron vertices.
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    double x, y, z;
} Vec3D;

/* Dodecahedron vertices — derived, not hardcoded separately */
static const Vec3D DODECA_VERTS_TABLE[DODECA_VERTS] = {
    /* Cube vertices (8): (±1, ±1, ±1) */
    { 1.0,  1.0,  1.0},  /* 0: (+,+,+) */
    { 1.0,  1.0, -1.0},  /* 1: (+,+,-) */
    { 1.0, -1.0,  1.0},  /* 2: (+,-,+) */
    { 1.0, -1.0, -1.0},  /* 3: (+,-,-) */
    {-1.0,  1.0,  1.0},  /* 4: (-,+,+) */
    {-1.0,  1.0, -1.0},  /* 5: (-,+,-) */
    {-1.0, -1.0,  1.0},  /* 6: (-,-,+) */
    {-1.0, -1.0, -1.0},  /* 7: (-,-,-) */
    
    /* Golden rectangle vertices (12): (0, ±1/φ, ±φ) and permutations */
    { 0.0,  INV_PHI,  PHI},  /* 8 */
    { 0.0,  INV_PHI, -PHI},  /* 9 */
    { 0.0, -INV_PHI,  PHI},  /* 10 */
    { 0.0, -INV_PHI, -PHI},  /* 11 */
    
    { INV_PHI,  PHI, 0.0},   /* 12 */
    { INV_PHI, -PHI, 0.0},   /* 13 */
    {-INV_PHI,  PHI, 0.0},   /* 14 */
    {-INV_PHI, -PHI, 0.0},   /* 15 */
    
    { PHI, 0.0,  INV_PHI},   /* 16 */
    { PHI, 0.0, -INV_PHI},   /* 17 */
    {-PHI, 0.0,  INV_PHI},   /* 18 */
    {-PHI, 0.0, -INV_PHI},   /* 19 */
};

/* ═══════════════════════════════════════════════════════════════
   CUBE VERTICES (8 vertices — indices 0-7 of dodeca)
   
   These are the 8 cube vertices embedded in the dodecahedron.
   They form one of the 5 possible cubes in the compound.
   
   Mapping to DiamondBlock 6 faces:
   - 3 axes × 2 signs = 6 half-axes
   - Each half-axis = one face of DiamondBlock
   - Store n (generation) independently per face
   ═══════════════════════════════════════════════════════════════ */

/* Cube vertex indices in DODECA_VERTS_TABLE */
static const uint8_t CUBE_INDICES[CUBE_VERTS] = {
    0, 1, 2, 3, 4, 5, 6, 7  /* first 8 = cube vertices */
};

/* ═══════════════════════════════════════════════════════════════
   AXIS MAPPING (3 axes × 2 signs = 6 half-axes)
   
   Axis 0 = X: vertices {0,1,2,3} (x=+1) and {4,5,6,7} (x=-1)
   Axis 1 = Y: vertices {0,1,4,5} (y=+1) and {2,3,6,7} (y=-1)
   Axis 2 = Z: vertices {0,2,4,6} (z=+1) and {1,3,5,7} (z=-1)
   
   Each half-axis corresponds to one face of DiamondBlock.
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    CUBE_AXIS_X = 0,
    CUBE_AXIS_Y = 1,
    CUBE_AXIS_Z = 2,
} CubeAxis;

typedef enum {
    CUBE_SIGN_POS = 0,
    CUBE_SIGN_NEG = 1,
} CubeSign;

/* Vertex indices for each half-axis (4 vertices per half-axis) */
static const uint8_t HALF_AXIS_VERTS[CUBE_AXES][2][4] = {
    /* X axis */
    {
        {0, 1, 2, 3},  /* X+ (x=+1) */
        {4, 5, 6, 7},  /* X- (x=-1) */
    },
    /* Y axis */
    {
        {0, 1, 4, 5},  /* Y+ (y=+1) */
        {2, 3, 6, 7},  /* Y- (y=-1) */
    },
    /* Z axis */
    {
        {0, 2, 4, 6},  /* Z+ (z=+1) */
        {1, 3, 5, 7},  /* Z- (z=-1) */
    },
};

/* ═══════════════════════════════════════════════════════════════
   CELL TYPES FROM PARITY (2³ = 8)
   
   (nx%2, ny%2, nz%2) → cell type
   (0,0,0) → (i,i,i)  — all icosa
   (0,0,1) → (i,i,d)  — 2 icosa, 1 dodeca
   ...
   (1,1,1) → (d,d,d)  — all dodeca
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    CELL_TYPE_III = 0,  /* (0,0,0) = icosa, icosa, icosa */
    CELL_TYPE_IID = 1,  /* (0,0,1) = icosa, icosa, dodeca */
    CELL_TYPE_IDI = 2,  /* (0,1,0) = icosa, dodeca, icosa */
    CELL_TYPE_IDD = 3,  /* (0,1,1) = icosa, dodeca, dodeca */
    CELL_TYPE_DII = 4,  /* (1,0,0) = dodeca, icosa, icosa */
    CELL_TYPE_DID = 5,  /* (1,0,1) = dodeca, icosa, dodeca */
    CELL_TYPE_DDI = 6,  /* (1,1,0) = dodeca, dodeca, icosa */
    CELL_TYPE_DDD = 7,  /* (1,1,1) = dodeca, dodeca, dodeca */
} CellType;

/* ═══════════════════════════════════════════════════════════════
   MAPPING FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* Get dodeca vertex by index */
static inline Vec3D dodeca_vertex(uint8_t idx) {
    return DODECA_VERTS_TABLE[idx % DODECA_VERTS];
}

/* Get cube vertex by index (0-7) */
static inline Vec3D cube_vertex(uint8_t idx) {
    return DODECA_VERTS_TABLE[idx % CUBE_VERTS];
}

/* Check if a dodeca vertex is also a cube vertex */
static inline int is_cube_vertex(uint8_t idx) {
    return idx < CUBE_VERTS;
}

/* Get cell type from parity of generation numbers */
static inline CellType cell_type_from_parity(uint32_t nx, uint32_t ny, uint32_t nz) {
    uint8_t px = nx & 1;
    uint8_t py = ny & 1;
    uint8_t pz = nz & 1;
    return (CellType)(px * 4 + py * 2 + pz);
}

/* Get parity bits from cell type */
static inline void cell_type_to_parity(CellType ct, uint8_t *px, uint8_t *py, uint8_t *pz) {
    *px = ((uint8_t)ct >> 2) & 1;
    *py = ((uint8_t)ct >> 1) & 1;
    *pz = (uint8_t)ct & 1;
}

/* ═══════════════════════════════════════════════════════════════
   PHI CONNECTION VERIFICATION
   
   cube edge = φ × pentagon edge
   
   Pentagon edge = distance between adjacent golden rectangle vertices
   Cube edge = distance between adjacent cube vertices
   ═══════════════════════════════════════════════════════════════ */

/* Distance between two 3D points */
static inline double vec3_distance(Vec3D a, Vec3D b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    double dz = a.z - b.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

/* Get pentagon edge length (distance between adjacent vertices on pentagonal face) */
static inline double pentagon_edge(void) {
    /* Pentagon face vertices: (1,1,1), (0, 1/φ, φ), (-1,1,1), (-1,-1,1), (0, -1/φ, φ) */
    /* Adjacent vertices: (1,1,1) and (0, 1/φ, φ) */
    Vec3D v0 = DODECA_VERTS_TABLE[0];  /* (1,1,1) */
    Vec3D v8 = DODECA_VERTS_TABLE[8];  /* (0, 1/φ, φ) */
    return vec3_distance(v0, v8);
}

/* Get cube edge length (distance between adjacent cube vertices) */
static inline double cube_edge(void) {
    /* Adjacent cube vertices: e.g., (1,1,1) and (1,1,-1) */
    Vec3D v0 = DODECA_VERTS_TABLE[0]; /* (1,1,1) */
    Vec3D v1 = DODECA_VERTS_TABLE[1]; /* (1,1,-1) */
    return vec3_distance(v0, v1);
}

/* Verify φ connection: cube_edge / pentagon_edge ≈ φ */
static inline double phi_ratio(void) {
    return cube_edge() / pentagon_edge();
}

/* ═══════════════════════════════════════════════════════════════
   ADDRESSING: (n, k) → (x, y, z)
   
   n = generation (layer number)
   k = face/vertex ID (0-19 for dodeca, 0-7 for cube)
   
   For cube vertices:
   - k = 0-7 maps to one of the 8 cube vertices
   - Position = vertex coordinate scaled by generation
   ═══════════════════════════════════════════════════════════════ */

/* Scale factor for generation n: grows by φ each generation */
static inline double gen_scale(uint32_t n) {
    /* Start from 1.0, scale by φ each generation */
    double scale = 1.0;
    for (uint32_t i = 0; i < n; i++) {
        scale *= PHI;
    }
    return scale;
}

/* Map (n, k) to 3D position for cube vertices */
static inline Vec3D cube_address_to_xyz(uint32_t n, uint8_t k) {
    Vec3D v = cube_vertex(k);
    double scale = gen_scale(n);
    Vec3D result;
    result.x = v.x * scale;
    result.y = v.y * scale;
    result.z = v.z * scale;
    return result;
}

/* Map (n, k) to 3D position for dodeca vertices */
static inline Vec3D dodeca_address_to_xyz(uint32_t n, uint8_t k) {
    Vec3D v = dodeca_vertex(k);
    double scale = gen_scale(n);
    Vec3D result;
    result.x = v.x * scale;
    result.y = v.y * scale;
    result.z = v.z * scale;
    return result;
}

/* ═══════════════════════════════════════════════════════════════
   DIAMOND BLOCK INTERFACE
   
   6 half-axes = 6 faces of DiamondBlock
   Each face stores n (generation) independently
   ═══════════════════════════════════════════════════════════════ */

/* Get the 4 vertex indices for a specific half-axis */
static inline const uint8_t* half_axis_vertices(uint8_t axis, uint8_t sign) {
    return HALF_AXIS_VERTS[axis % CUBE_AXES][sign % 2];
}

/* Get the center position of a half-axis at generation n */
static inline Vec3D half_axis_center(uint8_t axis, uint8_t sign, uint32_t n) {
    const uint8_t *verts = half_axis_vertices(axis, sign);
    Vec3D center = {0.0, 0.0, 0.0};
    double scale = gen_scale(n);
    
    for (int i = 0; i < 4; i++) {
        Vec3D v = cube_vertex(verts[i]);
        center.x += v.x;
        center.y += v.y;
        center.z += v.z;
    }
    
    center.x = (center.x / 4.0) * scale;
    center.y = (center.y / 4.0) * scale;
    center.z = (center.z / 4.0) * scale;
    
    return center;
}

/* ═══════════════════════════════════════════════════════════════
   VERIFICATION FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* Verify all cube vertices are in dodeca vertices */
static inline int verify_cube_in_dodeca(void) {
    for (uint8_t i = 0; i < CUBE_VERTS; i++) {
        Vec3D cv = cube_vertex(i);
        int found = 0;
        for (uint8_t j = 0; j < DODECA_VERTS; j++) {
            Vec3D dv = dodeca_vertex(j);
            if (fabs(cv.x - dv.x) < 1e-10 &&
                fabs(cv.y - dv.y) < 1e-10 &&
                fabs(cv.z - dv.z) < 1e-10) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* Verify φ ratio */
static inline int verify_phi_ratio(double tolerance) {
    double ratio = phi_ratio();
    return fabs(ratio - PHI) < tolerance;
}

/* Verify 6 half-axes = 6 faces */
static inline int verify_half_axes(void) {
    for (uint8_t axis = 0; axis < CUBE_AXES; axis++) {
        for (uint8_t sign = 0; sign < 2; sign++) {
            const uint8_t *verts = half_axis_vertices(axis, sign);
            /* Each half-axis should have 4 unique vertices */
            for (int i = 0; i < 4; i++) {
                if (verts[i] >= CUBE_VERTS) return 0;
                for (int j = i+1; j < 4; j++) {
                    if (verts[i] == verts[j]) return 0;
                }
            }
        }
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
   STATISTICS / PRINT
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cube_in_dodeca_stats(void) {
    printf("===============================================================\n");
    printf("  Cube-in-Dodecahedron Mapping\n");
    printf("---------------------------------------------------------------\n");
    printf("  Dodecahedron vertices:  %u\n", DODECA_VERTS);
    printf("  Cube vertices:          %u\n", CUBE_VERTS);
    printf("  Axes:                   %u\n", CUBE_AXES);
    printf("  Half-axes:              %u\n", CUBE_HALF_AXES);
    printf("  Cell types:             %u (2^3)\n", 8);
    printf("---------------------------------------------------------------\n");
    printf("  Pentagon edge:          %.6f\n", pentagon_edge());
    printf("  Cube edge:              %.6f\n", cube_edge());
    printf("  φ ratio:                %.6f (expected %.6f)\n", phi_ratio(), PHI);
    printf("  φ error:                %.2e\n", fabs(phi_ratio() - PHI));
    printf("---------------------------------------------------------------\n");
    printf("  Verification:\n");
    printf("    Cube in dodeca:       %s\n", verify_cube_in_dodeca() ? "PASS" : "FAIL");
    printf("    φ ratio:              %s\n", verify_phi_ratio(1e-6) ? "PASS" : "FAIL");
    printf("    Half-axes:            %s\n", verify_half_axes() ? "PASS" : "FAIL");
    printf("===============================================================\n");
}

#endif /* GEO_CUBE_IN_DODECA_H */
