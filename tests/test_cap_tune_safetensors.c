/* test_cap_tune_safetensors.c — Gate tuning on REAL safetensors
 * ═══════════════════════════════════════════════════════════════════════════
 * Same pipeline as test_cap_tune_real (GGUF) but on .safetensors files:
 *   LFM2.5-VL-450M/model.safetensors   (I:/model — 897 MB)
 *   smolVLM-256M-Instruct/model.safetensors (I:/model — 513 MB)
 *   F:/model/zimage/ae.safetensors     (167 MB — autoencoder)
 *
 * Safetensors: 8-byte header length + JSON header {name: {dtype, shape,
 * data_offsets}} + raw data.  We parse ONLY the header (no data read).
 *
 * Same metrics as the GGUF tuning:
 *   home(rank) = (rank·37)%20736,  w = home%144  (the placement formula)
 *   depth = w;  w > envelope_depth(gate) → LIFT;  w ≤ → PLACE (field)
 *   field windows: file-order vs targeted-smallest ranks (field ranks
 *   {0,4,39,74,109,113,...} get the smallest tensors — §15.33)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_tune_safetensors tests/test_cap_tune_safetensors.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "../core/geo_ghost_envelope.h"

#define WIN 20736u

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── minimal safetensors header parse (name + n_weights only) ── */
typedef struct {
    char     name[256];
    uint64_t shape[8];
    int      n_dims;
    uint64_t n_weights;
} STTensor;

static int parse_st_header(const char *json, uint64_t json_len,
                           STTensor *out, int max_out) {
    int n = 0;
    const char *p = json;
    const char *end = json + json_len;
    while (p < end && n < max_out) {
        const char *q1 = memchr(p, '"', end - p);
        if (!q1) break;
        q1++;
        const char *q2 = memchr(q1, '"', end - q1);
        if (!q2) break;
        int name_len = (int)(q2 - q1);
        if (name_len > 255 || name_len == 0) { p = q2 + 1; continue; }

        if (name_len == 12 && memcmp(q1, "__metadata__", 12) == 0) {
            const char *brace = memchr(q2, '{', end - (q2 + 1));
            if (brace) {
                int depth = 0;
                while (brace < end) {
                    if (*brace == '{') depth++;
                    else if (*brace == '}') { depth--; if (depth == 0) break; }
                    brace++;
                }
                if (brace < end) p = brace + 1; else p = q2 + 1;
            } else p = q2 + 1;
            continue;
        }

        const char *scan = q2 + 1;
        while (scan < end && (*scan == ' ' || *scan == ':' || *scan == '\n')) scan++;
        if (scan >= end || *scan != '{') { p = q2 + 1; continue; }

        const char *shape_pos = NULL;
        const char *search_end = (scan + 500 < end) ? scan + 500 : end;
        const char *s = scan + 1;
        while (s < search_end) {
            if (memcmp(s, "\"shape\"", 7) == 0) { shape_pos = s; break; }
            s++;
        }
        if (!shape_pos) { p = q2 + 1; continue; }

        STTensor *t = &out[n];
        memset(t, 0, sizeof(*t));
        memcpy(t->name, q1, name_len);
        t->name[name_len] = 0;

        const char *sp = shape_pos + 7;
        while (*sp && *sp != '[') sp++;
        if (*sp == '[') {
            sp++;
            while (*sp && *sp != ']' && t->n_dims < 8) {
                while (*sp && !isdigit((unsigned char)*sp) && *sp != '-') sp++;
                if (*sp == ']') break;
                t->shape[t->n_dims++] = strtoull(sp, (char **)&sp, 10);
            }
        }
        t->n_weights = 1;
        for (int d = 0; d < t->n_dims; d++) t->n_weights *= t->shape[d];
        n++;
        p = q2 + 1;
    }
    return n;
}

/* ── tuning helpers (same as test_cap_tune_real) ── */
static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}
static uint64_t view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}

