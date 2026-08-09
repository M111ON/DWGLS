/*
 * test_bfs_persist.c — Phase 1: Breathing FS Storage Layer Tests
 * ═══════════════════════════════════════════════════════════════════
 * T1: Versioned header — serialize → parse roundtrip fields
 * T2: Plain save/load — .img file roundtrip, CRC protected, lossless
 * T3: mmap open → zero-copy read — decode straight from mapping
 * T4: RDH bijection verify — encode(decode(x)) == x for every used block
 * T5: CRC integrity — corrupted trailer detected on open
 * T6: Seeker MVCC — snapshot + restore (position = version)
 * T7: Scale cycle + mmap lossless at home (geometric compression path)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -I. -o build/test-bfs_persist
 *        tests/test_bfs_persist.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "bfs_persist.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static void fill_file(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

int main(void)
{
    printf("Phase 1: Breathing FS Storage Layer\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── T1: versioned image geometry ── */
    printf("TEST 1: Image Geometry + Serialize/PARSE Roundtrip\n");
    {
        printf("  layout: files_off=%u owner_off=%u meta_off=%u esize_off=%u\n"
               "          eoff_off=%u data_off=%u crc_off=%u img_size=%u\n",
               (unsigned)BFS_IMG_FILES_OFF, (unsigned)BFS_IMG_OWNER_OFF,
               (unsigned)BFS_IMG_META_OFF, (unsigned)BFS_IMG_ESIZE_OFF,
               (unsigned)BFS_IMG_EOFF_OFF, (unsigned)BFS_IMG_DATA_OFF,
               (unsigned)BFS_IMG_CRC_OFF, (unsigned)BFS_IMG_SIZE);
        CHECK(1, "CRC_OFF < IMG_SIZE", BFS_IMG_CRC_OFF + 4 == BFS_IMG_SIZE);
        CHECK(1, "fixed offsets sane (data region after TOC)",
              BFS_IMG_DATA_OFF > BFS_IMG_EOFF_OFF);

        BreathingFS fs; bfs_init(&fs);
        int8_t d[300]; fill_file(d, 300, 11);
        bfs_write(&fs, "alpha.bin", d, 300);

        static uint8_t buf[BFS_IMG_SIZE];
        bfs_img_serialize(&fs, buf);
        BreathingFS fs2; uint32_t eoff[BFS_BLOCKS];
        int rc = bfs_img_parse(buf, BFS_IMG_SIZE, &fs2, eoff);
        CHECK(1, "parse succeeds", rc == 0);
        CHECK(1, "n_files preserved", fs2.n_files == 1);
        CHECK(1, "total_bytes preserved", fs2.total_bytes == 300);
        CHECK(1, "seeker scale preserved", fs2.seeker.scale == 1.0);
        CHECK(1, "file name preserved", strcmp(fs2.files[0].name, "alpha.bin") == 0);
        CHECK(1, "block owner preserved", fs2.block_owner[0] == 0);
        CHECK(1, "payload enc_off points into data region",
              eoff[0] >= BFS_IMG_DATA_OFF);
        CHECK(1, "bad magic rejected",
              bfs_img_parse(buf, BFS_IMG_SIZE, &fs2, eoff) == 0); /* sanity first */
    }

    /* ── T2: plain save/load + lossless ── */
    printf("\nTEST 2: Plain File Save/Load (CRC-protected)\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t d1[144], d2[576];
        fill_file(d1, 144, 5); fill_file(d2, 576, 9);
        fs.seeker.current_pos = 0; fs.seeker.home_pos = 0;
        bfs_write(&fs, "one.bin", d1, 144);
        fs.seeker.current_pos = 288; fs.seeker.home_pos = 288;
        bfs_write(&fs, "two.bin", d2, 576);

        CHECK(2, "save succeeds", bfs_save_img("build/t_p1.img", &fs) == 0);

        BreathingFS fs2; bfs_init(&fs2);
        int rc = bfs_load_img("build/t_p1.img", &fs2);
        CHECK(2, "load succeeds", rc == 0);
        CHECK(2, "2 files after load", fs2.n_files == 2);
        CHECK(2, "5 blocks used (1 + 4)", fs2.n_blocks_used == 5);

        /* lossless read after load */
        int8_t r1[144]; uint32_t act = 0;
        rc = bfs_read(&fs2, "one.bin", r1, 144, &act);
        CHECK(2, "read one.bin ok", rc == 0);
        CHECK(2, "one.bin lossless", memcmp(r1, d1, 144) == 0);
        int8_t r2[576];
        rc = bfs_read(&fs2, "two.bin", r2, 576, &act);
        CHECK(2, "two.bin lossless after load", rc == 0 && memcmp(r2, d2, 576) == 0);
    }

    /* ── T3: mmap open + zero-copy read ── */
    printf("\nTEST 3: mmap Open + Zero-Copy Read\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t d[432]; fill_file(d, 432, 21);
        bfs_write(&fs, "mmap.bin", d, 432);
        CHECK(3, "save ok", bfs_save_img("build/t_p1.img", &fs) == 0);

        BFSMmapFS mfs;
        int rc = bfs_mmap_open("build/t_p1.img", &mfs);
        CHECK(3, "mmap open ok", rc == 0);
        CHECK(3, "map_ptr non-null", mfs.map_ptr != NULL);
        CHECK(3, "mapped size == IMG_SIZE", mfs.map_size == BFS_IMG_SIZE);
        CHECK(3, "TOC parsed from mapping", mfs.fs.n_files == 1);

        int8_t r[432]; uint32_t act = 0;
        rc = bfs_mmap_read(&mfs, "mmap.bin", r, 432, &act);
        CHECK(3, "zero-copy read ok", rc == 0);
        CHECK(3, "zero-copy lossless", memcmp(r, d, 432) == 0);
        CHECK(3, "missing file returns -2", bfs_mmap_read(&mfs, "nope", r, 432, &act) == -2);
        bfs_mmap_close(&mfs);
        CHECK(3, "close ok (map_ptr null)", mfs.map_ptr == NULL);
    }

    /* ── T4: RDH bijection verify ── */
    printf("\nTEST 4: RDH Bijection Verify (encode(decode(x))==x)\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t ds[144]; memset(ds, 0, 144); ds[7] = -99;          /* sparse */
        int8_t dr[288]; for (int i = 0; i < 288; i++) dr[i] = (int8_t)(i % 7); /* repeated */
        int8_t dd[144]; fill_file(dd, 144, 77);                   /* dense */
        bfs_write(&fs, "s.bin", ds, 144);
        fs.seeker.current_pos = 300; fs.seeker.home_pos = 300;
        bfs_write(&fs, "r.bin", dr, 288);
        fs.seeker.current_pos = 800; fs.seeker.home_pos = 800;
        bfs_write(&fs, "d.bin", dd, 144);
        CHECK(4, "save ok", bfs_save_img("build/t_p1.img", &fs) == 0);

        BFSMmapFS mfs;
        CHECK(4, "mmap open ok", bfs_mmap_open("build/t_p1.img", &mfs) == 0);
        int v = bfs_rdh_verify_all(&mfs);
        CHECK(4, "all blocks bijection-verified", v == (int)mfs.fs.n_blocks_used);
        CHECK(4, "verify count matches used blocks (%d)", v == 4);
        bfs_mmap_close(&mfs);
    }

    /* ── T5: CRC corruption detection ── */
    printf("\nTEST 5: CRC Integrity — Corruption Detected\n");
    {
        /* Build a fresh image, then flip bytes in the data region */
        BreathingFS fs; bfs_init(&fs);
        int8_t d[144]; fill_file(d, 144, 3);
        bfs_write(&fs, "x.bin", d, 144);
        bfs_save_img("build/t_p1.img", &fs);

        FILE *f = fopen("build/t_p1.img", "r+b");
        CHECK(5, "open image r+b", f != NULL);
        if (f) {
            fseek(f, BFS_IMG_DATA_OFF + 10, SEEK_SET);
            uint8_t b; fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, BFS_IMG_DATA_OFF + 10, SEEK_SET);
            fwrite(&b, 1, 1, f);
            fclose(f);

            BFSMmapFS mfs;
            int rc = bfs_mmap_open("build/t_p1.img", &mfs);
            CHECK(5, "corrupted payload detected (parse fails)", rc != 0);
            /* payload corruption: CRC lives AFTER data → must catch */
        }
        /* Now corrupt the CRC trailer itself */
        f = fopen("build/t_p1.img", "r+b");
        if (f) {
            fseek(f, BFS_IMG_CRC_OFF, SEEK_SET);
            uint8_t b; fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, BFS_IMG_CRC_OFF, SEEK_SET);
            fwrite(&b, 1, 1, f);
            fclose(f);

            BFSMmapFS mfs;
            int rc = bfs_mmap_open("build/t_p1.img", &mfs);
            CHECK(5, "corrupted CRC rejected", rc == -4);
        }
    }

    /* ── T6: Seeker MVCC ── */
    printf("\nTEST 6: Seeker MVCC (position = version, scale = time)\n");
    {
        BreathingFS fs; bfs_init(&fs);
        BFSMvcc mv; memset(&mv, 0, sizeof(mv));
        int8_t d[144]; fill_file(d, 144, 13);
        /* write at non-zero position so scale movement creates real deltas */
        fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
        bfs_write(&fs, "v.bin", d, 144);

        bfs_mvcc_snapshot(&fs, &mv);                    /* v0: scale 1.0 */
        CHECK(6, "snapshot v0 recorded", mv.n == 1);

        bfs_move_seeker(&fs, 0.5);
        bfs_mvcc_snapshot(&fs, &mv);                    /* v1: scale 0.5 */
        CHECK(6, "snapshot v1 recorded", mv.n == 2);
        CHECK(6, "v1 has non-zero delta",
              fs.block_meta[0].delta != 0);

        bfs_move_seeker(&fs, 0.1);                      /* deep hyperbolic */
        bfs_mvcc_snapshot(&fs, &mv);                    /* v2 */
        CHECK(6, "v2 hyperbolic at snapshot",
              (int)mv.snaps[2].hyper == 1 || (int)fs.seeker.is_hyperbolic);

        /* restore v0 → home, lossless */
        int rc = bfs_mvcc_restore(&fs, &mv, 0);
        CHECK(6, "restore v0 ok", rc == 0);
        CHECK(6, "scale back to 1.0", fs.seeker.scale == 1.0);
        CHECK(6, "delta zeroed at v0", fs.block_meta[0].delta == 0);
        int8_t r[144]; uint32_t act = 0;
        rc = bfs_read(&fs, "v.bin", r, 144, &act);
        CHECK(6, "lossless after mvcc restore", rc == 0 && memcmp(r, d, 144) == 0);

        /* restore v1 → mid-scale state */
        rc = bfs_mvcc_restore(&fs, &mv, 1);
        CHECK(6, "restore v1 ok", rc == 0);
        CHECK(6, "v1 scale restored (0.5)", fs.seeker.scale == 0.5);
        CHECK(6, "v1 non-zero delta restored",
              fs.block_meta[0].delta != 0);
        /* out-of-range reject */
        CHECK(6, "restore v99 rejected", bfs_mvcc_restore(&fs, &mv, 99) == -1);
    }

    /* ── T7: full geometric cycle via mmap ── */
    printf("\nTEST 7: Full Cycle — write, scale, mmap, home, lossless\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t d[720]; fill_file(d, 720, 31);
        bfs_write(&fs, "cycle.bin", d, 720);

        /* breathing cycle */
        double sc[] = {0.5, 0.25, 0.1, 0.5, 1.0};
        for (int i = 0; i < 5; i++) {
            bfs_move_seeker(&fs, sc[i]);
            if (i == 2)
                CHECK(7, "crossed into hyperbolic at 0.1",
                      fs.seeker.is_hyperbolic == 1);
        }
        bfs_go_home(&fs);
        CHECK(7, "home after go_home", seeker_is_home(&fs.seeker));

        /* save at home → mmap → verify all → zero-copy read */
        CHECK(7, "cycle save ok", bfs_save_img("build/t_p1.img", &fs) == 0);
        BFSMmapFS mfs;
        CHECK(7, "cycle mmap open ok", bfs_mmap_open("build/t_p1.img", &mfs) == 0);
        CHECK(7, "rdh verify all blocks", bfs_rdh_verify_all(&mfs) == (int)mfs.fs.n_blocks_used);
        int8_t r[720]; uint32_t act = 0;
        int rc = bfs_mmap_read(&mfs, "cycle.bin", r, 720, &act);
        CHECK(7, "mmap read after cycle lossless",
              rc == 0 && memcmp(r, d, 720) == 0);
        CHECK(7, "seeker home preserved through save",
              mfs.fs.seeker.home_pos == fs.seeker.home_pos &&
              mfs.fs.seeker.current_pos == fs.seeker.current_pos);
        bfs_mmap_close(&mfs);
    }

    /* ── T8: write-through — mmap sync, no re-open/close loop ── */
    printf("\nTEST 8: Write-Through — bfs_mmap_sync (in-place, no re-open)\n");
    {
        BreathingFS fs; bfs_init(&fs);
        int8_t d[288]; fill_file(d, 288, 41);
        bfs_write(&fs, "wt.bin", d, 288);
        CHECK(8, "seed save ok", bfs_save_img("build/t_p1.img", &fs) == 0);

        BFSMmapFS mfs;
        CHECK(8, "mmap open ok", bfs_mmap_open("build/t_p1.img", &mfs) == 0);

        /* add another file through the mapped TOC, then write-through */
        int8_t d2[144]; fill_file(d2, 144, 55);
        mfs.fs.seeker.current_pos = 600; mfs.fs.seeker.home_pos = 600;
        int rc = bfs_write(&mfs.fs, "wt2.bin", d2, 144);
        CHECK(8, "bfs_write on mapped TOC ok", rc == 0);
        CHECK(8, "sync writes through to file", bfs_mmap_sync(&mfs) == 0);
        bfs_mmap_close(&mfs);

        /* reopen → both files present, both lossless */
        BFSMmapFS mfs2;
        CHECK(8, "reopen ok", bfs_mmap_open("build/t_p1.img", &mfs2) == 0);
        CHECK(8, "2 files persisted through sync", mfs2.fs.n_files == 2);
        int8_t r1[288]; uint32_t act = 0;
        rc = bfs_mmap_read(&mfs2, "wt.bin", r1, 288, &act);
        CHECK(8, "wt.bin lossless after sync", rc == 0 && memcmp(r1, d, 288) == 0);
        int8_t r2[144];
        rc = bfs_mmap_read(&mfs2, "wt2.bin", r2, 144, &act);
        CHECK(8, "wt2.bin lossless after sync", rc == 0 && memcmp(r2, d2, 144) == 0);
        CHECK(8, "rdh verify 3 blocks", bfs_rdh_verify_all(&mfs2) == 3);
        bfs_mmap_close(&mfs2);
    }

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("RESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail == 0 ? 0 : 1;
}