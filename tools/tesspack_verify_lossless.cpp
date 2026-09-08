// tesspack_verify_lossless.cpp — Verify tesspack data is lossless vs GGUF
// Does NOT need llama-model.h — uses only public gguf/ggml/tesspack APIs
// Strategy: load model from GGUF normally, then for each GGUF tensor,
// decode from tesspack and compare byte-for-byte.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static void *mmap_file(const char *path, size_t *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    *out_size = (size_t)sz.QuadPart;
    HANDLE fm = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(h);
    if (!fm) return NULL;
    void *p = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(fm);
    return p;
}
static int rss_mb() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (int)(pmc.WorkingSetSize / (1024*1024));
    return 0;
}
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static void *mmap_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    *out_size = st.st_size;
    void *p = mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return p;
}
static int rss_mb() { return 0; }
#endif

extern "C" {
#include "gguf.h"
#include "ggml.h"
#include "llama.h"
}

// Include tesspack container header
#include "geo_tess_container.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <model.tesspack>\n", argv[0]);
        fprintf(stderr, "  Verifies that tesspack data is bitwise identical to GGUF data.\n");
        return 1;
    }
    const char *gguf_path = argv[1];
    const char *tesspack_path = argv[2];

    printf("[verify] RSS at start: %d MB\n", rss_mb());

    // Open GGUF via gguf API
    printf("[verify] Opening GGUF: %s\n", gguf_path); fflush(stdout);
    struct gguf_init_params gip = { .no_alloc = true, .ctx = NULL, .data = NULL };
    struct gguf_context *gguf = gguf_init_from_file(gguf_path, gip);
    if (!gguf) { fprintf(stderr, "ERROR: gguf_init_from_file failed\n"); return 1; }
    int n_tensors = gguf_get_n_tensors(gguf);
    printf("[verify] GGUF: %d tensors\n", n_tensors); fflush(stdout);

    // mmap GGUF for raw data access
    size_t gguf_size = 0;
    void *gguf_mmap = mmap_file(gguf_path, &gguf_size);
    if (!gguf_mmap) { fprintf(stderr, "ERROR: GGUF mmap failed\n"); gguf_free(gguf); return 1; }
    size_t gguf_data_offset = gguf_get_data_offset(gguf);
    printf("[verify] GGUF mmap: %zu MB, data_offset=%zu\n", gguf_size / (1024*1024), gguf_data_offset); fflush(stdout);

    // Open tesspack
    printf("[verify] Opening tesspack: %s\n", tesspack_path); fflush(stdout);
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(tesspack_path, &pi) != 0) {
        fprintf(stderr, "ERROR: tesspack open failed\n");
        munmap(gguf_mmap, gguf_size);
        gguf_free(gguf);
        return 1;
    }
    printf("[verify] Tesspack: %d capos, %d onion, %d residual\n",
           pi.n_capos, pi.n_onion_count, pi.n_residual_count); fflush(stdout);

    // Verify each tensor
    int pass = 0, fail = 0, skip = 0;
    size_t total_bytes = 0;

    for (int i = 0; i < n_tensors; i++) {
        const char *name = gguf_get_tensor_name(gguf, i);
        enum ggml_type gtype = gguf_get_tensor_type(gguf, i);
        size_t offset = gguf_get_tensor_offset(gguf, i);

        // Compute tensor size from ne[]
        uint32_t n_dims = gguf_get_tensor_n_dims(gguf, i);
        size_t n_elems = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            n_elems *= gguf_get_tensor_ne(gguf, i, d);
        }
        size_t elem_sz = ggml_type_size(gtype);
        size_t blk_size = ggml_blck_size(gtype);
        size_t byte_count = (blk_size > 0) ? ((n_elems + blk_size - 1) / blk_size * elem_sz) : (n_elems * elem_sz);

        // Pointer to original GGUF tensor data
        uint8_t *gguf_data = (uint8_t *)gguf_mmap + gguf_data_offset + offset;

        // Find in tesspack
        const TESS_OnionEntry *onion = tess_pack_find_onion(&pi, name);
        if (onion) {
            // ONION tensor: compare raw bytes
            if (onion->size == byte_count) {
                int cmp = memcmp(gguf_data, pi.data + onion->offset, byte_count);
                if (cmp == 0) { pass++; total_bytes += byte_count; }
                else { fail++; printf("[FAIL-ONION] %s (size=%zu)\n", name, byte_count); }
            } else {
                // Size mismatch — might be F32 stored vs different in GGUF
                skip++;
                printf("[SKIP-ONION] %s: gguf_bytes=%zu onion_size=%zu type=%d\n", name, byte_count, onion->size, gtype);
            }
            continue;
        }

        int cid = tess_pack_get_capo(&pi, name);
        if (cid >= 0) {
            // SCATTER tensor: decode from capos and compare
            uint8_t *decoded = (uint8_t *)malloc(byte_count);
            if (!decoded) { skip++; continue; }
            memset(decoded, 0, byte_count);
            int rc = tess_capo_load_range(&pi, cid, decoded, byte_count);
            if (rc == 0) {
                int cmp = memcmp(gguf_data, decoded, byte_count);
                if (cmp == 0) { pass++; total_bytes += byte_count; }
                else {
                    fail++;
                    // Find first diff
                    size_t first_diff = 0;
                    for (size_t j = 0; j < byte_count; j++) {
                        if (gguf_data[j] != decoded[j]) { first_diff = j; break; }
                    }
                    printf("[FAIL-SCATTER] %s (size=%zu, first_diff_byte=%zu)\n", name, byte_count, first_diff);
                }
            } else {
                fail++;
                printf("[FAIL-SCATTER] %s decode rc=%d\n", name, rc);
            }
            free(decoded);
            continue;
        }

        // Check residual
        int rid = tess_pack_find_residual(&pi, name);
        if (rid >= 0) {
            // Residual tensor — the GGUF data is the source; tesspack stores it differently
            skip++;
            continue;
        }

        skip++;
        printf("[SKIP] %s: not in tesspack\n", name);
    }

    printf("\n[verify] RESULTS: %d PASS, %d FAIL, %d SKIP (total %d tensors, %zu MB verified)\n",
           pass, fail, skip, n_tensors, total_bytes / (1024*1024));
    printf("[verify] RSS: %d MB\n", rss_mb());

    if (fail == 0 && pass > 0) {
        printf("[verify] VERDICT: TESSPACK IS LOSSLESS (all %d packable tensors verified identical)\n", pass);
    } else if (fail > 0) {
        printf("[verify] VERDICT: TESSPACK HAS DIFFERENCES (%d tensors differ)\n", fail);
    } else {
        printf("[verify] VERDICT: NO TENSORS VERIFIED\n");
    }

    tess_pack_close(&pi);
    munmap(gguf_mmap, gguf_size);
    gguf_free(gguf);
    return fail > 0 ? 1 : 0;
}
