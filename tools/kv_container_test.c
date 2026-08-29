/*
 * tools/kv_container_test.c — Stage 3: DWGLS container format in dead KV slots
 * ════════════════════════════════════════════════════════════════════════════
 * Container format for storing data in dead KV cache positions.
 *
 * KEY INSIGHT: The container must be written AFTER all inference steps,
 * because each decode writes KV at position n_pos, which would overwrite
 * data at [n_pos+1) if written before. The model only ATTENDS to [0,n_pos),
 * so data at [n_pos+1,...) is safe — but the KV WRITE at n_pos overlaps.
 *
 * Flow: process prompt → generate N tokens → NOW write container → read back
 *
 * Container layout (stored in dead KV slots of ALL layers):
 *   Offset  Size   Field
 *   0       4      Magic: "DWGL"
 *   4       1      Version: 1
 *   5       2      n_pos at write time (uint16 LE)
 *   7       2      n_ctx (uint16 LE)
 *   9       2      n_layers (uint16 LE)
 *   11      2      k_stride (uint16 LE)
 *   13      4      payload_size (uint32 LE)
 *   17      4      checksum (FNV-1a of payload)
 *   21      N      payload data
 *
 * BUILD:
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -Icore -II:/llama/include -o build/kv_container_test.exe \
 *     tools/kv_container_test.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 *
 * RUN:
 *   $env:PATH = "C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;" + $env:PATH
 *   .\build\kv_container_test.exe [model.gguf] "prompt"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../core/gguf_reader.h"
#include "llama.h"

/* ── Container header ── */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];     /* "DWGL" */
    uint8_t  version;      /* 1 */
    uint16_t n_pos_w;      /* n_pos at write time */
    uint16_t n_ctx;        /* context size */
    uint16_t n_layers;     /* number of layers */
    uint16_t k_stride;     /* bytes per K position */
    uint32_t payload_size; /* payload bytes */
    uint32_t checksum;     /* FNV-1a of payload */
} dwgl_header_t;
#pragma pack(pop)

#define DWGL_MAGIC "DWGL"
#define DWGL_VERSION 1
#define DWGL_HEADER_SIZE 21

static uint32_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ── helpers ── */
static int top5_match(const float *a, const float *b, int n) {
    int ai[5], bi[5]; float av[5], bv[5];
    for (int k = 0; k < 5; k++) { av[k] = bv[k] = -1e9f; ai[k] = bi[k] = -1; }
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 5; k++) {
            if (a[i] > av[k]) {
                for (int j = 4; j > k; j--) { av[j] = av[j-1]; ai[j] = ai[j-1]; }
                av[k] = a[i]; ai[k] = i; break;
            }
        }
        for (int k = 0; k < 5; k++) {
            if (b[i] > bv[k]) {
                for (int j = 4; j > k; j--) { bv[j] = bv[j-1]; bi[j] = bi[j-1]; }
                bv[k] = b[i]; bi[k] = i; break;
            }
        }
    }
    int m = 0;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            if (ai[i] == bi[j]) { m++; break; }
    return m;
}

static double kl_div(const float *p, const float *q, int n) {
    double kl = 0.0;
    for (int i = 0; i < n; i++) {
        double pi = exp((double)p[i]);
        double qi = exp((double)q[i]);
        if (qi < 1e-15) qi = 1e-15;
        if (pi < 1e-15) pi = 1e-15;
        kl += pi * (log(pi) - log(qi));
    }
    return kl;
}

/* ── Model context ── */
static struct llama_model *g_model;
static struct llama_vocab *g_vocab;

typedef struct {
    struct llama_context *ctx;
    llama_memory_t mem;
    int n_pos, n_layers;
    size_t k_stride;
} run_t;

static run_t make_run(const char *prompt) {
    run_t r = {0};
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048; cp.n_batch = 512; cp.n_ubatch = 512;
    r.ctx = llama_init_from_model(g_model, cp);
    if (!r.ctx) return r;
    r.mem = llama_get_memory(r.ctx);
    r.n_layers = (int)llama_memory_kv_cache_get_n_layers(r.mem);
    struct ggml_tensor *k0 = llama_memory_kv_cache_get_layer_k(r.mem, 0);
    if (k0) r.k_stride = k0->nb[1];

    llama_token tok[1024];
    int n = llama_tokenize(g_vocab, prompt, strlen(prompt), tok, 1024, true, false);
    if (n < 0) { llama_free(r.ctx); r.ctx = NULL; return r; }
    struct llama_batch batch = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; i++) {
        batch.token[i] = tok[i]; batch.pos[i] = i;
        batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n - 1) ? 1 : 0;
    }
    batch.n_tokens = n;
    if (llama_decode(r.ctx, batch) != 0) { llama_batch_free(batch); llama_free(r.ctx); r.ctx = NULL; return r; }
    r.n_pos = n;
    llama_batch_free(batch);
    return r;
}

