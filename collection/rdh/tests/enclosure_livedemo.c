/*
 * enclosure_livedemo.c — Real-data demo of Entropy Enclosure
 *
 * Scans all .h files in core/ and collection/, classifies every
 * 48-byte block through the Metatron Hilbert+Peano maze, tests
 * different field scales, and integrates with frame seek.
 *
 * Build:
 *   gcc -I../collection/dgls/geo/include enclosure_livedemo.c -o enclosure_livedemo
 *
 * Run:
 *   enclosure_livedemo <path_to_fgls_root>
 *
 * Example:
 *   enclosure_livedemo I:/FGLS_new
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "gls_enclosure.h"

#define MAX_FILES  256
#define MAX_PATH   1024
#define MAX_DATA   (4 * 1024 * 1024)  /* 4 MB per file max */

/* ──────────────────────────────────────────────────────────────
 * File scanning
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    char  path[MAX_PATH];
    char  name[256];
    long  size;
    uint8_t *data;
} FileInfo;

static int scan_dir(const char *dir, const char *ext, FileInfo *files, int *count) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < MAX_FILES) {
        if (entry->d_name[0] == '.') continue;  /* skip . and .. */

        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            scan_dir(full, ext, files, count);
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;
        if (ext) {
            const char *dot = strrchr(entry->d_name, '.');
            if (!dot || strcmp(dot, ext) != 0) continue;
        }
        if (st.st_size < 48) continue;

        FileInfo *f = &files[*count];
        strncpy(f->path, full, MAX_PATH - 1);
        strncpy(f->name, entry->d_name, 255);
        f->size = st.st_size;

        /* Read file */
        f->data = (uint8_t *)malloc((size_t)(st.st_size < MAX_DATA ? st.st_size : MAX_DATA));
        if (!f->data) { closedir(d); return -1; }
        FILE *fp = fopen(full, "rb");
        if (!fp) { free(f->data); closedir(d); return -1; }
        long read_sz = (st.st_size < MAX_DATA) ? st.st_size : MAX_DATA;
        f->size = (long)fread(f->data, 1, (size_t)read_sz, fp);
        fclose(fp);

        (*count)++;
    }
    closedir(d);
    return 0;
}

/* ──────────────────────────────────────────────────────────────
 * Results
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    long   size;
    int    blocks;
    float  avg_matches;     /* scale=1 */
    float  strong_pct;
    float  weak_pct;
    float  chaos_pct;
    int    best_scale;      /* scale with highest avg matches */
    float  best_avg;
    int    scale_improved;  /* 1 if scale>1 improved classification */
} FileResult;

static int result_count = 0;
static FileResult results[MAX_FILES];

/* ──────────────────────────────────────────────────────────────
 * Classify a file at a given scale
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    int    total_blocks;
    int    strong;
    int    weak;
    int    chaos;
    float  avg_matches;
} ScaleResult;

static ScaleResult classify_file(const uint8_t *data, long size, uint32_t scale) {
    ScaleResult r = {0, 0, 0, 0, 0.0f};
    uint32_t extent = scale * ENC_BLOCK;

    if (size < (long)extent) return r;

    int n_extents = (int)(size / (int)extent);
    if (n_extents > 200) n_extents = 200;  /* sample cap */

    int total_m = 0;
    for (int i = 0; i < n_extents; i++) {
        const uint8_t *ext = data + i * extent;
        int n_blocks = (int)(extent / ENC_BLOCK);
        for (int b = 0; b < n_blocks; b++) {
            uint32_t m = enc_classify_block(ext + b * ENC_BLOCK);
            total_m += m;
            r.total_blocks++;
            if (m >= 38)      r.strong++;
            else if (m >= 14) r.weak++;
            else              r.chaos++;
        }
    }

    if (r.total_blocks > 0)
        r.avg_matches = (float)total_m / (float)r.total_blocks;

    return r;
}

/* ──────────────────────────────────────────────────────────────
 * Frame seek demo
 * ──────────────────────────────────────────────────────────── */

