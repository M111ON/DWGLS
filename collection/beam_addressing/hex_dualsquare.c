/*
 * hex_dualsquare.c — Full Pipeline: weight → Dual Square → hexagon
 * ═══════════════════════════════════════════════════════════════════
 *
 * Pipeline:
 *   weight w → absolute distance from calibrated zero → radius of circle
 *   → angular coordinate (sawtooth on 360×360 rotation xy)
 *   → Dual Square (XOR(θ,φ) = magnitude)
 *   → θ/60 = sector (0..5 of hexagon)
 *   → φ = within-sector position
 *
 * "calibrate ว่าค่า0 ตรงไหน → ได้ค่ารอบวงกลม → แปลงเป็น angular
 *  → เส้นกราฟฟันปลาบน rotation x,y ใน scope 360,360 → แบ่ง 60 ได้"
 *
 * Ref: beam_square.c (v7) — Dual Square 360×360 prototype
 *
 * Compile: gcc -O2 -I. hex_dualsquare.c -o hex_dualsquare.exe -lm
 * Run: ./hex_dualsquare.exe
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gguf_reader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
   DUAL SQUARE (360×360) — from beam_square.c (v7)
   ══════════════════════════════════════════════════════════════
 *
 *   XY layer (outer/+):   X = θ,  Y = φ
 *   YX layer (inner/-):   X = φ,  Y = θ  (transposed)
 *
 *   Weight = XOR(θ, φ)   (flat geometry, no sphere)
 *   Sign   = XY=outer(+), YX=inner(-)
 *
 *   360×360 = 129,600 positions per layer
 *   Dual total = 259,200 positions
 */

#define SQ_RES    360u      /* 0..359 per axis */
#define SQ_GRID   (SQ_RES * SQ_RES)  /* 129,600 */

typedef struct {
    uint16_t azimuth;   /* θ: 0..359 */
    uint16_t elevation; /* φ: 0..359 */
} DualCoord;

/* XOR-weighted distance on Dual Square (8-bit magnitude) */
static uint8_t dual_xor_magnitude(DualCoord c)
{
    uint16_t mix = (uint16_t)(c.azimuth ^ c.elevation);
    uint16_t m2 = mix ^ (mix >> 3) ^ (mix >> 5) ^ (mix >> 7);
    return (uint8_t)(m2 & 0xFF);
}

/* ══════════════════════════════════════════════════════════════
   PIPELINE: weight → Dual Square → hexagon sector
   ══════════════════════════════════════════════════════════════
 *
 *   1. CALIBRATE: choose zero reference
 *   2. weight w → absolute distance d = |w - ref|
 *   3. d → radius of circle → angular coordinate
 *      angle_R = (d × rotation_rate) mod 360
 *      (sawtooth: as d increases, angle wraps around 360)
 *   4. angle_R → Dual Square coordinate
 *      θ = angle_R  (azimuth)
 *      φ = some function of distance  (elevation)
 *   5. XOR(θ, φ) = weight magnitude  (verify)
 *   6. θ / 60 = sector (0..5) for hexagon
 *   7. φ determines within-sector position
 */

typedef struct {
    double   zero_ref;       /* calibrated zero point */
    double   rotation_rate;  /* how fast angle rotates with distance */
    uint16_t azimuth;        /* computed θ (0..359) */
    uint16_t elevation;      /* computed φ (0..359) */
    int      sector;         /* θ / 60 = 0..5 */
    double   within;         /* within-sector fraction (0..1) */
    uint8_t  xor_mag;        /* XOR(θ, φ) */
} WeightOnDual;

/* Dual Square mapping constants */
#define DUAL_RATE  97    /* rotation = distance × 97 mod 360 (prime) */
#define SECTOR_DIV 60    /* 360/60 = 6 sectors */

