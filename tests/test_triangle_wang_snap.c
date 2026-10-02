/* Triangle-route + Wang 12x6 composition probe.
 *
 * This is deliberately a small integer model of the picture:
 *   - 12 centroid positions around one Wang ring
 *   - 6 directions per centroid
 *   - the three triangle branches use directions 0, 2, 4
 *   - three layers snap to one centroid at each layer
 *
 * It is an experiment, not a replacement for an existing Wang core header.
 */
#include <stdint.h>
#include <stdio.h>

#define CENTROIDS 12u
#define DIRECTIONS 6u
#define LAYERS 3u

typedef struct {
    uint8_t layer;
    uint8_t centroid;
    uint8_t direction;
    uint8_t edge;
} Snap;

static uint8_t branch_direction(uint8_t branch)
{
    return (uint8_t)((branch % 3u) * 2u); /* 0, 2, 4: triangle symmetry */
}

static uint8_t wang_edge(uint8_t layer, uint8_t centroid, uint8_t direction)
{
    (void)layer;
    return (uint8_t)((centroid + direction) % DIRECTIONS);
}

static uint8_t opposite(uint8_t edge) { return (uint8_t)((edge + 3u) % DIRECTIONS); }

static Snap snap(uint8_t layer, uint8_t parent, uint8_t branch, uint8_t desired_edge)
{
    uint8_t direction = branch_direction(branch);
    uint8_t expected = (uint8_t)((parent + direction + layer) % CENTROIDS);
    uint8_t centroid = expected;
    if (desired_edge < DIRECTIONS) {
        /* Snap to the nearest of the two 12-ring centroids with the required
         * complementary edge color. The geometric branch remains the input. */
        uint8_t want = desired_edge;
        uint8_t best_dist = CENTROIDS;
        for (uint8_t candidate = 0; candidate < CENTROIDS; candidate++) {
            if (wang_edge(layer, candidate, direction) != want) continue;
            uint8_t d = (candidate > expected) ? (candidate - expected)
                                               : (expected - candidate);
            if (d < best_dist) { best_dist = d; centroid = candidate; }
        }
    }
    return (Snap){layer, centroid, direction, wang_edge(layer, centroid, direction)};
}

static int edge_join(const Snap *a, const Snap *b)
{
    return b->edge == opposite(a->edge);
}

static int check(int ok, const char *name, int *pass, int *fail)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) (*pass)++; else (*fail)++;
    return ok;
}

int main(void)
{
    int pass = 0, fail = 0;
    uint32_t routes = 1u << (2u * LAYERS); /* 3^3 checked below, not storage */
    (void)routes;

    for (uint32_t code = 0; code < 27u; code++) {
        uint32_t n = code;
        uint8_t parent = 0;
        Snap path[LAYERS];
        int valid = 1;
        for (uint8_t layer = 0; layer < LAYERS; layer++) {
            uint8_t branch = (uint8_t)(n % 3u);
            n /= 3u;
            uint8_t desired = layer ? opposite(path[layer - 1].edge) : DIRECTIONS;
            path[layer] = snap(layer, parent, branch, desired);
            parent = path[layer].centroid;
            if (path[layer].centroid >= CENTROIDS ||
                path[layer].direction >= DIRECTIONS) valid = 0;
            if (layer && !edge_join(&path[layer - 1], &path[layer])) valid = 0;
        }
        check(valid, "3-layer route snaps and Wang edges join", &pass, &fail);
    }

    /* Every layer exposes all 12 centroid slots under the 3 branches. */
    for (uint8_t layer = 0; layer < LAYERS; layer++) {
        uint8_t seen[CENTROIDS] = {0};
        for (uint8_t parent = 0; parent < CENTROIDS; parent++)
            for (uint8_t branch = 0; branch < 3; branch++)
                seen[snap(layer, parent, branch, DIRECTIONS).centroid] = 1;
        int all = 1;
        for (uint8_t i = 0; i < CENTROIDS; i++) if (!seen[i]) all = 0;
        check(all, "layer reaches all 12 Wang centroids", &pass, &fail);
    }

    printf("result: %d passed, %d failed\n", pass, fail);
    printf("model: %ux%u Wang positions/directions, %u triangle layers, %u routes\n",
           CENTROIDS, DIRECTIONS, LAYERS, 27u);
    return fail ? 1 : 0;
}
