/* cap_chain_scan.c — streaming chain over a folder or GGUF model (knobs ผ่าน CLI)
 * ═══════════════════════════════════════════════════════════════════════════
 * T1.2 follow-up: wire champion rule set (stride 29/41 · offset 7/122 ·
 * gate 3.0 → kmax 4 · orbit 1 · chunk 262144) เข้า chain จริง แล้วพิสูจน์
 * lossless byte-for-byte end-to-end (folder + GGUF model file)
 *
 *   file → chunks (chunk B) → per chunk: w = (stride·rank + offset) % 144
 *     admit (per-orbit capacity GHT_WIN/orbit, envelope ght_fp(k)):
 *       LIFT   → rs_freeze (bond = ghost_piece) → residual_space
 *       ADMIT  → pointer-home (data stays in source)
 *       REJECT → นับ (deterministic — ไม่ silent) → pointer-home (lossless ยังอยู่)
 *   streaming windows (WINDOW_BYTES bounded) — place → verify → teardown
 *
 * CLI:
 *   cap_chain_scan [root] [--stride S] [--offset O] [--gate G] [--orbit Q] [--chunk C]
 *   cap_chain_scan --gguf <model.gguf> [--stride S] ...   (ทั้งไฟล์ = 1 stream)
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/cap_chain_scan tools/cap_chain_scan.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_cap_account.h"
#include "../core/geo_ghost_lift.h"

/* 64-bit file seek/tell — Windows: ใช้ _fseeki64/_ftelli64 จริง (CRT 64-bit),
   non-Windows: fallback เป็น fseeko/ftello (off_t 64-bit ที่นั่น) —
   ftello บน Windows truncate 32-bit → ไฟล์ >2GB พัง (§15.71) */
#if !defined(_WIN32) && !defined(_fseeki64)
#define _fseeki64(f, o, w) fseeko((f), (off_t)(o), (w))
#endif
#if !defined(_WIN32) && !defined(_ftelli64)
#define _ftelli64(f) ftello(f)
#endif

#define WINDOW_BYTES (16u << 20)   /* 16 MB per stream window */
#define MAX_FILES 50000u

typedef struct {
    uint16_t stride;
    uint8_t  offset;
    double   gate;
    uint8_t  orbit;
    uint32_t chunk;
} Knobs;

static void knobs_default(Knobs *k) {
    /* trained default (§15.71 unified champion) — single source: core/geo_cap_account.h */
    k->stride = CAP_RULE_STRIDE;
    k->offset = (uint8_t)CAP_RULE_OFFSET;
    k->gate   = CAP_RULE_GATE;
    k->orbit  = CAP_RULE_ORBIT;
    k->chunk  = CAP_RULE_CHUNK;
}

static uint8_t scale_w(const Knobs *k, uint64_t rank) {
    return (uint8_t)(((uint64_t)k->stride * rank + k->offset) % 144u);
}

/* admission ตาม cap_admit + per-orbit partition */
static int admit(const Knobs *k, uint64_t *used, uint8_t w, uint64_t rank,
                 uint64_t *lifts, uint64_t *rejects, uint64_t *field) {
    uint32_t kd = ght_scale_depth(0, w);
    if (kd > ght_envelope_depth(k->gate)) { (*lifts)++; return CAP_LIFT; }
    uint64_t env = ght_fp(kd);
    uint8_t b = (uint8_t)(rank % k->orbit);
    uint64_t cap = GHT_WIN / k->orbit;
    if (used[b] + env > cap) { (*rejects)++; return CAP_REJECT; }
    used[b] += env;
    *field += env;
    return CAP_ADMIT;
}

typedef struct {
    uint64_t field_slots, lifts, rejects, teardowns, forced, peak;
    uint32_t n_windows_ok, n_windows_fail;
} Metrics;

