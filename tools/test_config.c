/*
 * tools/test_config.c — Test config system without llama.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include "core/infra/config.h"

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "config.json";

    printf("=== Testing Config System ===\n");
    printf("config: %s\n\n", config_path);

    AppConfig cfg;
    if (config_load(config_path, &cfg) < 0) {
        fprintf(stderr, "Failed to load config, using defaults\n");
        cfg = config_default();
    }
    config_print(&cfg);

    /* Test validation */
    printf("\n=== Testing Validation ===\n");
    
    /* Test invalid geometry */
    AppConfig bad = cfg;
    bad.geom.geo_slots = 145;
    if (config_validate_geometry(&bad.geom) < 0) {
        printf("✓ Invalid geo_slots (145) rejected\n");
    }
    
    bad.geom = cfg.geom;
    bad.geom.stride = 36;  /* not coprime to 144 */
    if (config_validate_geometry(&bad.geom) < 0) {
        printf("✓ Invalid stride (36, not coprime) rejected\n");
    }

    /* Test invalid model */
    bad = cfg;
    bad.model.temperature = 3.0f;
    if (config_validate_model(&bad.model) < 0) {
        printf("✓ Invalid temperature (3.0) rejected\n");
    }

    bad.model = cfg.model;
    bad.model.n_ctx = 0;
    if (config_validate_model(&bad.model) < 0) {
        printf("✓ Invalid n_ctx (0) rejected\n");
    }

    printf("\n=== All Tests Passed ===\n");
    return 0;
}