/* Initialize with calibration */
static WeightOnDual weight_to_dual(double weight, double zero_ref)
{
    WeightOnDual wd;
    wd.zero_ref = zero_ref;
    wd.rotation_rate = DUAL_RATE;

    /* Step 1: absolute distance from reference */
    double d = fabs(weight - zero_ref);

    /* Step 2: angular coordinate (sawtooth on rotation xy) */
    double angle_d = d * wd.rotation_rate;
    double angle_mod = fmod(angle_d, 360.0);
    if (angle_mod < 0) angle_mod += 360.0;

    /* Step 3: map to Dual Square 360×360
     *   θ = angle_mod (azimuth from distance)
     *   φ = fine_position from fractional part
     */
    uint16_t theta = (uint16_t)angle_mod;           /* 0..359 */
    uint16_t phi = (uint16_t)(fmod(d, 360.0));       /* 0..359 */

    wd.azimuth = theta;
    wd.elevation = phi;

    /* Step 4: XOR magnitude */
    DualCoord dc = {theta, phi};
    wd.xor_mag = dual_xor_magnitude(dc);

    /* Step 5: sector = θ / 60 */
    wd.sector = (int)(theta / SECTOR_DIV);  /* 0..5 */
    if (wd.sector > 5) wd.sector = 5;

    /* Step 6: within-sector = fractional position */
    wd.within = (double)(theta % SECTOR_DIV) / (double)SECTOR_DIV;

    return wd;
}

/* ══════════════════════════════════════════════════════════════
   HARDWARE: 7-centroid hexagon from Dual Square
   ══════════════════════════════════════════════════════════════
 *
 *   θ/60 = sector = which hexagon triangle (0..5)
 *   within-sector = position within that triangle
 *
 *   6 sectors → 7 centroids:
 *     sector 0: C0 (center) → toward C1 (azimuth 0°-59°)
 *     sector 1: C0 → C2 (azimuth 60°-119°)
 *     ...
 *     sector 5: C0 → C6 (azimuth 300°-359°)
 *
 *   Center (C0) = all sectors when θ = 0 (weight = ref)
 */

static void print_dual_encoding(double w, double ref)
{
    WeightOnDual wd = weight_to_dual(w, ref);

    char *sector_names[] = {"C0→C1(E)", "C0→C2(NE)", "C0→C3(NW)",
                            "C0→C4(W)", "C0→C5(SW)", "C0→C6(SE)"};

    printf("  w=%-8.2f | θ=%3u φ=%3u | XOR=%3u | sector=%d (%s) | within=%.4f\n",
           w, wd.azimuth, wd.elevation, wd.xor_mag,
           wd.sector, sector_names[wd.sector], wd.within);
}

/* ══════════════════════════════════════════════════════════════
   CALIBRATION: find zero_ref from Q8_0 weight distribution
   ══════════════════════════════════════════════════════════════ */

static double calibrate_from_weights(const int8_t *weights, uint64_t count)
{
    /* Use mean as zero reference */
    int64_t sum = 0;
    uint64_t sample = (count > 100000) ? 100000 : count;
    for (uint64_t i = 0; i < sample; i++) sum += weights[i];
    return (double)sum / (double)sample;
}

/* ══════════════════════════════════════════════════════════════
   TEST ON REAL MODEL
   ══════════════════════════════════════════════════════════════ */

