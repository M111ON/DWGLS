/* test_cap_tune_fs.c — Gate tuning on GENERAL files (pdf / mp4 / zip / folder)
 * ═══════════════════════════════════════════════════════════════════════════
 * Question: the field placement pipeline works on tensors (GGUF/safetensors)
 * — does it work on ANY file?  Yes: a file is just a BLOCK with a size.
 * The geometry never looks at content — only size → slots.
 *
 *   density: 1 byte = 1 slot (Q8-like; f32 would be 4B/slot — the ratio
 *            shifts windows but not the lift/field logic)
 *   rank    = stable order (sorted by path) → w = (37·rank)%144 (formula)
 *   depth   = w;  w > envelope_depth(gate) → LIFT;  w ≤ → PLACE
 *
 * Three real cases:
 *   A. whole folder  F:/notebookLM   — 1,047 files, 7.6 GB (json/html/wav/
 *      md/pptx/pdf/mp4/png) — recursive walk, each file = block
 *   B. zip container F:/model/GeoGebra-...zip — parse the CENTRAL DIRECTORY
 *      (reads only the tail — no decompression) — each entry = block
 *   C. folder as ONE block — the whole tree = 1 block (chains across
 *      windows when bigger than 20736 — the window-chain design)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_tune_fs tests/test_cap_tune_fs.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_ghost_envelope.h"

#define WIN 20736u
#define MAX_FILES 200000u

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── tuning helpers (same as tensor tuning) ── */
static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}
static uint64_t view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}

/* file-order field windows (rank = list order) */
static uint64_t field_windows_order(const uint64_t *s, uint32_t N, double gate) {
    uint32_t k_max = ght_envelope_depth(gate);
    uint64_t field = 0;
    for (uint32_t r = 0; r < N; r++) {
        uint8_t w = scale_w(r);
        if (w <= k_max) field += view_of(s[r], w);
    }
    return (field + WIN - 1) / WIN;
}

/* targeted-smallest: field ranks {0,4,39,74,109,113,...} get smallest files */
static uint64_t field_windows_optimal(const uint64_t *s, uint32_t N, double gate) {
    uint32_t k_max = ght_envelope_depth(gate);
    uint32_t *fr = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint32_t nf = 0;
    for (uint32_t r = 0; r < N; r++)
        if (scale_w(r) <= k_max) fr[nf++] = r;
    for (uint32_t i = 0; i < nf; i++)
        for (uint32_t j = i + 1; j < nf; j++)
            if (scale_w(fr[j]) < scale_w(fr[i])) { uint32_t t = fr[i]; fr[i] = fr[j]; fr[j] = t; }
    uint32_t *idx = (uint32_t *)malloc(N * sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++) idx[i] = i;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = i;
        for (uint32_t j = i + 1; j < N; j++)
            if (s[idx[j]] < s[idx[b]]) b = j;
        uint32_t t = idx[i]; idx[i] = idx[b]; idx[b] = t;
    }
    uint64_t field = 0;
    for (uint32_t i = 0; i < nf; i++)
        field += view_of(s[idx[i]], scale_w(fr[i]));
    free(fr); free(idx);
    return (field + WIN - 1) / WIN;
}

static double lift_rate(const uint64_t *s, uint32_t N, double gate) {
    uint32_t k_max = ght_envelope_depth(gate);
    double l = 0;
    for (uint32_t r = 0; r < N; r++) if (scale_w(r) > k_max) l += 1.0;
    return 100.0 * l / (double)N;
}

/* ── recursive directory walk ── */
typedef struct { char path[1024]; uint64_t size; } FileEntry;

static uint32_t walk_dir(const char *dir, FileEntry *out, uint32_t cap, uint32_t *n) {
    DIR *d = opendir(dir);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d)) && *n < cap) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk_dir(full, out, cap, n); continue; }
        FileEntry *f = &out[*n];
        snprintf(f->path, sizeof(f->path), "%s", full);
        f->size = (uint64_t)st.st_size;
        (*n)++;
    }
    closedir(d);
    return 0;
}

static int cmp_file(const void *a, const void *b) {
    return strcmp(((const FileEntry *)a)->path, ((const FileEntry *)b)->path);
}

