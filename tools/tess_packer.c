/* tools/tess_packer.c — .tess Multi-Capo Packer (single-file container)
 *
 * Packs all .tess capo files from a directory into one .tesspack file.
 * Unpacks .tesspack back to individual .tess files.
 * Lists contents of a .tesspack file.
 *
 * Format (.tesspack):
 *   [0..3]   magic: "TPAK" (0x5450414B)
 *   [4..7]   version: 1
 *   [8..11]  n_capos (uint32_t)
 *   [12..15] index_offset (uint32_t) — byte offset to index table
 *   [16..63] reserved (zeroed)
 *   [64...]  capo data (concatenated raw .tess bytes)
 *   [index_offset...]  index table:
 *     per capo:
 *       uint32_t name_len
 *       char     name[name_len]  (no null terminator)
 *       uint32_t capo_id
 *       uint64_t offset          (byte offset from file start)
 *       uint32_t size            (byte size of this capo)
 *
 * BUILD: gcc -O2 -Wall -o tess_packer tools/tess_packer.c -lm
 * USAGE:
 *   tess_packer pack   <dir> <output.tesspack>
 *   tess_packer unpack <input.tesspack> <dir>
 *   tess_packer info   <input.tesspack>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#define TPAK_MAGIC   0x5450414Bu  /* "TPAK" little-endian */
#define TPAK_VERSION 1u

typedef struct {
    char     name[256];
    uint32_t capo_id;
    uint64_t offset;
    uint32_t size;
} CapoEntry;

/* extract tensor name and capo_id from filename like "blk.0.ffn_down_exps.weight_capo3.tess" */
static int parse_capo_name(const char *fname, char *tensor_name, uint32_t *capo_id) {
    /* find last _capoN.tess */
    const char *p = strstr(fname, "_capo");
    if (!p) {
        /* single capo: name.tess */
        const char *dot = strrchr(fname, '.');
        if (!dot) return -1;
        size_t len = (size_t)(dot - fname);
        if (len >= 256) len = 255;
        memcpy(tensor_name, fname, len);
        tensor_name[len] = 0;
        *capo_id = 0;
        return 0;
    }
    size_t tlen = (size_t)(p - fname);
    if (tlen >= 256) tlen = 255;
    memcpy(tensor_name, fname, tlen);
    tensor_name[tlen] = 0;
    *capo_id = (uint32_t)atoi(p + 5);
    return 0;
}

static int cmp_entry(const void *a, const void *b) {
    const CapoEntry *ea = (const CapoEntry *)a;
    const CapoEntry *eb = (const CapoEntry *)b;
    int c = strcmp(ea->name, eb->name);
    if (c != 0) return c;
    if (ea->capo_id < eb->capo_id) return -1;
    if (ea->capo_id > eb->capo_id) return 1;
    return 0;
}

