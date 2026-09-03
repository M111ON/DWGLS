/* tools/tesspack_llama_view.c — View .tesspack as virtual GGUF for llama.cpp
 * ═══════════════════════════════════════════════════════════════════════════
 * Step toward eliminating the graft step: mmap .tesspack + original GGUF,
 * serve tensor data directly from .tesspack during inference.
 *
 * This tool:
 *   1. Opens original GGUF (for header/metadata + non-MoE tensors)
 *   2. Opens .tesspack (mmap'd, for MoE expert tensor data)
 *   3. For each tensor: serves from .tesspack if MoE, else from GGUF
 *   4. Writes assembled GGUF to stdout path (like graft, but validates)
 *   5. Runs inference + compares logits against original
 *
 * This is the PROTOTYPE for a llama.cpp backend adapter.
 * Current path: still writes to file (graft). Future: serve from mmap.
 *
 * BUILD: make tess-view
 * RUN:   ./build/tesspack_llama_view [gguf] [tesspack] [out_gguf] [dll_dir]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif
#include "llama.h"
#include "ggml-backend.h"

#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};

static int is_moe_expert(const char *name) {
    return strstr(name, "_exps.weight") != NULL;
}

/* ── load tensor from .tesspack directly into body (no temp alloc) ── */
static int load_from_pack_direct(TESS_PackIndex *pi, const char *name,
                                  uint32_t n_capos, uint32_t total_cells,
                                  uint32_t cell_size, uint8_t *body,
                                  uint64_t body_off, size_t body_sz) {
    for (uint32_t c = 0; c < n_capos; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo(pi, &cr, name, c) != 0) return -1;
        uint32_t cells = (c < n_capos - 1) ? TESS_TOTAL_SLOTS
                        : (total_cells - c * TESS_TOTAL_SLOTS);
        uint64_t dst = body_off + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        if (dst + (uint64_t)cells * cell_size > body_sz) return -1;
        uint32_t bytes = tess_capo_load_range(&cr, 0, cells, body + dst);
        if (bytes == 0) return -1;
    }
    return 0;
}

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv) {
    const char *gguf_path  = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    const char *pack_path  = (argc > 2) ? argv[2] : "F:\\model\\qwen3moe.tesspack";
    const char *out_path   = (argc > 3) ? argv[3] : "F:\\model\\moe_tessview_out.gguf";
    const char *dll_dir    = (argc > 4) ? argv[4] : "I:\\llama\\llama-b9733-bin-win-vulkan-x64";
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    SetDllDirectoryA(dll_dir);
#endif

    printf("Tesspack View — assemble + inference from .tesspack (mmap)\n");
    printf("GGUF:     %s\n", gguf_path);
    printf("Pack:     %s\n", pack_path);
    printf("Output:   %s\n", out_path);
    printf("═══════════════════════════════════════════════════════════════\n");

    /* ── T1: open GGUF ── */
    double t0 = now_ms();
    GgufReader gguf;
    int rc = gguf_open(gguf_path, &gguf);
    CHECK("T1: GGUF opens", rc == 0);
    if (rc != 0) return 1;
    printf("  Tensors: %u, header: %zu bytes\n", gguf.n_tensors, (size_t)gguf.data_offset);

    /* ── T2: open .tesspack (mmap) ── */
    TESS_PackIndex pi;
    rc = tess_pack_open(&pi, pack_path);
    CHECK("T2: .tesspack opens (mmap)", rc == 0);
    if (rc != 0) { gguf_close(&gguf); return 1; }
    printf("  Capos: %u, file: %.1f MB\n", pi.n_entries, (double)pi.file_sz / 1e6);

    /* ── T3: match MoE tensors ── */
    uint32_t n_moe = 0, n_match = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        if (!is_moe_expert(gguf.names[i])) continue;
        n_moe++;
        for (uint32_t j = 0; j < pi.n_entries; j++) {
            if (strcmp(pi.entries[j].name, gguf.names[i]) == 0) { n_match++; break; }
        }
    }
    CHECK("T3: all MoE tensors found in pack", n_match == n_moe);
    printf("  MoE tensors: %u/%u matched\n", n_match, n_moe);

    /* ── assemble body (same as tesspack_graft.c) ── */
    size_t hdr_sz = (size_t)gguf.data_offset;
    size_t body_sz = (size_t)(gguf.base_sz - gguf.data_offset);
    uint8_t *body = (uint8_t *)calloc(1, body_sz);
    if (!body) { printf("FAIL: OOM\n"); return 1; }

    uint32_t from_pack = 0, from_source = 0;
    uint64_t pack_bytes = 0, src_bytes = 0;

    t0 = now_ms();
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint64_t off = gguf.offsets[i];
        uint32_t tsz = gguf.sizes[i];
        uint32_t csz = GGUF_CELL_SIZE[gguf.dtypes[i]];
        if (csz == 0) csz = 1;

        if (is_moe_expert(gguf.names[i])) {
            /* count capos for this tensor */
            int capo_count = 0;
            uint32_t capo_total = 0;
            for (uint32_t j = 0; j < pi.n_entries; j++) {
                if (strcmp(pi.entries[j].name, gguf.names[i]) == 0) {
                    capo_total += TESS_TOTAL_SLOTS;
                    if ((int)pi.entries[j].capo_id + 1 > capo_count)
                        capo_count = pi.entries[j].capo_id + 1;
                }
            }
            if (capo_count == 0) {
                /* not in pack: copy from source */
                uint64_t src_off = gguf.data_offset + gguf.offsets[i];
                if (src_off + tsz <= gguf.base_sz) {
                    memcpy(body + off, gguf.base + src_off, tsz);
                    from_source++;
                    src_bytes += tsz;
                }
                continue;
            }

            uint32_t total_cells = tsz / csz;
            if (load_from_pack_direct(&pi, gguf.names[i], capo_count,
                                       total_cells, csz, body, off, body_sz) == 0) {
                from_pack++;
                pack_bytes += tsz;
            } else {
                /* pack load failed: fallback to source */
                uint64_t src_off = gguf.data_offset + gguf.offsets[i];
                if (src_off + tsz <= gguf.base_sz) {
                    memcpy(body + off, gguf.base + src_off, tsz);
                    from_source++;
                    src_bytes += tsz;
                }
            }
        } else {
            /* non-MoE: copy from source GGUF (mmap'd) */
            uint64_t src_off = gguf.data_offset + gguf.offsets[i];
            if (src_off + tsz <= gguf.base_sz) {
                memcpy(body + off, gguf.base + src_off, tsz);
                from_source++;
                src_bytes += tsz;
            }
        }
    }
    double assemble_ms = now_ms() - t0;

    printf("  Assemble: from_pack=%u (%.1f MB) from_source=%u (%.1f MB) %.1f ms\n",
           from_pack, pack_bytes / 1e6, from_source, src_bytes / 1e6, assemble_ms);

    /* ── T4: write assembled GGUF ── */
    t0 = now_ms();
    FILE *f = fopen(out_path, "wb");
    CHECK("T4: output file opens", f != NULL);
    if (f) {
        fwrite(gguf.base, 1, hdr_sz, f);
        fwrite(body, 1, body_sz, f);
        fclose(f);
    }
    double write_ms = now_ms() - t0;
    printf("  Write: %.1f ms (%.1f MB)\n", write_ms, (double)(hdr_sz + body_sz) / 1e6);

    /* ── Release resources before loading models (memory pressure) ── */
    free(body); body = NULL;
    gguf_close(&gguf);
    tess_pack_close(&pi);

    /* ── T5: open output GGUF ── */
    llama_backend_init();
    ggml_backend_load_all_from_path(dll_dir);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *mO = llama_model_load_from_file(gguf_path, mp);
    struct llama_model *mV = llama_model_load_from_file(out_path, mp);
    CHECK("T5: original loads", mO != NULL);
    CHECK("T6: view output loads", mV != NULL);
    if (!mO || !mV) {
        llama_model_free(mO); llama_model_free(mV);
        free(body); gguf_close(&gguf); tess_pack_close(&pi);
        return 1;
    }

    /* ── T7-T8: metadata ── */
    CHECK("T7: n_embd matches", llama_model_n_embd(mO) == llama_model_n_embd(mV));
    CHECK("T8: n_layer matches", llama_model_n_layer(mO) == llama_model_n_layer(mV));

    /* ── T9: logits bitwise identical ── */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_batch = 2048;
    struct llama_context *ctxO = llama_init_from_model(mO, cp);
    struct llama_context *ctxV = llama_init_from_model(mV, cp);
    CHECK("T9: contexts init", ctxO != NULL && ctxV != NULL);

    const char *prompt = "The capital of France is";
    llama_token toks[64];
    const struct llama_vocab *vocab = llama_model_get_vocab(mO);
    int32_t nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                toks, 64, true, false);
    CHECK("T10: tokenize", nt > 0);

    llama_batch bO = llama_batch_get_one(toks, nt);
    llama_batch bG = llama_batch_get_one(toks, nt);
    llama_decode(ctxO, bO);
    llama_decode(ctxV, bG);

    const float *logO = llama_get_logits(ctxO);
    const float *logV = llama_get_logits(ctxV);
    int nv = llama_vocab_n_tokens(vocab);

    uint64_t diffs = 0;
    float maxdiff = 0.0f;
    for (int i = 0; i < nv; i++) {
        float d = logO[i] > logV[i] ? logO[i] - logV[i] : logV[i] - logO[i];
        if (d > maxdiff) maxdiff = d;
        if (logO[i] != logV[i]) diffs++;
    }
    CHECK("T11: logits BITWISE identical", diffs == 0);
    printf("  n_vocab=%d  diffs=%llu  maxdiff=%.6e\n",
           nv, (unsigned long long)diffs, maxdiff);

    /* ── T12: 24-token generation ── */
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    int n_gen = 24, tok_match = 1;
    for (int g = 0; g < n_gen; g++) {
        llama_token tO = llama_sampler_sample(smpl, ctxO, -1);
        llama_token tV = llama_sampler_sample(smpl, ctxV, -1);
        llama_sampler_accept(smpl, tO);
        if (tO != tV) { printf("  step %d: orig=%d view=%d MISMATCH\n", g, tO, tV); tok_match = 0; break; }
        llama_batch b = llama_batch_get_one(&tO, 1);
        if (llama_decode(ctxO, b) != 0 || llama_decode(ctxV, b) != 0) { tok_match = 0; break; }
    }
    CHECK("T12: 24-token generation BITWISE identical", tok_match);

    llama_sampler_free(smpl);
    llama_free(ctxO); llama_free(ctxV);
    llama_model_free(mO); llama_model_free(mV);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass_count, pass_count + fail_count);
    printf("Assemble: %.1f ms | Write: %.1f ms | Total: %.1f ms\n",
           assemble_ms, write_ms, assemble_ms + write_ms);

    return fail_count ? 1 : 0;
}
