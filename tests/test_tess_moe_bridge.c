/* test_tess_moe_bridge.c — .tess ↔ MoE DtSlotRegion bridge
 * Synthetic stack: 2 layers × 4 experts × 8 blocks (144B each) = 64 cells
 * Baked into a .tess capo, then:
 *  (1) direct serve: load one expert's slice via bridge → compare
 *  (2) bulk bake: .tess → DtSlotRegion → dt_slot_get → compare
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "geo_tess_container.h"
#include "tess_moe_bridge.h"

#define N_LAYERS 2u
#define N_EXPERTS 4u
#define BLKS_PER 8u
#define CELL_SZ TESS_CELL_Q4_K
#define TOTAL (N_LAYERS * N_EXPERTS * BLKS_PER)

static uint8_t original[TOTAL][CELL_SZ];

static int write_stack(const char *path, uint32_t seed) {
    srand(seed);
    for (uint32_t i = 0; i < TOTAL; i++)
        for (uint32_t j = 0; j < CELL_SZ; j++) original[i][j] = (uint8_t)(rand() & 0xFF);

    TESS_Header hdr; memset(&hdr,0,sizeof(hdr));
    hdr.magic = GEO_TESS_MAGIC; hdr.version = TESS_VERSION;
    hdr.total_slots = TESS_TOTAL_SLOTS; hdr.cell_size = CELL_SZ;
    hdr.x_slots = TESS_X_SLOTS; hdr.y_slots = TESS_Y_SLOTS; hdr.z_slots = TESS_Z_SLOTS;
    hdr.tensor_count = TOTAL;

    TESS_Formula fml; memset(&fml,0,sizeof(fml));
    fml.capo_id = 0; fml.capo_total = 1;

    uint32_t cube_bytes = TESS_TOTAL_SLOTS * CELL_SZ;
    uint8_t *cube = calloc(1, cube_bytes);
    for (uint32_t i = 0; i < TOTAL; i++) {
        uint32_t slot = tess_stride_scatter(i);
        if (slot >= TESS_TOTAL_SLOTS) slot = i % TESS_TOTAL_SLOTS;
        memcpy(cube + (uint64_t)slot * CELL_SZ, original[i], CELL_SZ);
    }
    uint64_t crc = tess_crc64(cube, cube_bytes);
    FILE *f = fopen(path,"wb"); if(!f){free(cube); return -1;}
    fwrite(&hdr,1,TESS_HEADER_SIZE,f);
    fwrite(&fml,1,TESS_FORMULA_SIZE,f);
    fwrite(cube,1,cube_bytes,f);
    fwrite(&crc,1,TESS_CRC_SIZE,f);
    fclose(f); free(cube); return 0;
}

static int test_direct_serve(const char *path) {
    printf("── direct serve (TESS → expert slice) ──\n");
    TESS_CapoReader r; assert(tess_capo_open(&r, path)==0);
    assert(r.n_elems==TOTAL);
    uint32_t checked=0;
    for(uint32_t L=0;L<N_LAYERS;L++){
        for(uint32_t E=0;E<N_EXPERTS;E++){
            uint8_t buf[BLKS_PER][CELL_SZ];
            int n = tess_moe_load_expert_blocks(&r, L,E, N_EXPERTS, BLKS_PER, buf);
            assert(n == (int)(BLKS_PER*CELL_SZ));
            uint32_t base = tess_moe_block_offset(L,E,N_EXPERTS,BLKS_PER);
            for(uint32_t b=0;b<BLKS_PER;b++){
                assert(memcmp(buf[b], original[base+b], CELL_SZ)==0);
                checked++;
            }
            /* single-block variant for b=0 */
            uint8_t one[CELL_SZ];
            assert(tess_moe_load_expert_one(&r, L,E, N_EXPERTS, BLKS_PER, 0, one)==(int)CELL_SZ);
            assert(memcmp(one, original[base], CELL_SZ)==0);
        }
    }
    printf("  %u blocks verified via direct serve\n", checked);
    tess_capo_close(&r);
    printf("  PASS\n"); return 0;
}

static int test_bulk_bake(const char *path) {
    printf("── bulk bake (.tess → DtSlotRegion → verify) ──\n");
    TESS_CapoReader r; assert(tess_capo_open(&r, path)==0);
    DtSlotRegion region; memset(&region,0,sizeof(region));
    /* region needs to hold TOTAL slots worth; base_flat may wrap, so allocate full 20736 */
    assert(dt_slot_init(&region, TESS_TOTAL, CELL_SZ)==0);
    /* bake wtype=0 (GATE) for all layers starting at layer 0 */
    assert(tess_moe_bake_to_region(&r, &region, 0, N_LAYERS, N_EXPERTS, BLKS_PER, MOE_WTYPE_GATE)==0);
    uint32_t base_flat = moe_expert_to_flat(0,0,MOE_WTYPE_GATE);
    uint32_t checked=0;
    for(uint32_t i=0;i<TOTAL;i++){
        uint8_t out[CELL_SZ];
        uint32_t flat = (base_flat + i) % TESS_TOTAL;
        assert(dt_slot_get(&region, flat, out, CELL_SZ)==0);
        assert(memcmp(out, original[i], CELL_SZ)==0);
        checked++;
    }
    printf("  %u blocks verified via DtSlotRegion\n", checked);
    dt_slot_destroy(&region);
    tess_capo_close(&r);
    printf("  PASS\n"); return 0;
}

static int test_offset_helpers(void){
    printf("── offset helpers ──\n");
    assert(tess_moe_block_offset(0,0,4,8)==0);
    assert(tess_moe_block_offset(0,1,4,8)==8);
    assert(tess_moe_block_offset(1,0,4,8)==32);
    assert(tess_moe_block_offset(1,3,4,8)==56); /* (1*4+3)*8 */
    assert(tess_moe_block_offset_interleaved(0,0,0,4,8)==0);
    assert(tess_moe_block_offset_interleaved(0,0,1,4,8)==8);
    assert(tess_moe_block_offset_interleaved(0,1,0,4,8)==24);
    printf("  PASS\n"); return 0;
}

int main(void){
    const char *tmp="test_moe_bridge_tmp.tess";
    printf("═══ .tess ↔ MoE Bridge Tests ═══\n\n");
    assert(write_stack(tmp, 99)==0);
    int fail=0;
    fail+=test_offset_helpers();
    fail+=test_direct_serve(tmp);
    fail+=test_bulk_bake(tmp);
    remove(tmp);
    printf("\n═══ %s ═══\n", fail?"FAIL":"ALL PASS");
    return fail;
}
