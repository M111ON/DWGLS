/* tests/test_clim_record.c — CLIM climate-offset record.
 *
 * Oracles: struct size by sizeof (32B packing); FNV recomputed inline;
 * apply/invert against u64 arithmetic (definition, not the impl);
 * W bound from the shared-scale constraint (144, cf. SBR_RING).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_clim_record tests/test_clim_record.c
 * RUN:   ./build/test_clim_record
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "clim_record.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

/* independent FNV-1a over 28B (recomputed here, not via clim_fnv) */
static uint32_t ref_fnv(const ClimRec *r) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)r;
    for (int i = 0; i < 28; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int main(void) {
    printf("test_clim_record\n");
    check(sizeof(ClimRec) == 32, "record is 32B packed");

    ClimRec r;
    check(clim_init(&r, 0) == 0 && r.offset == 0 && !(r.flags & CLIM_FLAG_SLIDING),
          "init home W=0, sliding clear");
    check(r.cksum == ref_fnv(&r), "seal matches independent FNV");
    check(clim_verify(&r) == 0, "verify clean");

    /* W bound: shared scale only */
    check(clim_init(&r, 143) == 0, "W=143 accepted");
    check(clim_init(&r, 144) == -1, "W=144 rejected");

    /* slide: offset accumulates, flag sets, reseal stays valid */
    clim_init(&r, 12);
    clim_slide(&r, 20736u);
    clim_slide(&r, 1u);
    check(r.offset == 20737u && (r.flags & CLIM_FLAG_SLIDING) && clim_verify(&r) == 0,
          "slide 20736+1 accumulates + reseals");
    /* u64 unbounded: slide past 4G without wrap */
    clim_slide(&r, 0xFFFFFFFFull);
    check(r.offset == 20737ull + 0xFFFFFFFFull && clim_verify(&r) == 0, "offset u64 unbounded");

    /* apply/invert vs u64 arithmetic */
    clim_init(&r, 0);
    clim_slide(&r, 5u);
    check(clim_apply(&r, 0) == 5u && clim_apply(&r, 20735) == 20740u, "apply p=s+k");
    uint32_t s = 0;
    check(clim_invert(&r, 5u, &s) == 0 && s == 0, "invert window start");
    check(clim_invert(&r, 20740u, &s) == 0 && s == 20735, "invert window end");
    check(clim_invert(&r, 4u, NULL) == -1, "invert below window rejected");
    check(clim_invert(&r, 20741u, NULL) == -1, "invert above window rejected");

    /* unit slide A+1,B+1: [0,20736) -> [1,20737), whole window moves. */
    clim_init(&r, 0);
    clim_slide(&r, 1u);
    check(clim_apply(&r, 0) == 1u && clim_apply(&r, 20735) == 20736u, "unit slide apply");
    check(clim_invert(&r, 0u, NULL) == -1, "old start falls out");
    check(clim_invert(&r, 20737u, NULL) == -1, "past new end rejected");
    check(clim_invert(&r, 20736u, &s) == 0 && s == 20735, "new end inverts");

    /* tamper: any byte → -2; bad magic → -1 */
    ClimRec t = r;
    ((unsigned char *)&t)[10] ^= 0xFF;
    check(clim_verify(&t) == -2, "1-byte tamper rejected");
    t = r;
    t.magic = 0;
    check(clim_verify(&t) == -1, "bad magic rejected");
    t = r;
    t.w = 200;
    t.cksum = ref_fnv(&t);
    check(clim_verify(&t) == -3, "out-of-range W rejected");

    /* persist roundtrip + tampered file */
    const char *p = "build/test_clim_tmp.bin";
    clim_init(&r, 7);
    r.head = 1234;
    r.router = 0xA5A5;
    r.cksum = clim_fnv(&r);
    check(clim_save(p, &r) == 0, "save ok");
    ClimRec l;
    check(clim_load(p, &l) == 0 && memcmp(&l, &r, 32) == 0, "load roundtrip identical");
    FILE *f = fopen(p, "r+b");
    fseek(f, 8 + 20, SEEK_SET);
    int b = fgetc(f);
    fseek(f, 8 + 20, SEEK_SET);
    fputc(b ^ 0x01, f);
    fclose(f);
    check(clim_load(p, &l) == -2, "tampered file rejected");
    check(clim_load("build/no_such_clim.bin", &l) == -1, "missing file -1");
    remove(p);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
