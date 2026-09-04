/*
 * test_weight_placement.c — Experiment: weight placement strategies
 * ═══════════════════════════════════════════════════════════════
 * Compare: straight lines vs limacon curves vs direct addressing
 * Measure: access speed, memory usage, coverage ratio
 *
 * Build: gcc -O2 -Wall -Icore -o tests/test_weight_placement tests/test_weight_placement.c -lm
 * Run:   tests/test_weight_placement
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define FULL_FIELD 20736
#define N_24       24
#define TAU        (2.0 * M_PI)

/* ── Weight slots ─────────────────────────────────────────── */
typedef struct {
    uint32_t flat;      /* flat address 0..20735 */
    int      aa;        /* limacon parameter */
    int      hub;       /* origin hub */
    int      step;      /* step from hub */
    int      direction; /* 1=outward(kis), -1=inward(hyper) */
    double   path_len;  /* path length from hub */
} WeightSlot;

/* ── Strategy: straight line (aa=3) ──────────────────────── */
static int place_straight(WeightSlot *slots, int max_slots) {
    int count = 0;
    for (int hub = 0; hub < N_24 && count < max_slots; hub++) {
        for (int s = 1; s < N_24 && count < max_slots; s++) {
            int target = (hub + s) % N_24;
            /* Straight line: flat = hub * 144 + s * 6 + (target % 6) */
            uint32_t flat = (hub * 144 + s * 6 + (target % 6)) % FULL_FIELD;
            slots[count].flat = flat;
            slots[count].aa = 3;
            slots[count].hub = hub;
            slots[count].step = s;
            slots[count].direction = (s <= 12) ? 1 : -1;
            slots[count].path_len = (double)s * 6.0;  /* approximate */
            count++;
        }
    }
    return count;
}

/* ── Strategy: limacon curves (variable aa) ──────────────── */
static int place_limacon(WeightSlot *slots, int max_slots, int aa_base) {
    int count = 0;
    for (int hub = 0; hub < N_24 && count < max_slots; hub++) {
        for (int s = 1; s < N_24 && count < max_slots; s++) {
            /* Limacon: flat = hub * 144 + aa * s + (hub ^ s) mod 144 */
            int aa = aa_base + (s / 4);  /* vary aa with step */
            uint32_t flat = (hub * 144 + aa * s + ((hub ^ s) % 144)) % FULL_FIELD;
            slots[count].flat = flat;
            slots[count].aa = aa;
            slots[count].hub = hub;
            slots[count].step = s;
            slots[count].direction = (s <= 12) ? 1 : -1;
            slots[count].path_len = (double)s * (double)aa * 0.5;
            count++;
        }
    }
    return count;
}

/* ── Strategy: direct addressing (flat = weight index) ───── */
static int place_direct(WeightSlot *slots, int max_slots) {
    int count = 0;
    for (int i = 0; i < max_slots && i < FULL_FIELD; i++) {
        slots[count].flat = i;
        slots[count].aa = 0;
        slots[count].hub = i / 144;
        slots[count].step = i % 144;
        slots[count].direction = 0;
        slots[count].path_len = 0;
        count++;
    }
    return count;
}

/* ── Metrics ──────────────────────────────────────────────── */
typedef struct {
    int total_slots;
    int unique_addrs;
    int kis_count;      /* outward */
    int hyper_count;    /* inward */
    double avg_path;
    double max_path;
    long compute_ns;
} Metrics;

static Metrics measure(WeightSlot *slots, int count) {
    Metrics m = {0};
    m.total_slots = count;

    /* Count unique addresses */
    int seen[FULL_FIELD];
    memset(seen, 0, sizeof(seen));
    double total_path = 0;
    for (int i = 0; i < count; i++) {
        if (!seen[slots[i].flat]) {
            seen[slots[i].flat] = 1;
            m.unique_addrs++;
        }
        if (slots[i].direction == 1) m.kis_count++;
        else if (slots[i].direction == -1) m.hyper_count++;
        total_path += slots[i].path_len;
        if (slots[i].path_len > m.max_path)
            m.max_path = slots[i].path_len;
    }
    m.avg_path = (count > 0) ? total_path / count : 0;
    return m;
}

/* ── Main ─────────────────────────────────────────────────── */
int main(void) {
    printf("=== Weight Placement Strategies ===\n\n");

    WeightSlot *slots = malloc(FULL_FIELD * sizeof(WeightSlot));
    if (!slots) { perror("malloc"); return 1; }

    /* ── Strategy 1: Straight lines (aa=3) ── */
    printf("--- Strategy 1: Straight Lines (aa=3) ---\n");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n1 = place_straight(slots, FULL_FIELD);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    Metrics m1 = measure(slots, n1);
    m1.compute_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    printf("  slots: %d, unique: %d, kis: %d, hyper: %d\n",
           m1.total_slots, m1.unique_addrs, m1.kis_count, m1.hyper_count);
    printf("  avg path: %.1f, max path: %.1f\n", m1.avg_path, m1.max_path);
    printf("  compute: %ld ns\n\n", m1.compute_ns);

    /* ── Strategy 2: Limacon (aa=3 base) ── */
    printf("--- Strategy 2: Limacon (aa=3 base) ---\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n2 = place_limacon(slots, FULL_FIELD, 3);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    Metrics m2 = measure(slots, n2);
    m2.compute_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    printf("  slots: %d, unique: %d, kis: %d, hyper: %d\n",
           m2.total_slots, m2.unique_addrs, m2.kis_count, m2.hyper_count);
    printf("  avg path: %.1f, max path: %.1f\n", m2.avg_path, m2.max_path);
    printf("  compute: %ld ns\n\n", m2.compute_ns);

    /* ── Strategy 3: Limacon (aa=6 base) ── */
    printf("--- Strategy 3: Limacon (aa=6 base) ---\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n3 = place_limacon(slots, FULL_FIELD, 6);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    Metrics m3 = measure(slots, n3);
    m3.compute_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    printf("  slots: %d, unique: %d, kis: %d, hyper: %d\n",
           m3.total_slots, m3.unique_addrs, m3.kis_count, m3.hyper_count);
    printf("  avg path: %.1f, max path: %.1f\n", m3.avg_path, m3.max_path);
    printf("  compute: %ld ns\n\n", m3.compute_ns);

    /* ── Strategy 4: Direct addressing ── */
    printf("--- Strategy 4: Direct Addressing ---\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n4 = place_direct(slots, FULL_FIELD);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    Metrics m4 = measure(slots, n4);
    m4.compute_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    printf("  slots: %d, unique: %d\n", m4.total_slots, m4.unique_addrs);
    printf("  compute: %ld ns\n\n", m4.compute_ns);

    /* ── Comparison ── */
    printf("=== Comparison ===\n");
    printf("%-20s %8s %8s %8s %8s\n", "Strategy", "unique", "kis", "hyper", "ns");
    printf("%-20s %8d %8d %8d %8ld\n", "Straight (aa=3)", m1.unique_addrs, m1.kis_count, m1.hyper_count, m1.compute_ns);
    printf("%-20s %8d %8d %8d %8ld\n", "Limacon (aa=3)", m2.unique_addrs, m2.kis_count, m2.hyper_count, m2.compute_ns);
    printf("%-20s %8d %8d %8d %8ld\n", "Limacon (aa=6)", m3.unique_addrs, m3.kis_count, m3.hyper_count, m3.compute_ns);
    printf("%-20s %8d %8s %8s %8ld\n", "Direct", m4.unique_addrs, "-", "-", m4.compute_ns);

    free(slots);
    return 0;
}