static void demo_frame_seek(const FileInfo *f, uint32_t scale) {
    printf("\n  ── Frame Seek on %s (scale=%u) ──\n", f->name, scale);

    uint32_t extent = scale * ENC_BLOCK;
    int n_frames = (int)(f->size / (int)extent);
    if (n_frames > 12) n_frames = 12;
    if (n_frames < 1) return;

    /* Init enclosure from first 48 bytes */
    EntropyEnclosure enc;
    uint32_t seed = 0;
    for (int i = 0; i < 48 && i < f->size; i++)
        seed = (seed << 1) ^ f->data[i];
    enc_init(&enc, seed, 0, scale);

    printf("  \n  %-6s  %-8s  %-10s  %-8s  %-6s  %-10s\n",
           "Frame", "NodeID", "Tower,Cell", "Timeline", "Matches", "Shell");
    printf("  %s\n", "------  --------  ----------  --------  -------  ----------");

    int total_matches = 0;
    for (int fi = 0; fi < n_frames; fi++) {
        uint32_t t = (uint32_t)((uint64_t)fi * ENC_CLOCK_STRIDE % ENC_CLOCK);
        EncFrame frame = enc_skip(&enc, (uint32_t)fi);

        /* Classify the data at this frame position */
        int frame_offset = fi * (int)extent;
        ScaleResult sr = classify_file(f->data + frame_offset,
                                       f->size - frame_offset, scale);

        EncShellClass shell = enc_shell_classify(f->data + frame_offset);

        const char *shell_s = "STRONG";
        if (shell == ENC_SHELL_WEAK) shell_s = "WEAK";
        else if (shell == ENC_SHELL_CHAOS) shell_s = "CHAOS";

        printf("  %-6d  %-8u  %-3u,%-6u  %-8u  %-7.1f  %-10s\n",
               fi, frame.node_id, frame.tower, frame.cell,
               t, sr.avg_matches, shell_s);
        total_matches += (int)sr.avg_matches;
    }

    if (n_frames > 0) {
        printf("  ─────────────────────────────────────────────────\n");
        printf("  Avg/frame: %.1f matches\n",
               (float)total_matches / (float)n_frames);
    }
}

