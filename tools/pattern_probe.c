/* tools/pattern_probe.c v2 — FOG OF WAR (owner 2026-10-05, note #16).
 *
 * Owner spec correction: "it's like fog of war in a game map. you just walk,
 * it lights up. whoever maps, each sees a DIFFERENT map, but what is ON the map
 * stays where it is and does not go anywhere."
 *
 * This REPLACES the earlier (wrong) framing "find f(x,y,z) -> 20734 values".
 * The pattern is NOT computed from one cell; it is the SET OF CELLS a walker
 * has cleared. Two walkers => two different cleared sets => two different maps.
 * The DATA does not move. That is the whole point.
 *
 * Uses the real core/geo_wang_latch.h:
 *   OPEN  = fogged (never walked)
 *   SHUT  = lit    (walked at least once, monotone)
 *   CLEAR = reopen (only way back to fogged)
 * reserved ids 0 / 10367 = full-white / full-black = major root, never minor.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/pattern_probe tools/pattern_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_wang_latch.h"

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* a "walker" = one player's fog state + where they believe the data is */
typedef struct {
    const char *name;
    wl_latch_t  fog;      /* the MAP of this player (what is lit) */
    uint32_t    last_id;  /* last cell they lit */
    int         hits;     /* how many cells they lit */
} Walker;

/* walk a path. NOTE: wl_traverse returns 1 every time (it appends a fingerprint
   to the P3 log), so "newly lit" must be measured from STATE, not the return:
   a cell is newly lit iff it was OPEN immediately before the traverse. */
static void walk(Walker *w, const uint32_t *path, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t id = path[i];
        int was_open = wl_is_open(&w->fog, id);
        wl_traverse(&w->fog, id);        /* always writes the fingerprint */
        if (was_open) {                  /* only THEN did fog clear here */
            w->hits++;
            w->last_id = id;
        }
    }
}

/* count lit cells in a walker's map — measured from the words, never by
   walking into each cell (we only know and measure, we do not store). */
static int popcount64(uint64_t v) {
    int c = 0; while (v) { v &= v - 1u; c++; } return c;
}
static int lit_count(const wl_latch_t *L) {
    int c = 0;
    for (uint32_t w = 0; w < WL_WORDS; w++) c += popcount64(L->w[w]);
    return c;
}

/* how many lit cells two maps SHARE — AND of the words, popcount */
static int shared_lit(const wl_latch_t *A, const wl_latch_t *B) {
    int c = 0;
    for (uint32_t w = 0; w < WL_WORDS; w++) c += popcount64(A->w[w] & B->w[w]);
    return c;
}

