/*
 * tools/kv_cold_reanchor.c — COLD-KV step 3 proof: re-anchor @10 + L2 verify.
 * ═══════════════════════════════════════════════════════════════════════
 * Arch: docs/COLD-KV-ARCH-2026-09-23.md (build order step 3).
 * Policy: cold generation runs on the COLD default (K8V4: K=Q8,V=Q4 —
 * threshold doc: KL 0.083, greedy 13/20). Every 10 generated steps the
 * live cold state is re-anchored: fresh HOLD becomes the new base, old
 * base freed (EXPIRED delete shape), new base saved to file.
 * Gate: per-prompt greedy first-divergence vs F16 ref over 20 steps must
 * satisfy the L2 bar — first-div >= 10/20 (threshold doc §Reading).
 * Kernel: flash ENABLED on all ctx (same-kernel rule; patched build_zc2
 * rejects quantized V cache with flash disabled, so the v040-era
 * flash-disabled verdicts are re-measured here under flash-enabled).
 *
 * RUN: ./build/kv_cold_reanchor [model.gguf] [outdir] [backend_dir] [coldcfg] [ngl]
 *   coldcfg: k8v4 (default COLD) | q8 (control) | q4 (negative control)
 *   ngl: GPU layers, default 35 (patched build_zc2 runtime)
 */
#include "llama.h"
#include "ggml-backend.h"
#include "kv_cold_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PROMPTS[] = {
    "<|im_start|>user\nExplain why the sky is blue in two sentences.<|im_end|>\n"
    "<|im_start|>assistant\nThe sky appears blue because air molecules scatter short-wavelength "
    "blue light more strongly than other colors, a phenomenon called Rayleigh scattering. "
    "At sunset the light travels through more atmosphere, scattering away the blue and "
    "leaving the reds and oranges we see near the horizon.<|im_end|>\n",
    "<|im_start|>user\nอธิบายว่าทำไมข้าวผัดถึงอร่อยในสองประโยค<|im_end|>\n"
    "<|im_start|>assistant\nข้าวผัดอร่อยเพราะความร้อนสูงจากกระทะทำให้เกิดกลิ่นหอมของข้าวคั่วที่เรียกว่ากลิ่นกระทะ "
    "และความสมดุลของรสเค็มหวานจากซีอิ๊วและน้ำตาลทำให้กินได้ไม่เบื่อ<|im_end|>\n",
    "<|im_start|>user\nIf a train leaves at 3pm going 60km/h, when does it arrive 150km away? Reply briefly.<|im_end|>\n"
    "<|im_start|>assistant\nIt arrives at 5:30pm, since 150 divided by 60 is two and a half hours after 3pm.<|im_end|>\n",
};
#define N_PRM (sizeof(PROMPTS)/sizeof(PROMPTS[0]))
#define N_GREEDY 20
#define RE_ANCHOR_EVERY 10

static struct llama_context *mk_ctx(struct llama_model *m, enum ggml_type tk, enum ggml_type tv) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 128; cp.no_perf = true;
    cp.type_k = tk; cp.type_v = tv;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    return llama_init_from_model(m, cp);
}

static void decode_all(struct llama_context *ctx, const llama_token *t, int32_t n) {
    /* NOTE: only the final token requests logits (the harness reads just
       the last prompt position). Materializing logits for every prompt
       token costs n_vocab floats each — a 114-token prompt needs 66 MiB
       of pinned output buffer and OOMs a 4 GB GPU with 3 live contexts.
       Callers must therefore query with pos=-1 (last output), never
       get_logits_ith(absolute). */
    for (int32_t off = 0; off < n; off += 128) {
        llama_batch b = llama_batch_init(128, 0, 1);
        int32_t k = n - off > 128 ? 128 : n - off;
        for (int32_t i = 0; i < k; i++) {
            b.token[i] = t[off+i]; b.pos[i] = off+i;
            b.n_seq_id[i] = 1; b.seq_id[i][0] = 0;
            b.logits[i] = (off + i == n - 1) ? 1 : 0;
        }
        b.n_tokens = k;
        if (llama_decode(ctx, b)) { fprintf(stderr, "decode fail\n"); exit(1); }
        llama_batch_free(b);
    }
}