/* targeted-smallest field footprint (field ranks get the smallest tensors) */
static uint64_t field_windows_optimal(const uint64_t *s, uint32_t N, double gate) {
    uint32_t k_max = ght_envelope_depth(gate);
    uint32_t *fr = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint32_t nf = 0;
    for (uint32_t r = 0; r < N; r++)
        if (scale_w(r) <= k_max) fr[nf++] = r;
    for (uint32_t i = 0; i < nf; i++)
        for (uint32_t j = i + 1; j < nf; j++)
            if (scale_w(fr[j]) < scale_w(fr[i])) { uint32_t t = fr[i]; fr[i] = fr[j]; fr[j] = t; }
    uint32_t *idx = (uint32_t *)malloc(N * sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++) idx[i] = i;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = i;
        for (uint32_t j = i + 1; j < N; j++)
            if (s[idx[j]] < s[idx[b]]) b = j;
        uint32_t t = idx[i]; idx[i] = idx[b]; idx[b] = t;
    }
    uint64_t field = 0;
    for (uint32_t i = 0; i < nf; i++)
        field += view_of(s[idx[i]], scale_w(fr[i]));
    free(fr); free(idx);
    return (field + WIN - 1) / WIN;
}

typedef struct {
    const char *label;
    const char *path;
    int      present;
    uint32_t N;
    uint64_t E;
    double   lifts_050, lifts_100, lifts_200;
    uint64_t field_cur, field_opt, base_windows;
    const char *rank0_name;
    uint64_t rank0_elems;
} Model;

