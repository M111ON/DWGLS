/* test_bfs_tensor_pipeline.c — BFS tensor pipeline verification */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bfs_tensor_pipeline.h"
#include "frustum_route.h"

int main(void) {
    /* Create minimal BreathingFS */
    BreathingFS fs = {0};
    bfs_init(&fs);

    BFSTensorPipeline pipe = {0};

    /* Test 1: Initialize pipeline without pack */
    int rc = bfs_tensor_pipeline_init(&pipe, &fs, 1000, 0, 0xFFFFFFu, 4, NULL);
    if (rc != 0) {
        printf("[FAIL] T1: init failed (%d)\n", rc);
        return 1;
    }
    printf("[PASS] T1: pipeline init\n");

    /* Test 2: Route production */
    FrustumRouteEvent evt = {0};
    rc = bfs_tensor_route_produce(&pipe, &evt);
    if (rc != 1) {
        printf("[FAIL] T2: route produce returned %d\n", rc);
        return 1;
    }
    printf("[PASS] T2: route produced (level=%u, node=%u, tensor=%u, ref=%llu)\n",
           evt.level, evt.node_id, evt.tensor_id, (unsigned long long)evt.route_ref);

    /* Test 3: Idempotency */
    rc = bfs_tensor_verify_idempotent(&pipe, 10);
    if (!rc) {
        printf("[FAIL] T3: idempotency check failed\n");
        return 1;
    }
    printf("[PASS] T3: idempotent (10 iterations)\n");

    /* Test 4: Route from position outside mask */
    FrustumSeeker seeker_outside = {
        .position = 1,  /* cell 0: bit 1 */
        .view_id = 0,
        .voronoi_mask = 0xFFFFFFu & ~1u,  /* exclude cell 0 */
        .frustum_depth = 4
    };
    FrustumRouteEvent evt_out = {0};
    rc = fr_route_produce(&seeker_outside, &pipe.route_cache, &evt_out);
    if (rc != 0) {
        printf("[FAIL] T4: outside mask should return 0, got %d\n", rc);
        return 1;
    }
    printf("[PASS] T4: outside mask correctly rejected\n");

    bfs_tensor_pipeline_close(&pipe);
    printf("[PASS] All BFS tensor pipeline checks ok\n");
    return 0;
}