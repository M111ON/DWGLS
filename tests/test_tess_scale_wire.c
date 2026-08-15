/*
 * test_tess_scale_wire.c — th_subdivide ↔ Scale-Timeline Wiring
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Wires the 4-subdivision ladder (th_subdivide, aperture 4) into the scale
 * timeline (144 scale positions w, affine view p = (a_w·l + b_w) % 144).
 *
 * BRIDGE (integer only, rescope): node = hi·81 + lo with hi = 16H + h,
 * lo = 9L + l → scale position w = 9H + L (outer digits), local position
 * pos = 9h + l (inner digits). (w, pos) ↔ node is a bijection 144² = 20736.
 *
 * DEPTH → SCALE: the 4-ladder's first two base-4 digits live in the SCALE
 * axis (H), the last two in the LOCAL axis (h):
 *   d ≤ 2 → cell = w-block 144/4^d (144, 36, 9) × full pos
 *   d = 3 → w frozen (9), pos splits 144 → 4 × 36
 *   d = 4 → w frozen (9), pos splits 36 → 4 × 9
 * every depth-d cell is exactly one rectangle w_ext × pos_ext = 20736/4^d.
 *
 * Proof:
 *   W1  bridge bijection — node ↔ (w, pos), both directions, all 20736
 *   W2  cell == rectangle — every node sits in its depth-d cell's
 *       (w-block, pos-block) at EVERY level; area == 20736/4^d
 *   W3  w-axis refinement — depth-2 w-blocks tile depth-1 w-blocks; the
 *       w-axis saturates at d=2 (4 cells share a w-block at d=3, 16 at d=4)
 *   W4  a_w ↔ th_cell_anchor — at EVERY scale w and depth d, the a_w view
 *       separates the active depth-d cells: their pos-images partition pos
 *       (no two cells ever share a physical address at any level)
 *   W5  per-scale placement — at every scale w, slot owner == depth-d
 *       cell of its node (a_w matches th_cell_anchor slot-for-slot);
 *       read back at w (empty log) → lossless
 *   W6  canonical depth scales — gsw_depth_scale(d) ∈ [0,144) and equals
 *       the last depth-d cell's w-block start
 *   W7  subdivision respects the wiring — every child rectangle (via real
 *       th_subdivide4) lies inside its parent rectangle, all levels
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_scale_wire tests/test_tess_scale_wire.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/tri_hex_tess.h"
#include "../core/geo_scale_wire.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* depth-d cell index of a node (4-ladder hi-prefix, matches th_cell_anchor) */
static uint32_t cell_of(uint32_t n, uint32_t d) {
    return (n / 81u) >> (8u - 2u * d);
}

static uint8_t val_of(uint32_t n) {
    return (uint8_t)((n * 7u + 11u) % 251u);
}

/* is depth-d cell c active at scale w? (w ∈ cell's w-block) */
static int cell_active_at(uint32_t c, uint32_t d, uint32_t w) {
    uint32_t w0 = gsw_cell_scale(c, d);
    return (w >= w0 && w < w0 + gsw_scale_ext(d));
}

