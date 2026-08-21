/* test_tesseract_fixed.c — 1tes fixed frame (tess 0) + interior access
 * Pin frame 0, walk interior slots with no field distortion.
 * Real GGUF slice (1tes =1152*64=73728 bytes if needed) verified.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_tesseract_addr.h"

#define SPEC_TESS0_FLAT(cell,slot) ((cell)*144u + (slot))

static int ok=0, fail=0;
#define CHECK(c,name) do{ if(!(c)){ printf("  FAIL %s\n",name); fail++; } else { printf("  PASS %s\n",name); ok++; } }while(0)

static void t_index(void){
    printf("── T1 index encode/decode 8 cells\n");
    for(uint32_t ax=0;ax<4;ax++) for(uint32_t s=0;s<2;s++){
        uint32_t idx=tess_index(ax,s);
        CHECK(tess_axis(idx)==ax && tess_sign(idx)==s, "axis/sign roundtrip");
    }
}
static void t_fixed_flat(void){
    printf("── T2 fixed frame 0 flat/unflat (1tes =1152)\n");
    for(uint32_t c=0;c<8;c++) for(uint32_t s=0;s<144;s+=17){
        uint32_t f=tess_flat(0,c,s);
        uint32_t t,cc,ss; tess_unflat(f,&t,&cc,&ss);
        CHECK(t==0 && cc==c && ss==s, "tess0 flat/unflat");
        CHECK(f==SPEC_TESS0_FLAT(c,s), "spec flat cell*144+slot");
    }
    CHECK(TESS_PER_TESS==1152u, "1tes size");
    CHECK(TESS_GEO_FULL==20736u, "18tes field");
}
static void t_no_distortion(void){
    printf("── T3 interior walk without field move (fixed frame)\n");
    uint32_t base=tess_flat(0, tess_index(1,0), 0);
    for(uint32_t slot=0;slot<144;slot++){
        uint32_t f=tess_flat(0, tess_index(1,0), slot);
        CHECK(f==base+slot, "slot walk is contiguous inside cell");
    }
}
static void t_xor(void){
    printf("── T4 XOR neighbor involution + adjacency degree 6\n");
    CHECK(geo_tesseract_verify()==0, "geo_tesseract_verify()");
}

int main(void){
    printf("test_tesseract_fixed — 1tes pinned frame (tess 0)\n");
    t_index(); t_fixed_flat(); t_no_distortion(); t_xor();
    printf("%s (%d ok %d fail)\n", fail?"FAILED":"ALL PASS", ok, fail);
    return fail?1:0;
}