static int do_pack(const char *dir, const char *outpath) {
    /* scan for .tess files */
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "Cannot open dir: %s\n", dir); return 1; }

    uint32_t cap = 1024;
    uint32_t n = 0;
    CapoEntry *entries = (CapoEntry *)malloc(cap * sizeof(CapoEntry));

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *fname = ent->d_name;
        size_t flen = strlen(fname);
        if (flen < 6 || strcmp(fname + flen - 5, ".tess") != 0) continue;
        if (n >= cap) { cap *= 2; entries = (CapoEntry *)realloc(entries, cap * sizeof(CapoEntry)); }
        if (parse_capo_name(fname, entries[n].name, &entries[n].capo_id) != 0) continue;
        entries[n].offset = 0;
        entries[n].size = 0;
        n++;
    }
    closedir(d);

    if (n == 0) { fprintf(stderr, "No .tess files found in %s\n", dir); free(entries); return 1; }
    qsort(entries, n, sizeof(CapoEntry), cmp_entry);

    printf("Packing %u capos from %s\n", n, dir);

    /* pass 1: compute offsets (skip 64-byte header) */
    uint64_t offset = 64;
    for (uint32_t i = 0; i < n; i++) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s_capo%u.tess", dir, entries[i].name, entries[i].capo_id);
        struct stat st;
        if (stat(fpath, &st) != 0) {
            /* try without _capoN suffix (single-capo tensor) */
            snprintf(fpath, sizeof(fpath), "%s/%s.tess", dir, entries[i].name);
            if (stat(fpath, &st) != 0) { fprintf(stderr, "Missing: %s\n", fpath); continue; }
        }
        entries[i].offset = offset;
        entries[i].size = (uint32_t)st.st_size;
        offset += entries[i].size;
    }

    /* index starts after all capo data */
    uint32_t index_offset = (uint32_t)offset;

    /* pass 2: write file */
    FILE *fout = fopen(outpath, "wb");
    if (!fout) { fprintf(stderr, "Cannot create: %s\n", outpath); free(entries); return 1; }

    /* header */
    uint32_t header[16] = {0};
    header[0] = TPAK_MAGIC;
    header[1] = TPAK_VERSION;
    header[2] = n;
    header[3] = index_offset;
    fwrite(header, 1, 64, fout);

    /* capo data */
    uint8_t *copy_buf = (uint8_t *)malloc(1024 * 1024);
    for (uint32_t i = 0; i < n; i++) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s_capo%u.tess", dir, entries[i].name, entries[i].capo_id);
        FILE *fin = fopen(fpath, "rb");
        if (!fin) {
            snprintf(fpath, sizeof(fpath), "%s/%s.tess", dir, entries[i].name);
            fin = fopen(fpath, "rb");
        }
        if (!fin) continue;
        uint32_t rem = entries[i].size;
        while (rem > 0) {
            uint32_t chunk = rem > 1024*1024 ? 1024*1024 : rem;
            size_t rd = fread(copy_buf, 1, chunk, fin);
            if (rd == 0) break;
            fwrite(copy_buf, 1, rd, fout);
            rem -= (uint32_t)rd;
        }
        fclose(fin);
    }

    /* index table */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t name_len = (uint8_t)strlen(entries[i].name);
        fwrite(&name_len, 1, 1, fout);
        fwrite(entries[i].name, 1, name_len, fout);
        fwrite(&entries[i].capo_id, 4, 1, fout);
        fwrite(&entries[i].offset, 8, 1, fout);
        fwrite(&entries[i].size, 4, 1, fout);
    }

    fclose(fout);
    free(copy_buf);
    free(entries);

    uint64_t file_sz = offset + (uint64_t)(16 + n * (4 + 256 + 4 + 8 + 4));  /* approximate */
    printf("Packed: %s (%u capos)\n", outpath, n);
    return 0;
}

static int do_unpack(const char *inpath, const char *dir) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open: %s\n", inpath); return 1; }

    uint32_t header[16];
    if (fread(header, 1, 64, fin) != 64 || header[0] != TPAK_MAGIC) {
        fprintf(stderr, "Not a .tesspack file\n"); fclose(fin); return 1;
    }
    uint32_t n_capos = header[2];
    uint32_t index_offset = header[3];

    printf("Unpacking %u capos from %s to %s\n", n_capos, inpath, dir);
    mkdir(dir);

    /* read index */
    fseek(fin, index_offset, SEEK_SET);
    uint8_t *copy_buf = (uint8_t *)malloc(1024 * 1024);

    for (uint32_t i = 0; i < n_capos; i++) {
        long index_pos = ftell(fin);
        uint8_t name_len;
        if (fread(&name_len, 1, 1, fin) != 1) break;
        if (name_len == 0) break;
        char name[256];
        if (fread(name, 1, name_len, fin) != name_len) break;
        name[name_len] = 0;
        uint32_t capo_id;
        uint64_t offset;
        uint32_t size;
        if (fread(&capo_id, 4, 1, fin) != 1) break;
        if (fread(&offset, 8, 1, fin) != 1) break;
        if (fread(&size, 4, 1, fin) != 1) break;

        /* extract capo data */
        char outpath[512];
        snprintf(outpath, sizeof(outpath), "%s/%s_capo%u.tess", dir, name, capo_id);
        FILE *fout = fopen(outpath, "wb");
        if (!fout) { fseek(fin, index_pos + 4 + name_len + 4 + 8 + 4, SEEK_SET); continue; }

        fseek(fin, (long)offset, SEEK_SET);
        uint32_t rem = size;
        while (rem > 0) {
            uint32_t chunk = rem > 1024*1024 ? 1024*1024 : rem;
            size_t rd = fread(copy_buf, 1, chunk, fin);
            if (rd == 0) break;
            fwrite(copy_buf, 1, rd, fout);
            rem -= (uint32_t)rd;
        }
        fclose(fout);

        /* restore position to next index entry */
        fseek(fin, index_pos + 4 + name_len + 4 + 8 + 4, SEEK_SET);
    }

    free(copy_buf);
    fclose(fin);
    printf("Done: %u capos extracted\n", n_capos);
    return 0;
}

