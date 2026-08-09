/* breathing_fs_cli.c — Breathing FS CLI Tool */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "breathing_fs.h"

static int save_image(const char *path, const BreathingFS *fs) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("save"); return -1; }
    fwrite(&fs->magic, 4, 1, f);
    fwrite(&fs->version, 4, 1, f);
    fwrite(&fs->n_files, 4, 1, f);
    fwrite(&fs->n_blocks_used, 4, 1, f);
    fwrite(&fs->total_bytes, 4, 1, f);
    fwrite(&fs->seeker, sizeof(BreathingSeeker), 1, f);
    fwrite(fs->files, sizeof(BFSFileEntry), BFS_MAX_FILES, f);
    fwrite(fs->block_owner, 4, BFS_BLOCKS, f);
    fwrite(fs->block_meta, sizeof(BFSBlockMeta), BFS_BLOCKS, f);
    fwrite(fs->block_encoded_size, 2, BFS_BLOCKS, f);
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        if (fs->block_encoded_size[i] > 0)
            fwrite(fs->block_encoded[i], 1, fs->block_encoded_size[i], f);
    fclose(f);
    return 0;
}

static int load_image(const char *path, BreathingFS *fs) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("load"); return -1; }
    fread(&fs->magic, 4, 1, f);
    fread(&fs->version, 4, 1, f);
    fread(&fs->n_files, 4, 1, f);
    fread(&fs->n_blocks_used, 4, 1, f);
    fread(&fs->total_bytes, 4, 1, f);
    if (fs->magic != BFS_MAGIC) { fclose(f); return -1; }
    fread(&fs->seeker, sizeof(BreathingSeeker), 1, f);
    fread(fs->files, sizeof(BFSFileEntry), BFS_MAX_FILES, f);
    fread(fs->block_owner, 4, BFS_BLOCKS, f);
    fread(fs->block_meta, sizeof(BFSBlockMeta), BFS_BLOCKS, f);
    fread(fs->block_encoded_size, 2, BFS_BLOCKS, f);
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        if (fs->block_encoded_size[i] > 0)
            fread(fs->block_encoded[i], 1, fs->block_encoded_size[i], f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Breathing FS — create/write/get/list/info/scale/home/chk/demo\n");
        return 1;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        const char *p = argc > 2 ? argv[2] : "bfs.img";
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        bfs_init(fs);
        int r = save_image(p, fs); free(fs);
        if (r == 0) printf("Created: %s\n", p);
        return r;
    }

    if (strcmp(cmd, "write") == 0 && argc >= 4) {
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(argv[2], fs) != 0) { free(fs); return -1; }
        FILE *f = fopen(argv[3], "rb");
        if (!f) { perror(argv[3]); free(fs); return -1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        int8_t *data = (int8_t *)malloc(sz); fread(data, 1, sz, f); fclose(f);
        const char *nm = strrchr(argv[3], '/');
        if (!nm) nm = strrchr(argv[3], '\\');
        if (nm) nm++; else nm = argv[3];
        int rc = bfs_write(fs, nm, data, (uint32_t)sz); free(data);
        if (rc == 0) { save_image(argv[2], fs); printf("Wrote: %s (%ld bytes)\n", nm, sz); }
        else printf("Write error: %d\n", rc);
        free(fs); return rc;
    }

    if (strcmp(cmd, "get") == 0 && argc >= 5) {
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(argv[2], fs) != 0) { free(fs); return -1; }
        int8_t *data = (int8_t *)calloc(1, 20736);
        uint32_t actual = 0;
        int rc = bfs_read(fs, argv[3], data, 20736, &actual);
        if (rc == 0) {
            FILE *out = fopen(argv[4], "wb");
            fwrite(data, 1, actual, out); fclose(out);
            printf("Got: %s -> %s (%u bytes)\n", argv[3], argv[4], actual);
        } else printf("Get error: %d\n", rc);
        free(data); free(fs); return rc;
    }

    if (strcmp(cmd, "list") == 0) {
        const char *p = argc > 2 ? argv[2] : "bfs.img";
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(p, fs) != 0) { free(fs); return -1; }
        bfs_print_dir(fs); free(fs); return 0;
    }

    if (strcmp(cmd, "info") == 0) {
        const char *p = argc > 2 ? argv[2] : "bfs.img";
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(p, fs) != 0) { free(fs); return -1; }
        seeker_print(&fs->seeker);
        bfs_delta_stats(fs);
        printf("Files: %u | Blocks: %u/%u\n", fs->n_files, fs->n_blocks_used, BFS_BLOCKS);
        free(fs); return 0;
    }

    if (strcmp(cmd, "scale") == 0 && argc >= 4) {
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(argv[2], fs) != 0) { free(fs); return -1; }
        printf("Before: "); seeker_print(&fs->seeker);
        bfs_move_seeker(fs, atof(argv[3]));
        printf("After:  "); seeker_print(&fs->seeker);
        bfs_delta_stats(fs);
        save_image(argv[2], fs); free(fs); return 0;
    }

    if (strcmp(cmd, "home") == 0) {
        const char *p = argc > 2 ? argv[2] : "bfs.img";
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(p, fs) != 0) { free(fs); return -1; }
        printf("Before: "); seeker_print(&fs->seeker);
        bfs_go_home(fs);
        printf("After:  "); seeker_print(&fs->seeker);
        save_image(p, fs); free(fs); return 0;
    }

    if (strcmp(cmd, "chk") == 0 && argc >= 5) {
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        if (load_image(argv[2], fs) != 0) { free(fs); return -1; }
        FILE *f = fopen(argv[4], "rb");
        if (!f) { perror(argv[4]); free(fs); return -1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        int8_t *data = (int8_t *)malloc(sz); fread(data, 1, sz, f); fclose(f);
        /* Inline verify: read from image, compare to original */
        /* recon must be >= total_blocks * BFS_SLOTS_BLOCK for dyn_decode */
        uint32_t nbl = (uint32_t)((sz + BFS_SLOTS_BLOCK - 1) / BFS_SLOTS_BLOCK);
        int8_t *recon = (int8_t *)calloc(1, (size_t)nbl * BFS_SLOTS_BLOCK);
        uint32_t actual = 0;
        int rc = bfs_read(fs, argv[3], recon, (uint32_t)sz, &actual);
        int match = 0;
        if (rc == 0 && actual == (uint32_t)sz)
            match = (memcmp(data, recon, (size_t)sz) == 0);
        free(recon); free(data); free(fs);
        printf("%s: %s\n", argv[3], match ? "LOSSLESS" : "FAIL");
        return match ? 0 : 1;
    }

    if (strcmp(cmd, "demo") == 0) {
        printf("=== Breathing FS Demo ===\n\n");
        BreathingFS *fs = (BreathingFS *)calloc(1, sizeof(BreathingFS));
        bfs_init(fs);
        int8_t d1[144], d2[288], d3[144];
        memset(d1, 0, sizeof(d1)); d1[50] = 42;
        for (int i = 0; i < 288; i++) d2[i] = (int8_t)(i % 7);
        srand(42);
        for (int i = 0; i < 144; i++) d3[i] = (int8_t)(rand() % 256 - 128);
        bfs_write(fs, "sparse.bin", d1, 144);
        fs->seeker.current_pos = 500; fs->seeker.home_pos = 500;
        bfs_write(fs, "repeated.bin", d2, 288);
        fs->seeker.current_pos = 1200; fs->seeker.home_pos = 1200;
        bfs_write(fs, "random.bin", d3, 144);
        bfs_print_dir(fs);
        printf("\nBreathing cycle:\n");
        double sc[] = {0.5, 0.25, 0.1, 0.5, 1.0};
        for (int i = 0; i < 5; i++) {
            bfs_move_seeker(fs, sc[i]);
            printf("  scale=%.2f delta=[%d,%d,%d] %s\n", fs->seeker.scale,
                   fs->block_meta[0].delta, fs->block_meta[1].delta, fs->block_meta[3].delta,
                   fs->seeker.is_hyperbolic ? "HYPERBOLIC" : "");
        }
        bfs_go_home(fs);
        printf("\nVerify:\n");
        printf("  sparse:   %s\n", bfs_verify_file(fs, "sparse.bin", d1, 144) == 0 ? "LOSSLESS" : "FAIL");
        printf("  repeated: %s\n", bfs_verify_file(fs, "repeated.bin", d2, 288) == 0 ? "LOSSLESS" : "FAIL");
        printf("  random:   %s\n", bfs_verify_file(fs, "random.bin", d3, 144) == 0 ? "LOSSLESS" : "FAIL");
        uint32_t enc = 0;
        for (uint32_t b = 0; b < BFS_BLOCKS; b++)
            if (fs->block_owner[b] != 0xFFFFFFFF) enc += fs->block_encoded_size[b];
        printf("\nRatio: %u/%u = %.4f\n", enc, fs->total_bytes, (float)enc / (float)fs->total_bytes);
        free(fs); return 0;
    }

    printf("Unknown: %s\n", cmd);
    return 1;
}