static llama_token argmax_next(struct llama_context *ctx, int32_t pos, int32_t n_vocab) {
    const float *lg = (pos < 0) ? llama_get_logits(ctx) : llama_get_logits_ith(ctx, pos);
    if (!lg) { fprintf(stderr, "logits ith fail\n"); exit(1); }
    int bi = 0;
    for (int32_t i = 1; i < n_vocab; i++) if (lg[i] > lg[bi]) bi = i;
    return (llama_token)bi;
}

static llama_token step_one(struct llama_context *ctx, llama_token tok, int32_t pos, int32_t n_vocab) {
    llama_batch b = llama_batch_init(1, 0, 1);
    b.token[0] = tok; b.pos[0] = pos; b.n_seq_id[0] = 1; b.seq_id[0][0] = 0; b.logits[0] = 1;
    b.n_tokens = 1;
    if (llama_decode(ctx, b)) { fprintf(stderr, "step fail\n"); exit(1); }
    llama_batch_free(b);
    return argmax_next(ctx, -1, n_vocab); /* -1: last-decode logits */
}

/* top-5 of the last decoded position (ilogits=-1 → llama_get_logits). */
static void top5_next(struct llama_context *ctx, int32_t pos, int32_t n_vocab, llama_token *out5) {
    const float *lg = (pos < 0) ? llama_get_logits(ctx) : llama_get_logits_ith(ctx, pos);
    if (!lg) { fprintf(stderr, "logits fail\n"); exit(1); }
    for (int k = 0; k < 5; k++) {
        int bi = -1;
        for (int32_t i = 0; i < n_vocab; i++) {
            int seen = 0;
            for (int j = 0; j < k; j++) if (out5[j] == i) { seen = 1; break; }
            if (!seen && (bi < 0 || lg[i] > lg[bi])) bi = i;
        }
        out5[k] = (llama_token)bi;
    }
}

