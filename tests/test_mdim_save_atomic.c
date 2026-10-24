#include "geofs_mdim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    static uint8_t buf[MDIM_VOL_BYTES];
    MdimVolume v, w;
    mdim_volume_init(&v, buf);
    remove("build/a.vol");
    if (mdim_volume_save(&v, "build/a.vol") != MDIM_OK) { printf("FAIL create\n"); return 1; }
    if (mdim_volume_save(&v, "build/a.vol") != MDIM_OK) { printf("FAIL replace\n"); return 1; }
    if (mdim_volume_load(&w, "build/a.vol") != MDIM_OK) { printf("FAIL load\n"); return 1; }
    mdim_volume_free(&w);
    FILE *t = fopen("build/a.vol.tmp", "rb");
    if (t) { fclose(t); printf("FAIL tmp leftover\n"); return 1; }
    if (mdim_volume_save(&v, "/nonexistent/x.vol") == MDIM_OK) { printf("FAIL bad path\n"); return 1; }
    mdim_volume_free(&v);
    printf("ok\n");
    return 0;
}
