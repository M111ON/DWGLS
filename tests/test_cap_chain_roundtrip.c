/* test_cap_chain_roundtrip.c — REAL file through the full chain → lossless
 * ═══════════════════════════════════════════════════════════════════════════
 * End-to-end proof on a real PDF from F:/notebookLM:
 *
 *   file → chunk (≤ RS_MAX_DATA_SIZE) → per chunk:
 *     w_i = (37·i)%144                       (placement formula)
 *     cap_admit(gate, 0, w_i)                (§11.6 accounting)
 *       CAP_LIFT  → ghost_lift_auto(...)     (§15.32 — freeze in
 *                                              residual_space + route)
 *       CAP_ADMIT → field placement = pointer-home (data stays in the
 *                   source file — zero-copy, §15.11)
 *
 *   read back: LIFTED chunks via ghost_read (bond + route), ADMITTED
 *   chunks via the source (field address = source offset).
 *   Reconstruct the file → must equal the original byte-for-byte.
 *
 * The PDF is ~20 MB → ~1,200 chunks → most lift (w > 5), the field
 * ranks (w ≤ 5) stay pointer-home — this is the real chain, not a toy.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_chain_roundtrip tests/test_cap_chain_roundtrip.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_cap_account.h"
#include "../core/geo_ghost_lift.h"

#define CHUNK_SZ 16384u   /* 16 KB per block — well under RS_MAX_DATA_SIZE */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