static float *do_decode(run_t *r, const char *word) {
    llama_token tok[8];
    int n = llama_tokenize(g_vocab, word, strlen(word), tok, 8, true, false);
    if (n <= 0) return NULL;
    struct llama_batch batch = llama_batch_init(1, 0, 1);
    batch.token[0] = tok[0]; batch.pos[0] = r->n_pos;
    batch.n_seq_id[0] = 1; batch.seq_id[0][0] = 0;
    batch.logits[0] = 1; batch.n_tokens = 1;
    if (llama_decode(r->ctx, batch) != 0) { llama_batch_free(batch); return NULL; }
    r->n_pos++;
    int nv = llama_vocab_n_tokens(g_vocab);
    float *out = (float *)malloc(nv * sizeof(float));
    memcpy(out, llama_get_logits(r->ctx), nv * sizeof(float));
    llama_batch_free(batch);
    return out;
}

/* ── Write data into dead slots of ALL layers ── */
static void write_dead(run_t *r, int slot_start, int slot_end, const uint8_t *data, size_t data_len) {
    for (int li = 0; li < r->n_layers; li++) {
        struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, li);
        struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(r->mem, li);
        if (!k || !v || !k->data || !v->data) continue;
        for (int pos = slot_start; pos < slot_end; pos++) {
            size_t off = (size_t)pos * k->nb[1];
            size_t src = (size_t)(pos - slot_start) * k->nb[1];
            if (src + k->nb[1] <= data_len) {
                memcpy((uint8_t *)k->data + off, data + src, k->nb[1]);
                uint8_t tmp[512];
                for (size_t b = 0; b < k->nb[1]; b++) tmp[b] = data[src + b] ^ 0xFF;
                memcpy((uint8_t *)v->data + off, tmp, k->nb[1]);
            }
        }
    }
}

/* ── Read data from dead slots of one layer (K tensor) ── */
static void read_dead_k(run_t *r, int slot_start, int slot_end, uint8_t *out, size_t out_len, int layer) {
    struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(r->mem, layer);
    if (!k || !k->data) return;
    for (int pos = slot_start; pos < slot_end; pos++) {
        size_t off = (size_t)pos * k->nb[1];
        size_t dst = (size_t)(pos - slot_start) * k->nb[1];
        if (dst + k->nb[1] > out_len) break;
        memcpy(out + dst, (uint8_t *)k->data + off, k->nb[1]);
    }
}

/* ── Encode container into dead slots ── */
static int container_encode(run_t *r, const uint8_t *payload, uint32_t payload_size) {
    int dead_start = r->n_pos + 1;
    size_t total = DWGL_HEADER_SIZE + payload_size;
    int slots_needed = (int)((total + r->k_stride - 1) / r->k_stride);
    int dead_end = dead_start + slots_needed;
    if (dead_end > 2048) return -1;

    dwgl_header_t hdr;
    memcpy(hdr.magic, DWGL_MAGIC, 4);
    hdr.version = DWGL_VERSION;
    hdr.n_pos_w = (uint16_t)r->n_pos;
    hdr.n_ctx = 2048;
    hdr.n_layers = (uint16_t)r->n_layers;
    hdr.k_stride = (uint16_t)r->k_stride;
    hdr.payload_size = payload_size;
    hdr.checksum = fnv1a(payload, payload_size);

    size_t buf_size = (size_t)slots_needed * r->k_stride;
    uint8_t *buf = (uint8_t *)calloc(buf_size, 1);
    memcpy(buf, &hdr, DWGL_HEADER_SIZE);
    if (payload_size > 0) memcpy(buf + DWGL_HEADER_SIZE, payload, payload_size);

    write_dead(r, dead_start, dead_end, buf, buf_size);
    free(buf);
    return slots_needed;
}

/* ── Read container header from a known slot position ── */
static int container_read_header(run_t *r, int slot, dwgl_header_t *hdr) {
    uint8_t buf[256];
    read_dead_k(r, slot, slot + 1, buf, sizeof(buf), 0);
    if (memcmp(buf, DWGL_MAGIC, 4) != 0) return -1;
    memcpy(hdr, buf, DWGL_HEADER_SIZE);
    return 0;
}

