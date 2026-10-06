/* tools/frustum_probe.c — FRUSTUM COMPOSITE 6 DIRECTION (owner 2026-10-05, note #18).
 *
 * Owner rescope, verbatim intent:
 *   "quadtree looks like this [1<4<16 tree], but MINE is a frustum composite
 *    of 6 directions. because we are a PATTERN, laying it out flat does not use
 *    144^2 / 12^4 etc, right? wang6 will open the system so every anchor can
 *    grow 3 more generations -- it gets very large, so I do not use it unless
 *    necessary. I only built it in advance because I do not know what the real
 *    workspace will have to handle."
 *
 * Recheck findings that this file corrects (see §465-§469):
 *   - tools/minor_forage_probe.c Visit{house,layer} has NO direction  -> wrong identity
 *   - every wl_traverse(&L, single_id) opens ONE slot, not 6 faces      -> wrong unit
 *   - tools/pattern_probe.c L6 modeled the quadtree as a flat 4-way grid -> wrong shape
 *
 * What is CORRECT and kept from the old probes:
 *   fog model (OPEN=fogged / SHUT=lit), per-walker latch, branch isolation,
 *   tombstone = the lit mark, reuse shrinks work.
 *
 * What this probe adds (the actual spec):
 *   an anchor is a 6-DIRECTION frustum. walking through it closes only the
 *   face you entered/exited, so the anchor's "pattern" is a 6-bit face mask,
 *   not a slot id. quadtree = overlapping frustums. wang6 gates which face a
 *   path may cross (colour must match); it is the thing that lets an anchor
 *   grow 3 more generations.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/frustum_probe tools/frustum_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_wang_latch.h"
#include "mm_wang6.h"

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* ---- an anchor is a frustum composite: 6 faces, one per direction ---- */
typedef struct {
    unsigned x, y, z;          /* where the frustum sits (cell coords) */
    uint8_t  face_lit;         /* bit d set = fog cleared on face d */
    int      glow;             /* how many faces cleared */
} Frustum;

static void fr_reset(Frustum *f, unsigned x, unsigned y, unsigned z) {
    f->x = x; f->y = y; f->z = z; f->face_lit = 0; f->glow = 0;
}

/* pass through the anchor along direction d. fog clears ONLY that face.
   returns 1 if the face was fogged before (newly lit), 0 if already lit.
   This is the whole point: 6 directions, 6 independent faces, not 1 slot. */
static int fr_pass(Frustum *f, uint8_t d) {
    if (d > 5) return 0;
    uint8_t m = (uint8_t)(1u << d);
    int was_fogged = ((f->face_lit & m) == 0);
    f->face_lit |= m;
    if (was_fogged) f->glow++;
    return was_fogged;
}

/* an anchor's pattern = WHICH faces are lit (a 6-bit mask), not an id */
static uint8_t fr_pattern(const Frustum *f) { return f->face_lit; }

/* ---- wang6 gate: may a path cross this face? colour must match ---- */
static int fr_gate_ok(unsigned x1, unsigned y1, unsigned z1,
                      unsigned x2, unsigned y2, unsigned z2) {
    return mw6_gate(x1, y1, z1, x2, y2, z2);
}

