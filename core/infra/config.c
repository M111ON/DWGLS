/*
 * core/infra/config.c — Runtime Configuration Implementation
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CJSON_IMPLEMENTATION
#include "cJSON.h"

/* ── Helper: gcd ── */
static int gcd(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

/* ── Helper: get string from JSON (with default) ── */
static void json_get_string(cJSON *obj, const char *key, char *out, size_t out_sz, const char *def) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(out, item->valuestring, out_sz - 1);
        out[out_sz - 1] = '\0';
    } else {
        strncpy(out, def, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

/* ── Helper: get int from JSON (with default) ── */
static int json_get_int(cJSON *obj, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

/* ── Helper: get float from JSON (with default) ── */
static float json_get_float(cJSON *obj, const char *key, float def) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

/* ── Helper: get bool from JSON (with default) ── */
static bool json_get_bool(cJSON *obj, const char *key, bool def) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : def;
}

/* ── Validation ── */
int config_validate_geometry(const GeometryConfig *c) {
    if (c->geo_slots != 144) {
        fprintf(stderr, "config: geo_slots must be 144 (6ico compound), got %d\n", c->geo_slots);
        return -1;
    }
    if (gcd(c->stride, c->geo_slots) != 1) {
        fprintf(stderr, "config: stride %d must be coprime to geo_slots %d\n", c->stride, c->geo_slots);
        return -1;
    }
    if (c->max_layers <= 0 || c->max_layers > 64) {
        fprintf(stderr, "config: max_layers must be 1-64, got %d\n", c->max_layers);
        return -1;
    }
    return 0;
}

int config_validate_kv(const KVConfig *c) {
    if (c->max_slots_per_write <= 0 || c->max_slots_per_write > 1024) {
        fprintf(stderr, "config: max_slots_per_write must be 1-1024, got %d\n", c->max_slots_per_write);
        return -1;
    }
    if (c->container_version <= 0 || c->container_version > 10) {
        fprintf(stderr, "config: container_version must be 1-10, got %d\n", c->container_version);
        return -1;
    }
    return 0;
}

int config_validate_model(const ModelConfig *c) {
    if (c->n_ctx <= 0 || c->n_ctx > 131072) {
        fprintf(stderr, "config: n_ctx must be 1-131072, got %d\n", c->n_ctx);
        return -1;
    }
    if (c->n_batch <= 0 || c->n_batch > c->n_ctx) {
        fprintf(stderr, "config: n_batch must be 1-n_ctx, got %d\n", c->n_batch);
        return -1;
    }
    if (c->n_ubatch <= 0 || c->n_ubatch > c->n_batch) {
        fprintf(stderr, "config: n_ubatch must be 1-n_batch, got %d\n", c->n_ubatch);
        return -1;
    }
    if (c->n_gpu_layers < -1 || c->n_gpu_layers > 128) {
        fprintf(stderr, "config: n_gpu_layers must be -1 to 128, got %d\n", c->n_gpu_layers);
        return -1;
    }
    if (c->temperature < 0.0f || c->temperature > 2.0f) {
        fprintf(stderr, "config: temperature must be 0.0-2.0, got %f\n", c->temperature);
        return -1;
    }
    if (c->top_p < 0.0f || c->top_p > 1.0f) {
        fprintf(stderr, "config: top_p must be 0.0-1.0, got %f\n", c->top_p);
        return -1;
    }
    if (c->top_k < 0 || c->top_k > 1000) {
        fprintf(stderr, "config: top_k must be 0-1000, got %d\n", c->top_k);
        return -1;
    }
    if (strlen(c->model_path) == 0) {
        fprintf(stderr, "config: model_path is required\n");
        return -1;
    }
    return 0;
}

int config_validate_demo(const DemoConfig *c) {
    if (c->checkpoint_tokens < 0 || c->checkpoint_tokens > 1000) {
        fprintf(stderr, "config: checkpoint_tokens must be 0-1000, got %d\n", c->checkpoint_tokens);
        return -1;
    }
    if (c->gen_tokens < 0 || c->gen_tokens > 1000) {
        fprintf(stderr, "config: gen_tokens must be 0-1000, got %d\n", c->gen_tokens);
        return -1;
    }
    return 0;
}

/* ── Main Load ── */
int config_load(const char *json_path, AppConfig *out) {
    /* Read file */
    FILE *f = fopen(json_path, "rb");
    if (!f) {
        fprintf(stderr, "config: cannot open %s\n", json_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        fprintf(stderr, "config: JSON parse error: %s\n", err ? err : "unknown");
        return -1;
    }

    /* Start with defaults */
    *out = config_default();

    /* Geometry section */
    cJSON *geom = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    if (geom) {
        out->geom.geo_slots = json_get_int(geom, "geo_slots", out->geom.geo_slots);
        out->geom.stride = json_get_int(geom, "stride", out->geom.stride);
        out->geom.max_layers = json_get_int(geom, "max_layers", out->geom.max_layers);
    }

    /* KV section */
    cJSON *kv = cJSON_GetObjectItemCaseSensitive(root, "kv");
    if (kv) {
        out->kv.max_slots_per_write = json_get_int(kv, "max_slots_per_write", out->kv.max_slots_per_write);
        out->kv.use_dead_slots = json_get_bool(kv, "use_dead_slots", out->kv.use_dead_slots);
        out->kv.container_version = json_get_int(kv, "container_version", out->kv.container_version);
    }

    /* Model section */
    cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (model) {
        json_get_string(model, "model_path", out->model.model_path, sizeof(out->model.model_path), out->model.model_path);
        out->model.n_ctx = json_get_int(model, "n_ctx", out->model.n_ctx);
        out->model.n_batch = json_get_int(model, "n_batch", out->model.n_batch);
        out->model.n_ubatch = json_get_int(model, "n_ubatch", out->model.n_ubatch);
        out->model.n_gpu_layers = json_get_int(model, "n_gpu_layers", out->model.n_gpu_layers);
        out->model.temperature = json_get_float(model, "temperature", out->model.temperature);
        out->model.top_p = json_get_float(model, "top_p", out->model.top_p);
        out->model.top_k = json_get_int(model, "top_k", out->model.top_k);
        out->model.seed = json_get_int(model, "seed", out->model.seed);
    }

    /* Demo section */
    cJSON *demo = cJSON_GetObjectItemCaseSensitive(root, "demo");
    if (demo) {
        json_get_string(demo, "prompt_a", out->demo.prompt_a, sizeof(out->demo.prompt_a), out->demo.prompt_a);
        json_get_string(demo, "prompt_b", out->demo.prompt_b, sizeof(out->demo.prompt_b), out->demo.prompt_b);
        json_get_string(demo, "test_message", out->demo.test_message, sizeof(out->demo.test_message), out->demo.test_message);
        out->demo.checkpoint_tokens = json_get_int(demo, "checkpoint_tokens", out->demo.checkpoint_tokens);
        out->demo.gen_tokens = json_get_int(demo, "gen_tokens", out->demo.gen_tokens);
    }

    cJSON_Delete(root);

    /* Validate all */
    if (config_validate_geometry(&out->geom) < 0) return -1;
    if (config_validate_kv(&out->kv) < 0) return -1;
    if (config_validate_model(&out->model) < 0) return -1;
    if (config_validate_demo(&out->demo) < 0) return -1;

    return 0;
}

/* ── Default Config ── */
AppConfig config_default(void) {
    AppConfig c = {0};
    c.geom.geo_slots = 144;
    c.geom.stride = 37;
    c.geom.max_layers = 24;

    c.kv.dead_slots_start = 0;   /* computed at runtime */
    c.kv.dead_slots_count = 0;   /* computed at runtime */
    c.kv.max_slots_per_write = 256;
    c.kv.use_dead_slots = true;
    c.kv.container_version = 1;

    strncpy(c.model.model_path, "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf", sizeof(c.model.model_path) - 1);
    c.model.n_ctx = 2048;
    c.model.n_batch = 512;
    c.model.n_ubatch = 512;
    c.model.n_gpu_layers = 99;
    c.model.temperature = 0.7f;
    c.model.top_p = 0.9f;
    c.model.top_k = 40;
    c.model.seed = -1;

    strncpy(c.demo.prompt_a, "Hello", sizeof(c.demo.prompt_a) - 1);
    strncpy(c.demo.prompt_b, "World", sizeof(c.demo.prompt_b) - 1);
    strncpy(c.demo.test_message, "SESSION_STATE_V1: user=john, turn=5, topic=quantum", sizeof(c.demo.test_message) - 1);
    c.demo.checkpoint_tokens = 5;
    c.demo.gen_tokens = 10;

    return c;
}

/* ── Print Config ── */
void config_print(const AppConfig *c) {
    printf("=== Config ===\n");
    printf("  Geometry: slots=%d stride=%d max_layers=%d\n", c->geom.geo_slots, c->geom.stride, c->geom.max_layers);
    printf("  KV: max_slots_per_write=%d use_dead_slots=%d container_v=%d\n", c->kv.max_slots_per_write, c->kv.use_dead_slots, c->kv.container_version);
    printf("  Model: path=%s n_ctx=%d n_batch=%d n_ubatch=%d n_gpu=%d temp=%.2f top_p=%.2f top_k=%d seed=%d\n",
           c->model.model_path, c->model.n_ctx, c->model.n_batch, c->model.n_ubatch, c->model.n_gpu_layers,
           c->model.temperature, c->model.top_p, c->model.top_k, c->model.seed);
    printf("  Demo: prompt_a='%s' prompt_b='%s' checkpoint=%d gen=%d\n",
           c->demo.prompt_a, c->demo.prompt_b, c->demo.checkpoint_tokens, c->demo.gen_tokens);
    printf("================\n");
}