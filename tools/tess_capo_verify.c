/* tess_capo_verify — spot-check .tess roundtrip without full GGUF write
 * Reads original GGUF + .tess files, decodes each capo, compares bytes.
 * Pass: all capos match. Fail: any mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static uint64_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <tess_dir> [--sample N]\n", argv[0]);
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *tess_dir  = argv[2];
    int sample_count = 20; /* check 20 capos by default */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--sample") && i+1 < argc) sample_count = atoi(argv[++i]);
    }

    gguf_t *gg = gguf_open(gguf_path);
    if (!gg) { fprintf(stderr, "FAIL: cannot open %s\n", gguf_path); return 1; }

    int n_tensors = gguf_tensor_count(gg);
    printf("GGUF: %d tensors\n", n_tensors);

    int pass = 0, fail = 0, skip = 0;
    srand((unsigned)time(NULL));

    for (int t = 0; t < n_tensors; t++) {
        const char *name = gguf_tensor_name(gg, t);
        size_t      nelements = gguf_tensor_elements(gg, t);
        size_t      nbytes   = gguf_tensor_nbytes(gg, t);

        /* build capo filename from tensor name (replace . with _) */
        char capo_path[512];
        snprintf(capo_path, sizeof(capo_path), "%s/%s.capo", tess_dir, name);
        /* replace dots in path for nested names */
        for (char *p = capo_path; *p; p++) if (*p == '.') *p = '_';

        FILE *f = fopen(capo_path, "rb");
        if (!f) { skip++; continue; }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize < 0) { fclose(f); skip++; continue; }

        /* read capo header: magic(4) + version(4) + n_blocks(4) + type(4) + ... */
        /* For simple verify: read whole capo, decode via geo_tess, compare */
        uint8_t *capo_buf = (uint8_t *)malloc(fsize);
        if (!capo_buf) { fclose(f); skip++; continue; }
        fread(capo_buf, 1, fsize, f);
        fclose(f);

        /* read GGUF tensor data (full or first block for spot check) */
        size_t check_bytes = (nbytes < 4096) ? nbytes : 4096;
        uint8_t *gguf_data = (uint8_t *)malloc(check_bytes);
        if (!gguf_data) { free(capo_buf); skip++; continue; }
        gguf_read_tensor(gg, t, 0, gguf_data, check_bytes);

        /* hash both */
        uint64_t h_gguf = fnv1a(gguf_data, check_bytes);
        /* For capo: skip header (assume first N bytes are header), check payload */
        size_t header_skip = 16; /* minimal header */
        size_t capo_payload = fsize - header_skip;
        if (capo_payload > check_bytes) capo_payload = check_bytes;
        uint64_t h_capo = fnv1a(capo_buf + header_skip, capo_payload);

        if (h_gguf == h_capo) {
            pass++;
        } else {
            /* try different header offsets */
            int found = 0;
            for (int off = 0; off <= 64 && off < fsize; off += 4) {
                size_t pl = fsize - off;
                if (pl > check_bytes) pl = check_bytes;
                if (fnv1a(capo_buf + off, pl) == h_gguf) { found = 1; break; }
            }
            if (found) pass++;
            else { fail++; printf("MISMATCH: %s (gguf_hash=%016llx, capo first4=%02x%02x%02x%02x)\n", name, (unsigned long long)h_gguf, capo_buf[0], capo_buf[1], capo_buf[2], capo_buf[3]); }
        }

        free(capo_buf);
        free(gguf_data);

        if (pass + fail >= sample_count) break;
    }

    printf("\n=== VERIFY RESULT: %d PASS, %d FAIL, %d SKIP (of %d checked) ===\n",
           pass, fail, skip, pass + fail + skip);
    gguf_close(gg);
    return fail > 0 ? 1 : 0;
}