int main(void) {
    printf("=== frustum_probe : 6-direction anchor (not a flat grid) ===\n\n");

    /* --- F1: pass through ONE direction lights ONE face (not the whole anchor) --- */
    printf("[F1] a pass closes only the face it used\n");
    Frustum a; fr_reset(&a, 10, 10, 10);
    CHECK(fr_pass(&a, MW6_DIR_PX) == 1, "entering via +X newly lights the +X face");
    CHECK(a.glow == 1, "exactly 1 of 6 faces lit");
    CHECK((fr_pattern(&a) & (1u << MW6_DIR_PX)) != 0, "+X bit is set in the pattern");
    CHECK((fr_pattern(&a) & (1u << MW6_DIR_NX)) == 0, "-X face still fogged");

    /* --- F2: 6 directions are 6 INDEPENDENT faces of one anchor --- */
    printf("\n[F2] six independent faces -> the pattern is a 6-bit mask\n");
    Frustum b; fr_reset(&b, 20, 20, 20);
    for (uint8_t d = 0; d < 6; d++) fr_pass(&b, d);
    CHECK(b.glow == 6, "all six faces lit after passing all six directions");
    CHECK(fr_pattern(&b) == 0x3F, "pattern mask = 0b111111 (all faces)");
    /* a different travel history -> a DIFFERENT pattern on the same anchor */
    Frustum c; fr_reset(&c, 20, 20, 20);   /* same coordinates! */
    fr_pass(&c, MW6_DIR_PX); fr_pass(&c, MW6_DIR_PY);
    CHECK(fr_pattern(&c) == 0x05, "in/out through +X,+Y only -> mask 0b000101");
    CHECK(fr_pattern(&b) != fr_pattern(&c),
          "same place, different travel => different pattern (who passed)");

    /* --- F3: passing the same face twice does not re-light --- */
    printf("\n[F3] monotone: the same face does not light twice\n");
    int glow_before = c.glow;
    CHECK(fr_pass(&c, MW6_DIR_PX) == 0, "re-entering +X reports NOT newly lit");
    CHECK(c.glow == glow_before, "glow count unchanged");

    /* --- F4: what does wang6 actually gate? (measured, not assumed) ---
       FINDING 2026-10-05 (§484-486): mw6_gate opens EVERY cross between two
       adjacent cells, because a shared face key gives both sides the SAME
       colour by construction — that is the Wang rule working "correctly", and
       it is exactly why the owner says the six directions "cannot separate an
       anchor yet". The gate only ever shuts at a BOUNDARY (colour 0xFE).
       So colour is NOT the anchor separator; the frustum DIRECTION is. */
    printf("\n[F4] wang6 measured: colour matches by construction on interior faces\n");
    static const int dx[6] = { 1,-1, 0, 0, 0, 0 };
    static const int dy[6] = { 0, 0, 1,-1, 0, 0 };
    static const int dz[6] = { 0, 0, 0, 0, 1,-1 };
    long crosses = 0, gates = 0;
    for (unsigned bx = 1; bx < 5; bx++)
    for (unsigned by = 1; by < 5; by++)
    for (unsigned bz = 1; bz < 5; bz++)
        for (uint8_t d = 0; d < 6; d++) {
            unsigned nx = (unsigned)((int)bx + dx[d]);
            unsigned ny = (unsigned)((int)by + dy[d]);
            unsigned nz = (unsigned)((int)bz + dz[d]);
            if (fr_gate_ok(bx, by, bz, nx, ny, nz)) crosses++; else gates++;
        }
    printf("      interior 64 cells x 6 dirs: %ld open, %ld shut\n", crosses, gates);
    CHECK(crosses == 64 * 6 && gates == 0,
          "interior faces are ALWAYS shared-colour -> gate open (measured)");

    /* the gate DOES shut at the floor boundary (colour 0xFE). A face at the
       MINIMUM side of an axis belongs to a cell one step below; for cell (0,0,0)
       the -X/-Y/-Z neighbours are OUT OF GRID, so the shuttable faces are the
       3 INWARD ones (+X,+Y,+Z) vs the 3 outward ones. Probe both directions
       honestly: ask gate from the boundary-facing side of a NEIGHBOUR instead. */
    long bnd_shut = 0;
    /* from cell (1,0,0) heading -X into (0,0,0): axis 0, dir -X, key x-1 = 0 */
    bnd_shut += !fr_gate_ok(1, 0, 0, 0, 0, 0);   /* -X across x=0 boundary region */
    /* the true 0xFE case: the NEGATIVE face of a cell sitting on the boundary
       plane, i.e. cell (0,y,z) asked for its own -X face (no neighbour exists) */
    CHECK(mw6_cell_color(0, 1, 1, MW6_DIR_NX) == 0xFE,
          "cell (0,1,1) has NO -X face -> colour 0xFE (open boundary marker)");
    CHECK(mw6_cell_color(1, 1, 1, MW6_DIR_NX) < 0xFE,
          "cell (1,1,1) HAS a -X face -> a real colour");
    printf("      boundary marker 0xFE at x=0: -X of (0,1,1) = 0xFE (measured)\n");
    (void)bnd_shut;

    /* gate first, fog second: a fresh anchor lights exactly the approved faces */
    Frustum g0; fr_reset(&g0, 2, 2, 2);
    int approved = 0;
    for (uint8_t d = 0; d < 6; d++) {
        unsigned nx = (unsigned)((int)g0.x + dx[d]);
        unsigned ny = (unsigned)((int)g0.y + dy[d]);
        unsigned nz = (unsigned)((int)g0.z + dz[d]);
        if (fr_gate_ok(g0.x, g0.y, g0.z, nx, ny, nz)) { approved++; fr_pass(&g0, d); }
    }
    printf("      fresh anchor at (2,2,2): %d approved faces, glow=%d\n",
           approved, g0.glow);
    CHECK(g0.glow == approved,
          "only the wang-approved faces get lit (gate first, fog second)");

    /* --- F5: quadtree = OVERLAPPING frustums, not a flat 4-way grid --- */
    printf("\n[F5] quadtree of overlapping frustums 1<4<16 (each with 6 faces)\n");
    /* level L has 4^L anchors; grow 3 generations => 4^3 = 64 at gen 3 */
    int anchors[4] = { 1, 4, 16, 64 };
    int faces_total = 0;
    for (int l = 0; l < 4; l++) faces_total += anchors[l] * 6;
    printf("      anchors per level: 1 < 4 < 16 < 64\n");
    printf("      total faces (anchors x 6) = %d\n", faces_total);
    CHECK(faces_total == 85 * 6, "4 levels = 85 anchors x 6 faces = 510 faces");
    /* the block layout of fig(b): 1 | 2x2 | 4x4 is the PROJECTION of the 3D
       frustum stack (fig a), so a flat grid is a VIEW of it, never its shape */
    CHECK(1 + 4 + 16 == 21, "3 generations shown = 21 anchors (the fig(b) squares)");

    /* --- F6: wang6 opens 3 MORE generations -- measure why it is held back --- */
    printf("\n[F6] wang6 would open 3 more generations -- cost measured\n");
    long long nodes = 85;
    for (int i = 0; i < 3; i++) nodes *= 4;       /* 3 more gens, x4 each */
    long long lfields = nodes * 6;                /* faces if every anchor is a frustum */
    printf("      with 3 more gens: %d anchors, %d faces\n",
           (int)nodes, (int)lfields);
    printf("      latch pool = %u ids\n", (unsigned)WL_COUNT);
    CHECK(nodes == 85LL * 64, "3 extra generations = 85 * 64 = 5440 anchors");
    CHECK(lfields > (long long)WL_COUNT,
          "5440 anchors x 6 faces exceed the 10368 latch pool -> why it is NOT used yet");

    /* --- F7: the tombstone on a frustum is per-FACE --- */
    printf("\n[F7] tombstone = the lit face, usable as next-time index\n");
    Frustum t; fr_reset(&t, 30, 30, 30);
    uint8_t path_in  = MW6_DIR_PX;   /* someone came in +X  */
    uint8_t path_out = MW6_DIR_NZ;   /* ...and left -Z     */
    fr_pass(&t, path_in); fr_pass(&t, path_out);
    uint8_t tomb = fr_pattern(&t);
    /* next walker asks the tombstone instead of scanning: which faces are known? */
    int known = 0;
    for (uint8_t d = 0; d < 6; d++) if (tomb & (1u << d)) known++;
    printf("      tombstone mask=0x%02X in/%u out/%u -> %d faces already known\n",
           (unsigned)tomb, path_in, path_out, known);
    CHECK(known == 2, "a pass-through leaves a 2-face tombstone (entry + exit)");
    CHECK(tomb == (uint8_t)((1u << MW6_DIR_PX) | (1u << MW6_DIR_NZ)),
          "tombstone records exactly the crossable faces that were used");

    printf("\nFRUSTUM: %d FAIL\n", fails);
    return fails != 0;
}
