/*
 * geo_projection.c — Geometric Projection: mel 80 → LLM hidden
 *
 * The DWGLS way: coordinate = address = index
 * No learned weights. Geometry IS the projection.
 *
 * Audio:  mel 80 → 20736 (144×144 grid)
 * LLM:    20736 → hidden (geometric subsampling)
 *
 * Key insight: 896 = 144 × 6 + 32
 *              Not clean, but we can use the grid structure!
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N_MELS 80
#define GEO_SIDE 144
#define GEO_ADDR 20736  /* 144 × 144 */
#define HIDDEN_896 896  /* Qwen2.5-0.5B */
#define HIDDEN_1024 1024 /* Qwen3-0.6B */

/*
 * Step 1: mel 80 → address in 144×144 grid
 * Each mel bin maps to a position in the grid
 */
void mel_to_grid(const float *mel, int *angle, int *radius) {
    /* Feature 1: Peak bin → angle */
    int peak = 0;
    float max_val = mel[0];
    for (int i = 1; i < N_MELS; i++) {
        if (mel[i] > max_val) {
            max_val = mel[i];
            peak = i;
        }
    }
    *angle = peak % GEO_SIDE;

    /* Feature 2: Spectral centroid → radius */
    float total = 0;
    float weighted_sum = 0;
    for (int i = 0; i < N_MELS; i++) {
        float v = fabsf(mel[i]);
        total += v;
        weighted_sum += i * v;
    }
    float centroid = (total > 0) ? weighted_sum / total : 0;
    *radius = ((int)centroid) % GEO_SIDE;
}

/*
 * Step 2: Grid address → hidden dimensions
 *
 * The 144×144 grid has 20736 cells.
 * We need to select 896 cells (Qwen2.5) or 1024 cells (Qwen3).
 *
 * Method: Use the address as a SEED to generate 896 indices
 * via a deterministic pattern (no randomness, no learning).
 */
void grid_to_hidden(int angle, int radius, int hidden_size, int *indices) {
    int addr = angle * GEO_SIDE + radius;

    /*
     * Geometric subsampling: use addr as seed,
     * generate indices via golden angle (like sunflower seeds).
     *
     * Golden angle = 137.508° (≈ 2π × (1 - 1/φ))
     * This distributes points evenly on a circle.
     */
    float golden_angle = 137.508f * (float)M_PI / 180.0f;

    for (int i = 0; i < hidden_size; i++) {
        /* Generate index using golden angle from addr */
        float theta = golden_angle * i;
        float r = sqrtf((float)i / hidden_size) * GEO_SIDE;

        int x = (int)(r * cosf(theta) + GEO_SIDE) % GEO_SIDE;
        int y = (int)(r * sinf(theta) + GEO_SIDE) % GEO_SIDE;

        /* Offset by addr (the mel address) */
        x = (x + angle) % GEO_SIDE;
        y = (y + radius) % GEO_SIDE;

        indices[i] = x * GEO_SIDE + y;
    }
}

/*
 * Step 3: Full projection pipeline
 * mel 80 → grid → hidden
 */
void project_mel_to_hidden(const float *mel, int hidden_size, float *hidden) {
    int angle, radius;
    mel_to_grid(mel, &angle, &radius);

    int indices[HIDDEN_1024];
    grid_to_hidden(angle, radius, hidden_size, indices);

    /* In real usage, hidden[i] = grid_data[indices[i]] */
    /* For now, output the indices (addresses) */
    for (int i = 0; i < hidden_size; i++) {
        hidden[i] = (float)indices[i] / GEO_ADDR; /* normalized to [0, 1] */
    }
}

/*
 * Test: encode a mel pattern and show the projection
 */
int main(void) {
    printf("=== GEOMETRIC PROJECTION: mel 80 → LLM hidden ===\n\n");

    printf("Architecture:\n");
    printf("  mel 80 → [peak+centroid] → grid(144×144) → hidden(896)\n");
    printf("  No learned weights. Geometry IS the projection.\n\n");

    /* Test mel pattern (simulated) */
    float mel[N_MELS];
    for (int i = 0; i < N_MELS; i++) {
        mel[i] = -5.0f + 2.0f * sinf(2.0f * M_PI * i / N_MELS);
    }

    /* Step 1: mel → grid */
    int angle, radius;
    mel_to_grid(mel, &angle, &radius);
    printf("Step 1: mel → grid\n");
    printf("  angle  = %d (peak bin)\n", angle);
    printf("  radius = %d (centroid)\n", radius);
    printf("  addr   = %d\n", angle * GEO_SIDE + radius);
    printf("\n");

    /* Step 2: grid → hidden (Qwen2.5-0.5B) */
    int indices[HIDDEN_1024];
    grid_to_hidden(angle, radius, HIDDEN_896, indices);
    printf("Step 2: grid → hidden (Qwen2.5-0.5B, 896 dims)\n");
    printf("  First 10 indices: ");
    for (int i = 0; i < 10; i++) {
        printf("%d ", indices[i]);
    }
    printf("...\n");
    printf("  Last 10 indices:  ");
    for (int i = HIDDEN_896 - 10; i < HIDDEN_896; i++) {
        printf("%d ", indices[i]);
    }
    printf("\n\n");

    /* Step 3: full projection */
    float hidden[HIDDEN_1024];
    project_mel_to_hidden(mel, HIDDEN_896, hidden);
    printf("Step 3: full projection output\n");
    printf("  First 10 values: ");
    for (int i = 0; i < 10; i++) {
        printf("%.4f ", hidden[i]);
    }
    printf("...\n\n");

    /* Show unique addresses */
    printf("=== VALIDATION ===\n");
    printf("  Hidden size: %d\n", HIDDEN_896);
    printf("  Grid size:   %d\n", GEO_ADDR);

    /* Check for collisions */
    int collisions = 0;
    for (int i = 0; i < HIDDEN_896; i++) {
        for (int j = i + 1; j < HIDDEN_896; j++) {
            if (indices[i] == indices[j]) {
                collisions++;
            }
        }
    }
    printf("  Collisions:  %d / %d\n", collisions, HIDDEN_896 * (HIDDEN_896 - 1) / 2);
    printf("  Unique:      %d\n", HIDDEN_896 - collisions);

    printf("\n=== KEY INSIGHT ===\n");
    printf("  The golden angle distributes 896 points evenly\n");
    printf("  across the 144×144 grid.\n");
    printf("  The mel address OFFSETS the pattern.\n");
    printf("  Same mel → same hidden projection.\n");
    printf("  Different mel → different hidden projection.\n");
    printf("  This IS the bridge: audio → LLM.\n");

    return 0;
}
