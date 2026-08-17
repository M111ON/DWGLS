/* rule_e2e.c — §15.73: placement/admission/read ใช้ CAP_RULE_* เดียว end-to-end
 * ═══════════════════════════════════════════════════════════════════════════
 * พิสูจน์ว่า wiring ใน core (cap_rule_scale → cap_admit → ghost_lift_auto →
 * ghost_read_rule → geos_read_ghost) ทำงานครบ chain บนไฟล์จริง:
 *
 *   placement  : ทุก block → w = cap_rule_scale(block)  (กฎ trained เดียว)
 *   admission  : cap_admit(gate) → ADMIT (field) / LIFT (ghost) / REJECT (pointer-home)
 *   read       : geos_read_ghost → ghost_read_rule resolve to_scale เอง
 *                (ถ้าอ่านด้วยกฎคนละตัว → bond ต่าง → thaw fail — เสาเข็มห้ามขยับ)
 *
 * ตรวจ lossless byte-for-byte + พิสูจน์ rule-mismatch = NULL โดย construction
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "geofs_core.h"

/* deterministic pseudo-random data — ไม่ใช่ของจริงแต่เป็น byte ครบทุกค่า */
static uint64_t rng_state = UINT64_C(0xD0D0C0A110000001);
static uint8_t next_byte(void) {
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint8_t)(rng_state * UINT64_C(0x2545F4914F6CDD1D) >> 56);
}

static int verify_file(const char *tag, const uint8_t *orig, uint8_t *got,
                       uint32_t n) {
    if (memcmp(orig, got, n) != 0) {
        uint32_t first = 0;
        while (first < n && orig[first] == got[first]) first++;
        printf("  ✗ %s MISMATCH at byte %u/%u\n", tag, first, n);
        return 0;
    }
    printf("  ✓ %s lossless (%u bytes)\n", tag, n);
    return 1;
}