int main(void) {
    GSWScale scale;
    gsw_scale_init(&scale);

    printf("═ SUBDIVISION ↔ SCALE-TIMELINE WIRING (4-ladder × a_w) ═\n");
    printf("  node = hi·81 + lo;  w = 9H + L (scale), pos = 9h + l (local)\n\n");

    /* ── W1: bridge bijection — node ↔ (w, pos) both ways ───────────── */
    {
        int ok_fwd = 1, ok_bwd = 1;
        for (uint32_t n = 0; n < GSW_FULL; n++) {
            uint32_t w = gsw_scale_of_node(n);
            uint32_t pos = gsw_pos_of_node(n);
            if (w >= GSW_LOCAL || pos >= GSW_LOCAL) { ok_fwd = 0; break; }
            if (gsw_node_of_scale(w, pos) != n) { ok_fwd = 0; break; }
        }
        for (uint32_t w = 0; w < GSW_LOCAL && ok_bwd; w++)
            for (uint32_t pos = 0; pos < GSW_LOCAL && ok_bwd; pos++) {
                uint32_t n = gsw_node_of_scale(w, pos);
                if (gsw_scale_of_node(n) != w || gsw_pos_of_node(n) != pos)
                    ok_bwd = 0;
            }
        CHECK("W1: node ↔ (w, pos) bijection — both directions, all 20736", ok_fwd && ok_bwd);
        CHECK("W1b: 144 × 144 == 20736 — the (w, pos) grid tiles the field",
              GSW_LOCAL * GSW_LOCAL == GSW_FULL);
    }

    /* ── W2: cell == rectangle — every node inside its cell's block ──── */
    {
        int ok_rect = 1, ok_area = 1;
        for (uint32_t d = 0; d <= GSW_4_DEPTH && ok_rect; d++) {
            if (gsw_scale_ext(d) * gsw_pos_ext(d) != GSW_FULL / gsw_cell_count(d))
                ok_area = 0;
            for (uint32_t n = 0; n < GSW_FULL; n++) {
                uint32_t c = cell_of(n, d);
                uint32_t w = gsw_scale_of_node(n);
                uint32_t pos = gsw_pos_of_node(n);
                uint32_t w0 = gsw_cell_scale(c, d), w_ext = gsw_scale_ext(d);
                uint32_t p0 = gsw_cell_pos_base(c, d), p_ext = gsw_pos_ext(d);
                if (w < w0 || w >= w0 + w_ext || pos < p0 || pos >= p0 + p_ext) {
                    ok_rect = 0; break;
                }
            }
        }
        CHECK("W2: every node in its depth-d cell's (w-block, pos-block) — ALL levels, all 20736",
              ok_rect);
        CHECK("W2b: w_ext × pos_ext == 20736/4^d — cell area exact (144·144, 36·144, 9·144, 9·36, 9·9)",
              ok_area);
    }

    /* ── W3: w-axis refinement + saturation ──────────────────────────── */
    {
        /* depth-2 w-blocks tile depth-1 w-blocks exactly (4 × 9 = 36) */
        int ok_refine = 1;
        for (uint32_t c1 = 0; c1 < 4 && ok_refine; c1++) {
            uint32_t w0 = gsw_cell_scale(c1, 1u);
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t child = 4u * c1 + k;
                uint32_t cw = gsw_cell_scale(child, 2u);
                if (cw != w0 + 9u * k) ok_refine = 0;
            }
        }
        /* saturation: cells share w-blocks below the w-axis floor */
        int ok_sat = 1;
        for (uint32_t H = 0; H < 16 && ok_sat; H++) {
            uint32_t cnt3 = 0, cnt4 = 0;
            for (uint32_t c = 0; c < 64u; c++)
                if (gsw_cell_scale(c, 3u) == 9u * H) cnt3++;
            for (uint32_t c = 0; c < 256u; c++)
                if (gsw_cell_scale(c, 4u) == 9u * H) cnt4++;
            if (cnt3 != 4u || cnt4 != 16u) ok_sat = 0;
        }
        CHECK("W3: depth-2 w-blocks tile depth-1 w-blocks exactly (4×9=36)", ok_refine);
        CHECK("W3b: w-axis saturates at d=2 — 4 cells share a w-block at d=3, 16 at d=4",
              ok_sat);
        printf("     w-extents {144,36,9,9,9}  pos-extents {144,144,144,36,9}\n");
    }

    /* ── W4: a_w ↔ th_cell_anchor — the view separates cells at EVERY scale ── */
    {
        int ok = 1;
        for (uint32_t d = 0; d <= GSW_4_DEPTH && ok; d++)
            for (uint32_t w = 0; w < GSW_LOCAL && ok; w++) {
                uint16_t owner[GSW_LOCAL];
                memset(owner, 0, sizeof(owner));
                for (uint32_t c = 0; c < gsw_cell_count(d); c++) {
                    if (!cell_active_at(c, d, w)) continue;
                    uint32_t p0 = gsw_cell_pos_base(c, d);
                    uint32_t p_ext = gsw_pos_ext(d);
                    for (uint32_t pos = p0; pos < p0 + p_ext; pos++) {
                        uint32_t p = gsw_view(&scale, w, pos);
                        if (owner[p] != 0) { ok = 0; break; }   /* two cells collide */
                        owner[p] = (uint16_t)(c + 1u);
                    }
                    if (!ok) break;
                }
                for (uint32_t pos = 0; pos < GSW_LOCAL && ok; pos++)
                    if (owner[pos] == 0) ok = 0;                 /* a gap → not a partition */
            }
        CHECK("W4: a_w view separates depth-d cells at EVERY scale — pos-images partition pos",
              ok);
        printf("     active cells per scale: d0:1  d1:1  d2:1  d3:4  d4:16\n");
    }

    /* ── W5: per-scale placement — a_w addressing matches the anchor ── */
    /* at scale w, the w-row (144 nodes with scale_of_node == w) is placed
     * through the a_w view; every slot must be owned by the depth-d cell
     * of its node (W4 partition), and read-back at w (empty log) is
     * lossless — the timeline and the subdivision agree slot-for-slot. */
    {
        int ok = 1;
        for (uint32_t d = 0; d <= GSW_4_DEPTH && ok; d++)
            for (uint32_t w = 0; w < GSW_LOCAL && ok; w++) {
                uint16_t owner[GSW_LOCAL];
                memset(owner, 0, sizeof(owner));
                for (uint32_t c = 0; c < gsw_cell_count(d); c++) {
                    if (!cell_active_at(c, d, w)) continue;
                    uint32_t p0 = gsw_cell_pos_base(c, d);
                    uint32_t p_ext = gsw_pos_ext(d);
                    for (uint32_t pos = p0; pos < p0 + p_ext; pos++)
                        owner[gsw_view(&scale, w, pos)] = (uint16_t)(c + 1u);
                }
                uint8_t store[GSW_LOCAL];
                memset(store, 0, sizeof(store));
                for (uint32_t n = 0; n < GSW_FULL && ok; n++) {
                    if (gsw_scale_of_node(n) != w) continue;
                    uint32_t p = gsw_view(&scale, w, gsw_pos_of_node(n));
                    store[p] = val_of(n);
                    if (owner[p] != (uint16_t)(cell_of(n, d) + 1u)) ok = 0;
                }
                for (uint32_t n = 0; n < GSW_FULL && ok; n++) {
                    if (gsw_scale_of_node(n) != w) continue;
                    uint32_t p = gsw_view(&scale, w, gsw_pos_of_node(n));
                    if (store[p] != val_of(n)) ok = 0;
                }
            }
        CHECK("W5: at every scale, slot owner == depth-d cell of its node — a_w matches th_cell_anchor, lossless",
              ok);
    }

    /* ── W6: canonical depth scales ──────────────────────────────────── */
    {
        static const uint32_t want[5] = {0u, 108u, 135u, 135u, 135u};
        int ok_seq = 1, ok_last = 1;
        for (uint32_t d = 0; d <= GSW_4_DEPTH; d++) {
            if (gsw_depth_scale(d) != want[d] || gsw_depth_scale(d) >= GSW_LOCAL)
                ok_seq = 0;
            if (d <= 2u && gsw_depth_scale(d) != gsw_cell_scale(gsw_cell_count(d) - 1u, d))
                ok_last = 0;
        }
        CHECK("W6: gsw_depth_scale(d) == {0,108,135,135,135} — canonical scale per depth",
              ok_seq);
        CHECK("W6b: depth scale == the last depth-d cell's w-block start (d ≤ 2)",
              ok_last);
    }

    /* ── W7: subdivision respects the wiring (real th_subdivide4) ───── */
    {
        int ok = 1;
        for (uint32_t n = 0; n < GSW_FULL && ok; n += 4)
            for (uint32_t d = 0; d < GSW_4_DEPTH && ok; d++) {
                uint32_t anchor = th_cell_anchor(n, 4u, d);
                uint32_t parent = cell_of(n, d);
                for (uint32_t k = 0; k < 4u && ok; k++) {
                    uint32_t child_node = th_subdivide4(anchor, d, k);
                    uint32_t child = cell_of(child_node, d + 1u);
                    uint32_t pw0 = gsw_cell_scale(parent, d), pwe = gsw_scale_ext(d);
                    uint32_t pp0 = gsw_cell_pos_base(parent, d), ppe = gsw_pos_ext(d);
                    uint32_t cw0 = gsw_cell_scale(child, d + 1u), cwe = gsw_scale_ext(d + 1u);
                    uint32_t cp0 = gsw_cell_pos_base(child, d + 1u), cpe = gsw_pos_ext(d + 1u);
                    if (cw0 < pw0 || cw0 + cwe > pw0 + pwe ||
                        cp0 < pp0 || cp0 + cpe > pp0 + ppe) ok = 0;
                    if ((child >> 2u) != parent) ok = 0;   /* child of the parent */
                }
            }
        CHECK("W7: every th_subdivide4 child rectangle lies inside its parent rectangle (all levels)",
              ok);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
