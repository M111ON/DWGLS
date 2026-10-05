/*
 * tools/field_address.c — list every tensor's ADDRESS inside a DWGLS field.
 *
 * Why: geo_field_query.c answers "name -> chain position -> bytes" but needs
 * BOTH the field AND the source GGUF (it memcmps against the source as an
 * oracle). Once the source GGUF is deleted (the field's whole point) that
 * tool can only count-check and exit. This tool reads the field ALONE —
 * the addresses are already stored inside it:
 *
 *   kis.layout.body_off   u64   where the data body begins
 *   kis.layout.fpos[]     array chain position of each tensor, in the SAME
 *                               inference order the bake used
 *   tensor table (GGUF)         name + size, in file order
 *
 * address(t) = body_off + fpos[t];  display = address and slot span.
 * Nothing is transformed here; this is a read-only VIEW (MAP not COMPRESS).
 *
 * BUILD: gcc -O2 -Icore -o build/field_address tools/field_address.c
 * RUN:   ./build/field_address <field.bin> [name-substring-filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"

#define ALIGN 32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

/* ── KV walk (same walker as geo_field_query.c / dual_lazy_serve.c) ──
 * arr_ptr is stored as an OFFSET from base, not a raw pointer — the field
 * mmap base is fixed for the lifetime of the walk, and offsets survive the
 * infos[] shuffle that invalidates intra-array pointers. */
typedef struct { size_t val_start; char name[64]; uint32_t arr_type; uint64_t arr_count;
                 size_t arr_off; } KVInfo;
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

int main(int argc, char **argv) {
    const char *field_path = argc > 1 ? argv[1] : "build/fieldA.bin";
    const char *filter = argc > 2 ? argv[2] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("field_address — tensor -> ADDRESS inside the field (field alone, no source GGUF)\n");

    GGUFBox fld;
    if (gguf_box_open(&fld, field_path) != 0) { printf("(cannot open field %s)\n", field_path); return 1; }
    uint32_t N = fld.n_tensors;

    KVInfo fk[128]; uint32_t nfk = 0;
    if (kv_walk(fld.reader.base, fk, 96, &nfk) != 0) { printf("(field kv walk failed)\n"); return 1; }

    uint64_t body_off = 0, ver = 0;
    const KVInfo *fpos_kv = NULL;
    for (uint32_t i = 0; i < nfk; i++) {
        if (strcmp(fk[i].name, "kis.layout.body_off") == 0) memcpy(&body_off, fld.reader.base + fk[i].val_start, 8);
        if (strcmp(fk[i].name, "kis.format.version") == 0) memcpy(&ver, fld.reader.base + fk[i].val_start, 8);
        if (strcmp(fk[i].name, "kis.layout.fpos") == 0) fpos_kv = &fk[i];
    }
    if (!body_off) { printf("(no kis.layout.body_off — not a lazy-serve field)\n"); return 1; }
    printf("field: %s   v%I64u   tensors=%u   body_off=%I64u   fpos[]=%s\n\n",
           field_path, (unsigned long long)ver, N, (unsigned long long)body_off,
           (fpos_kv && fpos_kv->arr_count >= N) ? "present" : "absent");

    /* fpos[] is indexed by FILE tensor index directly — proven by
     * verify_field.c (291/291) and frustum_real.c (384/384). A chain-order
     * sort was WRONG: it printed addresses for the wrong tensor. */
    int shown = 0;
    for (uint32_t t = 0; t < N; t++) {
        const char *name = fld.entries[t].name;
        if (filter[0] && !strstr(name, filter)) continue;
        uint64_t fpos = fpos_kv ? arr_u64(fld.reader.base, fpos_kv, t) : 0;
        uint64_t addr = body_off + fpos;
        uint64_t sz = fld.entries[t].size;
        printf("  tensor[%3u]  %-44s %10I64u B   addr=%-12I64u body+%-12I64u  region=%-10I64u\n",
               t, name, (unsigned long long)sz, (unsigned long long)addr,
               (unsigned long long)fpos, (unsigned long long)align32(sz));
        shown++;
    }
    printf("\nfield_address: %d tensor(s) shown of %u; address = body_off + fpos[file_idx]\n", shown, N);
    printf("  addresses are STORED in the field (kis.layout.fpos); no source GGUF needed\n");
    gguf_box_close(&fld);
    return 0;
}
