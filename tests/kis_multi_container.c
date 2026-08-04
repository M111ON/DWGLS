/* kis_multi_container.c — Multi-container placement on KIS field (20736 slots)
 * ═══════════════════════════════════════════════════════════════════════════
 * Tests: place 3+ containers at different W positions (W=0, W=1, W=5),
 *        read from each, verify no overlap, data integrity.
 * Uses kis_layer.h for slot/offset/layer calculations.
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "kis_layer.h"

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */
#define KIS_SNAP    20736u              /* 144 × 144 slots           */
#define ICO_VERTS   144u                /* spatial vertices (6ico)    */
#define FIELD_BYTES (KIS_SNAP * SLOT_SZ)/* 1,327,104 bytes            */

/* ═══════════════════════════════════════════════════════════════
   CONTAINER (FGLS-like)
   ═══════════════════════════════════════════════════════════════ */
#define FGLS_MAGIC 0x46474C53u  /* "FGLS" in little-endian */

typedef struct {
    uint32_t magic;
    uint32_t n_weights;
    uint32_t n_dims;
    uint32_t dims[4];       /* X, Y, Z, W */
    float    weights[];     /* flexible array member */
} Container;

static Container* container_create(uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, float fill_base)
{
    uint32_t n = x * y * z * w;
    Container *c = (Container*)malloc(sizeof(Container) + n * sizeof(float));
    if (!c) return NULL;
    c->magic     = FGLS_MAGIC;
    c->n_weights = n;
    c->n_dims    = 4;
    c->dims[0]   = x;
    c->dims[1]   = y;
    c->dims[2]   = z;
    c->dims[3]   = w;
    for (uint32_t i = 0; i < n; i++)
        c->weights[i] = fill_base + (float)i;
    return c;
}

static void container_free(Container *c) { free(c); }

/* ═══════════════════════════════════════════════════════════════
   KIS FIELD — 20736 slots, each SLOT_SZ bytes
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  data[FIELD_BYTES];           /* flat byte array          */
    uint32_t slot_owner[KIS_SNAP];        /* container index+1, 0=free */
    uint32_t n_used;
} KISField;

static void kis_field_init(KISField *f) {
    memset(f->data, 0, sizeof(f->data));
    memset(f->slot_owner, 0, sizeof(f->slot_owner));
    f->n_used = 0;
}

/* ── 4D mapping ──────────────────────────────────────────────
 * 4D (x,y,z,w) → flat slot index: vertex * 144 + scale
 * vertex = spatial position on 6ico (0..143)
 * scale  = W / temporal position (0..143)
 * ─────────────────────────────────────────────────────────── */
static uint32_t kis_slot_4d(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    (void)z;  /* z folded into vertex for 6ico mapping */
    uint32_t vertex = (x * 12 + y) % ICO_VERTS;
    uint32_t scale  = w % ICO_VERTS;
    return vertex * ICO_VERTS + scale;
}

/* ── Layer decomposition (uses kis_layer.h) ──────────────────
 * Flat slot index → (layer, k) using alternating ICO/DEC pattern.
 * ─────────────────────────────────────────────────────────── */
static void slot_to_layer_k(uint32_t slot, uint32_t *layer, uint32_t *k)
{
    uint32_t pair = slot / (ICO_SLOTS + DEC_SLOTS);   /* which 32-slot pair */
    uint32_t rem  = slot % (ICO_SLOTS + DEC_SLOTS);
    if (rem < ICO_SLOTS) {
        *layer = pair * 2;
        *k     = rem;
    } else {
        *layer = pair * 2 + 1;
        *k     = rem - ICO_SLOTS;
    }
}

/* Byte offset for a flat slot index (kis_layer.h macro) */
static uint64_t slot_byte_offset(uint32_t slot)
{
    uint32_t layer, k;
    slot_to_layer_k(slot, &layer, &k);
    return KIS_ADDR(layer, k);
}

/* ── Place / Get ───────────────────────────────────────────── */
static int kis_place(KISField *f, uint32_t slot, Container *c, uint32_t id)
{
    if (slot >= KIS_SNAP)          return -1;  /* out of range */
    if (f->slot_owner[slot] != 0) return -2;  /* already occupied */
    f->slot_owner[slot] = id + 1;
    uint64_t off = slot_byte_offset(slot);
    memcpy(f->data + off, &c, sizeof(c));
    f->n_used++;
    return 0;
}

