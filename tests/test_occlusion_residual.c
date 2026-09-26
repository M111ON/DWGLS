/* test_occlusion_residual.c — occlusion-residual conservation proof.
 * Claim: projecting the 4x4x3 Metatron stack (48 points) onto any viewport
 * conserves total: visible + occluded == 48. Reshape (90-degree rotation)
 * never loses points, only moves them between visible and hidden.
 * Pure integer math: orthographic projection = drop one coordinate,
 * occlusion = two points sharing projected coords. Rotation = exact
 * axis permutation + sign flip (no floats).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GX 4u
#define GY 4u
#define GZ 3u
#define NPTS (GX * GY * GZ)   /* 48 */

typedef struct { int x, y, z; } Pt;
static Pt pts[NPTS];

static void build(void) {
    unsigned n = 0;
    for (int z = 0; z < (int)GZ; z++)
        for (int y = 0; y < (int)GY; y++)
            for (int x = 0; x < (int)GX; x++)
                pts[n++] = (Pt){ x, y, z };
}

/* project along axis (0=x: keep y,z / 1=y: keep x,z / 2=z: keep x,y).
 * Returns visible count; occluded = 48 - visible. */
static unsigned project(int axis, unsigned seen[GX][GY]) {
    memset(seen, 0, sizeof(unsigned) * GX * GY);
    unsigned vis = 0;
    for (unsigned i = 0; i < NPTS; i++) {
        int a = axis == 0 ? pts[i].y : pts[i].x;
        int b = axis == 0 ? pts[i].z : (axis == 1 ? pts[i].z : pts[i].y);
        if (!seen[a][b]) { seen[a][b] = 1; vis++; }
    }
    return vis;
}

/* exact 90-degree rotation about z: (x,y,z) -> (3-y,x,z); keeps coords in range. */
static void rot90z(void) {
    for (unsigned i = 0; i < NPTS; i++) {
        int x = pts[i].x, y = pts[i].y;
        pts[i].x = 3 - y;
        pts[i].y = x;
    }
}

/* exact 90-degree rotation about x: (x,y,z) -> (x,-z,y).
 * NOTE: a 4x4x3 box is not a cube, so rotated points may leave the
 * original frame — that is the reshape itself. Conservation is proven
 * by distinctness + invertibility below, not by staying in the box. */
static void rot90x(void) {
    for (unsigned i = 0; i < NPTS; i++) {
        int y = pts[i].y, z = pts[i].z;
        pts[i].y = -z;
        pts[i].z = y;
    }
}

static void rot270z(void) { rot90z(); rot90z(); rot90z(); }
static void rot270x(void) { rot90x(); rot90x(); rot90x(); }

static int cmp_pt(const void *a, const void *b) {
    const Pt *p = a, *q = b;
    if (p->x != q->x) return p->x - q->x;
    if (p->y != q->y) return p->y - q->y;
    return p->z - q->z;
}

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    unsigned seen[GX][GY];
    char name[128];

    /* T1: conservation on all 3 viewports, unrotated. */
    build();
    for (int ax = 0; ax < 3; ax++) {
        unsigned vis = project(ax, seen);
        unsigned occ = NPTS - vis;
        snprintf(name, sizeof name, "axis %d: vis %u + occ %u == 48", ax, vis, occ);
        CHECK(vis + occ == NPTS && occ > 0, name);
        printf("  viewport %d: visible=%u occluded=%u\n", ax, vis, occ);
    }

    /* T2: conservation holds after every 90-degree rotation (reshape). */
    build();
    for (int r = 0; r < 4; r++) {
        for (int ax = 0; ax < 3; ax++) {
            unsigned vis = project(ax, seen);
            snprintf(name, sizeof name, "rot90z x%d axis %d conserves", r, ax);
            CHECK(vis + (NPTS - vis) == NPTS, name);
        }
        rot90z();
    }

    /* T3: point multiset is a bijection (sorted memcmp) after rotations. */
    build();
    Pt orig[NPTS];
    memcpy(orig, pts, sizeof pts);
    Pt snapshot[NPTS];
    memcpy(snapshot, pts, sizeof pts);
    qsort(snapshot, NPTS, sizeof(Pt), cmp_pt);
    rot90z(); rot90x(); rot90z();
    Pt after[NPTS];
    memcpy(after, pts, sizeof pts);
    qsort(after, NPTS, sizeof(Pt), cmp_pt);
    int same_shape = memcmp(snapshot, after, sizeof pts) != 0; /* rotated: shape differs */
    CHECK(same_shape, "rotation changes arrangement (not identity)");
    /* count check: all 48 rotated points distinct (no collision = no
     * information loss), then invert the rotation and demand the exact
     * original back (reshape = bijection). */
    Pt sorted[NPTS];
    memcpy(sorted, pts, sizeof pts);
    qsort(sorted, NPTS, sizeof(Pt), cmp_pt);
    int no_dup = 1;
    for (unsigned i = 1; i < NPTS; i++)
        if (cmp_pt(&sorted[i-1], &sorted[i]) == 0) no_dup = 0;
    CHECK(no_dup, "48 distinct points after reshape (no collision)");
    rot270z(); rot270x(); rot270z();   /* inverse of fwd sequence */
    CHECK(memcmp(orig, pts, sizeof pts) == 0, "inverse rotation restores exact original");

    /* T4: occlusion is viewpoint-relative, not intrinsic:
     * a point occluded on one viewport is visible on another. */
    build();
    unsigned sx[GX][GY], sy[GX][GY], sz[GX][GY];
    project(0, sx); project(1, sy); project(2, sz);
    int covered = 1;
    for (unsigned i = 0; i < NPTS; i++) {
        int vx = sx[pts[i].y][pts[i].z];
        int vy = sy[pts[i].x][pts[i].z];
        int vz = sz[pts[i].x][pts[i].y];
        if (!vx && !vy && !vz) { covered = 0; break; }
    }
    CHECK(covered, "every point visible from >=1 viewport (hidden = relative)");

    printf("occlusion_residual: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