/* stream หนึ่งไฟล์ผ่าน chain — windowed (memory bounded) · returns 1 if lossless */
static int scan_stream(FILE *fp, uint64_t fsize, const Knobs *k, Metrics *m) {
    uint32_t win_chunks = WINDOW_BYTES / k->chunk;
    if (win_chunks < 1) win_chunks = 1;
    uint32_t win_bytes = win_chunks * k->chunk;

    uint8_t *orig = (uint8_t *)malloc(win_bytes);
    uint8_t *recon = (uint8_t *)malloc(win_bytes);
    uint8_t *lifted = (uint8_t *)malloc(win_chunks);
    uint64_t used[24] = {0};
    uint64_t total_chunks = (fsize + k->chunk - 1) / k->chunk;
    uint64_t placed = 0;
    int lossless = 1;

    while (placed < total_chunks) {
        uint64_t end = placed + win_chunks;
        if (end > total_chunks) end = total_chunks;
        uint64_t win_n = end - placed;
        /* read window bytes (last window partial) */
        uint64_t win_data = win_n * (uint64_t)k->chunk;
        if (win_data > fsize - (placed * (uint64_t)k->chunk))
            win_data = fsize - (placed * (uint64_t)k->chunk);
        if (win_data == 0) break;
        size_t rd = fread(orig, 1, (size_t)win_data, fp);
        if (rd != win_data) {
            fprintf(stderr, "  READ FAIL window placed=%I64u/%I64u win_data=%I64u rd=%I64u\n",
                    (unsigned long long)placed, (unsigned long long)total_chunks,
                    (unsigned long long)win_data, (unsigned long long)rd);
            lossless = 0; break;
        }

        /* rs capacity must be power-of-two (mask = cap-1) and ≥ all
           frozen sub-entries in this window: win_n chunks × sub-pieces
           (chunk > RS_MAX_DATA_SIZE 64KB → split).  Else the table fills
           → LRU evicts oldest before thaw (§15.66 bug: last partial
           window, capacity=312 non-pow2 + overcommit → evict chunk 12288). */
        uint32_t max_subs = (k->chunk + RS_MAX_DATA_SIZE - 1) / RS_MAX_DATA_SIZE;
        uint32_t need = (uint32_t)win_n * (max_subs ? max_subs : 1);
        uint32_t rs_cap = 1;
        while (rs_cap < need) rs_cap <<= 1;
        ResidualSpace rs;
        if (rs_init(&rs, rs_cap) != 0) {
            fprintf(stderr, "  RS INIT FAIL (cap=%u)\n", rs_cap);
            free(lifted); free(recon); free(orig); return 0;
        }
        memset(lifted, 0, win_chunks);

        for (uint64_t i = placed; i < end; i++) {
            uint8_t w = scale_w(k, i);
            uint64_t off = (i - placed) * (uint64_t)k->chunk;
            uint32_t len = (uint32_t)((i == total_chunks - 1)
                          ? (uint32_t)(fsize - i * (uint64_t)k->chunk) : k->chunk);
            if (len > win_data - off) len = (uint32_t)(win_data - off);
            int v = admit(k, used, w, i, &m->lifts, &m->rejects, &m->field_slots);
            if (v == CAP_LIFT) {
                /* freeze as 64KB sub-pieces (RS_MAX_DATA_SIZE) — bond
                   derived per sub via from_scale=sub → rdh_addr(block,sub)
                   collision-free, reversible (coordinate = address) */
                uint32_t n_sub = (len + RS_MAX_DATA_SIZE - 1) / RS_MAX_DATA_SIZE;
                uint32_t stored = 0;
                for (uint32_t s = 0; s < n_sub; s++) {
                    uint32_t sl = (len - s * RS_MAX_DATA_SIZE > RS_MAX_DATA_SIZE)
                                ? RS_MAX_DATA_SIZE : (len - s * RS_MAX_DATA_SIZE);
                    PoglsPiece p = ghost_piece((uint32_t)i, (uint8_t)s, w);
                    uint64_t bk = rs_freeze(&rs, &p, orig + off + s * RS_MAX_DATA_SIZE, sl, 0);
                    if (bk == RS_BOND_KEY_RESERVED) break;
                    stored++;
                }
                if (stored == n_sub) lifted[i - placed] = (uint8_t)n_sub;
                else m->forced++;
            }
            /* ADMIT/REJECT → pointer-home (data stays in source) */
        }
        if (rs.count > m->peak) m->peak = rs.count;

        /* verify window NOW + reconstruct */
        for (uint64_t i = placed; i < end; i++) {
            uint8_t w = scale_w(k, i);
            uint64_t off = (i - placed) * (uint64_t)k->chunk;
            uint32_t len = (uint32_t)((i == total_chunks - 1)
                          ? (uint32_t)(fsize - i * (uint64_t)k->chunk) : k->chunk);
            if (len > win_data - off) len = (uint32_t)(win_data - off);
            const uint8_t *src;
            if (lifted[i - placed]) {
                /* reassemble from sub-pieces (same derived bonds) */
                uint8_t *tmp = recon + off;
                uint32_t got = 0;
                for (uint32_t s = 0; s < lifted[i - placed]; s++) {
                    uint32_t out_sz = 0;
                    PoglsPiece p = ghost_piece((uint32_t)i, (uint8_t)s, w);
                    src = (const uint8_t *)rs_thaw(&rs, pogls_bond_key(&p), &out_sz);
                    if (!src) {
                        fprintf(stderr, "  THAW FAIL chunk=%I64u sub=%u\n",
                                (unsigned long long)i, s);
                        rs_free(&rs); free(lifted); free(recon); free(orig); return 0;
                    }
                    memcpy(tmp + got, src, out_sz);
                    got += out_sz;
                }
                if (got != len) {
                    fprintf(stderr, "  THAW SIZE chunk=%I64u got=%u len=%u\n",
                            (unsigned long long)i, got, len);
                    rs_free(&rs); free(lifted); free(recon); free(orig); return 0;
                }
            } else {
                src = orig + off;
                memcpy(recon + off, src, len);
            }
        }
        int wok = memcmp(recon, orig, (size_t)win_data) == 0;
        if (wok) m->n_windows_ok++; else { m->n_windows_fail++; lossless = 0; }

        rs_free(&rs);
        m->teardowns++;
        placed = end;
    }
    free(lifted); free(recon); free(orig);
    return lossless;
}