/* ──────────────────────────────────────────────────────────────
 * Main
 * ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: enclosure_livedemo <fgls_root_dir>\n");
        return 1;
    }

    const char *root = argv[1];

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║      ENTROPY ENCLOSURE — LIVE REAL-DATA DEMO            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── Scan .h files ── */
    char core_dir[MAX_PATH];
    snprintf(core_dir, sizeof(core_dir), "%s/core", root);
    char coll_dir[MAX_PATH];
    snprintf(coll_dir, sizeof(coll_dir), "%s/collection", root);

    FileInfo files[MAX_FILES];
    int n_files = 0;

    printf("Scanning %s ...\n", core_dir);
    scan_dir(core_dir, ".h", files, &n_files);
    printf("  Found %d .h files in core/\n", n_files);

    printf("Scanning %s ...\n", coll_dir);
    scan_dir(coll_dir, ".h", files, &n_files);
    printf("  Found %d .h files total\n", n_files);

    if (n_files == 0) {
        printf("\nNo files found. Try a different path.\n");
        return 0;
    }

    /* ── Per-file classification at multiple scales ── */
    int scales[] = {1, 2, 4, 8, 16, 32, 64, 144};
    int n_scales = 8;

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║      BLOCK-LEVEL CLASSIFICATION (scale=1)                ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("%-35s %8s %7s %9s %8s %8s %8s\n",
           "File", "Size", "Blocks", "AvgMatch", "STRONG%", "WEAK%", "CHAOS%");
    printf("%s\n", "──────────────────────────────────────────────────────────────────────────────");

    for (int fi = 0; fi < n_files && fi < 30; fi++) {
        FileInfo *f = &files[fi];
        ScaleResult r = classify_file(f->data, f->size, 1);
        FileResult *res = &results[result_count++];
        res->name = f->name;
        res->size = f->size;
        res->blocks = r.total_blocks;
        res->avg_matches = r.avg_matches;
        res->strong_pct = r.total_blocks ? (float)r.strong / r.total_blocks * 100 : 0;
        res->weak_pct   = r.total_blocks ? (float)r.weak   / r.total_blocks * 100 : 0;
        res->chaos_pct  = r.total_blocks ? (float)r.chaos  / r.total_blocks * 100 : 0;
        res->best_scale = 1;
        res->best_avg   = r.avg_matches;
        res->scale_improved = 0;

        printf("%-35s %8ld %7d %9.1f %8.1f %8.1f %8.1f\n",
               f->name, f->size, r.total_blocks,
               r.avg_matches,
               (double)res->strong_pct,
               (double)res->weak_pct,
               (double)res->chaos_pct);
    }

    /* ── Scale sweep on top 3 files ── */
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║      SCALE SWEEP — top 3 files at all scales            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Sort by avg_matches descending */
    for (int i = 0; i < result_count; i++) {
        for (int j = i + 1; j < result_count; j++) {
            if (results[j].avg_matches > results[i].avg_matches) {
                FileResult t = results[i]; results[i] = results[j]; results[j] = t;
                FileInfo ft = files[i]; files[i] = files[j]; files[j] = ft;
            }
        }
    }

    /* Print scale sweep for top 3 */
    for (int fi = 0; fi < 3 && fi < result_count; fi++) {
        FileInfo *f = &files[fi];
        printf("  [%s]\n", f->name);
        printf("  %-7s", "Scale");
        for (int si = 0; si < n_scales; si++)
            printf(" %7d", scales[si]);
        printf("\n  %-7s", "AvgMatch");
        for (int si = 0; si < n_scales; si++) {
            ScaleResult sr = classify_file(f->data, f->size, (uint32_t)scales[si]);
            printf(" %7.1f", sr.avg_matches);
        }
        printf("\n\n");
    }

    /* ── Scale sweep for bottom 3 (chaos files) ── */
    printf("  [Bottom 3 files (highest chaos) — does scale help?]\n");
    for (int fi = result_count - 1; fi >= result_count - 3 && fi >= 0; fi--) {
        FileInfo *f = &files[fi];
        printf("  %-35s scale=1: %5.1f  scale=4: %5.1f  scale=16: %5.1f  scale=144: %5.1f\n",
               f->name,
               (double)classify_file(f->data, f->size, 1).avg_matches,
               (double)classify_file(f->data, f->size, 4).avg_matches,
               (double)classify_file(f->data, f->size, 16).avg_matches,
               (double)classify_file(f->data, f->size, 144).avg_matches);
    }

    /* ── Frame Seek integration ── */
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║      FRAME SEEK INTEGRATION (scale=4)                    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* Pick a mid-range file for frame seek demo */
    int mid_idx = result_count / 2;
    if (mid_idx >= n_files) mid_idx = n_files - 1;
    demo_frame_seek(&files[mid_idx], 4);

    /* Also demo on best and worst */
    if (result_count > 0)
        demo_frame_seek(&files[0], 4);
    if (result_count > 1)
        demo_frame_seek(&files[result_count - 1], 4);

    /* ── Summary ── */
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║      SUMMARY                                            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    int strong_files = 0, weak_files = 0, chaos_files = 0;
    for (int i = 0; i < result_count; i++) {
        if (results[i].avg_matches >= 38) strong_files++;
        else if (results[i].avg_matches >= 14) weak_files++;
        else chaos_files++;
    }

    printf("  Files tested:  %d\n", result_count);
    printf("  STRONG (≥38):  %d (%.0f%%)\n",
           strong_files, (double)strong_files / result_count * 100);
    printf("  WEAK   (≥14):  %d (%.0f%%)\n",
           weak_files, (double)weak_files / result_count * 100);
    printf("  CHAOS  (<14):  %d (%.0f%%)\n",
           chaos_files, (double)chaos_files / result_count * 100);
    printf("\n  Field scaling: 1 block = 48 bytes (atomic Hilbert unit)\n");
    printf("                 12-frame cycle = %d bytes at scale=1\n", 12 * 48);
    printf("                 Total field = %d KB at scale=1\n",
           (ENC_FULL * 48) / 1024);

    /* Cleanup */
    for (int i = 0; i < n_files; i++) {
        if (files[i].data) free(files[i].data);
    }

    printf("\n═══ DONE ═══\n");
    return 0;
}