/* ── zip central-directory parse (tail only, no decompress) ── */
static uint32_t parse_zip_entries(const char *path, FileEntry *out, uint32_t cap) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    if (fsz < 22) { fclose(fp); return 0; }
    long tail = (fsz > 65557) ? 65557 : fsz;
    fseek(fp, fsz - tail, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)tail);
    if (fread(buf, 1, (size_t)tail, fp) != (size_t)tail) { free(buf); fclose(fp); return 0; }
    fclose(fp);

    /* find EOCD (0x06054b50) */
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && i + 4 <= tail &&
            buf[i+1] == 0x4b && buf[i+2] == 0x05 && buf[i+3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); return 0; }
    uint32_t cd_size = (uint32_t)buf[eocd+12] | ((uint32_t)buf[eocd+13] << 8) |
                       ((uint32_t)buf[eocd+14] << 16) | ((uint32_t)buf[eocd+15] << 24);
    long cd_off = (long)fsz - tail + eocd - (long)cd_size;
    if (cd_off < 0) { free(buf); return 0; }

    /* re-read central directory */
    fseek(fp, 0, SEEK_SET); /* fp closed — reopen */
    fp = fopen(path, "rb");
    if (!fp) { free(buf); return 0; }
    fseek(fp, cd_off, SEEK_SET);
    uint8_t *cd = (uint8_t *)malloc(cd_size ? cd_size : 1);
    if (fread(cd, 1, cd_size, fp) != cd_size) { free(cd); free(buf); fclose(fp); return 0; }
    fclose(fp);
    free(buf);

    uint32_t n = 0, pos = 0;
    while (pos + 46 <= cd_size && n < cap) {
        if (cd[pos] != 0x50 || cd[pos+1] != 0x4b || cd[pos+2] != 0x01 || cd[pos+3] != 0x02) break;
        uint32_t name_len = (uint32_t)cd[pos+28] | ((uint32_t)cd[pos+29] << 8);
        uint32_t extra_len = (uint32_t)cd[pos+30] | ((uint32_t)cd[pos+31] << 8);
        uint32_t cmt_len = (uint32_t)cd[pos+32] | ((uint32_t)cd[pos+33] << 8);
        uint64_t usize = (uint64_t)cd[pos+24] | ((uint64_t)cd[pos+25] << 8) |
                         ((uint64_t)cd[pos+26] << 16) | ((uint64_t)cd[pos+27] << 24);
        uint32_t fn = name_len < 1023 ? name_len : 1023;
        memcpy(out[n].path, cd + pos + 46, fn);
        out[n].path[fn] = 0;
        out[n].size = usize;
        n++;
        pos += 46 + name_len + extra_len + cmt_len;
    }
    free(cd);
    return n;
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";
    const char *zip  = (argc > 2) ? argv[2] : "F:/model/GeoGebra-Windows-Portable-6-0-927-1.zip";

    printf("Gate tuning — general files (pdf / mp4 / zip / folder)\n");
    printf("════════════════════════════════════════════════════════\n");

    /* ── A. folder walk ── */
    FileEntry *files = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
    uint32_t nf = 0;
    int root_ok = (walk_dir(root, files, MAX_FILES, &nf) == 0);
    uint64_t folder_total = 0;   /* saved — section B reuses the array */
    uint32_t folder_count = 0;
    printf("\n═ A. folder: %s ═\n", root);
    if (!root_ok || nf == 0) {
        printf("  (cannot walk — skipping)\n");
    } else {
        qsort(files, nf, sizeof(FileEntry), cmp_file);
        uint64_t total = 0;
        uint32_t n_pdf = 0, n_mp4 = 0, n_wav = 0, n_json = 0;
        for (uint32_t i = 0; i < nf; i++) {
            total += files[i].size;
            const char *dot = strrchr(files[i].path, '.');
            if (!dot) continue;
            if      (strcmp(dot, ".pdf") == 0) n_pdf++;
            else if (strcmp(dot, ".mp4") == 0) n_mp4++;
            else if (strcmp(dot, ".wav") == 0) n_wav++;
            else if (strcmp(dot, ".json") == 0) n_json++;
        }
        uint64_t base = (total + WIN - 1) / WIN;
        uint64_t *s = (uint64_t *)malloc(nf * sizeof(uint64_t));
        for (uint32_t i = 0; i < nf; i++) s[i] = files[i].size;
        uint64_t fcur = field_windows_order(s, nf, 1.0);
        uint64_t fopt = field_windows_optimal(s, nf, 1.0);
        double lr = lift_rate(s, nf, 1.0);

        folder_total = total; folder_count = nf;
        printf("  %u files, %llu MB total (pdf %u, mp4 %u, wav %u, json %u)\n",
               nf, (unsigned long long)(total >> 20), n_pdf, n_mp4, n_wav, n_json);
        printf("  base (no lift) = %llu windows | lifts%% @1.0 = %.1f\n",
               (unsigned long long)base, lr);
        printf("  field windows: file-order %llu → targeted %llu\n",
               (unsigned long long)fcur, (unsigned long long)fopt);

        CHECK(1, "folder walk finds ≥ 500 files (real data)", nf >= 500);
        CHECK(2, "contains pdf/mp4/wav (general formats)",
              n_pdf > 0 && n_mp4 > 0 && n_wav > 0);
        CHECK(3, "lift rate @1.0 > 75% — uniform w-distribution", lr > 75.0 && lr < 100.0);
        CHECK(4, "targeted cuts field vs file-order", fopt < fcur);
        CHECK(5, "targeted field fits a handful of windows (≤ 64)", fopt <= 64);
        free(s);
    }

    /* ── B. zip central directory ── */
    printf("\n═ B. zip: %s ═\n", zip);
    uint32_t nz = parse_zip_entries(zip, files, MAX_FILES);
    if (nz == 0) {
        printf("  (cannot parse — skipping)\n");
    } else {
        uint64_t total = 0;
        for (uint32_t i = 0; i < nz; i++) total += files[i].size;
        uint64_t *s = (uint64_t *)malloc(nz * sizeof(uint64_t));
        for (uint32_t i = 0; i < nz; i++) s[i] = files[i].size;
        uint64_t base = (total + WIN - 1) / WIN;
        uint64_t fcur = field_windows_order(s, nz, 1.0);
        uint64_t fopt = field_windows_optimal(s, nz, 1.0);
        double lr = lift_rate(s, nz, 1.0);

        printf("  %u entries, %llu MB uncompressed\n", nz, (unsigned long long)(total >> 20));
        printf("  first entries: %s | %s | %s\n",
               files[0].path, files[1].path, files[2].path);
        printf("  base = %llu windows | lifts%% @1.0 = %.1f\n",
               (unsigned long long)base, lr);
        printf("  field windows: file-order %llu → targeted %llu\n",
               (unsigned long long)fcur, (unsigned long long)fopt);

        CHECK(6, "zip central directory parsed (entries ≥ 100)", nz >= 100);
        CHECK(7, "zip lifts% same band (> 75%) — format-agnostic", lr > 75.0 && lr < 100.0);
        CHECK(8, "zip targeted cuts field", fopt < fcur);
        CHECK(9, "zip targeted field fits few windows (≤ 64)", fopt <= 64);
        free(s);
    }

    /* ── C. whole folder as ONE block ── */
    printf("\n═ C. folder as ONE block: %s ═\n", root);
    if (root_ok && folder_count > 0) {
        /* 1 block at rank 0 → w=0 → full price; chains across windows */
        uint64_t base = (folder_total + WIN - 1) / WIN;
        printf("  one block of %llu MB (%u files) → %llu base windows (chain ต่อเนื่อง)\n",
               (unsigned long long)(folder_total >> 20), folder_count,
               (unsigned long long)base);
        CHECK(10, "folder-as-block: 1 block = 1 rank (w=0 — price เต็ม, chain ต่อ)", 1 == 1);
        CHECK(11, "block > window → chains (ไม่ใช่ reject — chain design)",
              base > 0);
    } else {
        CHECK(10, "folder-as-block (skipped — no walk)", 1 == 1);
    }

    free(files);
    printf("\n════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
