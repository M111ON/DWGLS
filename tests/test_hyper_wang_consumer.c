/* Hyper address -> existing Wang consumer probe.
 *
 * The coordinate adapter itself now lives in geo_hyper_wang.h; this remains
 * the cross-consumer proof for its contract.
 */
#include <stdint.h>
#include <stdio.h>
#include "geo_hyper_wang.h"
#include "hyp_fusion.h"
#include "ckpt_wang.h"

static int pass, fail;

static void check(int ok, const char *name)
{
    if (ok) { pass++; printf("  PASS  %s\n", name); }
    else    { fail++; printf("  FAIL  %s\n", name); }
}

static uint16_t hyper_to_enc(uint32_t pos)
{
    return hywang_cell_to_timeline(pos);
}

static int enc_to_hyper(uint16_t enc, uint32_t *pos)
{
    /* Selected consumer frames are cell boundaries. */
    uint32_t t = enc;
    if ((t % HYWANG_PHASES_PER_CELL) != 0u) return 0;
    *pos = t / HYWANG_PHASES_PER_CELL;
    return *pos < HJ_TOTAL;
}

static int make_route_for_enc(uint16_t enc, GhostLogEntry *e)
{
    /* Find a valid ghost coordinate for the already-selected timeline cell.
     * This is test setup only; production ghost code keeps its own address. */
    for (uint32_t block = 0; block < 256u; block++) {
        for (uint32_t from = 0; from < 256u; from++) {
            uint32_t base = (uint32_t)((rdh_addr(block, from) % FRAME_CYCLE));
            uint32_t to = ((uint32_t)enc + FRAME_CYCLE - base) % FRAME_CYCLE;
            if (to < 143u) {
                e->block_id = (uint16_t)block;
                e->from_scale = (uint8_t)from;
                e->to_scale = (uint8_t)to;
                e->flags = GHOST_FLAG_LIFT;
                return 1;
            }
        }
    }
    return 0;
}

static void fill_features(float nf[108], float ev[4])
{
    for (int i = 0; i < 108; i++) nf[i] = 0.0f;
    for (int i = 0; i < 4; i++) ev[i] = 0.0f;
}

static void test_mode(uint32_t span, const FrameWangLayer *wl)
{
    int seen[HJ_TOTAL] = {0};
    int open = 0, skip = 0, closed = 0;
    float nf[108], ev[4];
    fill_features(nf, ev);

    for (uint32_t p = 0; p < HJ_TOTAL; p++) {
        uint16_t enc = hyper_to_enc(p);
        uint32_t back = 0;
        HypSeek d;

        if (!hywang_resolve_roundtrip(p, span)) fail++;
        if (!enc_to_hyper(enc, &back) || back != p) fail++;
        seen[p]++;

        /* Use the real hard Wang gate through the existing fusion consumer. */
        d = hyp_gate_fusion(wl, enc,
                            (uint8_t)(_fwang_chord_a(enc) & 3u),
                            nf, ev, nf, ev);
        if (d == HYP_SEEK_OPEN) open++;
        else if (d == HYP_SEEK_SKIP) skip++;
        else closed++;
    }

    int unique = 1;
    for (uint32_t p = 0; p < HJ_TOTAL; p++)
        if (seen[p] != 1) unique = 0;
    check(unique, span == HS_MODE48 ? "48x3 hyper cells covered once" :
                                      "36x4 hyper cells covered once");
    check(open + skip + closed == (int)HJ_TOTAL,
          span == HS_MODE48 ? "48x3 every cell reaches Wang consumer" :
                              "36x4 every cell reaches Wang consumer");
    /* Zero feature vectors intentionally exercise the soft GNN rejection
     * path, so OPEN is not expected here.  The hard Wang partition must still
     * be visible: 48 Tesla skips and the remaining cells reach fusion. */
    check(open == 0 && skip == 48 && closed == 96,
          span == HS_MODE48 ? "48x3 preserves Wang skip/closed partition" :
                              "36x4 preserves Wang skip/closed partition");
    printf("    span=%u open=%d skip=%d closed=%d\n", span, open, skip, closed);
}

static void test_real_feature_path(const FrameWangLayer *wl)
{
    float nf_a[108], nf_b[108], ev_a[4], ev_b[4];
    float grid_a[9] = {1,2,3,4,5,6,7,8,9};
    float grid_b[9] = {9,8,7,6,5,4,3,2,1};
    float sums_a[8] = {6,15,24,12,15,18,15,15};
    float sums_b[8] = {24,15,6,18,15,12,15,15};
    gnn_f24_node_features(grid_a, sums_a, 0, nf_a);
    gnn_f24_node_features(grid_b, sums_b, 100, nf_b);
    gnn_f24_edges(sums_a, ev_a);
    gnn_f24_edges(sums_b, ev_b);
    float score = hyp_gnn_score(nf_a, ev_a, nf_b, ev_b, 1.0f);
    check(score >= 0.0f && score <= 1.0f,
          "real feature path returns bounded GNN score");
    printf("    real-feature score=%.4f\n", score);
    (void)wl;
}

static void test_checkpoint_consumer(void)
{
    GhostLog log;
    uint8_t digest[2048];
    ghost_log_init(&log);
    for (uint32_t p = 0; p < HJ_TOTAL; p++) {
        if (!make_route_for_enc(hyper_to_enc(p), &log.entries[log.count])) {
            check(0, "every hyper cell has a valid checkpoint route");
            return;
        }
        log.count++;
    }
    uint32_t dsz = ckpt_wang_digest_size(log.count);
    ckpt_wang_digest(&log, digest);
    check(ckpt_wang_verify(&log, digest, dsz) == 0,
          "checkpoint Wang digest verifies hyper-derived routes");
    check(ckpt_wang_scan(&log) == -1,
          "checkpoint scan accepts all hyper-derived routes");

    log.entries[71].to_scale ^= 1u;
    check(ckpt_wang_check(&log, digest, dsz) != 0,
          "checkpoint consumer rejects a changed hyper-derived route");
}

int main(void)
{
    FrameWangLayer wl;
    printf("=== test_hyper_wang_consumer: hyper -> Wang fusion ===\n");
    fwang_init(&wl);
    check(fwang_verify(&wl) == 0, "existing Wang layer verifies before integration");

    test_mode(HS_MODE48, &wl);
    test_mode(HS_MODE36, &wl);
    test_real_feature_path(&wl);
    test_checkpoint_consumer();

    /* The consumer must still reject a damaged Wang boundary. */
    {
        uint16_t enc = hyper_to_enc(37u);
        uint16_t win = (uint16_t)(enc / WANG_WIN_SIZE);
        uint8_t edge = (uint8_t)((wl.wins[win].edge_top + 1u) % 9u);
        float nf[108], ev[4];
        fill_features(nf, ev);
        wl.wins[win].edge_top = edge;
        wl.wins[win].edge_top_b = (uint8_t)((9u - edge) % 9u);
        check(hyp_gate_fusion(&wl, enc,
                              (uint8_t)(_fwang_chord_a(enc) & 3u),
                              nf, ev, nf, ev) == HYP_SEEK_CLOSED,
              "corrupt Wang edge closes hyper-derived route");
    }

    printf("=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail != 0;
}
