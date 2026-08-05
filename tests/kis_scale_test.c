/*
 * kis_scale_test.c — Scale operation on KIS field
 *
 * Tests that changing W position (scale) moves data correctly
 * through the 4D mapping: slot = vertex * 144 + scale
 *
 * Demonstrates:
 *   1. Place container at W=0, write values
 *   2. Scale to W=5 — values appear at new W
 *   3. Scale to W=10 — values appear at new W
 *   4. W=0 original data preserved (different slot)
 *   5. Different W = different slot = different viewpoint
 *
 * Compile:
 *   gcc -O2 -Wall -Icore -o tests/kis_scale_test.exe tests/kis_scale_test.c
 * Run:
 *   tests/kis_scale_test.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "kis_layer.h"

/* ═══════════════════════════════════════════════════════════════
   4D COORDINATE SYSTEM
   Approach C from kis_4d_explore.c:
     slot = vertex × 144 + scale
     vertex = spatial position (0..143)
     scale  = W dimension / temporal position (0..143)
     144 × 144 = 20736 = SNAP
   ═══════════════════════════════════════════════════════════════ */

#define VERTICES   144u   /* spatial positions (6 × ico-24) */
#define W_SLOTS    144u   /* scale / temporal positions */
#define SNAP       (VERTICES * W_SLOTS)  /* 20736 */

/* Map (vertex, W) → slot index on the KIS field */
static inline uint32_t kis_slot4d(uint32_t vertex, uint32_t w) {
    return vertex * W_SLOTS + (w % W_SLOTS);
}

