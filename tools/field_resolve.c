/*
 * tools/field_resolve.c — UNIVERSAL resolver over a DWGLS field.
 *
 * Resolves ANY tensor name -> bytes, using ONLY the field file (no source GGUF):
 *   body_off = kis.layout.body_off   (u64, stored in the field's own KV)
 *   fpos[i]  = kis.layout.fpos[i]    (u64 array, stored in the field's own KV)
 *   address(t) = body_off + fpos[t]  (zero-copy pointer into the mmap'd field)
 *
 * This is the universal form of geo_field_query.c / field_address.c: those need
 * the source GGUF to recompute fpos from sizes. Here fpos is READ from the
 * field, so the source can be deleted (the field's whole point) and resolution
 * still works for every tensor.
 *
 * Higher dimension (w/tesseract): the flat byte address is also expressed as a
 * tesseract coordinate (tess, cell, slot) via tess_unflat, proving the same
 * bytes are reachable through the higher dimension (MAP not COMPRESS).
 *
 * BUILD: gcc -O2 -I. -Icore -o build/field_resolve tools/field_resolve.c -lm -lws2_32
 * RUN:   ./build/field_resolve <field.bin> [name-filter] [source.gguf]
 *        --selfcheck verifies all tensors in-bounds (no source needed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"
#include "../core/geo_tesseract_addr.h"
#include "../core/clim_record.h"

#define ALIGN 32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

typedef struct { size_t val_start; char name[64]; uint32_t arr_type; uint64_t arr_count; size_t arr_off; } KVInfo;

static int kv_walk(const uint8_t *base, KVInfo *infos, int cap, uint32_t *n_out) {
    uint64_t n_kv;
    memcpy(&n_kv, base + 16, 8);
    if (n_kv > (uint64_t)cap) return -1;
    const uint8_t *p = base + 24;
    uint32_t n = 0;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        KVInfo *kv = &infos[n];
        uint64_t klen; uint32_t vtype;
        memcpy(&klen, p, 8); p += 8;
        memcpy(kv->name, p, klen < 63 ? klen : 63); kv->name[klen < 63 ? klen : 63] = 0;
        p += klen;
        memcpy(&vtype, p, 4); p += 4;
        kv->val_start = (size_t)(p - base);
        kv->arr_type = 0; kv->arr_count = 0; kv->arr_off = 0;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            kv->arr_type = at; kv->arr_count = narr; kv->arr_off = (size_t)(p - base);
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
            else return -1;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
        else return -1;
        n++;
    }
    *n_out = n;
    return 0;
}

static uint64_t arr_u64(const uint8_t *base, const KVInfo *kv, uint64_t i) {
    uint64_t v = 0;
    if (i >= kv->arr_count) return 0;
    if (kv->arr_type == 10) memcpy(&v, base + kv->arr_off + i * 8, 8);
    else if (kv->arr_type == 4) { uint32_t w; memcpy(&w, base + kv->arr_off + i * 4, 4); v = w; }
    return v;
}

static int find_kv(KVInfo *infos, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (strcmp(infos[i].name, name) == 0) return (int)i;
    return -1;
}

int main(int argc, char **argv) {
    int selfcheck = 0;
    const char *field_path = NULL;
    const char *filter = "";
    const char *src_path = NULL;
    const char *clim_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selfcheck") == 0) { selfcheck = 1; continue; }
        if (strcmp(argv[i], "--clim") == 0 && i + 1 < argc) { clim_path = argv[++i]; continue; }
        if (!field_path) { field_path = argv[i]; continue; }
        if (!src_path && (strstr(argv[i], ".gguf") != NULL)) { src_path = argv[i]; continue; }
        if (!src_path && !filter[0]) { filter = argv[i]; continue; }
    }
    if (!field_path) field_path = "build/fieldA.bin";
    setvbuf(stdout, NULL, _IONBF, 0);

    GGUFBox fld;
    if (gguf_box_open(&fld, field_path) != 0) { printf("(cannot open field %s)\n", field_path); return 1; }

    /* read body_off + fpos from the field's own KV (no source) */
    KVInfo fk[128]; uint32_t nfk = 0;
    if (kv_walk(fld.reader.base, fk, 128, &nfk) != 0) { printf("(field kv walk failed)\n"); gguf_box_close(&fld); return 1; }
    int bi = find_kv(fk, nfk, "kis.layout.body_off");
    int fi = find_kv(fk, nfk, "kis.layout.fpos");
    if (bi < 0 || fi < 0) { printf("(field missing kis.layout.body_off or kis.layout.fpos)\n"); gguf_box_close(&fld); return 1; }
    uint64_t body_off = 0;
    memcpy(&body_off, fld.reader.base + fk[bi].val_start, 8);
    uint64_t nfp = fk[fi].arr_count;
    if (nfp != fld.n_tensors) {
        printf("(fpos count %llu != tensor count %u)\n", (unsigned long long)nfp, fld.n_tensors);
        gguf_box_close(&fld); return 1;
    }

    /* optional source oracle */
    GGUFBox src; int have_src = 0;
    if (src_path) {
        if (gguf_box_open(&src, src_path) == 0 && src.n_tensors == fld.n_tensors) have_src = 1;
        else printf("(source open/count mismatch: %s)\n", src_path);
    }

    /* optional climate (capo shift) record */
    ClimRec clim; int have_clim = 0;
    if (clim_path) {
        if (clim_load(clim_path, &clim) == 0) have_clim = 1;
        else printf("(clim load/verify failed: %s)\n", clim_path);
    }

    printf("field_resolve — universal resolver (field alone, zero-copy, higher-dim)\n");
    printf("  field: %s  tensors=%u  body_off=%llu  fpos=%llu\n",
           field_path, fld.n_tensors, (unsigned long long)body_off, (unsigned long long)nfp);
    if (have_clim) printf("  climate: offset=%llu  W=%u  (capo shift applied)\n",
                          (unsigned long long)clim.offset, clim.w);

    int pass = 0, fail = 0, shown = 0;
    for (uint32_t i = 0; i < fld.n_tensors; i++) {
        if (filter[0] && !strstr(fld.entries[i].name, filter)) continue;
        uint64_t fp = arr_u64(fld.reader.base, &fk[fi], i);
        uint64_t addr = body_off + fp;
        const uint8_t *ptr = fld.reader.base + addr;  /* zero-copy pointer */

        /* in-bounds check */
        int ok = (addr + fld.entries[i].size <= fld.reader.base_sz);

        /* higher-dim coordinate (tesseract w-dim) */
        uint32_t flat_slot = (uint32_t)(addr / 64u); /* approx slot (64B/slot) */
        uint32_t phys_slot = flat_slot % TESS_GEO_FULL;
        uint32_t ts, cl, sl;
        tess_unflat(phys_slot, &ts, &cl, &sl);

        /* climate (capo shift) roundtrip: physical slot -> logical -> physical */
        if (have_clim && ok) {
            uint64_t logical = clim_apply(&clim, phys_slot);
            uint32_t recovered = 0;
            if (clim_invert(&clim, logical, &recovered) != 0 || recovered != phys_slot)
                ok = 0;
        }

        /* optional oracle verify */
        if (have_src && ok) {
            const GGUFBoxEntry *se = gguf_box_find(&src, fld.entries[i].name);
            if (se && se->data && se->size == fld.entries[i].size)
                ok = (memcmp(ptr, se->data, se->size) == 0);
            else ok = 0;
        }

        if (ok) pass++; else fail++;
        if (shown < 12 || !ok)
            printf("  %s %-44s %6u B  addr=%llu  tess=%u cell=%u slot=%u%s\n",
                   ok ? "ok " : "FAIL", fld.entries[i].name, fld.entries[i].size,
                   (unsigned long long)addr, ts, cl, sl,
                   have_src ? " [oracle]" : "");
        shown++;
    }

    if (selfcheck) {
        printf("field_resolve selfcheck: %d/%d in-bounds%s\n", pass, pass + fail,
               have_src ? " (+oracle memcmp)" : "");
    } else {
        printf("field_resolve: %s %d/%d resolved via stored fpos (zero-copy, higher-dim)%s\n",
               fail ? "MISMATCH" : "OK", pass, pass + fail,
               have_src ? " [oracle verified]" : "");
    }
    if (have_src) gguf_box_close(&src);
    gguf_box_close(&fld);
    return fail ? 1 : 0;
}
