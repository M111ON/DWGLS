/* tools/bake_q4.c — bake Q4_0 + Q4_K_M GGUFs from one base via llama_model_quantize.
 * (v040 llama-quantize.exe is broken here: 0xC0000139 on every exe — call the API directly.)
 * Usage: bake_q4 <in.gguf>  →  writes <base>-Q4_0-bake.gguf and <base>-Q4_K_M-bake.gguf beside input.
 */
#include "llama.h"
#include <stdio.h>
#include <string.h>

static int bake(const char *in, const char *out, enum llama_ftype ft) {
    struct llama_model_quantize_params q = llama_model_quantize_default_params();
    q.nthread = 0;
    q.ftype = ft;
    q.allow_requantize = true;
    q.only_copy = false;
    q.quantize_output_tensor = true;
    uint32_t n = llama_model_quantize(in, out, &q);
    printf("bake %s -> %s : %u tensors%s\n", in, out, n, n ? "" : "  FAILED");
    return n ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bake_q4 <in.gguf>\n"); return 1; }
    const char *in = argv[1];
    char o1[1024], o2[1024];
    const char *dot = strrchr(in, '.');
    size_t bl = dot ? (size_t)(dot - in) : strlen(in);
    snprintf(o1, sizeof(o1), "%.*s-Q4_0-bake.gguf", (int)bl, in);
    snprintf(o2, sizeof(o2), "%.*s-Q4_K_M-bake.gguf", (int)bl, in);
    int rc = bake(in, o1, LLAMA_FTYPE_MOSTLY_Q4_0);
    rc |= bake(in, o2, LLAMA_FTYPE_MOSTLY_Q4_K_M);
    return rc;
}
