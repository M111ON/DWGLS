/*
 * tools/geo_dupscan.c — chunk-level duplicate structure inside real weights
 * ════════════════════════════════════════════════════════════════════════
 * Question never measured before: within ONE baked model, how many
 * 128KB chunks are byte-identical to each other? (The tied-embedding
 * finding was TENSOR-level 137MB — chunk level is unexplored.)
 *
 * Method: FNV-1a digest per part -> sort -> groups with equal digests
 * get memcmp-CONFIRMED pairwise (hash collisions cannot fake a dup).
 *
 * Oracle: memcmp. Reports wasted slots = duplicate chunks beyond the first.
 *
 * BUILD: gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Icore -o build/geo_dupscan tools/geo_dupscan.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/gguf_reader.h"

#define PART_BYTES (128u * 1024u)

static uint64_t fnv1a(const uint8_t *p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i += 8) {          /* word-stepped FNV */
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct { uint64_t dig; uint32_t part; } Dig;

static int cmp_dig(const void *a, const void *b) {
    uint64_t da = ((const Dig *)a)->dig, db = ((const Dig *)b)->dig;
    return da < db ? -1 : da > db ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open\n"); return 1; }

    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    printf("=== geo_dupscan — %s ===\nparts %u (%.1f MB)\n",
           path, total_parts, (double)total_bytes / 1e6);

    Dig *digs = (Dig *)malloc(sizeof(Dig) * total_parts);
    double t0 = time(NULL);
    for (uint32_t fid = 0, i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++, fid++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            digs[fid].dig  = fnv1a(src + off, len);
            digs[fid].part = fid;
        }
    }
    printf("hashed %u parts · %.0f s\n", total_parts, difftime(time(NULL), t0));

    qsort(digs, total_parts, sizeof(Dig), cmp_dig);

    /* walk groups */
    uint32_t groups = 0, dup_groups = 0, dup_chunks = 0;
    uint64_t waste = 0;
    uint32_t biggest = 0, biggest_members = 0;
    for (uint32_t i = 0; i < total_parts;) {
        uint32_t j = i + 1;
        while (j < total_parts && digs[j].dig == digs[i].dig) j++;
        uint32_t members = j - i;
        if (members > 1) {
            /* memcmp-confirm all pairs vs group head (real duplicates only) */
            uint32_t confirmed = 1;
            uint32_t head = digs[i].part;
            uint32_t hoff = (head % total_parts) * 0;   /* need src slice: recompute below */
            /* locate source slice of head by walking tensors again is costly;
               instead compare member-vs-member via their own slices */
            uint32_t ti_head = 0, off_head = 0, len_head = 0;
            {
                uint32_t acc = 0;
                for (uint32_t k = 0; k < r.n_tensors; k++) {
                    uint32_t np = (r.sizes[k] + PART_BYTES - 1) / PART_BYTES;
                    if (head >= acc && head < acc + np) {
                        ti_head = k;
                        off_head = (head - acc) * PART_BYTES;
                        len_head = r.sizes[ti_head] - off_head;
                        if (len_head > PART_BYTES) len_head = PART_BYTES;
                        break;
                    }
                    acc += np;
                }
            }
            const uint8_t *hsrc = r.base + r.data_offset + r.offsets[ti_head] + off_head;
            for (uint32_t q = i + 1; q < j; q++) {
                uint32_t m = digs[q].part, t2 = 0, o2 = 0, l2 = 0;
                uint32_t acc = 0;
                for (uint32_t k = 0; k < r.n_tensors; k++) {
                    uint32_t np = (r.sizes[k] + PART_BYTES - 1) / PART_BYTES;
                    if (m >= acc && m < acc + np) {
                        t2 = k; o2 = (m - acc) * PART_BYTES;
                        l2 = r.sizes[t2] - o2; if (l2 > PART_BYTES) l2 = PART_BYTES;
                        break;
                    }
                    acc += np;
                }
                const uint8_t *msrc = r.base + r.data_offset + r.offsets[t2] + o2;
                if (l2 == len_head && memcmp(hsrc, msrc, len_head) == 0) confirmed++;
            }
            if (confirmed > 1) {
                dup_groups++;
                dup_chunks += confirmed - 1;
                waste += (uint64_t)(confirmed - 1) * len_head;
                if (confirmed > biggest_members) { biggest_members = confirmed; biggest = head; }
            }
            (void)hoff;
        }
        groups++;
        i = j;
    }

    printf("\nchunks          : %u (%.1f MB)\n", total_parts, (double)total_bytes / 1e6);
    printf("unique contents : %u\n", total_parts - dup_chunks);
    printf("dup groups      : %u (memcmp-confirmed) · extra chunks %u\n",
           dup_groups, dup_chunks);
    if (biggest_members)
        printf("biggest group   : %u identical chunks (head part %u)\n",
               biggest_members, biggest);
    printf("wasted slots    : %.1f MB (%.2f%% of payload)\n",
           (double)waste / 1e6, 100.0 * (double)waste / (double)total_bytes);

    free(digs);
    gguf_close(&r);
    return 0;
}
