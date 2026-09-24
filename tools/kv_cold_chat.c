/*
 * tools/kv_cold_chat.c — interactive chat ON the cold-KV stack (wiring).
 * ═══════════════════════════════════════════════════════════════════════
 * Every turn decodes ONLY new tokens — history is never re-decoded
 * (persistent serving ctx carries it; tot_skip counts the structural saving).
 * Overlap rule (measured): this llama build rejects decoding a token at an
 * already-filled position ("failed to initialize batch"). Hence the file
 * scheme stores bases covering history-minus-LAST-token: the last token is
 * always decoded fresh (new cell), which also primes the generator logits.
 * Per turn: HOLD + save pfx_<hash(hist[0..n-1])>.kvcb (pre-reply, enables
 * cross-session HIT) and pfx_<hash(full history)>.kvcb + .kvd at turn end.
 * Repeat/cross-session question → prefix-HIT (resume + 1 fresh token).
 * Re-anchor HOLD fires every 10 generated tokens (licensed policy).
 *
 * BUILD: gcc -O2 -I I:/llama/include -I core -o build/kv_cold_chat
 *   tools/kv_cold_chat.c .../llama.dll .../ggml.dll .../ggml-base.dll .../ggml-cpu-x64.dll -lm
 * RUN: ./build/kv_cold_chat [model.gguf] [outdir] [backend_dir]
 *   stdin lines; "quit" exits. Scriptable: printf 'hi\nquit\n' | ./build/kv_cold_chat
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST_CAP 3800
#define GEN_CAP 128
#define RE_ANCHOR_EVERY 10

static uint64_t prefix_hash(const llama_token *toks, int32_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (int32_t i = 0; i < n; i++) { h ^= (uint64_t)(uint32_t)toks[i]; h *= 1099511628211ULL; }
    return h;
}

static struct llama_context *mk_ctx(struct llama_model *m) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096; cp.n_batch = 512; cp.no_perf = true;
    cp.type_k = GGML_TYPE_Q8_0; cp.type_v = GGML_TYPE_Q4_0;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return llama_init_from_model(m, cp);
}

/* Teacher-decode toks[0..n) at positions pos0+i. Returns output count of the
   LAST batch (prime index = return-1 via llama_get_logits_ith). NOTE: this
   build's llama_get_logits() returns output 0 (first), NOT last — always
   index explicitly. */
static int32_t decode_at(struct llama_context *ctx, const llama_token *toks,
                      int32_t n, int32_t pos0) {
    int32_t k_last = 0;
    for (int32_t off = 0; off < n; off += 512) {
        llama_batch b = llama_batch_init(512, 0, 1);
        int32_t k = n - off > 512 ? 512 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = toks[off+i]; b.pos[i] = pos0+off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0; b.logits[i] = 1;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail\n"); exit(1); }
        llama_batch_free(b);
        k_last = k;
    }
    return k_last;
}