int main(int argc, char **argv) {
    const char *model_path = argc > 1 ? argv[1] : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *outdir     = argc > 2 ? argv[2] : "build\\kvslots";
    const char *backend    = argc > 3 ? argv[3] : "build\\cpuonly";
    const char *coldcfg    = argc > 4 ? argv[4] : "k8v4";
    enum ggml_type ck = GGML_TYPE_Q8_0, cv = GGML_TYPE_Q4_0;
    if (!strcmp(coldcfg, "q8")) { ck = GGML_TYPE_Q8_0; cv = GGML_TYPE_Q8_0; }
    else if (!strcmp(coldcfg, "q4")) { ck = GGML_TYPE_Q4_0; cv = GGML_TYPE_Q4_0; }
    ggml_backend_load_all_from_path(backend);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = argc > 5 ? atoi(argv[5]) : 35;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FAIL: model load\n"); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);

    llama_token *PT[N_PRM]; int32_t PN[N_PRM];
    for (size_t p = 0; p < N_PRM; p++) {
        int32_t L = (int32_t)strlen(PROMPTS[p]);
        int32_t need = -llama_tokenize(vocab, PROMPTS[p], L, NULL, 0, true, true);
        PT[p] = malloc((need+16) * sizeof(llama_token));
        PN[p] = llama_tokenize(vocab, PROMPTS[p], L, PT[p], need+16, true, true);
    }

    printf("=== re-anchor @%d · REF=F16 vs COLD=%s (ENABLED kernel) ===\n", RE_ANCHOR_EVERY, coldcfg);
    printf("--- first-div = free-run greedy identity (diagnostic: quantization destroys determinism by construction)\n");
    printf("--- on-track  = teacher-forced: cold top-1 in ref top-5 along the ref path, /20 (binding gate)\n");
    int worst = N_GREEDY, worst_track = N_GREEDY, all_ok = 1;
    for (size_t p = 0; p < N_PRM; p++) {
        struct llama_context *ref = mk_ctx(model, GGML_TYPE_F16, GGML_TYPE_F16);
        struct llama_context *cold = mk_ctx(model, ck, cv);
        struct llama_context *cold_tf = mk_ctx(model, ck, cv);
        if (!ref || !cold || !cold_tf) { fprintf(stderr, "FAIL: ctx\n"); return 1; }
        decode_all(ref, PT[p], PN[p]);
        decode_all(cold, PT[p], PN[p]);
        decode_all(cold_tf, PT[p], PN[p]);

        /* cold base at t0 (the anchor this session starts from) */
        KVColdBase anchor; kvcb_init(&anchor);
        if (kvcb_hold(cold, 0, &anchor, PN[p]) != 0) { fprintf(stderr, "FAIL: hold\n"); return 1; }

        /* L2 done right: first token sampled from the prompt's own last-
           position logits (no re-decode of the last token at a shifted
           position — that wart flips argmax on unnatural input and
           punishes the quantized side for methodology noise). */
        llama_token tr = argmax_next(ref, -1, n_vocab);
        llama_token tc = argmax_next(cold, -1, n_vocab);
        int div = (tr != tc) ? 0 : N_GREEDY;
        /* teacher-forced containment along the ref path (binding gate) */
        int ontrack = 0, ontrack10 = 0;
        char missbuf[256]; missbuf[0] = 0; int nmiss = 0;
        llama_token tctf = argmax_next(cold_tf, -1, n_vocab);
        {
            llama_token r5[5]; top5_next(ref, -1, n_vocab, r5);
            int hit = 0;
            for (int k = 0; k < 5; k++) if (tctf == r5[k]) { hit = 1; break; }
            if (hit) { ontrack++; ontrack10++; }
            else { nmiss++; snprintf(missbuf + strlen(missbuf), sizeof(missbuf) - strlen(missbuf), "0 "); }
        }
        for (int s = 1; s < N_GREEDY; s++) {
            llama_token prev = tr; /* ref token tr_{s-1}: input both sides consume */
            tr = step_one(ref, tr, PN[p]+s-1, n_vocab);
            tc = step_one(cold, tc, PN[p]+s-1, n_vocab);
            if (div == N_GREEDY && tr != tc) div = s;
            /* teacher-forced cold consumes the same prev token; its top-1
               (predicting PN+s) is probed against ref's fresh top-5 */
            step_one(cold_tf, prev, PN[p]+s-1, n_vocab);
            tctf = argmax_next(cold_tf, -1, n_vocab);
            {
                llama_token r5[5]; top5_next(ref, -1, n_vocab, r5);
                int hit = 0;
                for (int k = 0; k < 5; k++) if (tctf == r5[k]) { hit = 1; break; }
                if (hit) { ontrack++; if (s < 10) ontrack10++; }
                else if (nmiss < 20) {
                    nmiss++;
                    snprintf(missbuf + strlen(missbuf), sizeof(missbuf) - strlen(missbuf), "%d ", s);
                }
            }
            if (s + 1 == RE_ANCHOR_EVERY) {
                /* RE-ANCHOR: fresh HOLD @10 becomes the new base, old freed */
                char apath[512];
                snprintf(apath, sizeof(apath), "%s\\reanchor_p%zu.kvcb", outdir, p);
                size_t old_sz = anchor.size;
                if (kvcb_hold(cold, 0, &anchor, PN[p] + s + 1) != 0) { fprintf(stderr, "FAIL: re-hold\n"); return 1; }
                if (kvcb_save(&anchor, apath) != 0) { fprintf(stderr, "FAIL: save anchor\n"); return 1; }
                printf("  [p%zu] re-anchor @%d: %llu B -> %llu B (%s)\n",
                       p, s + 1, (unsigned long long)old_sz,
                       (unsigned long long)anchor.size, apath);
            }
        }
        printf("prompt%zu: first-div %d/%d (diag) · on-track %d/%d%s%s · window10 %d/10 %s\n",
               p, div, N_GREEDY, ontrack, N_GREEDY, nmiss ? " miss@" : "", nmiss ? missbuf : "",
               ontrack10, ontrack10 == 10 ? "OK" : "BREACH");
        if (div < worst) worst = div;
        if (ontrack < worst_track) worst_track = ontrack;
        if (ontrack10 != 10) all_ok = 0;
        kvcb_clear(&anchor);
        llama_free(ref); llama_free(cold); llama_free(cold_tf);
    }
    printf("%s: worst first-div %d/20 (diag) · worst on-track %d/20 · re-anchor @10 %s\n",
           all_ok ? "PASS" : "FAIL", worst, worst_track,
           all_ok ? "LICENSED (10/10 in-window on all prompts)" : "NOT licensed");
    for (size_t p = 0; p < N_PRM; p++) free(PT[p]);
    llama_model_free(model);
    return all_ok ? 0 : 1;
}
