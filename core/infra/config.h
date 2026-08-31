/*
 * core/infra/config.h — Runtime Configuration System for DWGLS
 *
 * Philosophy: Config per component, explicit dependency injection.
 * No global config singleton. Each module gets its own config struct.
 */

#ifndef DWGLS_CONFIG_H
#define DWGLS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ── JSON parser (cJSON header-only) ── */
#include "cJSON.h"

/* ── Component Config Structs ── */

/* Geometry addressing config */
typedef struct {
    int geo_slots;        /* 144 (6ico compound) — SACRED, validate == 144 */
    int stride;           /* 37 (coprime to 144) — SACRED, validate gcd(stride, slots)==1 */
    int max_layers;       /* 24 for Qwen2.5-0.5B */
} GeometryConfig;

/* KV Storage config */
typedef struct {
    int dead_slots_start;  /* computed at runtime: n_pos + 1 */
    int dead_slots_count;  /* computed at runtime: n_ctx - dead_slots_start */
    int max_slots_per_write; /* safety limit */
    bool use_dead_slots;   /* enable/disable dead slot storage */
    int container_version; /* DWGL container format version */
} KVConfig;

/* Model/Inference config */
typedef struct {
    char model_path[512];
    int n_ctx;
    int n_batch;
    int n_ubatch;
    int n_gpu_layers;      /* for llama_model_params */
    float temperature;
    float top_p;
    int top_k;
    int seed;
} ModelConfig;

/* Demo/Testing config */
typedef struct {
    char prompt_a[256];
    char prompt_b[256];
    char test_message[512];
    int checkpoint_tokens;
    int gen_tokens;
} DemoConfig;

/* ── Unified App Config (parsed from JSON, split to components) ── */
typedef struct {
    GeometryConfig geom;
    KVConfig kv;
    ModelConfig model;
    DemoConfig demo;
} AppConfig;

/* ── API ── */

/* Load and validate config from JSON file.
 * Returns 0 on success, -1 on error.
 * On success, `out` is filled with validated config.
 * On error, prints to stderr and returns -1. */
int config_load(const char *json_path, AppConfig *out);

/* Validate a single component config.
 * Returns 0 on success, -1 on validation error (prints to stderr). */
int config_validate_geometry(const GeometryConfig *c);
int config_validate_kv(const KVConfig *c);
int config_validate_model(const ModelConfig *c);
int config_validate_demo(const DemoConfig *c);

/* Print config summary (for debugging) */
void config_print(const AppConfig *c);

/* Default config (compile-time fallbacks) */
AppConfig config_default(void);

#endif /* DWGLS_CONFIG_H */