/* Inverse: slot → (vertex, W) */
static inline void kis_unslot4d(uint32_t slot, uint32_t *vertex, uint32_t *w) {
    *vertex = slot / W_SLOTS;
    *w      = slot % W_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════
   KIS FIELD — 20736 slots, each SLOT_SZ bytes
   Uses kis_layer.h SLOT_SZ = 64 bytes per slot
   ═══════════════════════════════════════════════════════════════ */

/* Each slot holds SLOT_SZ bytes. We store a small float vector (16 floats = 64B) */
typedef struct {
    float data[SLOT_SZ / sizeof(float)];  /* 16 floats */
} Slot;

typedef struct {
    Slot  slots[SNAP];    /* 20736 × 64 bytes = 1.3 MB */
    uint8_t occupied[SNAP]; /* 1-bit occupancy per slot */
} KISField;

static void kis_field_init(KISField *f) {
    memset(f, 0, sizeof(KISField));
}

/* Write a value into a specific vertex+W position */
static void kis_write(KISField *f, uint32_t vertex, uint32_t w, uint32_t idx, float val) {
    uint32_t slot = kis_slot4d(vertex, w);
    assert(slot < SNAP);
    assert(idx < SLOT_SZ / sizeof(float));
    f->slots[slot].data[idx] = val;
}

/* Read a value from a specific vertex+W position */
static float kis_read(const KISField *f, uint32_t vertex, uint32_t w, uint32_t idx) {
    uint32_t slot = kis_slot4d(vertex, w);
    assert(slot < SNAP);
    assert(idx < SLOT_SZ / sizeof(float));
    return f->slots[slot].data[idx];
}

/* Mark a slot as occupied */
static void kis_occupy(KISField *f, uint32_t vertex, uint32_t w) {
    uint32_t slot = kis_slot4d(vertex, w);
    assert(slot < SNAP);
    f->occupied[slot] = 1;
}

/* Check if a slot is occupied */
static int kis_is_occupied(const KISField *f, uint32_t vertex, uint32_t w) {
    uint32_t slot = kis_slot4d(vertex, w);
    assert(slot < SNAP);
    return f->occupied[slot] != 0;
}

/* ═══════════════════════════════════════════════════════════════
   SCALE OPERATION
   "Scale" = change W position (temporal position)
   Copy container data from old_w → new_w for all vertices
   ═══════════════════════════════════════════════════════════════ */

/* Scale: copy data from (start_vertex..start_vertex+n-1, old_w) → same vertices, new_w */
static void kis_scale(KISField *f, uint32_t old_w, uint32_t new_w,
                      int n_vertices, int start_vertex) {
    for (int v = 0; v < n_vertices; v++) {
        uint32_t sv = (uint32_t)(start_vertex + v);
        uint32_t src_slot = kis_slot4d(sv, old_w);
        uint32_t dst_slot = kis_slot4d(sv, new_w);

        /* Copy entire slot (64 bytes = 16 floats) */
        memcpy(&f->slots[dst_slot], &f->slots[src_slot], SLOT_SZ);
        f->occupied[dst_slot] = 1;
    }
}

/* Scale single vertex: (vertex, old_w) → (vertex, new_w) */
static void kis_scale_vertex(KISField *f, uint32_t vertex, uint32_t old_w, uint32_t new_w) {
    uint32_t src_slot = kis_slot4d(vertex, old_w);
    uint32_t dst_slot = kis_slot4d(vertex, new_w);
    memcpy(&f->slots[dst_slot], &f->slots[src_slot], SLOT_SZ);
    f->occupied[dst_slot] = 1;
}

/* ═══════════════════════════════════════════════════════════════
   VERIFICATION HELPERS
   ═══════════════════════════════════════════════════════════════ */

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(n, desc, cond) do { \
    if (cond) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* Compare two vertex+W positions, check all 16 floats match */
static int slots_match(const KISField *f, uint32_t v1, uint32_t w1,
                       uint32_t v2, uint32_t w2) {
    uint32_t s1 = kis_slot4d(v1, w1);
    uint32_t s2 = kis_slot4d(v2, w2);
    return memcmp(&f->slots[s1], &f->slots[s2], SLOT_SZ) == 0;
}

/* Print all 16 floats in a slot */
static void print_slot(const KISField *f, uint32_t vertex, uint32_t w, const char *label) {
    uint32_t slot = kis_slot4d(vertex, w);
    printf("  [%s] slot=%u (v=%u, w=%u): ", label, slot, vertex, w);
    for (int i = 0; i < 16; i++) {
        printf("%.1f ", f->slots[slot].data[i]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN TEST
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS Scale Test — Changing W Position\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("4D mapping: slot = vertex × %u + scale (W)\n", W_SLOTS);
    printf("SNAP = %u slots × SLOT_SZ=%u bytes = %u KB\n\n",
           SNAP, SLOT_SZ, (SNAP * SLOT_SZ) / 1024);

    KISField field;
    kis_field_init(&field);

    /* ── Test 1: Address mapping ──────────────────────────────── */
    printf("T1: Address Mapping\n");
    uint32_t slot_w0 = kis_slot4d(0, 0);
    uint32_t slot_v0_w5 = kis_slot4d(0, 5);
    uint32_t slot_v1_w0 = kis_slot4d(1, 0);
    CHECK(1, "slot(0,0) = 0", slot_w0 == 0);
    CHECK(2, "slot(0,5) = 5", slot_v0_w5 == 5);
    CHECK(3, "slot(1,0) = 144", slot_v1_w0 == 144);
    CHECK(4, "slot(0,0) != slot(0,5)", slot_w0 != slot_v0_w5);
    printf("    slot(0,0)=%u, slot(0,5)=%u, slot(1,0)=%u\n\n",
           slot_w0, slot_v0_w5, slot_v1_w0);

    /* ── Test 2: Place container at W=0, write values ─────────── */
    printf("T2: Place at W=0 and Write\n");
    /* Write distinct values into vertices 0..3 at W=0 */
    float expected[] = {10.0f, 20.0f, 30.0f, 40.0f};
    for (int v = 0; v < 4; v++) {
        kis_write(&field, (uint32_t)v, 0, 0, expected[v]);
        kis_occupy(&field, (uint32_t)v, 0);
    }
    CHECK(5, "vertex 0 at W=0", kis_read(&field, 0, 0, 0) == 10.0f);
    CHECK(6, "vertex 1 at W=0", kis_read(&field, 1, 0, 0) == 20.0f);
    CHECK(7, "vertex 2 at W=0", kis_read(&field, 2, 0, 0) == 30.0f);
    CHECK(8, "vertex 3 at W=0", kis_read(&field, 3, 0, 0) == 40.0f);
    CHECK(9, "all 4 slots occupied at W=0",
          kis_is_occupied(&field, 0, 0) &&
          kis_is_occupied(&field, 1, 0) &&
          kis_is_occupied(&field, 2, 0) &&
          kis_is_occupied(&field, 3, 0));
    print_slot(&field, 0, 0, "vertex 0, W=0");
    print_slot(&field, 1, 0, "vertex 1, W=0");
    printf("\n");

    /* ── Test 3: Scale W=0 → W=5 ─────────────────────────────── */
    printf("T3: Scale W=0 → W=5\n");
    kis_scale(&field, 0, 5, 4, 0);

    /* Verify W=5 has same values */
    CHECK(10, "vertex 0 at W=5 == 10.0", kis_read(&field, 0, 5, 0) == 10.0f);
    CHECK(11, "vertex 1 at W=5 == 20.0", kis_read(&field, 1, 5, 0) == 20.0f);
    CHECK(12, "vertex 2 at W=5 == 30.0", kis_read(&field, 2, 5, 0) == 30.0f);
    CHECK(13, "vertex 3 at W=5 == 40.0", kis_read(&field, 3, 5, 0) == 40.0f);

    /* Verify W=0 still has original values (data preserved) */
    CHECK(14, "W=0 preserved — vertex 0 still 10.0",
          kis_read(&field, 0, 0, 0) == 10.0f);
    CHECK(15, "W=0 preserved — vertex 3 still 40.0",
          kis_read(&field, 3, 0, 0) == 40.0f);

    /* Verify slots are different (different W = different slot) */
    CHECK(16, "slot(0,0) != slot(0,5)",
          kis_slot4d(0, 0) != kis_slot4d(0, 5));

    /* Verify full slot match */
    CHECK(17, "full slot match v=0 W=0↔W=5",
          slots_match(&field, 0, 0, 0, 5));
    CHECK(18, "full slot match v=1 W=0↔W=5",
          slots_match(&field, 1, 0, 1, 5));

    print_slot(&field, 0, 5, "vertex 0, W=5 (scaled)");
    print_slot(&field, 1, 5, "vertex 1, W=5 (scaled)");
    printf("\n");

    /* ── Test 4: Scale W=5 → W=10 ────────────────────────────── */
    printf("T4: Scale W=5 → W=10\n");
    kis_scale(&field, 5, 10, 4, 0);

    /* Verify W=10 has the values */
    CHECK(19, "vertex 0 at W=10 == 10.0", kis_read(&field, 0, 10, 0) == 10.0f);
    CHECK(20, "vertex 1 at W=10 == 20.0", kis_read(&field, 1, 10, 0) == 20.0f);
    CHECK(21, "vertex 2 at W=10 == 30.0", kis_read(&field, 2, 10, 0) == 30.0f);
    CHECK(22, "vertex 3 at W=10 == 40.0", kis_read(&field, 3, 10, 0) == 40.0f);

    /* W=0 and W=5 still preserved */
    CHECK(23, "W=0 still intact", kis_read(&field, 0, 0, 0) == 10.0f);
    CHECK(24, "W=5 still intact", kis_read(&field, 0, 5, 0) == 10.0f);

    print_slot(&field, 0, 10, "vertex 0, W=10 (scaled)");
    print_slot(&field, 3, 10, "vertex 3, W=10 (scaled)");
    printf("\n");

    /* ── Test 5: Different W = different slot = different viewpoint */
    printf("T5: Different W = Different Slot = Different Viewpoint\n");
    uint32_t slot_v0_w0_5  = kis_slot4d(0, 0);
    uint32_t slot_v0_w5_5  = kis_slot4d(0, 5);
    uint32_t slot_v0_w10_5 = kis_slot4d(0, 10);
    CHECK(25, "3 distinct slots for vertex 0 at W=0,5,10",
          slot_v0_w0_5 != slot_v0_w5_5 && slot_v0_w5_5 != slot_v0_w10_5 && slot_v0_w0_5 != slot_v0_w10_5);
    CHECK(26, "W gap = 144 × (W difference)",
          slot_v0_w5_5 - slot_v0_w0_5 == 5 && slot_v0_w10_5 - slot_v0_w5_5 == 5);
    printf("    slot(0,0)=%u, slot(0,5)=%u, slot(0,10)=%u\n",
           slot_v0_w0_5, slot_v0_w5_5, slot_v0_w10_5);
    printf("    Gap: %u slots (= W diff × 1)\n\n", slot_v0_w5_5 - slot_v0_w0_5);

    /* ── Test 6: Single vertex scale ──────────────────────────── */
    printf("T6: Single Vertex Scale (isolated operation)\n");
    kis_write(&field, 5, 0, 0, 99.0f);  /* vertex 5 at W=0 */
    kis_occupy(&field, 5, 0);
    kis_scale_vertex(&field, 5, 0, 20);  /* → W=20 */
    CHECK(27, "vertex 5 at W=20 == 99.0", kis_read(&field, 5, 20, 0) == 99.0f);
    CHECK(28, "vertex 5 at W=0 still 99.0", kis_read(&field, 5, 0, 0) == 99.0f);
    printf("\n");

    /* ── Test 7: Verify occupancy bitmap ──────────────────────── */
    printf("T7: Occupancy Verification\n");
    int occ_count = 0;
    for (uint32_t s = 0; s < SNAP; s++) {
        if (field.occupied[s]) occ_count++;
    }
    /* vertices 0-3 at W=0,5,10 + vertex 5 at W=0,20 = 4×3 + 2 = 14 occupied */
    CHECK(29, "14 slots occupied", occ_count == 14);
    CHECK(30, "W=0 slots occupied for v=0..3",
          kis_is_occupied(&field, 0, 0) &&
          kis_is_occupied(&field, 3, 0));
    CHECK(31, "W=5 slots occupied for v=0..3",
          kis_is_occupied(&field, 0, 5) &&
          kis_is_occupied(&field, 3, 5));
    CHECK(32, "W=10 slots occupied for v=0..3",
          kis_is_occupied(&field, 0, 10) &&
          kis_is_occupied(&field, 3, 10));
    printf("    Total occupied: %d / %u slots\n\n", occ_count, SNAP);

    /* ── Test 8: Multi-field data at same W ───────────────────── */
    printf("T8: Multi-Vertex Write at Same W\n");
    /* Write different data to vertex 10..13 at W=0 */
    float vdata[] = {1.1f, 2.2f, 3.3f, 4.4f};
    for (int v = 0; v < 4; v++) {
        kis_write(&field, (uint32_t)(v + 10), 0, 0, vdata[v]);
        kis_occupy(&field, (uint32_t)(v + 10), 0);
    }
    kis_scale(&field, 0, 30, 4, 10);  /* v=10..13, W=0 → W=30 */
    CHECK(33, "vertex 10 at W=30 == 1.1", kis_read(&field, 10, 30, 0) == 1.1f);
    CHECK(34, "vertex 13 at W=30 == 4.4", kis_read(&field, 13, 30, 0) == 4.4f);
    CHECK(35, "vertex 10 at W=0 still 1.1", kis_read(&field, 10, 0, 0) == 1.1f);
    print_slot(&field, 10, 30, "vertex 10, W=30 (new data)");
    printf("\n");

    /* ═══════════════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("═══════════════════════════════════════════════════════════════\n\n");

    printf("KEY INSIGHTS:\n");
    printf("  1. W = temporal position — each W value is a separate viewpoint\n");
    printf("  2. slot = vertex × %u + W — W changes slot linearly\n", W_SLOTS);
    printf("  3. Scale copies data between W positions without destroying originals\n");
    printf("  4. Data at W=0, W=5, W=10 coexist — same vertex, different time slices\n");
    printf("  5. SLOT_SZ=%u bytes per slot — each W position is a full 64-byte container\n", SLOT_SZ);

    return fail_count;
}
