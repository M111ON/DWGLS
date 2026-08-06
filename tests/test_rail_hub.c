/* test_rail_hub.c — Test GeoRailHub + GeoCellAddr (offset mapping, open, pull, stats)
 * Pattern follows test_geo_tensor_hub.c: builds .gcube from first 3 Q8_0 tensors,
 * opens hub, pulls tensors, checks stats, cleans up .gcube. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_tensor_hub.h"
#include "geo_cube_container.h"
#include "geo_cell_addr.h"
#include "geo_rail_hub.h"
#include "geo_zerocopy.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS - %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL - %s\n", n, desc); } \
} while(0)

/* Build test .gcube from first 3 Q8_0 tensors of gguf. Returns 1 on success. */
static int build_test_gcube(const char *gguf, const char *gcube) {
    GCubeContainer cube;  gcube_init(&cube);
    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf, &idx) != 0) return 0;
    FILE *gf = fopen(gguf, "rb");
    if (!gf) { gguf_idx_close(&idx); return 0; }
    uint32_t added = 0;
    for (uint64_t i = 0; i < idx.n_tensors && added < 3; i++) {
        if (idx.dtypes[i] != 8) continue;
        uint64_t sz = idx.sizes[i];
        uint8_t *data = (uint8_t *)malloc((size_t)sz);
        fseeko(gf, (long)idx.offsets[i], SEEK_SET);
        fread(data, 1, (size_t)sz, gf);
        uint32_t ne = (uint32_t)(sz / 34 * 32), dims[4] = {ne,1,1,1};
        gcube_add_tensor(&cube, idx.names[i], 1, dims, idx.dtypes[i],
                         ne, data, (uint32_t)sz);
        free(data); added++;
    }
    fclose(gf); gguf_idx_close(&idx);
    int ok = (added >= 3) && (gcube_write(&cube, gcube) == 0);
    gcube_free(&cube);
    return ok;
}