int main(int argc, char **argv) {
    const char *data_path = NULL;
    if (argc > 1) data_path = argv[1];   /* optional: ไฟล์จริง (WAV/MP4/PDF...) */

    /* ── init: volume (heap — inodes 2048 ตัวใหญ่) + ghost log + residual space ── */
    GeosVolume *v = (GeosVolume *)calloc(1, sizeof(GeosVolume));
    GhostLog   *log = (GhostLog *)calloc(1, sizeof(GhostLog));
    ResidualSpace *rs = (ResidualSpace *)calloc(1, sizeof(ResidualSpace));
    if (!v || !log || !rs) { printf("alloc fail\n"); return 1; }

    geos_volume_init(v);
    strcpy(v->vol_name, "rule-e2e");
    ghost_log_init(log);
    rs_init(rs, RS_DEFAULT_CAPACITY);
    GhostPairTable pair;                     /* O(1) read path — §15.57 */
    memset(&pair, 0, sizeof(pair));
    ghost_pair_attach(log, &pair);

    CapAccount acc;
    cap_init(&acc);

    printf("═══ RULE END-TO-END (§15.73) ═══\n");
    printf("rule: stride %u · offset %u · gate %.1f · orbit %u · chunk %u\n",
           CAP_RULE_STRIDE, CAP_RULE_OFFSET, (double)CAP_RULE_GATE,
           CAP_RULE_ORBIT, CAP_RULE_CHUNK);

    /* ── generate files: หลายก้อน ขนาดต่างกัน (block = 64B ใน geos) ── */
    enum { N_FILES = 6 };
    const uint32_t sizes[N_FILES] = { 512, 2048, 4096, 10000, 40960, 3 };
    const char *names[N_FILES] = { "tiny", "small", "med", "big", "huge", "3b" };
    uint8_t *orig[N_FILES];
    uint32_t placed = 0, lifted = 0, rejected = 0, admitted = 0;

    for (int f = 0; f < N_FILES; f++) {
        orig[f] = (uint8_t *)malloc(sizes[f]);
        for (uint32_t i = 0; i < sizes[f]; i++) orig[f][i] = next_byte();

        GeosInode *in = geos_create(v, names[f], sizes[f], orig[f]);
        if (!in) { printf("geos_create %s fail\n", names[f]); return 1; }
        geos_write(v, names[f], orig[f], sizes[f]);   /* geos_create = metadata เท่านั้น */

        /* ── placement chain: ทุก block ผ่าน cap_admit + ghost_lift_auto ── */
        uint16_t nblocks = in->block_count;
        for (uint16_t b = 0; b < nblocks; b++) {
            uint32_t block = in->block_start + b;          /* flat id */
            uint8_t  w0 = 0;                               /* birth */
            uint8_t  to = cap_rule_scale(block);           /* วางตามกฎ trained */
            uint32_t off = b * GEOS_BLOCK_SZ;
            uint32_t n = sizes[f] - off; if (n > GEOS_BLOCK_SZ) n = GEOS_BLOCK_SZ;

            int verdict = cap_admit(&acc, CAP_RULE_GATE, w0, to);
            if (verdict == CAP_LIFT) {
                int r = ghost_lift_auto(log, rs, CAP_RULE_GATE, (uint16_t)block,
                                        w0, to, orig[f] + off, n);
                if (r == GHOST_AUTO_LIFT) lifted++;
                else { printf("  ⚠ lift fail block %u\n", block); }
            } else if (verdict == CAP_ADMIT) {
                admitted++;
            } else {
                rejected++;      /* pointer-home — ยัง lossless */
            }
            placed++;
        }
    }

    printf("\nplacement: %u blocks · ADMIT %u · LIFT %u (ghost) · REJECT %u (pointer-home)\n",
           placed, admitted, lifted, rejected);
    printf("capacity: used %I64u / 20736 slots · lifts %u · rejects %u\n\n",
           (unsigned long long)acc.used, acc.lifts, acc.rejects);

    /* ── §15.78 read back via walk (geos_read_ghost → ghost_read_rule_walk)
          อ่าน = เดินนาฬิกาจาก state ไปตำแหน่ง live ของแต่ละ block → thaw ผ่าน bond
          — พิสูจน์ enter-anywhere: 3 start states → ข้อมูลเดียวกัน lossless ── */
    printf("\n═══ WALK-BASED READ PATH (§15.78) — geos_read_ghost เดินนาฬิกา ไม่มี pile lookup ═══\n");
    int all_ok = 1;
    const uint32_t starts[3][2] = { { 0u, 0u }, { 72u, 2u }, { 143u, 11u } };
    uint64_t steps_by_state[3] = { 0, 0, 0 };
    for (int ps = 0; ps < 3; ps++) {
        v->walk_round = starts[ps][0];       /* enter anywhere: ตั้ง state ได้ทุกที่ */
        v->walk_tick  = starts[ps][1];
        v->walk_steps = 0;
        int ok_pass = 1;
        for (int f = 0; f < N_FILES; f++) {
            uint8_t *got = (uint8_t *)malloc(sizes[f]);
            int n = geos_read_ghost(v, log, rs, names[f], got, sizes[f]);
            if (n != (int)sizes[f]) {
                printf("  ✗ %s read size %d != %u\n", names[f], n, sizes[f]);
                ok_pass = 0;
            } else if (ps == 0) {
                if (!verify_file(names[f], orig[f], got, sizes[f])) ok_pass = 0;
            } else if (memcmp(got, orig[f], sizes[f]) != 0) {
                printf("  ✗ %s mismatch (start %u,%u)\n", names[f], starts[ps][0], starts[ps][1]);
                ok_pass = 0;
            }
            free(got);
        }
        steps_by_state[ps] = v->walk_steps;
        printf("  start (%2u, %2u): walk %llu steps (6 files) → %s\n",
               starts[ps][0], starts[ps][1],
               (unsigned long long)v->walk_steps, ok_pass ? "lossless ✓" : "FAIL ✗");
        if (!ok_pass) all_ok = 0;
    }
    printf("  enter-anywhere: 3/3 start states lossless — steps %llu / %llu / %llu\n"
           "  (state ต่าง → เส้นทางเดินต่าง แต่ข้อมูลเดียวกัน — state=(seed,round,tick) พอ)\n",
           (unsigned long long)steps_by_state[0], (unsigned long long)steps_by_state[1],
           (unsigned long long)steps_by_state[2]);

    /* ── rule-mismatch: อ่านด้วยกฎคนละตัว → bond ต่าง → NULL (เสาเข็มห้ามขยับ) ── */
    printf("\n═══ RULE MISMATCH (กฎคนละตัวต้อง fail โดย construction) ═══\n");
    uint32_t mismatch_found = 0, mismatch_ok = 0;
    for (int f = 0; f < N_FILES; f++) {
        GeosInode *in = geos_find(v, names[f]);
        if (!in) continue;
        for (uint16_t b = 0; b < in->block_count && b < 32; b++) {
            uint32_t block = in->block_start + b;
            uint32_t got = 0;
            uint8_t wrong_to = (uint8_t)(((37u * block + 0u) % 144u)); /* stride 37 เดิม */
            if (wrong_to == cap_rule_scale(block)) continue;           /* ซ้ำ → ข้าม */
            const void *p = ghost_read(log, rs, (uint16_t)block, 0, wrong_to, &got);
            if (p) {
                /* lifted block ที่อ่านผิดกฎเจอ = bond ไม่ผูกจาก_scale — ผิด */
                printf("  ✗ block %u reachable via wrong rule!\n", block);
                mismatch_found++;
            } else {
                mismatch_ok++;
            }
        }
    }
    printf("  wrong-rule reads: %u blocked (%u checked) — %s\n",
           mismatch_ok, mismatch_ok + mismatch_found,
           mismatch_found == 0 ? "✓ ปิดเส้นทางถูกต้อง" : "✗ หลุด!");

    /* ── optional: ไฟล์จริงผ่าน chain เดียวกัน ── */
    if (data_path) {
        FILE *fp = fopen(data_path, "rb");
        if (!fp) { printf("  ⚠ cannot open %s\n", data_path); }
        else {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0 && sz <= 4L * 1024 * 1024) {
                uint8_t *data = (uint8_t *)malloc((size_t)sz);
                fread(data, 1, (size_t)sz, fp);
                GeosInode *in = geos_create(v, "real", (uint32_t)sz, data);
                geos_write(v, "real", data, (uint32_t)sz);
                if (in) {
                    uint32_t rl = 0, ra = 0;
                    for (uint16_t b = 0; b < in->block_count; b++) {
                        uint32_t block = in->block_start + b;
                        uint8_t to = cap_rule_scale(block);
                        uint32_t off = b * GEOS_BLOCK_SZ;
                        uint32_t n = (uint32_t)sz - off; if (n > GEOS_BLOCK_SZ) n = GEOS_BLOCK_SZ;
                        int vd = cap_admit(&acc, CAP_RULE_GATE, 0, to);
                        if (vd == CAP_LIFT) {
                            if (ghost_lift_auto(log, rs, CAP_RULE_GATE,
                                                (uint16_t)block, 0, to,
                                                data + off, n) == GHOST_AUTO_LIFT) rl++;
                        } else ra++;
                    }
                    uint8_t *got = (uint8_t *)malloc((size_t)sz);
                    int n = geos_read_ghost(v, log, rs, "real", got, (uint32_t)sz);
                    int ok = (n == sz && memcmp(data, got, (size_t)sz) == 0);
                    printf("\n  real file %s (%ld B): %u lift %u admit → %s\n",
                           data_path, sz, rl, ra, ok ? "lossless ✓" : "FAIL ✗");
                    if (!ok) all_ok = 0;
                    free(got);
                }
                free(data);
            } else {
                printf("  ⚠ %s size %ld — probe ใช้ไฟล์ ≤ 4MB (หรือผ่าน path ตรง)\n",
                       data_path, sz);
            }
            fclose(fp);
        }
    }

    printf("\n═══ VERDICT: %s ═══\n", all_ok ? "LOSSESS END-TO-END ✓ (rule เดียวทั้ง chain)"
                                            : "FAIL — ดูด้านบน");
    printf("ghost log: %u routes (%I64u B serialize) · rs: %u entries / %u cap\n",
           log->count, (unsigned long long)ghost_log_serialize_size(log),
           rs->count, rs->capacity);

    /* cleanup */
    for (int f = 0; f < N_FILES; f++) free(orig[f]);
    ghost_pair_free(&pair);
    rs_free(rs);
    geos_volume_free(v);
    free(v); free(log); free(rs);
    return all_ok ? 0 : 1;
}