static Container* kis_get(KISField *f, uint32_t slot)
{
    if (slot >= KIS_SNAP || f->slot_owner[slot] == 0)
        return NULL;
    uint64_t off = slot_byte_offset(slot);
    Container *c = NULL;
    memcpy(&c, f->data + off, sizeof(c));
    return c;
}

/* ═══════════════════════════════════════════════════════════════
   TEST HARNESS
   ═══════════════════════════════════════════════════════════════ */
static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name)   do { printf("  %-48s ", name); tests_run++; } while(0)
#define PASS()       do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg)    do { printf("[FAIL] %s\n", msg); } while(0)

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=================================================================\n");
    printf("  KIS Multi-Container Test — 20736-slot field\n");
    printf("=================================================================\n\n");

    /* Print kis_layer.h constants */
    printf("kis_layer.h constants:\n");
    printf("  ICO_SLOTS = %u   DEC_SLOTS = %u   SLOT_SZ = %u bytes\n",
           ICO_SLOTS, DEC_SLOTS, SLOT_SZ);
    printf("  KIS_SLOTS(0) = %u (ico)   KIS_SLOTS(1) = %u (dec)\n",
           KIS_SLOTS(0), KIS_SLOTS(1));
    printf("  KIS_TOTAL(0) = %lu   KIS_TOTAL(1) = %lu   KIS_TOTAL(2) = %lu\n",
           (unsigned long)KIS_TOTAL(0),
           (unsigned long)KIS_TOTAL(1),
           (unsigned long)KIS_TOTAL(2));
    printf("  Field size: %u slots × %u bytes = %lu bytes (%.1f MB)\n\n",
           KIS_SNAP, SLOT_SZ, (unsigned long)FIELD_BYTES,
           FIELD_BYTES / (1024.0 * 1024.0));

    KISField field;
    kis_field_init(&field);

    /* ══════════════════════════════════════════════════════════
       TEST 1 — Container Creation
       ══════════════════════════════════════════════════════════ */
    printf("-- Container Creation ---------------------------------\n");
    Container *c0 = container_create(4, 4, 4, 4, 0.0f);      /* 256 weights */
    Container *c1 = container_create(2, 2, 2, 2, 1000.0f);   /*  16 weights */
    Container *c2 = container_create(8, 2, 2, 1, 2000.0f);   /*  32 weights */
    Container *c3 = container_create(4, 2, 2, 2, 5000.0f);   /*  32 weights */

    TEST("create c0 (4x4x4x4, 256 weights)");
    if (c0 && c0->magic == FGLS_MAGIC && c0->n_weights == 256) PASS();
    else FAIL("bad c0");

    TEST("create c1 (2x2x2x2, 16 weights)");
    if (c1 && c1->magic == FGLS_MAGIC && c1->n_weights == 16) PASS();
    else FAIL("bad c1");

    TEST("create c2 (8x2x2x1, 32 weights)");
    if (c2 && c2->magic == FGLS_MAGIC && c2->n_weights == 32) PASS();
    else FAIL("bad c2");

    TEST("create c3 (4x2x2x2, 32 weights)");
    if (c3 && c3->magic == FGLS_MAGIC && c3->n_weights == 32) PASS();
    else FAIL("bad c3");
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 2 — 4D Slot Mapping
       ══════════════════════════════════════════════════════════ */
    printf("-- 4D Slot Mapping ------------------------------------\n");
    /* vertex=0, varying W */
    uint32_t slot_w0 = kis_slot_4d(0, 0, 0, 0);   /* slot 0   */
    uint32_t slot_w1 = kis_slot_4d(0, 0, 0, 1);   /* slot 1   */
    uint32_t slot_w5 = kis_slot_4d(0, 0, 0, 5);   /* slot 5   */

    TEST("W=0 maps to slot 0");
    if (slot_w0 == 0) PASS(); else FAIL("wrong");

    TEST("W=1 maps to slot 1");
    if (slot_w1 == 1) PASS(); else FAIL("wrong");

    TEST("W=5 maps to slot 5");
    if (slot_w5 == 5) PASS(); else FAIL("wrong");

    /* vertex=5, W=10 → slot = 5*144+10 = 730 */
    uint32_t slot_v5w10 = kis_slot_4d(0, 5, 0, 10);
    TEST("vertex=5, W=10 maps to slot 730");
    if (slot_v5w10 == 730) PASS(); else FAIL("wrong");

    /* vertex=10, W=100 → slot = 10*144+100 = 1540 */
    uint32_t slot_v10w100 = kis_slot_4d(0, 10, 0, 100);
    TEST("vertex=10, W=100 maps to slot 1540");
    if (slot_v10w100 == 1540) PASS(); else FAIL("wrong");

    printf("  Slot assignments: c0@%u, c1@%u, c2@%u, c3@%u\n\n",
           slot_w0, slot_w1, slot_w5, slot_v5w10);

    /* ══════════════════════════════════════════════════════════
       TEST 3 — Layer Decomposition (kis_layer.h)
       ══════════════════════════════════════════════════════════ */
    printf("-- Layer Decomposition (kis_layer.h) ------------------\n");
    /* Print decomposition for slots 0..31 and our container slots */
    printf("  Slot │ Layer │  k  │ KIS_ADDR\n");
    printf("  ─────┼───────┼─────┼─────────────\n");
    uint32_t demo_slots[] = {0, 1, 5, 19, 20, 31, 32, 730};
    for (int i = 0; i < 8; i++) {
        uint32_t sl = demo_slots[i];
        uint32_t la, kk;
        slot_to_layer_k(sl, &la, &kk);
        printf("  %4u │ %5u │ %3u │ %lu\n",
               sl, la, kk, (unsigned long)KIS_ADDR(la, kk));
    }
    printf("\n");

    TEST("slot 0  -> layer 0,  k 0");
    { uint32_t l,k; slot_to_layer_k(0,&l,&k);
      if (l==0 && k==0) PASS(); else FAIL("wrong"); }

    TEST("slot 19 -> layer 0,  k 19");
    { uint32_t l,k; slot_to_layer_k(19,&l,&k);
      if (l==0 && k==19) PASS(); else FAIL("wrong"); }

    TEST("slot 20 -> layer 1,  k 0");
    { uint32_t l,k; slot_to_layer_k(20,&l,&k);
      if (l==1 && k==0) PASS(); else FAIL("wrong"); }

    TEST("slot 31 -> layer 1,  k 11");
    { uint32_t l,k; slot_to_layer_k(31,&l,&k);
      if (l==1 && k==11) PASS(); else FAIL("wrong"); }

    TEST("slot 32 -> layer 2,  k 0");
    { uint32_t l,k; slot_to_layer_k(32,&l,&k);
      if (l==2 && k==0) PASS(); else FAIL("wrong"); }

    TEST("slot 730 -> layer 45, k 6");
    { uint32_t l,k; slot_to_layer_k(730,&l,&k);
      if (l==45 && k==6) PASS(); else FAIL("wrong"); }

    TEST("KIS_ADDR(slot) == slot * SLOT_SZ");
    {
        int ok = 1;
        for (uint32_t sl = 0; sl < 200; sl++) {
            if (slot_byte_offset(sl) != (uint64_t)sl * SLOT_SZ) {
                printf("\n    mismatch at slot %u\n", sl);
                ok = 0; break;
            }
        }
        if (ok) PASS(); else FAIL("mismatch");
    }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 4 — Place Containers
       ══════════════════════════════════════════════════════════ */
    printf("-- Place Containers on Field -------------------------\n");
    int rc;

    TEST("place c0 at W=0 (slot 0)");
    rc = kis_place(&field, slot_w0, c0, 0);
    if (rc == 0) PASS(); else FAIL("failed");

    TEST("place c1 at W=1 (slot 1)");
    rc = kis_place(&field, slot_w1, c1, 1);
    if (rc == 0) PASS(); else FAIL("failed");

    TEST("place c2 at W=5 (slot 5)");
    rc = kis_place(&field, slot_w5, c2, 2);
    if (rc == 0) PASS(); else FAIL("failed");

    TEST("place c3 at vertex=5, W=10 (slot 730)");
    rc = kis_place(&field, slot_v5w10, c3, 3);
    if (rc == 0) PASS(); else FAIL("failed");

    TEST("field n_used == 4");
    if (field.n_used == 4) PASS(); else FAIL("wrong count");
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 5 — No Overlap
       ══════════════════════════════════════════════════════════ */
    printf("-- Overlap Verification -------------------------------\n");

    TEST("all 4 slots are distinct");
    {
        uint32_t slots[] = {slot_w0, slot_w1, slot_w5, slot_v5w10};
        int distinct = 1;
        for (int i = 0; i < 4 && distinct; i++)
            for (int j = i+1; j < 4 && distinct; j++)
                if (slots[i] == slots[j]) distinct = 0;
        if (distinct) PASS(); else FAIL("slots collide");
    }

    TEST("byte offsets are non-overlapping");
    {
        uint64_t offs[] = { slot_byte_offset(slot_w0),
                            slot_byte_offset(slot_w1),
                            slot_byte_offset(slot_w5),
                            slot_byte_offset(slot_v5w10) };
        int non_overlap = 1;
        for (int i = 0; i < 4 && non_overlap; i++)
            for (int j = i+1; j < 4 && non_overlap; j++)
                if (offs[i] == offs[j]) non_overlap = 0;
        if (non_overlap) PASS(); else FAIL("offsets overlap");
    }

    TEST("offsets are SLOT_SZ-aligned");
    {
        int aligned = 1;
        for (int i = 0; i < 4; i++) {
            if (slot_byte_offset(kis_slot_4d(0,0,0,(uint32_t)i)) % SLOT_SZ != 0)
                aligned = 0;
        }
        if (aligned) PASS(); else FAIL("misaligned");
    }

    TEST("KIS_ADDR matches direct calc for slots 0,1,5");
    {
        uint64_t a0 = KIS_ADDR(0,0), a1 = KIS_ADDR(0,1), a5 = KIS_ADDR(0,5);
        if (a0 == 0 && a1 == 64 && a5 == 320) PASS(); else FAIL("mismatch");
    }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 6 — Read from Each Container
       ══════════════════════════════════════════════════════════ */
    printf("-- Read from Containers -------------------------------\n");

    TEST("retrieve c0 from slot 0");
    { Container *r = kis_get(&field, slot_w0);
      if (r && r->magic==FGLS_MAGIC && r->n_weights==256) PASS();
      else FAIL("bad c0"); }

    TEST("retrieve c1 from slot 1");
    { Container *r = kis_get(&field, slot_w1);
      if (r && r->magic==FGLS_MAGIC && r->n_weights==16) PASS();
      else FAIL("bad c1"); }

    TEST("retrieve c2 from slot 5");
    { Container *r = kis_get(&field, slot_w5);
      if (r && r->magic==FGLS_MAGIC && r->n_weights==32) PASS();
      else FAIL("bad c2"); }

    TEST("retrieve c3 from slot 730");
    { Container *r = kis_get(&field, slot_v5w10);
      if (r && r->magic==FGLS_MAGIC && r->n_weights==32) PASS();
      else FAIL("bad c3"); }

    TEST("empty slot 2 returns NULL");
    { Container *r = kis_get(&field, 2);
      if (r == NULL) PASS(); else FAIL("not NULL"); }

    TEST("empty slot 500 returns NULL");
    { Container *r = kis_get(&field, 500);
      if (r == NULL) PASS(); else FAIL("not NULL"); }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 7 — Data Integrity
       ══════════════════════════════════════════════════════════ */
    printf("-- Data Integrity -------------------------------------\n");

    TEST("c0.weights[0] == 0.0");
    { Container *r = kis_get(&field, slot_w0);
      if (r && r->weights[0] == 0.0f) PASS(); else FAIL("wrong"); }

    TEST("c0.weights[5] == 5.0");
    { Container *r = kis_get(&field, slot_w0);
      if (r && r->weights[5] == 5.0f) PASS(); else FAIL("wrong"); }

    TEST("c0.weights[255] == 255.0");
    { Container *r = kis_get(&field, slot_w0);
      if (r && r->weights[255] == 255.0f) PASS(); else FAIL("wrong"); }

    TEST("c1.weights[0] == 1000.0");
    { Container *r = kis_get(&field, slot_w1);
      if (r && r->weights[0] == 1000.0f) PASS(); else FAIL("wrong"); }

    TEST("c1.weights[15] == 1015.0");
    { Container *r = kis_get(&field, slot_w1);
      if (r && r->weights[15] == 1015.0f) PASS(); else FAIL("wrong"); }

    TEST("c2.weights[0] == 2000.0");
    { Container *r = kis_get(&field, slot_w5);
      if (r && r->weights[0] == 2000.0f) PASS(); else FAIL("wrong"); }

    TEST("c2.weights[31] == 2031.0");
    { Container *r = kis_get(&field, slot_w5);
      if (r && r->weights[31] == 2031.0f) PASS(); else FAIL("wrong"); }

    TEST("c3.weights[0] == 5000.0");
    { Container *r = kis_get(&field, slot_v5w10);
      if (r && r->weights[0] == 5000.0f) PASS(); else FAIL("wrong"); }

    TEST("c3.weights[31] == 5031.0");
    { Container *r = kis_get(&field, slot_v5w10);
      if (r && r->weights[31] == 5031.0f) PASS(); else FAIL("wrong"); }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 8 — Cross-Container Isolation
       ══════════════════════════════════════════════════════════ */
    printf("-- Cross-Container Isolation --------------------------\n");

    TEST("c0 and c1 are distinct pointers");
    { Container *r0 = kis_get(&field, slot_w0);
      Container *r1 = kis_get(&field, slot_w1);
      if (r0 && r1 && r0 != r1) PASS(); else FAIL("same/NULL"); }

    TEST("c0 weights unaffected by c1 placement");
    { Container *r0 = kis_get(&field, slot_w0);
      if (r0 && r0->weights[0] == 0.0f && r0->weights[255] == 255.0f)
          PASS(); else FAIL("corrupted"); }

    TEST("duplicate placement rejected (rc=-2)");
    { int rc2 = kis_place(&field, slot_w0, c0, 0);
      if (rc2 == -2) PASS(); else FAIL("not rejected"); }

    TEST("out-of-range slot rejected (rc=-1)");
    { int rc2 = kis_place(&field, KIS_SNAP, c0, 0);
      if (rc2 == -1) PASS(); else FAIL("not rejected"); }

    TEST("n_used still 4 after rejected placements");
    if (field.n_used == 4) PASS(); else FAIL("wrong count");
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 9 — Stress: fill slots across W positions
       ══════════════════════════════════════════════════════════ */
    printf("-- Stress: Multiple W Positions ----------------------\n");
    {
        uint32_t w_positions[] = {0, 1, 5, 10, 20, 50, 100, 143};
        int n_placed = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t sl = kis_slot_4d(0, 0, 0, w_positions[i]);
            if (sl < KIS_SNAP && field.slot_owner[sl] == 0) {
                Container *tmp = container_create(2,1,1,1, (float)(i*100));
                if (tmp && kis_place(&field, sl, tmp, (uint32_t)(4+i)) == 0)
                    n_placed++;
            }
        }
        TEST("placed 8 mini-containers at W={0,1,5,10,20,50,100,143}");
        /* Some W values (0,1,5) are already occupied, so 5 will be placed */
        if (n_placed == 5) PASS();
        else { char buf[64]; sprintf(buf, "placed %d, expected 5", n_placed); FAIL(buf); }

        TEST("field n_used == 9 (4 original + 5 new)");
        if (field.n_used == 9) PASS(); else FAIL("wrong count");

        TEST("all 9 occupied slots readable");
        {
            int ok = 1;
            for (uint32_t s = 0; s < KIS_SNAP && ok; s++) {
                if (field.slot_owner[s] != 0) {
                    Container *r = kis_get(&field, s);
                    if (!r || r->magic != FGLS_MAGIC) ok = 0;
                }
            }
            if (ok) PASS(); else FAIL("read failure");
        }
    }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       TEST 10 — Layer Boundary Crossing
       ══════════════════════════════════════════════════════════ */
    printf("-- Layer Boundary Crossing ----------------------------\n");
    {
        /* slot 19 is last in layer 0, slot 20 is first in layer 1 */
        uint32_t l19, k19, l20, k20;
        slot_to_layer_k(19, &l19, &k19);
        slot_to_layer_k(20, &l20, &k20);

        TEST("slot 19 and 20 are in different layers");
        if (l19 != l20) PASS(); else FAIL("same layer");

        TEST("slot 19 k = ICO_SLOTS-1 = 19");
        if (k19 == ICO_SLOTS - 1) PASS(); else FAIL("wrong k");

        TEST("slot 20 k = 0 in layer 1");
        if (l20 == 1 && k20 == 0) PASS(); else FAIL("wrong");

        TEST("byte addresses don't overlap at boundary");
        { uint64_t a19 = slot_byte_offset(19);
          uint64_t a20 = slot_byte_offset(20);
          if (a19 + SLOT_SZ == a20) PASS(); else FAIL("gap/overlap"); }
    }
    printf("\n");

    /* ══════════════════════════════════════════════════════════
       SUMMARY
       ══════════════════════════════════════════════════════════ */
    printf("=================================================================\n");
    printf("  RESULTS: %d / %d tests passed\n", tests_passed, tests_run);
    printf("=================================================================\n");

    /* Cleanup (containers placed in field are tracked by pointers) */
    container_free(c0);
    container_free(c1);
    container_free(c2);
    container_free(c3);

    return (tests_passed == tests_run) ? 0 : 1;
}
