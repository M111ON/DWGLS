/* jet_coalesce_bench — production geo_pipeline_want coalesce ratio.
 *
 * Drives wants through GeoPipelineCtx at controlled density, counts
 * bridges (dispatches) vs wants coalesced. Reports reduction factor.
 * Expected: full-round batch ≈ WIN=12 → ratio near 10-12+ with dense fill.
 *
 * Oracle-independent: counts real ctx fields, no self-derived expects.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_gpu_pipeline.h"

int main(void)
{
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);
    if (!ctx.is_init) { printf("FATAL: init failed\n"); return 1; }

    /* Scenario A: 1 want per tick, 120 ticks (matches small-batch A5) */
    uint32_t wants_A = 0;
    uint32_t t = 0;
    for (uint32_t i = 0; i < 120; i++) {
        (void)geo_pipeline_want(&ctx, t, 3); /* L=3 phase-align region */
        wants_A++;
        (void)geo_pipeline_tick(&ctx);
        t++;
    }
    /* drain only until wants flush into next bridge (no idle dilution) */
    for (int k = 0; k < 12 && ctx.wants_ready; k++) (void)geo_pipeline_tick(&ctx);

    uint32_t brA = ctx.bridges, coA = ctx.wants_coalesced;
    double ratioA = coA ? (double)coA / (double)(brA ? brA : 1) : 0.0;
    printf("A: 1 want/tick x120 + drain\n");
    printf("   wants_arrived=%u coalesced=%u bridges=%u ratio=%.2fx\n",
           wants_A, coA, brA, ratioA);

    /* Scenario B: dense fill — 12 wants per tick (full WIN batch ready)
     * for 12 ticks, then drain. Expect coalesce into 1-2 bridges. */
    geo_pipeline_reset(&ctx);
    uint32_t wants_B = 0;
    for (uint32_t ph = 0; ph < 12; ph++) {
        for (int w = 0; w < 12; w++) {
            (void)geo_pipeline_want(&ctx, ph, 1); /* L=1 short hop */
            wants_B++;
        }
        (void)geo_pipeline_tick(&ctx);
    }
    for (int k = 0; k < 12 && ctx.wants_ready; k++) (void)geo_pipeline_tick(&ctx);

    uint32_t brB = ctx.bridges, coB = ctx.wants_coalesced;
    double ratioB = coB ? (double)coB / (double)(brB ? brB : 1) : 0.0;
    printf("B: 12 wants/tick x12 ticks (dense WIN batch)\n");
    printf("   wants_arrived=%u coalesced=%u bridges=%u ratio=%.2fx\n",
           wants_B, coB, brB, ratioB);

    /* Scenario C: sustained dense — 12 wants/tick x 120 ticks */
    geo_pipeline_reset(&ctx);
    uint32_t wants_C = 0;
    for (uint32_t i = 0; i < 120; i++) {
        for (int w = 0; w < 12; w++) {
            (void)geo_pipeline_want(&ctx, i, 0); /* L=0 in-place */
            wants_C++;
        }
        (void)geo_pipeline_tick(&ctx);
    }
    for (int k = 0; k < 12 && ctx.wants_ready; k++) (void)geo_pipeline_tick(&ctx);

    uint32_t brC = ctx.bridges, coC = ctx.wants_coalesced;
    double ratioC = coC ? (double)coC / (double)(brC ? brC : 1) : 0.0;
    printf("C: 12 wants/tick x120 ticks (sustained dense)\n");
    printf("   wants_arrived=%u coalesced=%u bridges=%u ratio=%.2fx\n",
           wants_C, coC, brC, ratioC);

    /* Verdict thresholds (spec: user asks >10-12) */
    int passA = ratioA >= 10.0;
    int passB = ratioB >= 12.0;
    int passC = ratioC >= 12.0;
    printf("\nVERDICT\n");
    printf("  A (sparse 1/tick): %.2fx  %s  (threshold 10x)\n",
           ratioA, passA ? "PASS" : "FAIL");
    printf("  B (dense burst):   %.2fx  %s  (threshold 12x)\n",
           ratioB, passB ? "PASS" : "FAIL");
    printf("  C (sustained):     %.2fx  %s  (threshold 12x)\n",
           ratioC, passC ? "PASS" : "FAIL");

    int all = passA && passB && passC;
    printf("RESULT: %s\n", all ? "ALL PASS" : "SOME FAIL");
    geo_pipeline_reset(&ctx);
    return all ? 0 : 1;
}
