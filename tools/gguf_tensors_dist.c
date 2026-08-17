/* gguf_tensors_dist.c — tensor size distribution ของ GGUF จริง
 * เพื่อดูโหลด: chunky (MILLIONs+), mixed, tiny — ตัดสินใจ slot layout
 * gcc -O2 -Icore -o build/gguf_tensors_dist tools/gguf_tensors_dist.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_reader.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <model.gguf>\n", argv[0]); return 1; }
    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("cannot open\n"); return 1; }

    printf("tensors=%u data_offset=%llu\n",
           r.n_tensors, (unsigned long long)r.data_offset);

    uint64_t total = 0, tiny = 0, small = 0, mid = 0, big = 0, huge = 0;
    uint64_t n_tiny = 0, n_small = 0, n_mid = 0, n_big = 0, n_huge = 0;
    uint64_t max_sz = 0;
    const char *max_name = "";
    uint64_t min_sz = (uint64_t)-1;
    const char *min_name = "";

    printf("\n%-44s %10s %8s  dims\n", "tensor", "bytes", "type");
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        uint64_t sz = (uint64_t)r.sizes[i];
        total += sz;
        if (sz > max_sz) { max_sz = sz; max_name = r.names[i]; }
        if (sz < min_sz) { min_sz = sz; min_name = r.names[i]; }
        const char *cat;
        if (sz < 1024)       { cat="tiny";  tiny+=sz;  n_tiny++;  }
        else if (sz < 65536) { cat="small"; small+=sz; n_small++; }
        else if (sz < 1<<20) { cat="mid";   mid+=sz;   n_mid++;   }
        else if (sz < 1<<24) { cat="big";   big+=sz;   n_big++;   }
        else                 { cat="huge";  huge+=sz;  n_huge++;  }

        char dimbuf[64] = "";
        if (r.n_dims[i] <= 4) {
            char t[16];
            dimbuf[0] = 0;
            for (int d = 0; d < r.n_dims[i]; d++) {
                snprintf(t, sizeof(t), "%s%llu", d ? "x" : "",
                         (unsigned long long)r.dims[i * 4 + d]);
                strcat(dimbuf, t);
            }
        }
        if (i < 40 || sz > (1<<20))
            printf("%-44s %10llu %8u  %s\n", r.names[i],
                   (unsigned long long)sz, r.dtypes[i], dimbuf);
    }

    printf("\n──── distribution ────\n");
    printf("%-8s %6s %12s\n", "class", "count", "bytes");
    printf("%-8s %6llu %12llu\n", "tiny<1K",  (unsigned long long)n_tiny,  (unsigned long long)tiny);
    printf("%-8s %6llu %12llu\n", "small<64K",(unsigned long long)n_small, (unsigned long long)small);
    printf("%-8s %6llu %12llu\n", "mid<1M",   (unsigned long long)n_mid,   (unsigned long long)mid);
    printf("%-8s %6llu %12llu\n", "big<16M",  (unsigned long long)n_big,   (unsigned long long)big);
    printf("%-8s %6llu %12llu\n", "huge>=16M",(unsigned long long)n_huge,  (unsigned long long)huge);
    printf("%-8s %6llu %12llu\n", "TOTAL", (unsigned long long)r.n_tensors, (unsigned long long)total);
    printf("max=%s (%llu B)\n", max_name, (unsigned long long)max_sz);
    printf("min=%s (%llu B)\n", min_name, (unsigned long long)min_sz);

    gguf_close(&r);
    return 0;
}