static int do_info(const char *inpath) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open: %s\n", inpath); return 1; }

    uint32_t header[16];
    if (fread(header, 1, 64, fin) != 64 || header[0] != TPAK_MAGIC) {
        fprintf(stderr, "Not a .tesspack file\n"); fclose(fin); return 1;
    }
    uint32_t n_capos = header[2];
    uint32_t index_offset = header[3];

    /* get file size */
    fseek(fin, 0, SEEK_END);
    long file_sz = ftell(fin);

    printf("── .tesspack: %s ──\n", inpath);
    printf("  file size:   %ld bytes\n", file_sz);
    printf("  capos:       %u\n", n_capos);
    printf("  data region: 0..%u (%u bytes)\n", index_offset, index_offset);
    printf("  index at:    %u\n", index_offset);

    /* read index and summarize by tensor */
    fseek(fin, index_offset, SEEK_SET);
    uint32_t n_tensors = 0;
    uint64_t total_data = 0;
    char last_name[256] = {0};
    uint32_t last_n_capos = 0;
    uint32_t last_first_capo = 0;

    for (uint32_t i = 0; i < n_capos; i++) {
        uint8_t name_len;
        if (fread(&name_len, 1, 1, fin) != 1) break;
        if (name_len == 0) break;
        char name[256];
        if (fread(name, 1, name_len, fin) != name_len) break;
        name[name_len] = 0;
        uint32_t capo_id;
        uint64_t offset;
        uint32_t size;
        if (fread(&capo_id, 4, 1, fin) != 1) break;
        if (fread(&offset, 8, 1, fin) != 1) break;
        if (fread(&size, 4, 1, fin) != 1) break;

        total_data += size;

        if (strcmp(name, last_name) != 0) {
            if (last_name[0]) {
                printf("  [%3u] %-50s  %u capos\n", n_tensors - last_n_capos, last_name, last_n_capos);
            }
            n_tensors++;
            last_n_capos = 0;
            strncpy(last_name, name, 255);
        }
        last_n_capos++;
    }
    if (last_name[0]) {
        printf("  [%3u] %-50s  %u capos\n", n_tensors - last_n_capos, last_name, last_n_capos);
    }

    printf("\n  Total: %u tensors, %u capos, %.1f MB data\n",
           n_tensors, n_capos, (double)total_data / (1024.0 * 1024.0));
    fclose(fin);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: tess_packer <cmd> [args]\n");
        fprintf(stderr, "  pack   <dir> <output.tesspack>\n");
        fprintf(stderr, "  unpack <input.tesspack> <dir>\n");
        fprintf(stderr, "  info   <input.tesspack>\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "pack") == 0) {
        if (argc < 4) { fprintf(stderr, "pack needs <dir> <output>\n"); return 1; }
        return do_pack(argv[2], argv[3]);
    }
    if (strcmp(cmd, "unpack") == 0) {
        if (argc < 4) { fprintf(stderr, "unpack needs <input> <dir>\n"); return 1; }
        return do_unpack(argv[2], argv[3]);
    }
    if (strcmp(cmd, "info") == 0) return do_info(argv[2]);
    fprintf(stderr, "Unknown: %s\n", cmd);
    return 1;
}
