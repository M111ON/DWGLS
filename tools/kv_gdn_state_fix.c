/*
 * tools/kv_gdn_state_fix.c — Fix GDN state extraction for Qwen3.5
 * ════════════════════════════════════════════════════════════════════════════
 * Previous probe showed: state SIZE query works (452/460 bytes) but
 * state DATA extraction fails with "unexpectedly reached end of buffer".
 *
 * This probe tries multiple approaches:
 * 1. Oversized buffer (1MB instead of exact size)
 * 2. llama_state_save_file / llama_state_load_from_file (file-based)
 * 3. Sequential partial reads
 * 4. llama_state_seq_set_data (reverse/inject)
 *
 * BUILD:
 *   C:\msys64\usr\bin\env.exe PATH="/mingw64/bin:$PATH" gcc -O2 -std=c11 \
 *     -Icore -II:/llama/include -o build/kv_gdn_state_fix.exe tools/kv_gdn_state_fix.c \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
 *     I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
 *
 * RUN:
 *   $env:PATH = "C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;" + $env:PATH
 *   .\build\kv_gdn_state_fix.exe "F:\model\Qwen3.5-2B-Q8_0.gguf" "Hello"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../core/gguf_reader.h"
#include "llama.h"

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint64_t fnv1a_hash(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "F:\\model\\Qwen3.5-2B-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    const char *state_file = "gdn_state_test.bin";

    fprintf(stderr, "=== kv_gdn_state_fix — GDN state extraction fix ===\n");
    fprintf(stderr, "model: %s\n", model_path);

    llama_backend_init();

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mparams);
    if (!model) { fprintf(stderr, "FAIL: load model\n"); return 1; }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;

    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) { fprintf(stderr, "FAIL: init context\n"); return 1; }

    /* Get state sizes */
    size_t sz_partial = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
    size_t sz_full   = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
    size_t sz_std    = llama_state_get_size(ctx);
    fprintf(stderr, "State sizes: partial=%zu full=%zu std=%zu\n", sz_partial, sz_full, sz_std);

    /* Tokenize prompt */
    struct llama_vocab *vocab = (struct llama_vocab *)llama_model_get_vocab(model);
    llama_token tokens[1024];
    int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), tokens, 1024, true, false);
    if (n_prompt < 0) { fprintf(stderr, "FAIL: tokenize\n"); return 1; }

    /* Process prompt */
    struct llama_batch batch = llama_batch_init(n_prompt, 0, 1);
    for (int i = 0; i < n_prompt; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n_prompt - 1) ? 1 : 0;
    }
    batch.n_tokens = n_prompt;
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "FAIL: decode\n"); return 1; }
    llama_batch_free(batch);
    fprintf(stderr, "Prompt processed (%d tokens)\n", n_prompt);

    /* ─── APPROACH 1: Oversized buffer ─── */
    fprintf(stderr, "\n--- APPROACH 1: Oversized buffer (1MB) ---\n");
    {
        size_t big_size = 1024 * 1024;  /* 1MB */
        uint8_t *buf = (uint8_t *)malloc(big_size);
        if (!buf) { fprintf(stderr, "FAIL: alloc 1MB\n"); return 1; }

        /* Try PARTIAL first */
        size_t got = llama_state_seq_get_data_ext(ctx, buf, big_size, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
        fprintf(stderr, "  PARTIAL: got=%zu bytes (reported_size=%zu)\n", got, sz_partial);
        if (got > 0) {
            fprintf(stderr, "  hash=%016llx\n", (unsigned long long)fnv1a_hash(buf, got));
        }

        /* Try FULL */
        got = llama_state_seq_get_data_ext(ctx, buf, big_size, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        fprintf(stderr, "  FULL:    got=%zu bytes (reported_size=%zu)\n", got, sz_full);
        if (got > 0) {
            fprintf(stderr, "  hash=%016llx\n", (unsigned long long)fnv1a_hash(buf, got));
        }

        /* Try standard API */
        got = llama_state_get_data(ctx, buf, big_size);
        fprintf(stderr, "  STD:     got=%zu bytes (reported_size=%zu)\n", got, sz_std);
        if (got > 0) {
            fprintf(stderr, "  hash=%016llx\n", (unsigned long long)fnv1a_hash(buf, got));
        }

        free(buf);
    }

    /* ─── APPROACH 2: File-based save/load ─── */
    fprintf(stderr, "\n--- APPROACH 2: File-based (llama_state_save_file) ---\n");
    {
        double t0 = now_ms();
        bool ok = llama_state_save_file(ctx, state_file, NULL, 0);
        double dt = now_ms() - t0;
        fprintf(stderr, "  save_file(seq=0): %s (%.0f ms)\n", ok ? "OK" : "FAIL", dt);

        if (ok) {
            /* Check file size */
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(state_file, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz;
                sz.LowPart = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                fprintf(stderr, "  file size: %lld bytes (%.2f KB)\n", sz.QuadPart, sz.QuadPart / 1024.0);
                FindClose(h);
            }

            /* Try to load into fresh context */
            fprintf(stderr, "  Loading into SAME context (for testing)...\n");
            t0 = now_ms();
            llama_token tok_out[1024];
            size_t n_tok = 0;
            bool loaded = llama_state_load_file(ctx, state_file, tok_out, 1024, &n_tok);
            dt = now_ms() - t0;
            fprintf(stderr, "  load_from_file: %s (%.0f ms)\n", loaded ? "OK" : "FAIL", dt);
        }
    }

    /* ─── APPROACH 3: Various buffer sizes ─── */
    fprintf(stderr, "\n--- APPROACH 3: Buffer size sweep ---\n");
    {
        size_t reported = llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
        size_t sizes[] = { reported, reported*2, reported*4, reported*8, 65536, 1048576 };
        int n_sizes = sizeof(sizes)/sizeof(sizes[0]);
        for (int i = 0; i < n_sizes; i++) {
            uint8_t *buf = (uint8_t *)malloc(sizes[i]);
            size_t got = llama_state_seq_get_data_ext(ctx, buf, sizes[i], 0, LLAMA_STATE_SEQ_FLAGS_NONE);
            fprintf(stderr, "  buf=%8zu: got=%zu %s\n", sizes[i], got, got > 0 ? "OK" : "FAIL");
            if (got > 0) {
                fprintf(stderr, "    hash=%016llx\n", (unsigned long long)fnv1a_hash(buf, got));
                free(buf);
                break;
            }
            free(buf);
        }
    }

    /* ─── APPROACH 4: Try SET_DATA (inject) to see reverse API ─── */
    fprintf(stderr, "\n--- APPROACH 4: Set data (reverse API) ---\n");
    {
        /* Check if set_data API exists */
        /* First save to file, then try set from file data */
        bool saved = llama_state_save_file(ctx, state_file, NULL, 0);
        if (saved) {
            /* Read file into buffer */
            FILE *f = fopen(state_file, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                uint8_t *fbuf = (uint8_t *)malloc(fsize);
                fread(fbuf, 1, fsize, f);
                fclose(f);

                fprintf(stderr, "  file data: %ld bytes\n", fsize);

                /* Try set_data_ext */
                size_t written = llama_state_seq_set_data_ext(ctx, fbuf, fsize, 0, LLAMA_STATE_SEQ_FLAGS_NONE);
                fprintf(stderr, "  set_data_ext: wrote=%zu (expected %zu)\n", written, fsize);

                free(fbuf);
            }
        }
    }

    /* ─── APPROACH 5: Per-layer recurrent state ─── */
    fprintf(stderr, "\n--- APPROACH 5: Check per-layer recurrent memory API ---\n");
    {
        /* The log showed "llama_memory_recurrent, layer N: dev = CPU" */
        /* There might be a way to access per-layer recurrent state */
        /* Check what APIs exist for recurrent memory */

        /* Try state with different seq_ids */
        for (int seq = 0; seq < 2; seq++) {
            size_t sz = llama_state_seq_get_size_ext(ctx, seq, LLAMA_STATE_SEQ_FLAGS_NONE);
            fprintf(stderr, "  seq=%d: size=%zu\n", seq, sz);
        }

        /* Check total state including ALL sequences */
        size_t sz_all = llama_state_get_size(ctx);
        fprintf(stderr, "  state_get_size (all seqs): %zu\n", sz_all);
    }

    /* ─── Generate a few tokens to verify model still works ─── */
    fprintf(stderr, "\n--- Verify model works after state ops ---\n");
    {
        struct llama_batch gen_batch = llama_batch_init(1, 0, 1);
        llama_token last = tokens[n_prompt - 1];
        int n_pos = n_prompt;

        for (int i = 0; i < 8; i++) {
            gen_batch.token[0] = last;
            gen_batch.pos[0] = n_pos;
            gen_batch.n_seq_id[0] = 1;
            gen_batch.seq_id[0][0] = 0;
            gen_batch.logits[0] = 1;
            gen_batch.n_tokens = 1;

            if (llama_decode(ctx, gen_batch) != 0) break;
            n_pos++;

            float *logits = llama_get_logits(ctx);
            int best = 0;
            float best_val = -1e9f;
            for (int t = 0; t < llama_vocab_n_tokens(vocab); t++) {
                if (logits[t] > best_val) { best_val = logits[t]; best = t; }
            }
            char piece[64] = {0};
            llama_token_to_piece(vocab, best, piece, 63, 0, false);
            printf("%s", piece);
            last = best;
        }
        printf("\n");
        llama_batch_free(gen_batch);
    }

    /* Cleanup */
    remove(state_file);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    fprintf(stderr, "\n=== DONE ===\n");
    return 0;
}