/* ── Read container payload ── */
static int container_read_payload(run_t *r, int slot, const dwgl_header_t *hdr, uint8_t *out, size_t out_cap) {
    if (hdr->payload_size == 0) return 0;
    size_t payload_slots = ((size_t)hdr->payload_size + hdr->k_stride - 1) / hdr->k_stride;
    size_t total_bytes = payload_slots * hdr->k_stride;
    uint8_t *tmp = (uint8_t *)malloc(total_bytes);
    read_dead_k(r, slot, slot + (int)payload_slots + 1, tmp, total_bytes, 0);
    memcpy(out, tmp + DWGL_HEADER_SIZE, hdr->payload_size < out_cap ? hdr->payload_size : out_cap);
    free(tmp);
    uint32_t ck = fnv1a(out, hdr->payload_size);
    return (ck == hdr->checksum) ? 0 : -2;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";

    fprintf(stderr, "=== kv_container_test — DWGLS container format ===\n");
    fprintf(stderr, "model: %s\n\n", model_path);

    llama_backend_init();
    g_model = llama_model_load_from_file(model_path, llama_model_default_params());
    if (!g_model) { fprintf(stderr, "FAIL: load model\n"); return 1; }
    g_vocab = (struct llama_vocab *)llama_model_get_vocab(g_model);

    int pass = 0, fail = 0;

    /* ──────────────────────────────────────────────
     * TEST 1: Container roundtrip (encode AFTER all decode)
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 1: Container roundtrip (encode after decode) ===\n");
    {
        run_t r = make_run(prompt);
        if (!r.ctx) { printf("FAIL\n"); return 1; }

        /* Generate some tokens first */
        for (int i = 0; i < 4; i++) { float *l = do_decode(&r, " the"); free(l); }
        printf("  After prompt + 4 tokens: n_pos=%d\n", r.n_pos);

        /* NOW encode container — no more decodes will happen */
        const char *msg = "Hello from DWGLS! Container test payload.";
        uint32_t plen = (uint32_t)strlen(msg);
        int slots = container_encode(&r, (const uint8_t *)msg, plen);
        printf("  Encoded at slots [%d, %d), %d slots\n", r.n_pos + 1, r.n_pos + 1 + slots, slots);

        /* Readback */
        dwgl_header_t hdr;
        uint8_t payload[4096];
        int cstart = r.n_pos + 1;
        if (container_read_header(&r, cstart, &hdr) == 0 &&
            container_read_payload(&r, cstart, &hdr, payload, sizeof(payload)) == 0) {
            int match = (memcmp(payload, msg, plen) == 0);
            printf("  Readback: '%.*s' → %s\n", plen, payload, match ? "PASS" : "FAIL");
            if (match) pass++; else fail++;
        } else {
            printf("  Readback: FAIL (bad header/checksum)\n");
            fail++;
        }

        llama_free(r.ctx);
    }

    /* ──────────────────────────────────────────────
     * TEST 2: Container survives generation (encode after, read after)
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 2: Encode after 8 tokens → read back ===\n");
    {
        run_t r = make_run(prompt);
        for (int i = 0; i < 8; i++) { float *l = do_decode(&r, " a"); free(l); }
        printf("  After prompt + 8 tokens: n_pos=%d\n", r.n_pos);

        const char *msg = "GENERATION_TEST_XYZ";
        uint32_t plen = (uint32_t)strlen(msg);
        container_encode(&r, (const uint8_t *)msg, plen);

        dwgl_header_t hdr;
        uint8_t payload[4096];
        int cstart = r.n_pos + 1;
        if (container_read_header(&r, cstart, &hdr) == 0 &&
            container_read_payload(&r, cstart, &hdr, payload, sizeof(payload)) == 0) {
            int match = (memcmp(payload, msg, plen) == 0);
            printf("  Readback: '%.*s' → %s\n", plen, payload, match ? "PASS" : "FAIL");
            if (match) pass++; else fail++;
        } else {
            printf("  Readback: FAIL\n"); fail++;
        }

        llama_free(r.ctx);
    }

    /* ──────────────────────────────────────────────
     * TEST 3: Large payload (1000 bytes)
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 3: Large payload (1000 bytes) ===\n");
    {
        run_t r = make_run(prompt);
        for (int i = 0; i < 4; i++) { float *l = do_decode(&r, " x"); free(l); }

        uint8_t bigpayload[1000];
        for (int i = 0; i < 1000; i++) bigpayload[i] = (uint8_t)((i * 7 + 42) & 0xFF);
        container_encode(&r, bigpayload, 1000);

        dwgl_header_t hdr;
        uint8_t readback[1000];
        int cstart = r.n_pos + 1;
        if (container_read_header(&r, cstart, &hdr) == 0 &&
            container_read_payload(&r, cstart, &hdr, readback, sizeof(readback)) == 0) {
            int match = (memcmp(readback, bigpayload, 1000) == 0);
            printf("  Readback: 1000 bytes → %s\n", match ? "PASS" : "FAIL");
            if (match) pass++; else fail++;
        } else {
            printf("  Readback: FAIL\n"); fail++;
        }

        llama_free(r.ctx);
    }

    /* ──────────────────────────────────────────────
     * TEST 4: Multiple containers (write 2 different payloads)
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 4: Two containers at different positions ===\n");
    {
        run_t r = make_run(prompt);

        const char *msg1 = "FIRST_CONTAINER";
        const char *msg2 = "SECOND_CONTAINER";
        uint32_t len1 = (uint32_t)strlen(msg1);
        uint32_t len2 = (uint32_t)strlen(msg2);

        /* Container 1: after prompt */
        container_encode(&r, (const uint8_t *)msg1, len1);
        int c1_start = r.n_pos + 1;

        /* Container 2: after 4 more tokens */
        for (int i = 0; i < 4; i++) { float *l = do_decode(&r, " y"); free(l); }
        container_encode(&r, (const uint8_t *)msg2, len2);
        int c2_start = r.n_pos + 1;

        /* Read both back */
        dwgl_header_t h1, h2;
        uint8_t p1[256], p2[256];
        int ok1 = (container_read_header(&r, c1_start, &h1) == 0 &&
                   container_read_payload(&r, c1_start, &h1, p1, sizeof(p1)) == 0 &&
                   memcmp(p1, msg1, len1) == 0);
        int ok2 = (container_read_header(&r, c2_start, &h2) == 0 &&
                   container_read_payload(&r, c2_start, &h2, p2, sizeof(p2)) == 0 &&
                   memcmp(p2, msg2, len2) == 0);

        printf("  Container 1 at slot %d: '%.*s' → %s\n", c1_start, len1, p1, ok1 ? "PASS" : "FAIL");
        printf("  Container 2 at slot %d: '%.*s' → %s\n", c2_start, len2, p2, ok2 ? "PASS" : "FAIL");
        if (ok1 && ok2) pass++; else fail++;

        llama_free(r.ctx);
    }

    /* ──────────────────────────────────────────────
     * TEST 5: Cross-model — short vs long prompt
     * ────────────────────────────────────────────── */
    printf("\n=== TEST 5: Cross-prompt-length validation ===\n");
    {
        const char *short_p = "Hi";
        const char *long_p = "The quick brown fox jumps over the lazy dog near the river bank in the morning";

        run_t rs = make_run(short_p);
        run_t rl = make_run(long_p);

        if (rs.ctx && rl.ctx) {
            const char *msg = "CROSS_PROMPT_TEST";
            uint32_t plen = (uint32_t)strlen(msg);

            /* Short prompt: more dead slots */
            container_encode(&rs, (const uint8_t *)msg, plen);
            dwgl_header_t hs;
            uint8_t ps[256];
            int cs = container_read_header(&rs, rs.n_pos + 1, &hs);
            if (cs == 0) cs = container_read_payload(&rs, rs.n_pos + 1, &hs, ps, sizeof(ps));
            int ok_s = (cs == 0 && memcmp(ps, msg, plen) == 0);

            /* Long prompt: fewer dead slots */
            container_encode(&rl, (const uint8_t *)msg, plen);
            dwgl_header_t hl;
            uint8_t pl_buf[256];
            int cl = container_read_header(&rl, rl.n_pos + 1, &hl);
            if (cl == 0) cl = container_read_payload(&rl, rl.n_pos + 1, &hl, pl_buf, sizeof(pl_buf));
            int ok_l = (cl == 0 && memcmp(pl_buf, msg, plen) == 0);

            printf("  Short prompt (n_pos=%d): %s\n", rs.n_pos, ok_s ? "PASS" : "FAIL");
            printf("  Long prompt  (n_pos=%d): %s\n", rl.n_pos, ok_l ? "PASS" : "FAIL");
            if (ok_s && ok_l) pass++; else fail++;

            llama_free(rs.ctx);
            llama_free(rl.ctx);
        }
    }

    /* ── Final Summary ── */
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  DWGLS CONTAINER TEST: %d/%d PASSED\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════\n");

    printf("\nContainer format: DWGL v1, %d-byte header\n", DWGL_HEADER_SIZE);
    printf("Storage: ALL layers K+V, positions [n_pos+1, ...)\n");
    printf("Constraint: encode AFTER all inference (decode writes at n_pos)\n");

    llama_model_free(g_model);
    llama_backend_free();

    printf("\nRESULT: DWGLS container %s\n", fail == 0 ? "COMPLETE" : "HAS FAILURES");
    return fail == 0 ? 0 : 1;
}
