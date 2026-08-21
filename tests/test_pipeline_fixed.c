/* test_pipeline_fixed.c — wired 1tes fixed frame → GeoFS → hyperbolic scatter
 * 6 cells (2..7) avoid header 0..255, each holds up to 144 blocks (9KB).
 * Real GGUF slices (header + weights) via fixed cell bases, RDH integrity.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_pipeline_fixed.h"
#define rdh_decompose rdh_capture_rdh_decompose
#include "../collection/rdh/rdh_capture.h"
#undef rdh_decompose

static int ok=0, fail=0;
#define CHECK(c,name) do{ if(!(c)){ printf("  FAIL %s\n",name); fail++; } else { printf("  PASS %s\n",name); ok++; } }while(0)
static int64_t rdh_of(const uint8_t *p,size_t n){ return rdh_capture(p,n,&RDH_CAPTURE_144); }

#ifdef _WIN32
#define FS_FSEEK _fseeki64
#define FS_FTELL _ftelli64
#else
#define FS_FSEEK fseeko
#define FS_FTELL ftello
#endif

static long read_at(const char *path, uint8_t *buf, int64_t off, size_t want){
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    FS_FSEEK(f,0,SEEK_END); int64_t sz=FS_FTELL(f);
    if(off+ (int64_t)want>sz) off=sz-want;
    FS_FSEEK(f,off,SEEK_SET); size_t g=fread(buf,1,want,f); fclose(f); return (long)g;
}

int main(void){
    printf("test_pipeline_fixed — 1tes wired (tess 0, cells 2..7)\n");
    CHECK(pipe_verify()==0, "pipe_verify");
    CHECK(geo_tesseract_verify()==0, "tesseract verify");

    GeosVolume vol; geos_volume_init(&vol);
    const char *gguf="I:\\DWGLS\\build\\qwen05-direct.gguf";
    uint8_t *hdr=(uint8_t*)malloc(PIPE_SLOTS*GEOS_BLOCK_SZ*6);
    uint8_t *wgt=(uint8_t*)malloc(PIPE_SLOTS*GEOS_BLOCK_SZ*6);
    if(!hdr||!wgt){ printf("malloc fail\n"); return 1; }
    long hg=read_at(gguf,hdr,0, PIPE_SLOTS*GEOS_BLOCK_SZ*6);
    long wg=read_at(gguf,wgt, 0,0); // dummy to get size
    { FILE *f=fopen(gguf,"rb"); FS_FSEEK(f,0,SEEK_END); int64_t sz=FS_FTELL(f); fclose(f); wg=read_at(gguf,wgt, sz/2, PIPE_SLOTS*GEOS_BLOCK_SZ*6); }
    printf("  slices: header %ld bytes, weights %ld bytes\n", hg, wg);

    /* place 6 cells contiguous via fixed frame */
    for(uint32_t c=2;c<8;c++){
        char name[16]; snprintf(name,sizeof(name),"cell%u.bin",c);
        uint8_t *src=(c%2==0)? hdr+(c-2)*PIPE_SLOTS*GEOS_BLOCK_SZ : wgt+(c-2)*PIPE_SLOTS*GEOS_BLOCK_SZ;
        GeosInode *in=pipe_place_cell(&vol,name,src, PIPE_SLOTS*GEOS_BLOCK_SZ, c);
        CHECK(in!=NULL, name);
        if(in) CHECK(in->block_start==pipe_cell_base(c), "base == tess_flat");
    }
    /* read back via flat and via geos_read, compare + RDH */
    for(uint32_t c=2;c<8;c++){
        char name[16]; snprintf(name,sizeof(name),"cell%u.bin",c);
        uint8_t *rb=(uint8_t*)malloc(PIPE_SLOTS*GEOS_BLOCK_SZ);
        int got=geos_read(&vol,name,rb, PIPE_SLOTS*GEOS_BLOCK_SZ);
        uint8_t *src=(c%2==0)? hdr+(c-2)*PIPE_SLOTS*GEOS_BLOCK_SZ : wgt+(c-2)*PIPE_SLOTS*GEOS_BLOCK_SZ;
        CHECK(got==(int)(PIPE_SLOTS*GEOS_BLOCK_SZ) && memcmp(src,rb,PIPE_SLOTS*GEOS_BLOCK_SZ)==0, "readback lossless");
        CHECK(rdh_of(src,PIPE_SLOTS*GEOS_BLOCK_SZ)==rdh_of(rb,PIPE_SLOTS*GEOS_BLOCK_SZ), "RDH same (no FNV-1a)");
        /* scatter inside cell: stride 27 should stay inside cell window */
        for(uint32_t b=0;b<10;b++){
            uint32_t flat=pipe_scatter_in_cell(c,3,b);
            CHECK(flat>=pipe_cell_base(c) && flat<pipe_cell_base(c)+PIPE_SLOTS, "scatter stays in cell");
        }
        free(rb);
    }
    CHECK(vol.total_blocks_free == (20736-256) - 6*PIPE_SLOTS, "free count 6 cells");

    geos_volume_free(&vol); free(hdr); free(wgt);
    printf("%s (%d ok %d fail)\n", fail?"FAILED":"ALL PASS", ok, fail);
    return fail?1:0;
}
