/* mdim_cli.c — GeoFS MDIM Multidimensional Native Volume CLI
 *   create    <vol>              new volume (20736 slots × 64 B)
 *   summon    <vol> <name> <file> copy a real file into the volume
 *   get       <vol> <name> <out> extract a file
 *   list      <vol>              list files with geometric info
 *   info      <vol>              volume + timeline state
 *   view      <vol> <flat>       a slot through all four views
 *   history   <vol> <name>       every timeline version of a file
 *   unsummon  <vol> <name>       remove a file (tombstone)
 *   mmap      <vol> <name> <file> summon via mmap'd volume (zero-copy)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_mdim.h"

static int cmd_create(const char *path) {
    MdimVolume v;
    if (mdim_volume_init(&v, NULL) != MDIM_OK) { printf("init error\n"); return 1; }
    int rc = mdim_volume_save(&v, path);
    if (rc == MDIM_OK)
        printf("Created: %s — %u slots × 64B = %u B | journal %u slots | data starts slot %u\n",
               path, MDIM_SLOTS, MDIM_VOL_BYTES, MDIM_JRNL_SLOTS, MDIM_DATA_START);
    mdim_volume_free(&v);
    return rc == MDIM_OK ? 0 : 1;
}

static int cmd_summon(const char *vol, const char *name, const char *file, int mmap_mode) {
    MdimVolume v;
    int rc = mmap_mode ? mdim_volume_mmap_open(vol, &v) : mdim_volume_load(&v, vol);
    if (rc != MDIM_OK) { printf("open error: %d\n", rc); return 1; }

    FILE *f = fopen(file, "rb");
    if (!f) { perror(file); goto fail; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz > (long)MDIM_MAX_FILE_BYTES) {
        printf("file too big: %ld > %u B (volume capacity)\n", sz, MDIM_MAX_FILE_BYTES);
        fclose(f); goto fail;
    }
    uint8_t *data = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (sz) { if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(data); goto fail; } }
    fclose(f);

    int err = MDIM_OK;
    uint32_t entry = mdim_summon(&v, name, data, (uint32_t)sz, &err);
    free(data);
    if (entry == MDIM_FRAME_NONE) { printf("summon error: %d\n", err); goto fail; }

    if (mmap_mode) mdim_volume_mmap_flush(&v);
    else { rc = mdim_volume_save(&v, vol); if (rc != MDIM_OK) printf("save error: %d\n", rc); }
    MdimSlot *s = mdim_slot(&v, entry);
    printf("Summoned %s (%ld B) → slot %u (frame %u, %u run%s)\n",
           name, sz, entry, s->home_frame, s->n_runs, s->n_runs == 1 ? "" : "s");
    mdim_volume_free(&v);
    return 0;
fail:
    mdim_volume_free(&v);
    return 1;
}

static int cmd_get(const char *vol, const char *name, const char *out) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    MdimFile f;
    int rc = mdim_open(&v, name, &f);
    if (rc != MDIM_OK) { printf("open file error: %d\n", rc); mdim_volume_free(&v); return 1; }
    uint8_t *buf = (uint8_t *)malloc(f.size ? f.size : 1);
    uint32_t actual = 0;
    mdim_read(&v, &f, buf, f.size, &actual);
    FILE *o = fopen(out, "wb");
    if (!o) { perror(out); free(buf); mdim_volume_free(&v); return 1; }
    fwrite(buf, 1, actual, o);
    fclose(o);
    printf("Got %s → %s (%u bytes) from slot %u\n", name, out, actual, f.entry);
    free(buf);
    mdim_volume_free(&v);
    return 0;
}

static int cmd_list(const char *vol) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    printf("%-24s %8s %8s  %6s  %-10s %-10s\n", "name", "bytes", "blocks", "runs", "entry slot", "born frame");
    char names[256][MDIM_MAX_NAME];
    uint32_t n = mdim_ls(&v, names, 256);
    for (uint32_t i = 0; i < n; i++) {
        MdimFile f;
        if (mdim_open(&v, names[i], &f) != MDIM_OK) continue;
        MdimSlot *s = mdim_slot(&v, f.entry);
        printf("%-24s %8u %8u  %6u  %-10u %-10u\n",
               names[i], f.size, s->n_data_slots, s->n_runs, f.entry, s->home_frame);
    }
    MdimStats st = mdim_stats(&v);
    printf("\n%d files | %u blocks used / %u free\n", n, st.n_blocks_used, st.blocks_free);
    mdim_volume_free(&v);
    return 0;
}

static int cmd_info(const char *vol) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    MdimStats st = mdim_stats(&v);
    printf("volume     : %s (%u B, magic %.4s)\n", vol, MDIM_VOL_BYTES, v.bytes);
    printf("files      : %u\n", st.n_files);
    printf("blocks     : %u used / %u free\n", st.n_blocks_used, st.blocks_free);
    printf("timeline   : last frame %u | checkpoint %u | %u frames in ring\n",
           st.last_frame, st.checkpoint_frame, st.n_frames_ring);
    printf("name probes : max %u\n", st.max_probe);
    printf("journal    : head slot %u | ring [%u, %u)\n",
           st.journal_head, MDIM_JRNL_START, MDIM_JRNL_END);
    mdim_volume_free(&v);
    return 0;
}

static int cmd_view(const char *vol, uint32_t flat) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    if (flat >= MDIM_SLOTS) { printf("slot out of range 0..%u\n", MDIM_SLOTS - 1); mdim_volume_free(&v); return 1; }
    MdimSlot *s = mdim_slot(&v, flat);
    mdim_print_views(&v, flat);
    if (s->type == MDIM_T_FILE && s->name[0])
        printf("  → file \"%s\" (%u B) data at slot %u\n", s->name, s->size, s->prev);
    else if (s->type == MDIM_T_DATA)
        printf("  → data slot (63 B payload)\n");
    else
        printf("  → type %u (%s)\n", s->type,
               s->type == MDIM_T_EMPTY ? "EMPTY" : s->type == MDIM_T_TOMB ? "TOMB" : "other");
    mdim_volume_free(&v);
    return 0;
}

static int cmd_history(const char *vol, const char *name) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    MdimStats st = mdim_stats(&v);
    if (st.last_frame == MDIM_FRAME_NONE) { printf("no timeline\n"); mdim_volume_free(&v); return 0; }
    printf("history of \"%s\" (frames %u..%u, evicted below %u):\n",
           name, st.checkpoint_frame + 1, st.last_frame, st.checkpoint_frame);
    uint8_t buf[256];
    for (uint32_t fr = st.checkpoint_frame + 1; fr <= st.last_frame; fr++) {
        uint32_t actual = 0;
        int rc = mdim_read_at(&v, name, fr, buf, sizeof(buf), &actual);
        if (rc == MDIM_OK) {
            printf("  frame %4u  %u bytes", fr, actual);
            if (actual > 0) {
                printf("  \"");
                for (uint32_t i = 0; i < actual && i < 40; i++)
                    putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
                printf("\"");
            }
            printf("\n");
        } else if (rc == MDIM_ERR_NOENT) {
            printf("  frame %4u  (absent)\n", fr);
        } else {
            printf("  frame %4u  err=%d\n", fr, rc);
        }
    }
    mdim_volume_free(&v);
    return 0;
}

static int cmd_unsummon(const char *vol, const char *name) {
    MdimVolume v;
    if (mdim_volume_load(&v, vol) != MDIM_OK) { printf("open error\n"); return 1; }
    int rc = mdim_unsummon(&v, name);
    if (rc != MDIM_OK) { printf("unsummon error: %d\n", rc); mdim_volume_free(&v); return 1; }
    if (mdim_volume_save(&v, vol) != MDIM_OK) { printf("save error\n"); }
    printf("Unsummoned %s\n", name);
    mdim_volume_free(&v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("GeoFS MDIM — create/summon/get/list/info/view/history/unsummon/mmap\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "create") == 0 && argc >= 3)
        return cmd_create(argv[2]);
    if (strcmp(cmd, "summon") == 0 && argc >= 5)
        return cmd_summon(argv[2], argv[3], argv[4], 0);
    if (strcmp(cmd, "mmap") == 0 && argc >= 5)
        return cmd_summon(argv[2], argv[3], argv[4], 1);
    if (strcmp(cmd, "get") == 0 && argc >= 5)
        return cmd_get(argv[2], argv[3], argv[4]);
    if (strcmp(cmd, "list") == 0 && argc >= 3)
        return cmd_list(argv[2]);
    if (strcmp(cmd, "info") == 0 && argc >= 3)
        return cmd_info(argv[2]);
    if (strcmp(cmd, "view") == 0 && argc >= 4)
        return cmd_view(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    if (strcmp(cmd, "history") == 0 && argc >= 4)
        return cmd_history(argv[2], argv[3]);
    if (strcmp(cmd, "unsummon") == 0 && argc >= 4)
        return cmd_unsummon(argv[2], argv[3]);
    printf("Unknown: %s\n", cmd);
    return 1;
}
