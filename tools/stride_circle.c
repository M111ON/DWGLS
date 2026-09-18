/* stride_circle.c — modular-multiplication circle (cf. user Fibonacci pattern)
 * ─────────────────────────────────────────────────────────────────────────
 * Points 0..N-1 on a circle, one chord x → (k·x mod N) each, colored by
 * fiber (x mod gcd(k,N)). Panels: (a) N=144,k=37 — bijection, coverage 144
 * (our stride-37: every slot hit exactly once, no holes/collisions);
 * (b) N=144,k=36 — everything drains into 0, coverage 4/144 (holes —
 * counter-proof the picture discriminates); (c) N=20736,k=37 — the real
 * stride, coverage 20736 (dense envelope). Coverage/image-size is computed
 * (== N/gcd) and printed = the proof scatter needs. Cycle decomposition
 * (72 / 1 / 336 attractors) shown informationally — multiplicative stride
 * is a bijection of many small cycles, NOT one ring.
 *
 * BUILD: gcc -O2 -Wall -o build/stride_circle tools/stride_circle.c -lm
 * RUN:   ./build/stride_circle  →  build/stride_circle.svg
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INF 0xFFFFFFFFu

static uint32_t ugcd(uint32_t a, uint32_t b) {
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* Functional-graph attractor analysis: comp[x] = attractor id (the cycle
   x drains to). Always terminates (finite graph). Returns #attractors. */
static uint32_t analyze(uint32_t N, uint32_t k, uint32_t *comp) {
    static uint32_t stamp[20736];
    static uint32_t curmark = 0;
    for (uint32_t i = 0; i < N; i++) comp[i] = INF;
    uint32_t nattr = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (comp[i] != INF) continue;
        uint32_t cur = ++curmark;
        uint32_t x = i;
        while (comp[x] == INF && stamp[x] != cur) { stamp[x] = cur; x = (uint32_t)(((uint64_t)x * k) % N); }
        uint32_t a = (comp[x] != INF) ? comp[x] : nattr++;
        uint32_t y = i;
        while (comp[y] == INF) { comp[y] = a; y = (uint32_t)(((uint64_t)y * k) % N); }
    }
    return nattr;
}

/* cycle length of the attractor containing `start` (follow until repeat) */
static uint32_t cycle_len(uint32_t N, uint32_t k, uint32_t start) {
    uint32_t x = (uint32_t)(((uint64_t)start * k) % N), n = 1;
    while (x != start) { x = (uint32_t)(((uint64_t)x * k) % N); n++; }
    return n;
}

/* image size = |{ k·x mod N }| = coverage. Scatter needs coverage == N
   (bijection: no holes, no collisions). == N/gcd(k,N) by group theory. */
static uint32_t image_size(uint32_t N, uint32_t k) {
    static uint8_t hit[20736];
    for (uint32_t i = 0; i < N; i++) hit[i] = 0;
    for (uint32_t x = 0; x < N; x++)
        hit[(uint32_t)(((uint64_t)x * k) % N)] = 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < N; i++) n += hit[i];
    return n;
}

static uint32_t comp144[144], comp144b[144], compbig[20736];

static void draw_panel(FILE *f, uint32_t N, uint32_t k, uint32_t g,
                       double ox, double oy, double R,
                       const char *tag, uint32_t nattr, int draw_points) {
    static const char *cols[] = {"#1f77b4","#d62728","#2ca02c","#ff7f0e",
        "#9467bd","#8c564b","#e377c2","#7f7f7f","#bcbd22","#17becf",
        "#393b79","#637939"};
    for (uint32_t x = 0; x < N; x++) {
        uint32_t y = (uint32_t)(((uint64_t)x * k) % N);
        double a1 = 2 * M_PI * x / N - M_PI / 2;
        double a2 = 2 * M_PI * y / N - M_PI / 2;
        const char *c = (g == 1) ? "#1f77b4" : cols[(x % g) % 12];
        if (x == y) { /* fixed point: dot */
            fprintf(f, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"#d62728\"/>\n",
                    ox + R * cos(a1), oy + R * sin(a1));
            continue;
        }
        fprintf(f, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"%s\" stroke-width=\"%s\"/>\n",
                ox + R * cos(a1), oy + R * sin(a1),
                ox + R * cos(a2), oy + R * sin(a2),
                c, N > 1000 ? "0.35" : "0.9");
    }
    if (draw_points)
        for (uint32_t x = 0; x < N; x++) {
            double a = 2 * M_PI * x / N - M_PI / 2;
            fprintf(f, "<circle cx=\"%.2f\" cy=\"%.2f\" r=\"1.6\" fill=\"black\"/>\n",
                    ox + R * cos(a), oy + R * sin(a));
        }
    fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" text-anchor=\"middle\" font-size=\"17\">%s N=%u k=%u attr=%u</text>\n",
            ox, oy + R + 26, tag, N, k, nattr);
}

int main(void) {
    uint32_t g1 = ugcd(144, 37), g2 = ugcd(144, 36), g3 = ugcd(20736, 37);
    uint32_t a1 = analyze(144, 37, comp144);
    uint32_t a2 = analyze(144, 36, comp144b);
    uint32_t a3 = analyze(20736, 37, compbig);
    uint32_t c1 = cycle_len(144, 37, 1);
    uint32_t c3 = cycle_len(20736, 37, 1);
    uint32_t v1 = image_size(144, 37);
    uint32_t v2 = image_size(144, 36);
    uint32_t v3 = image_size(20736, 37);
    printf("N=144 k=37: gcd=%u attractors=%u main-cycle=%u coverage=%u (expect 1/72/4/144)\n", g1, a1, c1, v1);
    printf("N=144 k=36: gcd=%u attractors=%u coverage=%u (expect 36/1/4: drains to 0, holes)\n", g2, a2, v2);
    printf("N=20736 k=37: gcd=%u attractors=%u main-cycle=%u coverage=%u (expect 1/336/576/20736)\n", g3, a3, c3, v3);
    if (g1 != 1 || v1 != 144 || g2 != 36 || a2 != 1 || v2 != 4 ||
        g3 != 1 || v3 != 20736) {
        printf("MISMATCH — abort\n");
        return 1;
    }
    FILE *f = fopen("build/stride_circle.svg", "w");
    if (!f) { printf("cannot open output\n"); return 1; }
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1360\" height=\"480\">\n");
    draw_panel(f, 144, 37, g1, 170, 215, 175, "(a)", a1, 1);
    draw_panel(f, 144, 36, g2, 560, 215, 175, "(b)", a2, 1);
    draw_panel(f, 20736, 37, g3, 1030, 215, 185, "(c)", a3, 0);
    fprintf(f, "</svg>\n");
    fclose(f);
    printf("stride_circle.svg written\n");
    return 0;
}