static void run_model(Model *m) {
    FILE *fp = fopen(m->path, "rb");
    if (!fp) { printf("  (cannot open — skipping)\n"); m->present = 0; return; }
    m->present = 1;

    uint64_t header_len = 0;
    if (fread(&header_len, 8, 1, fp) != 1) { fclose(fp); m->present = 0; return; }
    char *header = (char *)malloc((size_t)header_len + 1);
    if (!header || fread(header, 1, (size_t)header_len, fp) != (size_t)header_len) {
        printf("  (header read failed — skipping)\n");
        free(header); fclose(fp); m->present = 0; return;
    }
    header[header_len] = 0;

    STTensor tensors[2048];
    int n = parse_st_header(header, header_len, tensors, 2048);
    free(header); fclose(fp);
    if (n == 0) { printf("  (no tensors parsed — skipping)\n"); m->present = 0; return; }

    uint32_t N = (uint32_t)n;
    uint64_t *s = (uint64_t *)calloc(N, sizeof(uint64_t));
    uint64_t E = 0;
    for (uint32_t i = 0; i < N; i++) { s[i] = tensors[i].n_weights; E += s[i]; }
    m->N = N; m->E = E;
    m->base_windows = (E + WIN - 1) / WIN;

    double lifts[3] = { 0, 0, 0 };
    double gates[3] = { 0.5, 1.0, 2.0 };
    uint64_t field[3] = { 0, 0, 0 };
    for (uint32_t g = 0; g < 3; g++) {
        uint32_t k_max = ght_envelope_depth(gates[g]);
        for (uint32_t i = 0; i < N; i++) {
            uint8_t w = scale_w(i);
            if (w > k_max) lifts[g] += 1.0;
            else           field[g] += view_of(s[i], w);
        }
        lifts[g] = 100.0 * lifts[g] / (double)N;
    }
    m->lifts_050 = lifts[0]; m->lifts_100 = lifts[1]; m->lifts_200 = lifts[2];
    m->field_cur = (field[1] + WIN - 1) / WIN;
    m->field_opt = field_windows_optimal(s, N, 1.0);
    m->rank0_name = tensors[0].name; m->rank0_elems = s[0];
    {
        uint64_t mn = s[0]; uint32_t bi = 0;
        for (uint32_t i = 1; i < N; i++) if (s[i] < mn) { mn = s[i]; bi = i; }
        (void)bi;
    }

    printf("  %u tensors, E = %llu elems, base (no lift) = %llu windows\n",
           N, (unsigned long long)E, (unsigned long long)m->base_windows);
    printf("  lifts%%: 0.5→%.1f | 1.0→%.1f | 2.0→%.1f\n", lifts[0], lifts[1], lifts[2]);
    printf("  field windows: file-order %llu → targeted-smallest %llu (%.1f×)\n",
           (unsigned long long)m->field_cur, (unsigned long long)m->field_opt,
           (double)m->field_cur / (double)(m->field_opt ? m->field_opt : 1));
    printf("  rank-0 tensor [%s] %llu elems (w=0 — field full price ถ้าอยู่ file order)\n",
           m->rank0_name, (unsigned long long)m->rank0_elems);

    free(s);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Envelope gate tuning — real safetensors\n");
    printf("════════════════════════════════════════════════════════\n");

    Model models[3];
    models[0] = (Model){ "LFM2.5-VL-450M", "I:/model/LFM2.5-VL-450M/model.safetensors", 0,0,0,0,0,0,0,0,0,0,0 };
    models[1] = (Model){ "smolVLM-256M",   "I:/model/smolVLM-256M-Instruct/model.safetensors", 0,0,0,0,0,0,0,0,0,0,0 };
    models[2] = (Model){ "zimage-ae",      "F:/model/zimage/ae.safetensors", 0,0,0,0,0,0,0,0,0,0,0 };
    if (argc > 1) models[0].path = argv[1];
    if (argc > 2) models[1].path = argv[2];
    if (argc > 3) models[2].path = argv[3];

    uint32_t n_present = 0;
    for (uint32_t i = 0; i < 3; i++) {
        printf("\n═ %s — %s ═\n", models[i].label, models[i].path);
        run_model(&models[i]);
        if (models[i].present) n_present++;
    }

    double l050 = 0, l100 = 0, l200 = 0;
    uint64_t fcur = 0, fopt = 0, base = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (!models[i].present) continue;
        l050 += models[i].lifts_050; l100 += models[i].lifts_100; l200 += models[i].lifts_200;
        fcur += models[i].field_cur; fopt += models[i].field_opt; base += models[i].base_windows;
    }
    if (n_present) { l050 /= n_present; l100 /= n_present; l200 /= n_present; }

    printf("\n════════════════════════════════════════════════════════\n");
    printf("AGGREGATE (%u/%u present)\n", n_present, 3u);
    printf("  lifts%%: 0.5→%.1f | 1.0→%.1f | 2.0→%.1f\n", l050, l100, l200);
    printf("  field: file-order %llu → targeted %llu windows (Σ); base %llu\n",
           (unsigned long long)fcur, (unsigned long long)fopt, (unsigned long long)base);

    CHECK(1, "≥2 safetensors parsed (real data)", n_present >= 2);
    CHECK(2, "lift rate @1.0 > 75% — w-distribution uniform (สูตร placement)",
          n_present >= 2 && l100 > 75.0 && l100 < 100.0);
    CHECK(3, "knob monotonic: rate(2.0) ≥ rate(1.0) ≥ rate(0.5)",
          n_present >= 2 && l200 >= l100 && l100 >= l050);
    int never_worse = 1;
    for (uint32_t i = 0; i < 3; i++)
        if (models[i].present && models[i].field_opt > models[i].field_cur) never_worse = 0;
    CHECK(4, "targeted ranks never worse than file order (ทุกไฟล์)", n_present >= 2 && never_worse);
    CHECK(5, "targeted cuts aggregate field", n_present >= 2 && fopt < fcur);
    CHECK(6, "targeted field fits in a handful of windows (Σ ≤ 64)", n_present >= 2 && fopt <= 64);

    printf("\n════════════════════════════════════════════════════════\n");
    printf("files present: %u/3 — RESULTS: %d/%d PASS\n", n_present, pass, pass + fail);
    return fail ? 1 : 0;
}
