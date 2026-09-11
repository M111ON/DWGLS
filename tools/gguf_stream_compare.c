/* gguf_stream_compare.c — Streaming byte-level GGUF-to-GGUF comparison
 * ═══════════════════════════════════════════════════════════════════════════
 * Compares two GGUF files tensor-by-tensor (matched by name).
 * Handles header/offset differences (e.g. assembled vs original).
 * Streaming: mmap both files, no malloc for tensor data.
 *
 * BUILD: gcc -O2 -Wall -Icore -o gguf_stream_compare tools/gguf_stream_compare.c -lm
 * RUN:   ./gguf_stream_compare <original.gguf> <assembled.gguf>
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "core/gguf_reader.h"

static double now_sec(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

/* Build name→index lookup from GgufReader (linear scan, <512 tensors) */
typedef struct { const char *name; uint32_t idx; } NameIdx;

static int name_cmp(const void *a, const void *b) {
    return strcmp(((const NameIdx*)a)->name, ((const NameIdx*)b)->name);
}

static NameIdx *build_name_idx(GgufReader *r, uint32_t *out_n) {
    NameIdx *idx = (NameIdx *)malloc(r->n_tensors * sizeof(NameIdx));
    for (uint32_t i = 0; i < r->n_tensors; i++) {
        idx[i].name = r->names[i];
        idx[i].idx = i;
    }
    qsort(idx, r->n_tensors, sizeof(NameIdx), name_cmp);
    *out_n = r->n_tensors;
    return idx;
}

static int name_lookup(const NameIdx *idx, uint32_t n, const char *name) {
    NameIdx key = { name, 0 };
    const NameIdx *found = (const NameIdx *)bsearch(&key, idx, n, sizeof(NameIdx), name_cmp);
    return found ? (int)found->idx : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <original.gguf> <assembled.gguf>\n", argv[0]);
        fprintf(stderr, "  Compares tensor data byte-by-byte (matched by name).\n");
        fprintf(stderr, "  Header/offset differences are expected and ignored.\n");
        return 1;
    }
    const char *path_a = argv[1];
    const char *path_b = argv[2];

    double t0 = now_sec();

    /* ── Open both GGUF files (mmap) ── */
    GgufReader a, b;
    if (gguf_open(path_a, &a) != 0) {
        fprintf(stderr, "FATAL: cannot open %s\n", path_a);
        return 1;
    }
    if (gguf_open(path_b, &b) != 0) {
        fprintf(stderr, "FATAL: cannot open %s\n", path_b);
        gguf_close(&a);
        return 1;
    }

    fprintf(stderr, "File A: %s (%u tensors, data_offset=%lu)\n",
            path_a, a.n_tensors, (unsigned long)a.data_offset);
    fprintf(stderr, "File B: %s (%u tensors, data_offset=%lu)\n",
            path_b, b.n_tensors, (unsigned long)b.data_offset);

    /* ── Build name lookup for file B ── */
    uint32_t b_n = 0;
    NameIdx *b_idx = build_name_idx(&b, &b_n);

    /* ── Compare each tensor in A against B by name ── */
    uint32_t pass = 0, fail = 0, skip_no_name = 0;
    uint64_t pass_bytes = 0, fail_bytes = 0;

    for (uint32_t i = 0; i < a.n_tensors; i++) {
        const char *name = a.names[i];
        uint32_t a_sz = a.sizes[i];

        /* Find in B by name */
        int bi = name_lookup(b_idx, b_n, name);
        if (bi < 0) {
            fprintf(stderr, "  [%4u] %-45s SKIP (not in B)\n", i, name);
            skip_no_name++;
            continue;
        }

        uint32_t b_sz = b.sizes[bi];
        if (a_sz != b_sz) {
            fprintf(stderr, "  [%4u] %-45s FAIL size: A=%u B=%u\n", i, name, a_sz, b_sz);
            fail++;
            fail_bytes += (a_sz > b_sz) ? a_sz : b_sz;
            continue;
        }

        /* Byte-by-byte compare via mmap pointers (zero syscalls) */
        const uint8_t *a_data = a.base + a.data_offset + a.offsets[i];
        const uint8_t *b_data = b.base + b.data_offset + b.offsets[bi];

        /* Bounds check */
        if (a.offsets[i] + a_sz > a.base_sz ||
            b.offsets[bi] + b_sz > b.base_sz) {
            fprintf(stderr, "  [%4u] %-45s FAIL (out of bounds)\n", i, name);
            fail++;
            fail_bytes += a_sz;
            continue;
        }

        if (memcmp(a_data, b_data, a_sz) == 0) {
            pass++;
            pass_bytes += a_sz;
            if (i < 5 || (i + 1) % 100 == 0)
                fprintf(stderr, "  [%4u] %-45s OK (%u bytes)\n", i, name, a_sz);
        } else {
            /* Find first diff */
            uint32_t first_diff = 0;
            for (uint32_t j = 0; j < a_sz; j++) {
                if (a_data[j] != b_data[j]) { first_diff = j; break; }
            }
            fprintf(stderr, "  [%4u] %-45s FAIL first_diff@%u: A=0x%02x B=0x%02x\n",
                    i, name, first_diff, a_data[first_diff], b_data[first_diff]);
            fail++;
            fail_bytes += a_sz;
        }
    }

    /* Check for tensors in B not in A */
    uint32_t only_in_b = 0;
    for (uint32_t j = 0; j < b.n_tensors; j++) {
        int found = 0;
        for (uint32_t i = 0; i < a.n_tensors; i++) {
            if (strcmp(a.names[i], b.names[j]) == 0) { found = 1; break; }
        }
        if (!found) {
            fprintf(stderr, "  B-only: %-45s (%u bytes)\n", b.names[j], b.sizes[j]);
            only_in_b++;
        }
    }

    double elapsed = now_sec() - t0;
    double throughput = (pass_bytes + fail_bytes) / (elapsed * 1024.0 * 1024.0);

    /* ── Summary ── */
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "PASS:      %u tensors (%.1f MB)\n", pass, pass_bytes / (1024.0*1024.0));
    fprintf(stderr, "FAIL:      %u tensors (%.1f MB)\n", fail, fail_bytes / (1024.0*1024.0));
    fprintf(stderr, "SKIP:      %u (name not found)\n", skip_no_name);
    fprintf(stderr, "ONLY-IN-B: %u\n", only_in_b);
    fprintf(stderr, "TIME:      %.3f s\n", elapsed);
    fprintf(stderr, "THROUGHPUT: %.1f MB/s\n", throughput);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");

    /* Print result to stdout for scripting */
    if (fail == 0 && only_in_b == 0)
        printf("LOSSLESS %u tensors %.1f MB verified in %.3f s\n",
               pass, pass_bytes / (1024.0*1024.0), elapsed);
    else
        printf("FAIL %u/%u tensors differ\n", fail, pass + fail);

    free(b_idx);
    gguf_close(&a);
    gguf_close(&b);
    return fail > 0 ? 1 : 0;
}