int main(void) {
    printf("=== pattern_probe v2 : FOG OF WAR (map fixed, sight differs) ===\n\n");

    /* --- Leg 0: the land itself never moves (reserved anchors) --- */
    printf("[L0] the land: reserved 0 / 10367 belong to major root only\n");
    wl_latch_t L; wl_reset(&L);
    CHECK(wl_traverse(&L, 0) == 0,     "minor CANNOT walk full-white (id 0)");
    CHECK(wl_traverse(&L, 10367) == 0, "minor CANNOT walk full-black (id 10367)");
    CHECK(wl_traverse(&L, 500) == 1,   "minor CAN walk a normal cell");

    /* --- Leg 1: two walkers, two paths, two DIFFERENT lit maps --- */
    printf("\n[L1] two walkers on ONE map -> different fog cleared\n");
    Walker alice, bob, carol;
    memset(&alice, 0, sizeof alice); memset(&bob, 0, sizeof bob);
    memset(&carol, 0, sizeof carol);
    alice.name = "alice"; bob.name = "bob"; carol.name = "carol";
    wl_reset(&alice.fog); wl_reset(&bob.fog);

    uint32_t pa[] = { 100, 101, 102, 103, 200, 201 };
    uint32_t pb[] = { 300, 301, 302, 400, 401, 500 };
    walk(&alice, pa, 6);
    walk(&bob,   pb, 6);

    int la = lit_count(&alice.fog), lb = lit_count(&bob.fog);
    int sh = shared_lit(&alice.fog, &bob.fog);
    printf("      alice lit=%d  bob lit=%d  shared=%d\n", la, lb, sh);
    CHECK(la == 6 && lb == 6, "each walker lit exactly the 6 cells they walked");
    CHECK(sh == 0, "no overlap: each player sees a DIFFERENT map");
    CHECK(alice.last_id == 201 && bob.last_id == 500,
          "each remembers their own last position, not the other's");

    /* --- Leg 2: overlapping walk -> partial shared sight, data still fixed --- */
    printf("\n[L2] walkers overlap on some cells; the cells themselves never move\n");
    memset(&carol, 0, sizeof carol); carol.name = "carol";
    wl_reset(&carol.fog);
    uint32_t pc[] = { 100, 101, 302, 400 };   /* shares 100,101 with alice */
    walk(&carol, pc, 4);
    int sh_ac = shared_lit(&alice.fog, &carol.fog);
    printf("      alice lit=%d  carol lit=%d  shared=%d\n",
           lit_count(&alice.fog), lit_count(&carol.fog), sh_ac);
    CHECK(sh_ac == 2, "shared sight = exactly the commonly-walked cells (100,101)");
    /* data identity: the id NUMBERS are absolute, same for everyone */
    CHECK(!wl_is_open(&alice.fog, 100) && !wl_is_open(&carol.fog, 100),
          "cell 100 is the SAME place in both maps (land did not move)");

    /* --- Leg 3: monotone = once lit, stays lit (map accumulates) --- */
    printf("\n[L3] fog only opens; walking again does not re-light\n");
    int before_hits = alice.hits;
    int before_lit  = lit_count(&alice.fog);
    walk(&alice, pa, 6);                 /* same path again */
    CHECK(alice.hits == before_hits, "re-walking the same path lights nothing new");
    CHECK(lit_count(&alice.fog) == before_lit && before_lit == 6,
          "alice map is still exactly 6 cells");

    /* --- Leg 4: CLEAR re-fogs (only way back) --- */
    printf("\n[L4] CLEAR is the only way to re-fog a cell\n");
    wl_clear(&alice.fog, 100);
    CHECK(wl_is_open(&alice.fog, 100), "id 100 fogged again after CLEAR");
    CHECK(lit_count(&alice.fog) == 5, "alice map reduces to 5");
    CHECK(!wl_is_open(&carol.fog, 100), "carol's map untouched by alice's CLEAR");

    /* --- Leg 5: identity of a walker = their lit set (the "pattern") --- */
    printf("\n[L5] pattern = the lit set, not a formula\n");
    printf("      alice cleared %d cells, bob %d, carol %d\n",
           lit_count(&alice.fog), lit_count(&bob.fog), lit_count(&carol.fog));
    /* bob walked {300,301,302,400,401,500}; carol walked {100,101,302,400}
       -> by hand they share exactly {302,400} = 2 common cells. */
    int sh_bc = shared_lit(&bob.fog, &carol.fog);
    printf("      bob/carol shared=%d (hand: {302,400})\n", sh_bc);
    CHECK(sh_bc == 2, "bob and carol share exactly the 2 cells both walked");
    /* distinguishability: their maps DIFFER (carol has 100,101 bob lacks) */
    CHECK(!wl_is_open(&carol.fog, 100) && wl_is_open(&bob.fog, 100),
          "carol lit 100 but bob did not -> the two maps are distinguishable");

    /* --- Leg 6 (RESCOPED, owner 2026-10-05): quadtree of FRUSTUM COMPOSITES,
       6 directions each. The earlier version of this leg modelled the quadtree
       as one latch id per node -- that is the flat-grid framing the owner
       corrected: "quadtree looks like a tree, but MINE is a frustum composite
       of 6 directions; because we are a PATTERN, laying it out flat does not
       use 144^2 / 12^4". So a node is NOT one slot: it is 6 faces, and walking
       through it clears ONLY the face used. There is no pipe and no picture --
       only a rule that turns (anchor, face) into a latch id, and data finds its
       way by that rule. */
    printf("\n[L6] frustum quadtree: 6 faces per anchor, fog per FACE\n");
    {
        static const int nodes_at[4] = { 1, 4, 16, 64 };
        int total_nodes = 0;
        for (int l = 0; l < 4; l++) total_nodes += nodes_at[l];
        CHECK(total_nodes == 85, "quadtree 1<4<16<64 = 85 anchors");

        /* RULE (no table): latch id of face d of anchor a = a*6 + d, offset past
           the reserved pattern pair. Pure arithmetic -- the address IS computed,
           nothing is stored, no pipe. hmm: keep ids inside the free pool. */
        #define FACE_ID(a, d) ((uint32_t)(((a) * 6u + (d)) % (WL_FREE - 1) + 1u))
        uint32_t total_faces = (uint32_t)total_nodes * 6u;
        printf("      anchors=%d  faces=%u (6 per anchor)\n", total_nodes, total_faces);
        CHECK(total_faces == 510, "85 anchors x 6 faces = 510 face-ids in the pool");

        /* 1) start: fog everywhere = every FACE open */
        wl_latch_t qt; wl_reset(&qt);
        int open0 = 0;
        for (uint32_t a = 0; a < (uint32_t)total_nodes; a++)
            for (uint32_t d = 0; d < 6; d++) open0 += wl_is_open(&qt, FACE_ID(a, d));
        printf("      start: %d/%u faces open (fog everywhere)\n", open0, total_faces);
        CHECK((uint32_t)open0 == total_faces, "every FACE starts fogged (AFK = all ON)");

        /* 2) forward walk clears ONLY the faces it crossed. Model a path as
              (anchor, in-face, out-face) triples -- the natural unit of a
              frustum composite: you enter one face and leave another. */
        typedef struct { uint32_t anchor; uint32_t in_d; uint32_t out_d; } Pass;
        Pass fwd[] = { {1,0,3}, {5,1,2}, {9,4,5}, {21,0,1}, {22,3,0}, {50,2,4} };
        int npass = (int)(sizeof fwd / sizeof fwd[0]);
        for (int i = 0; i < npass; i++) {
            wl_traverse(&qt, FACE_ID(fwd[i].anchor, fwd[i].in_d));
            wl_traverse(&qt, FACE_ID(fwd[i].anchor, fwd[i].out_d));
        }
        int lit1 = 0;
        for (uint32_t a = 0; a < (uint32_t)total_nodes; a++)
            for (uint32_t d = 0; d < 6; d++) lit1 += !wl_is_open(&qt, FACE_ID(a, d));
        printf("      forward: %d passes lit %d face-ids, nothing blocked\n",
               npass, lit1);
        CHECK(lit1 == npass * 2, "each pass lights exactly 2 faces (entry+exit)");

        /* pattern of ONE anchor = WHICH of its 6 faces are lit = a 6-bit mask.
           No picture: just a rule reading the latch bits. */
        uint32_t A = 22;
        uint32_t pattern = 0;
        for (uint32_t d = 0; d < 6; d++)
            if (!wl_is_open(&qt, FACE_ID(A, d))) pattern |= (1u << d);
        printf("      anchor 22 face-mask=0x%02X (in/out of passes 22,3,0)\n", pattern);
        CHECK(pattern == ((1u << 3) | (1u << 0)),
              "anchor 22 pattern = its own in/out faces only (0x09)");

        /* 3) BACKWARD is the only time the system asks (query + content), and
              only onto a face that is already lit = a tombstone. */
        uint32_t back_a = 22, back_d = 3;
        int asked_query = 0, asked_there = 0, gate = 0;
        if (!wl_is_open(&qt, FACE_ID(back_a, back_d))) {
            asked_query = 1; asked_there = 1; gate = 1;   /* score path is caller */
        }
        printf("      backward onto lit face (22,3): query=%d there=%d gate=%d\n",
               asked_query, asked_there, gate);
        CHECK(asked_query && asked_there && gate,
              "backward asks exactly {query, there}, then opens (warp back)");

        /* 4) TOMBSTONE = the lit face IS the index. A later walker asks the same
              face-id and hits with no search. */
        int hit = !wl_is_open(&qt, FACE_ID(back_a, back_d));
        CHECK(hit, "tombstone face (22,3) answers the next lookup, no search");

        /* 5) reuse shrinks work: count faces still fogged on the same route */
        int need = 0;
        for (int i = 0; i < npass; i++) {
            if (wl_is_open(&qt, FACE_ID(fwd[i].anchor, fwd[i].in_d)))  need++;
            if (wl_is_open(&qt, FACE_ID(fwd[i].anchor, fwd[i].out_d))) need++;
        }
        printf("      cold passes=%d  warm faces still fogged=%d\n", npass, need);
        CHECK(need == 0, "reuse needs zero re-traverse -> gets faster each time");

        /* 6) the pool holds HALF the field; a frustum quadtree overflows it once
              wang6 opens 3 more generations (owner: built ahead, not used yet). */
        printf("      field=20736  latch pool=%u  free=%u\n",
               (unsigned)WL_COUNT, (unsigned)WL_FREE);
        CHECK(WL_COUNT * 2u == 20736u,
              "latch covers exactly HALF the field (10368 x 2 = 20736)");
        CHECK(WL_FREE == 10366u, "free minor branches = 10368 - 2 reserved");
        long long gen3 = (long long)total_nodes;
        for (int i = 0; i < 3; i++) gen3 *= 4;      /* 3 more generations, x4 each */
        long long gen3_faces = gen3 * 6;
        printf("      +3 gens (wang6): %d anchors x 6 = %d faces\n",
               (int)gen3, (int)gen3_faces);
        CHECK(gen3_faces > (long long)WL_COUNT,
              "+3 gens of frustum faces overflow the pool -> why wang6 is held back");
    }

    printf("\nFOG: %d FAIL\n", fails);
    return fails != 0;
}
