/* tools/gguf_tnames.c — tensor inventory of any GGUF: count, prefix histogram,
 * first dims/dtype per tensor. Answers "what arch is this" without loading weights.
 * BUILD: gcc -O2 -I core -o build/gguf_tnames tools/gguf_tnames.c
 * RUN: ./build/gguf_tnames <file.gguf> [max_list] */
#include "gguf_reader.h"
#include <stdio.h>
#include <string.h>

typedef struct { char pre[48]; int n; } PreC;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "I:/model/Kokoro_no_espeak_Q8.gguf";
    int max_list = argc > 2 ? atoi(argv[2]) : 25;
    GgufReader r; memset(&r, 0, sizeof(r));
    if (gguf_open(path, &r) != 0) { fprintf(stderr, "FAIL: open %s\n", path); return 1; }
    printf("tensors=%u data_off=%llu\n", r.n_tensors, (unsigned long long)r.data_offset);

    PreC hist[256]; int nh = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        const char *nm = r.names[i];
        /* prefix = up to 2nd dot (e.g. blk.0, encoder.layer) */
        char pre[48]; int dots = 0, k = 0;
        while (nm[k] && k < 47 && dots < 2) { pre[k] = nm[k]; if (nm[k] == '.') dots++; k++; }
        pre[k] = 0;
        int f = -1;
        for (int j = 0; j < nh; j++) if (!strcmp(hist[j].pre, pre)) { f = j; break; }
        if (f < 0 && nh < 256) { f = nh++; snprintf(hist[f].pre, sizeof(hist[f].pre), "%s", pre); hist[f].n = 0; }
        if (f >= 0) hist[f].n++;
    }
    printf("--- prefix histogram (%d groups) ---\n", nh);
    for (int j = 0; j < nh; j++) printf("  %-40s %d\n", hist[j].pre, hist[j].n);
    printf("--- first %d tensors ---\n", max_list);
    for (uint32_t i = 0; i < r.n_tensors && i < (uint32_t)max_list; i++) {
        printf("  [%u] %s nd=%u dims=", i, r.names[i], r.n_dims[i]);
        for (int d = 0; d < r.n_dims[i]; d++) printf("%s%llu", d ? "x" : "", (unsigned long long)r.dims[i*4+d]);
        printf(" dt=%u\n", r.dtypes[i]);
    }
    gguf_close(&r);
    return 0;
}