static uint64_t read_q8_weights(GGUF_File *gf, int tensor_idx,
                                 int8_t **out_weights)
{
    GGUF_Tensor *t = &gf->tensors[tensor_idx];
    uint64_t n_blocks = (t->n_weights + 31) / 32;
    uint64_t data_start = gf->tensor_data_start + t->offset;
    data_start = (data_start + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)data_start, SEEK_SET);

    int8_t *all_w = (int8_t *)malloc(n_blocks * 32);
    if (!all_w) return 0;

    uint64_t total = 0;
    for (uint64_t b = 0; b < n_blocks; b++) {
        uint16_t scale;
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(all_w + total, 1, 32, gf->fp) != 32) break;
        total += 32;
    }
    *out_weights = all_w;
    return total;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Hex DualSquare — Full Pipeline                       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("Pipeline:\n");
    printf("  weight → distance from zero → radius of circle\n");
    printf("  → angular (sawtooth on 360×360 rotation xy)\n");
    printf("  → Dual Square XOR(θ,φ) = magnitude\n");
    printf("  → θ/60 = sector (0..5 hexagon)\n");
    printf("  → φ = within-sector position\n");
    printf("\n");

    /* ── Demo with synthetic weights ── */
    printf("─── Weight → Dual Square → Hexagon ───\n");
    printf("  zero_ref = 0.0 (calibrated at origin)\n");
    printf("  rotation_rate = 97 (sawtooth period)\n\n");

    printf("  w         θ    φ    XOR  sector  within\n");
    printf("  ────────  ───  ───  ───  ──────  ──────\n");
    double test_w[] = {0, 0.5, 1.0, 1.5, 2.0, 3.0,
                       4.0, 5.0, 6.0, 10.0, 20.0,
                       -0.5, -1.0, -2.0};
    for (int i = 0; i < 14; i++)
        print_dual_encoding(test_w[i], 0.0);

    printf("\n  สังเกต: θ/distance ratio = %d (sawtooth: ทุก %.2f units ตกกลับ 0)\n",
           DUAL_RATE, 360.0 / DUAL_RATE);
    printf("  sector = θ/60 → 6 sectors of hexagon\n");
    printf("\n");

    /* ── Real model test ── */
    const char *model_path = argc > 1 ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("─── Real Model: %s ───\n", model_path);

    GGUF_File *gf = gguf_open(model_path);
    if (!gf) { fprintf(stderr, "FAIL: open\n"); return 1; }

    int ti = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == 8) { ti = (int)i; break; }
    if (ti < 0) { fprintf(stderr, "FAIL: no Q8\n"); gguf_close(gf); return 1; }

    uint64_t nw = 0;
    int8_t *weights = NULL;
    nw = read_q8_weights(gf, ti, &weights);
    gguf_close(gf);

    if (!weights || nw == 0) { fprintf(stderr, "FAIL: read\n"); return 1; }
    printf("  Weights: %llu\n\n", (unsigned long long)nw);

    /* Calibrate */
    double ref = calibrate_from_weights(weights, nw);
    printf("  Calibrated zero_ref = %.4f (mean of sample)\n\n", ref);

    /* Analyze sector distribution */
    uint64_t sector_count[6] = {0};
    uint64_t sample = (nw > 100000) ? 100000 : nw;
    for (uint64_t i = 0; i < sample; i++) {
        WeightOnDual wd = weight_to_dual((double)weights[i], ref);
        sector_count[wd.sector % 6]++;
    }
    printf("  Sector distribution (sample=%llu):\n", (unsigned long long)sample);
    char *sec_names[] = {"E(0°)", "NE(60°)", "NW(120°)", "W(180°)", "SW(240°)", "SE(300°)"};
    for (int s = 0; s < 6; s++) {
        double pct = 100.0 * (double)sector_count[s] / (double)sample;
        printf("    %s: %llu (%.1f%%)\n",
               sec_names[s], (unsigned long long)sector_count[s], pct);
    }

    /* Print first 20 weights with their hex positions */
    printf("\n  First 20 weights with hex position:\n");
    printf("  weight    θ    φ   XOR sector within\n");
    printf("  ───────  ───  ───  ─── ────── ──────\n");
    for (int i = 0; i < 20 && i < (int)nw; i++) {
        WeightOnDual wd = weight_to_dual((double)weights[i], ref);
        printf("  %7d  %3u  %3u  %3u  %d     %.4f\n",
               (int)weights[i], wd.azimuth, wd.elevation,
               wd.xor_mag, wd.sector, wd.within);
    }

    free(weights);

    printf("\n─── Summary ───\n");
    printf("  Dual Square (360×360) → 6 sectors (θ/60) → hexagon\n");
    printf("  7 centroids: C0(center, θ=0) + C1..C6 (sectors 0..5)\n");
    printf("  weight → distance → angular → sector → hexagon position\n");
    printf("  beam ขึ้นอยู่กับ weight = θ,φ กำหนดโดย w\n");

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Pipeline Complete — weight → Dual Square → Hexagon   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    return 0;
}