static llama_token argmax_ith(struct llama_context *ctx, int32_t out_idx, int32_t n_vocab) {
    const float *lg = llama_get_logits_ith(ctx, out_idx);
    if (!lg) { fprintf(stderr, "logits ith %d fail\n", out_idx); exit(1); }
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

static llama_token gen_one(struct llama_context *ctx, llama_token tok, int32_t pos,
                           const struct llama_vocab *vocab, int32_t n_vocab) {
    (void)vocab; (void)n_vocab;
    llama_batch b = llama_batch_init(1, 0, 1);
    b.token[0] = tok; b.pos[0] = pos; b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
    b.n_tokens = 1;
    if (llama_decode(ctx, b)) { fprintf(stderr, "step fail\n"); exit(1); }
    llama_batch_free(b);
    const float *lg = llama_get_logits(ctx);
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

static void print_piece(const struct llama_vocab *vocab, llama_token t) {
    char buf[64]; int n = llama_token_to_piece(vocab, t, buf, sizeof(buf), 0, true);
    if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
}

static int32_t tokenize(const struct llama_vocab *vocab, const char *txt,
                        int add_bos, llama_token *out, int32_t cap) {
    int32_t n = llama_tokenize(vocab, txt, (int32_t)strlen(txt), out, cap, add_bos, true);
    if (n < 0) { fprintf(stderr, "tokenize fail\n"); exit(1); }
    return n;
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots";
    const char *backend    = argc > 3 ? argv[3] : "build\\cpuonly";
    ggml_backend_load_all_from_path(backend);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    static llama_token hist[4096];
    int32_t n_hist = 0;
    long tot_skip = 0, tot_dec = 0, tot_gen = 0;
    int turn = 0;
    char line[2048];

    /* one persistent serving ctx: live state carries history across turns */
    struct llama_context *ctx = mk_ctx(model);
    if (!ctx) { fprintf(stderr, "FAIL: ctx\n"); return 1; }

    printf("cold-chat (K8V4) — type lines, 'quit' exits\n");
    for (;;) {
        printf("\n> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "quit")) break;
        if (!line[0]) continue;

        /* wrap user turn */
        char wrapped[2300];
        snprintf(wrapped, sizeof(wrapped),
                 "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", line);
        int32_t prev_hist = n_hist;
        int32_t nu = tokenize(vocab, wrapped, n_hist == 0, hist + n_hist, 4096 - n_hist);
        n_hist += nu;
        if (n_hist > HIST_CAP) { printf("history cap %d hit — stop, no truncation\n", HIST_CAP); break; }
        turn++;

        /* file key covers history-minus-last: last token always decoded fresh
           (overlap decode is rejected) and primes the generator */
        int32_t key_n = n_hist - 1;
        char pfx[512];
        snprintf(pfx, sizeof(pfx), "%s\\chat_%016llx.kvcb", outdir,
                 (unsigned long long)prefix_hash(hist, key_n));
        KVColdBase pre; kvcb_init(&pre);
        llama_token t;
        if (kvcb_load(&pre, pfx) == 0 && pre.n_tokens == key_n &&
            kvcb_resume(ctx, 0, &pre) == pre.size) {
            tot_skip += key_n;
            printf("[turn %d] prefix-HIT %d tok resumed + 1 fresh\n", turn, key_n);
            decode_at(ctx, hist + key_n, 1, key_n);
            tot_dec += 1;
            t = argmax_ith(ctx, 0, n_vocab);
        } else {
            /* normal path: decode suffix except last (history resident, never re-decoded) */
            if (nu > 1) decode_at(ctx, hist + prev_hist, nu - 1, prev_hist);
            if (key_n > 0) {
                KVColdBase preb; kvcb_init(&preb);
                if (kvcb_hold(ctx, 0, &preb, key_n) != 0) { fprintf(stderr, "FAIL: pre-hold\n"); return 1; }
                if (kvcb_save(&preb, pfx) != 0) { fprintf(stderr, "FAIL: pre-save\n"); return 1; }
                kvcb_clear(&preb);
            }
            /* fresh last token → live logits prime the generator */
            decode_at(ctx, hist + key_n, 1, key_n);
            tot_dec += nu; tot_skip += prev_hist;
            printf("[turn %d] decoded suffix %d tok (history %d resident, skipped)\n",
                   turn, nu, prev_hist);
            t = argmax_ith(ctx, 0, n_vocab);
        }
        kvcb_clear(&pre);

        printf("assistant: "); fflush(stdout);
        int32_t ngen = 0, since_anchor = 0;
        KVColdBase anchor; kvcb_init(&anchor);
        for (int g = 0; g < GEN_CAP; g++) {
            if (llama_vocab_is_eog(vocab, t)) break;
            print_piece(vocab, t);
            if (n_hist >= 4096) break;
            hist[n_hist++] = t; ngen++; since_anchor++;
            t = gen_one(ctx, t, n_hist-1, vocab, n_vocab);
            if (since_anchor == RE_ANCHOR_EVERY) {
                if (kvcb_hold(ctx, 0, &anchor, n_hist) != 0) { fprintf(stderr, "FAIL: re-hold\n"); return 1; }
                printf("\n[re-anchor @+%d gen]\n", ngen);
                since_anchor = 0;
            }
        }
        printf("\n");
        tot_gen += ngen;

        /* close turn: im_end + hold full history + save base + spill delta */
        int32_t ne = tokenize(vocab, "<|im_end|>\n", 0, hist + n_hist, 4096 - n_hist);
        n_hist += ne;
        decode_at(ctx, hist + n_hist - ne, ne, n_hist - ne);
        KVColdBase base; kvcb_init(&base);
        if (kvcb_hold(ctx, 0, &base, n_hist) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }
        char pfx2[512], pd[512];
        snprintf(pfx2, sizeof(pfx2), "%s\\chat_%016llx.kvcb", outdir,
                 (unsigned long long)prefix_hash(hist, n_hist));
        snprintf(pd, sizeof(pd), "%s\\chat_t%d.kvd", outdir, turn);
        if (kvcb_save(&base, pfx2) != 0) { fprintf(stderr, "FAIL: save\n"); return 1; }
        if (kvcb_spill(hist + prev_hist, (uint32_t)(n_hist - prev_hist), prev_hist, pd) != 0) {
            fprintf(stderr, "FAIL: spill\n"); return 1;
        }
        printf("[turn %d] reply %d tok · base %llu B saved · delta %d tok spilled\n",
               turn, ngen, (unsigned long long)base.size, n_hist - prev_hist);
        kvcb_clear(&base); kvcb_clear(&anchor);
    }

    printf("\nchat done: %d turns · decoded %ld tok · resident-skipped %ld tok · generated %ld tok\n",
           turn, tot_dec, tot_skip, tot_gen);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