typedef struct { char path[1100]; uint64_t size; } FileEntry;

static void walk_dir(const char *dir, FileEntry *out, uint32_t cap, uint32_t *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < cap) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1120];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk_dir(full, out, cap, n); continue; }
        snprintf(out[*n].path, 1100, "%s", full);
        out[*n].size = (uint64_t)st.st_size;
        (*n)++;
    }
    closedir(d);
}

static int cmp_file(const void *a, const void *b) {
    return strcmp(((const FileEntry *)a)->path, ((const FileEntry *)b)->path);
}

static void print_knobs(const Knobs *k) {
    printf("  knobs: stride=%u offset=%u gate=%.2f (kmax=%u) orbit=%u chunk=%u\n",
           k->stride, k->offset, k->gate, (unsigned)ght_envelope_depth(k->gate),
           k->orbit, k->chunk);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Knobs k; knobs_default(&k);
    const char *root = NULL, *gguf = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stride") == 0 && i + 1 < argc) k.stride = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) k.offset = (uint8_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--gate") == 0 && i + 1 < argc) k.gate = atof(argv[++i]);
        else if (strcmp(argv[i], "--orbit") == 0 && i + 1 < argc) k.orbit = (uint8_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) k.chunk = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--gguf") == 0 && i + 1 < argc) gguf = argv[++i];
        else if (argv[i][0] != '-') root = argv[i];
    }

    Metrics agg = {0};
    uint64_t tot_bytes = 0;
    uint32_t n_ok = 0, n_fail = 0, n_skip = 0, n_files = 0;

    if (gguf) {
        FILE *fp = fopen(gguf, "rb");
        if (!fp) { printf("cannot open %s\n", gguf); return 1; }
        /* 64-bit size — ftell() เป็น 32-bit บน Windows → overflow ไฟล์ >2GB (§15.71) */
        if (_fseeki64(fp, 0, SEEK_END) != 0) { printf("seek end fail\n"); return 1; }
        uint64_t sz = (uint64_t)_ftelli64(fp);
        _fseeki64(fp, 0, SEEK_SET);
        printf("GGUF stream: %s (%I64u MB)\n", gguf, (unsigned long long)(sz >> 20));
        print_knobs(&k);
        Metrics m = {0};
        int ok = scan_stream(fp, sz, &k, &m);
        fclose(fp);
        tot_bytes = sz;
        n_files = 1;
        if (ok) n_ok++; else n_fail++;
        printf("  %6.1f MB  lossless=%s  field=%I64u slots  lift=%I64u  rej=%I64u  win=%I64u  (win ok %u / fail %u)\n",
               (double)sz / 1048576.0, ok ? "OK" : "FAIL",
               (unsigned long long)m.field_slots, (unsigned long long)m.lifts,
               (unsigned long long)m.rejects, (unsigned long long)m.teardowns,
               m.n_windows_ok, m.n_windows_fail);
        agg = m;
    } else {
        if (!root) root = "F:/notebookLM";
        printf("Folder: %s\n", root);
        print_knobs(&k);
        FileEntry *files = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
        uint32_t nf = 0;
        walk_dir(root, files, MAX_FILES, &nf);
        qsort(files, nf, sizeof(FileEntry), cmp_file);
        for (uint32_t f = 0; f < nf; f++) {
            FILE *fp = fopen(files[f].path, "rb");
            if (!fp) { n_skip++; continue; }
            Metrics m = {0};
            int ok = scan_stream(fp, files[f].size, &k, &m);
            fclose(fp);
            if (ok) n_ok++; else n_fail++;
            tot_bytes += files[f].size;
            n_files++;
            agg.field_slots += m.field_slots; agg.lifts += m.lifts;
            agg.rejects += m.rejects; agg.teardowns += m.teardowns;
            agg.forced += m.forced;
            if (m.peak > agg.peak) agg.peak = m.peak;
            if ((f + 1) % 250 == 0) printf("  ... %u/%u files\n", f + 1, nf);
        }
        free(files);
    }

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("AGGREGATE (%u files, %I64u MB)\n", n_files, (unsigned long long)(tot_bytes >> 20));
    printf("  lossless (byte-for-byte): %u ok / %u (fail %u, skip %u)\n", n_ok, n_ok + n_fail, n_fail, n_skip);
    printf("  field slots: %I64u (~%I64u windows of 20736)\n",
           (unsigned long long)agg.field_slots,
           (unsigned long long)((agg.field_slots + GHT_WIN - 1) / GHT_WIN));
    printf("  lifted chunks → residual_space: %I64u | rejects: %I64u\n",
           (unsigned long long)agg.lifts, (unsigned long long)agg.rejects);
    printf("  stream windows: %I64u | forced evictions: %I64u | peak rs.count: %I64u\n",
           (unsigned long long)agg.teardowns, (unsigned long long)agg.forced,
           (unsigned long long)agg.peak);
    printf("════════════════════════════════════════════════════════════\n");
    return n_fail ? 1 : 0;
}