/* find the first .pdf under root (recursive) */
static int find_first_pdf(const char *dir, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) && !found) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { found = find_first_pdf(full, out, outsz); continue; }
        size_t len = strlen(full);
        if (len >= 4 && strcmp(full + len - 4, ".pdf") == 0) {
            snprintf(out, outsz, "%s", full);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static int read_file(const char *path, uint8_t **buf, uint64_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(fp); return -1; }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) { free(b); fclose(fp); return -1; }
    fclose(fp);
    *buf = b; *size = (uint64_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";

    printf("Real file through the chain — cap_admit + ghost_lift_auto → lossless\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    char pdf_path[1100];
    int found = find_first_pdf(root, pdf_path, sizeof(pdf_path));
    if (!found) {
        printf("  (no .pdf under %s — skipping)\n", root);
        printf("══════════════════════════════════════════════════════════════════\n");
        printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
        return 0;
    }
    printf("  PDF: %s\n", pdf_path);

    uint8_t *orig = NULL;
    uint64_t fsize = 0;
    if (read_file(pdf_path, &orig, &fsize) != 0) {
        printf("  (cannot read — skipping)\n");
        printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
        return 0;
    }

    uint32_t nchunks = (uint32_t)((fsize + CHUNK_SZ - 1) / CHUNK_SZ);
    printf("  size %llu bytes → %u chunks of %u B\n",
           (unsigned long long)fsize, nchunks, CHUNK_SZ);

    /* ── the chain ── */
    CapAccount acc; cap_init(&acc);
    GhostLog log;  ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 4096);

    uint8_t *placed = (uint8_t *)calloc(1, sizeof(uint8_t)); /* flags per chunk */
    uint8_t *lifted = (uint8_t *)calloc(nchunks, sizeof(uint8_t));
    uint32_t n_lift = 0, n_field = 0, n_err = 0;

    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        const uint8_t *chunk = orig + (uint64_t)i * CHUNK_SZ;

        int verdict = cap_admit(&acc, 1.0, 0, w);
        if (verdict == CAP_LIFT) {
            int r = ghost_lift_auto(&log, &rs, 1.0, (uint16_t)i, 0, w,
                                    chunk, len);
            if (r == GHOST_AUTO_LIFT) { lifted[i] = 1; n_lift++; }
            else                       { n_err++; }
        } else if (verdict == CAP_ADMIT) {
            n_field++;          /* pointer-home — data stays in source */
        } else {
            n_err++;            /* CAP_REJECT — shouldn't happen here */
        }
    }

    printf("  placed: %u lifted → residual_space, %u in-field (pointer-home), %u errors\n",
           n_lift, n_field, n_err);
    CHECK(1, "PDF found + size ≥ 1 KB", fsize >= 1024);
    CHECK(2, "chunked into ≥ 2 blocks (chain — not a toy)",
          nchunks >= 2 && nchunks <= GHOST_LOG_MAX);
    CHECK(3, "every chunk placed — zero errors, zero rejects",
          n_err == 0 && acc.rejects == 0 && n_lift + n_field == nchunks);

    /* ── per-chunk lossless read-back (lifted) ── */
    int all_chunks_ok = 1;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        if (lifted[i]) {
            uint32_t out_sz = 0;
            const void *got = ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
            if (!got || out_sz != len ||
                memcmp(got, orig + (uint64_t)i * CHUNK_SZ, len) != 0)
                all_chunks_ok = 0;
        }
    }
    CHECK(4, "every LIFTED chunk reads back exact bytes (lossless per chunk)",
          all_chunks_ok);

    /* ── full reconstruction ── */
    uint8_t *recon = (uint8_t *)malloc((size_t)fsize);
    int recon_ok = 1;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        const uint8_t *src;
        if (lifted[i]) {
            uint32_t out_sz = 0;
            src = (const uint8_t *)ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
            if (!src || out_sz != len) { recon_ok = 0; break; }
        } else {
            src = orig + (uint64_t)i * CHUNK_SZ;   /* pointer-home — source */
        }
        memcpy(recon + (uint64_t)i * CHUNK_SZ, src, len);
    }
    CHECK(5, "FULL FILE reconstructed byte-for-byte (lossless end-to-end)",
          recon_ok && memcmp(recon, orig, (size_t)fsize) == 0);

    /* ── integrity: wrong route / wrong scale breaks access ──
       chunk 1 (rank 1 → w=37 > 5) is LIFTED — the route checks use it */
    {
        uint8_t w = scale_w(1);
        uint32_t out_sz = 0;
        CHECK(6, "correct route readable (chunk 1, lifted)",
              ghost_read(&log, &rs, 1, 0, w, &out_sz) != NULL);
        CHECK(6, "wrong to_scale → NULL (route is the authority)",
              ghost_read(&log, &rs, 1, 0, (uint8_t)(w + 1), &out_sz) == NULL);
        CHECK(6, "wrong from_scale → NULL (เสาเข็มห้ามขยับ)",
              ghost_read(&log, &rs, 1, 1, w, &out_sz) == NULL);
        CHECK(6, "wrong block_id → NULL",
              ghost_read(&log, &rs, 9999, 0, w, &out_sz) == NULL);
    }

    /* ── accounting consistency ── */
    CHECK(7, "cap accounting: blocks + lifts == chunks",
          acc.blocks == n_field && acc.lifts == n_lift);
    CHECK(7, "admitted field usage ≤ 20736 (Σ envelope fits)",
          acc.used <= CAP_WIN);
    CHECK(7, "ghost log records exactly the lifted routes", log.count == n_lift);

    /* ── determinism: fresh account → same verdicts ── */
    {
        CapAccount b; cap_init(&b);
        int same = 1;
        for (uint32_t i = 0; i < nchunks; i++)
            if (cap_admit(&b, 1.0, 0, scale_w(i)) !=
                (lifted[i] ? CAP_LIFT : CAP_ADMIT)) same = 0;
        CHECK(8, "deterministic: fresh account → same verdicts", same);
    }

    printf("\n  summary: %u/%u chunks lifted → ghost; field holds %u pointer-home; used %llu/20736 slots\n",
           n_lift, nchunks, n_field, (unsigned long long)acc.used);

    rs_free(&rs);
    free(orig); free(recon); free(lifted); free(placed);

    printf("══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