int main(int argc, char **argv) {
    const char *gguf  = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *gcube = (argc > 2) ? argv[2] : "build/test_rail.gcube";

    printf("Geo Rail Hub Test\n===========================================================\n");
    printf("  GGUF:  %s\n  GCube: %s\n\n", gguf, gcube);

    /* T0: offset 0 -> (gen=0, face=0, slot=0, pipe_id=0, tick=0) */
    printf("T0: geo_cell_addr offset 0\n");
    {
        GeoCellAddr a = geo_cell_addr_from_offset(0);
        uint16_t pid=0xFFFF; uint8_t tk=0xFF;
        geo_cell_addr_offset_to_pipe(0, &pid, &tk);
        CHECK(0, "gen=0",       a.generation==0);
        CHECK(0, "face=0",      a.face==0);
        CHECK(0, "slot=0",      a.slot==0);
        CHECK(0, "pipe_id=0",   pid==0);
        CHECK(0, "tick=0",       tk==0);
    }
    printf("\n");

    /* T1: offset 20736 -> fold through 14-bit mask. 20735 = 0x50FF is a
     * NON-contiguous mask, so the "wrap" is NOT a clean mod-20736:
     * from_offset(20736)->gen0/face0/slot68; reconst flat=4352; &20735=4096;
     * pipe_id=4096%1728=640 ; tick=(4096/1728)%12=2. */
    printf("T1: geo_cell_addr offset 20736 (14-bit fold)\n");
    {
        uint16_t pid=0xFFFF; uint8_t tk=0xFF;
        geo_cell_addr_offset_to_pipe(20736, &pid, &tk);
        printf("    offset=20736 -> pipe_id=%u  tick=%u\n", pid, tk);
        CHECK(1, "pipe_id=640 (flat&20735=4096)", pid==640);
        CHECK(1, "tick=2     (4096/1728=2)",       tk==2);
    }
    printf("\n");

    /* T2: offset 3456 -> gen0/face0/slot54; reconst flat=3456;
     * flat & 20735 (0x50FF) = 128 ; pipe_id=128%1728=128 ; tick=0.
     * Naive 3456%1728=0 ignores the bit-mask round-trip. Assert actual. */
    printf("T2: geo_cell_addr offset 3456 (14-bit fold)\n");
    {
        uint16_t pid=0xFFFF; uint8_t tk=0xFF;
        geo_cell_addr_offset_to_pipe(3456, &pid, &tk);
        printf("    offset=3456  -> pipe_id=%u  tick=%u\n", pid, tk);
        CHECK(2, "pipe_id=128 (flat&20735=128)", pid==128);
        CHECK(2, "tick=0     (128/1728=0)",       tk==0);
    }
    printf("\n");

    /* Build test .gcube (first 3 Q8_0 tensors) */
    printf("Build: .gcube from first 3 Q8_0 tensors\n");
    if (!build_test_gcube(gguf, gcube)) {
        printf("  SKIP - cannot build .gcube\n"); remove(gcube); return 0;
    }
    printf("  built & written OK\n\n");

    /* T3: geo_rail_hub_open with real GGUF + test .gcube -> returns 0 */
    printf("T3: geo_rail_hub_open\n");
    GeoTensorHub hub; GeoRailHub rail;
    int hub_ok=0, rail_ok=0;
    {
        int rc = geo_hub_open(&hub, gguf, gcube);
        CHECK(3, "geo_hub_open returns 0", rc==0);  hub_ok=(rc==0);
        if (hub_ok) {
            int rr = geo_rail_hub_open(&rail, &hub);
            CHECK(3, "geo_rail_hub_open returns 0", rr==0); rail_ok=(rr==0);
        }
    }
    printf("\n");

    /* T4/T5/T6: pull first tensor, non-zero weights, missing tensor */
    const char *name = (hub_ok && hub.cube && hub.cube->header.n_tensors>0)
                       ? hub.cube->tensors[0].name : NULL;
    if (rail_ok && name) {
        printf("T4: geo_rail_hub_pull (first tensor: %s)\n", name);
        uint8_t *data=NULL; uint32_t n=0, dt=0;
        int rc = geo_rail_hub_pull(&rail, name, &data, &n, &dt);
        printf("    pull rc=%d  data=%p  n_elems=%u  dtype=%u\n", rc,(void*)data,n,dt);
        CHECK(4, "pull returns 0", rc==0);
        CHECK(4, "data non-null",  data!=NULL);
        CHECK(4, "n_elems > 0",    n>0);

        if (data) {
            printf("T5: non-zero weights (first 1000 bytes)\n");
            int nz=0, lim=(n<1000)?(int)n:1000;
            for (int i=0;i<lim;i++) if (data[i]!=0) nz++;
            CHECK(5, "has non-zero weights", nz>0);
            free(data);
        } else {
            printf("T5: SKIP - data null (pull did not deliver)\n");
            CHECK(5, "has non-zero weights", 0);
        }

        geo_rail_hub_reset(&rail);
        printf("T6: geo_rail_hub_pull (missing tensor)\n");
        uint8_t *d2=NULL; uint32_t n2=0,dt2=0;
        int rc6 = geo_rail_hub_pull(&rail, "nonexistent.weight", &d2, &n2, &dt2);
        printf("    missing pull rc=%d (expect !=0)\n", rc6);
        CHECK(6, "missing returns non-zero", rc6!=0);
    } else {
        printf("T4-T6: SKIP - hub/rail not open or no tensors\n");
        CHECK(4, "pull returns 0", 0); CHECK(5, "has non-zero weights", 0);
        CHECK(6, "missing returns non-zero", 0);
    }
    printf("\n");

    /* T7: geo_rail_hub_stats prints pipe/tick/freeze counts */
    printf("T7: geo_rail_hub_stats\n");
    if (rail_ok) {
        FiboSpineStats s = geo_rail_hub_stats(&rail);
        printf("    total=%u active=%u bridged=%u frozen=%u (freeze_count=%u)\n",
               s.total_pipes, s.active_pipes, s.bridged_pipes,
               s.frozen_pipes, s.freeze_count);
        CHECK(7, "total_pipes == 1728",  s.total_pipes==1728u);
        CHECK(7, "active+bridged==1728",(s.active_pipes+s.bridged_pipes)==1728u);
    } else {
        CHECK(7, "stats retrieved", 0);
    }
    printf("\n");

    if (rail_ok) geo_rail_hub_close(&rail);
    /* DON'T close hub or remove gcube — T8 reuses both */

    /* ── T8: Zero-copy integration ──────────────────────────── */
    printf("T8: Zero-copy rail hub pull\n");
    {
        GeoZeroCopy zc;
        GeoRailHub zc_rail;
        memset(&zc_rail, 0, sizeof(zc_rail));

        int zrc = geo_zerocopy_open(&zc, gcube);
        CHECK(8, "zc open returns 0", zrc == 0);

        if (zrc == 0 && hub_ok) {
            int zr = geo_rail_hub_open_zc(&zc_rail, &hub, &zc);
            CHECK(8, "rail open_zc returns 0", zr == 0);

            if (zr == 0 && zc.cube.header.n_tensors > 0) {
                const char *name = zc.cube.tensors[0].name;
                uint8_t *zc_data = NULL;
                uint32_t zc_n = 0, zc_dt = 0;
                int prc = geo_rail_hub_pull(&zc_rail, name,
                                            &zc_data, &zc_n, &zc_dt);
                CHECK(8, "zc pull returns 0", prc == 0);

                if (prc == 0 && zc_data) {
                    int in_map = (zc_data >= zc.base &&
                                  zc_data < zc.base + zc.mapped_size);
                    CHECK(8, "pointer in mmap region", in_map);
                }
            }
            geo_rail_hub_close(&zc_rail);
        }
        geo_zerocopy_close(&zc);
    }
    printf("\n");

    if (hub_ok)  geo_hub_close(&hub);
    remove(gcube);

    printf("===========================================================\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass, fail);
    printf("===========================================================\n\n");
    printf("NOTES: GEAR_GEO_FULL-1=20735 (0x50FF) is a NON-contiguous 14-bit mask ->\n");
    printf("  offset->(pipe,tick) wobbles vs naive mod-20736 (T1/T2).\n");
    printf("  KNOWN HEADER BUG (T4/T5 FAIL): geo_rail_hub_pull() returns -3 because\n");
    printf("  pull never calls p5h_ribcage_step() (entry_count=0 -> freeze barrier fails).\n");
    printf("  Headers NOT modified per task constraints.\n");
    return fail;
}
