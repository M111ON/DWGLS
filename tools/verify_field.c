/*
 * tools/verify_field.c — PROVE the field's stored addresses point at the
 * right bytes. This is the end-to-end check the "field is self-describing"
 * claim needs: read an ADDRESS out of the field alone, fetch the bytes there,
 * and compare against the source GGUF — every tensor, byte for byte.
 *
 * Why a separate tool: field_address.c only SHOWS addresses. It cannot tell a
 * correct address from a plausible one. This tool supplies the missing oracle
 * by pairing the field with its source GGUF (available at bake time) and
 * doing a straight memcmp through the address, per tensor name.
 *
 *   field:  kis.layout.fpos[fi]  -> body position (file-idx order, v2)
 *           kis.layout.body_off  -> start of the data body
 *           address(t) = body_off + fpos[file_idx(t)]
 *   source: tensor bytes located by NAME via the GGUF tensor table.
 *
 * A tensor is PASS when all `size` bytes at the field address equal the source
 * tensor bytes. Any mismatch is reported with the first differing byte offset
 * so a layout bug is localizable, not just a count.
 *
 * BUILD: gcc -O2 -Icore -o build/verify_field tools/verify_field.c
 * RUN:   ./build/verify_field <field.bin> <source.gguf> [name-filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"

/* ── KV walk: arr_off is an OFFSET from base (survives infos[] shuffle). ── */
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

/* find a tensor by exact name in a GGUFBox; returns index or -1 */
static int find_tensor(const GGUFBox *b, const char *name) {
    for (uint32_t i = 0; i < b->n_tensors; i++)
        if (strcmp(b->entries[i].name, name) == 0) return (int)i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: verify_field <field.bin> <source.gguf> [name-filter]\n");
        return 2;
    }
    const char *field_path = argv[1];
    const char *src_path   = argv[2];
    const char *filter     = argc > 3 ? argv[3] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("verify_field — address(field) -> bytes -> memcmp(source)\n\n");

    GGUFBox fld, src;
    if (gguf_box_open(&fld, field_path) != 0) { printf("(cannot open field %s)\n", field_path); return 1; }
    if (gguf_box_open(&src, src_path)  != 0) { printf("(cannot open source %s)\n", src_path); return 1; }

    KVInfo fk[128]; uint32_t nfk = 0;
    if (kv_walk(fld.reader.base, fk, 128, &nfk) != 0) { printf("(field kv walk failed)\n"); return 1; }

    uint64_t body_off = 0, ver = 0;
    const KVInfo *fpos_kv = NULL;
    for (uint32_t i = 0; i < nfk; i++) {
        if (strcmp(fk[i].name, "kis.layout.body_off") == 0) memcpy(&body_off, fld.reader.base + fk[i].val_start, 8);
        if (strcmp(fk[i].name, "kis.format.version") == 0) memcpy(&ver, fld.reader.base + fk[i].val_start, 8);
        if (strcmp(fk[i].name, "kis.layout.fpos") == 0) fpos_kv = &fk[i];
    }
    if (!body_off) { printf("(no kis.layout.body_off — not a lazy-serve field)\n"); return 1; }
    if (!fpos_kv)  { printf("(no kis.layout.fpos — field predates format v2; bake again)\n"); return 1; }

    const uint8_t *fb = fld.reader.base;

    uint32_t N = fld.n_tensors;
    int pass = 0, fail = 0, missing = 0, filt = 0;
    uint64_t bytes_ok = 0;

    printf("field : %s  v%I64u  tensors=%u  body_off=%I64u\n", field_path,
           (unsigned long long)ver, N, (unsigned long long)body_off);
    printf("source: %s  tensors=%u\n\n", src_path, src.n_tensors);

    for (uint32_t i = 0; i < N; i++) {
        const char *name = fld.entries[i].name;
        if (filter[0] && !strstr(name, filter)) continue;
        filt++;

        uint64_t fpos = arr_u64(fb, fpos_kv, i);
        uint64_t addr = body_off + fpos;
        uint64_t sz   = fld.entries[i].size;

        int si = find_tensor(&src, name);
        if (si < 0) { printf("  MISSING  %-44s (%I64u B) — not in source\n", name, (unsigned long long)sz); missing++; continue; }

        uint64_t ssz = src.entries[si].size;
        if (ssz != sz) {
            printf("  SIZE     %-44s field=%I64u src=%I64u\n", name,
                   (unsigned long long)sz, (unsigned long long)ssz);
            fail++; continue;
        }
        /* bounds check inside the field file */
        if (addr + sz > fld.reader.base_sz) {
            printf("  OOB      %-44s addr=%I64u sz=%I64u > file=%zu\n", name,
                   (unsigned long long)addr, (unsigned long long)sz, fld.reader.base_sz);
            fail++; continue;
        }
        const uint8_t *fd = fb + addr;
        const uint8_t *sd = (const uint8_t *)src.entries[si].data;
        if (!sd) { printf("  NODATA   %-44s — source pointer null\n", name); fail++; continue; }

        if (memcmp(fd, sd, (size_t)sz) == 0) {
            pass++; bytes_ok += sz;
        } else {
            uint64_t d = 0;
            while (d < sz && fd[d] == sd[d]) d++;
            printf("  DIFF     %-44s addr=%I64u first_diff=%I64u\n", name,
                   (unsigned long long)addr, (unsigned long long)d);
            fail++;
        }
    }

    printf("\nverify_field: %d PASS, %d FAIL, %d MISSING  (filtered %d of %u)\n",
           pass, fail, missing, filt, N);
    printf("  bytes proven at stored addresses: %I64u (%.1f MB)\n",
           (unsigned long long)bytes_ok, (double)bytes_ok / (1024.0 * 1024.0));
    printf("  %s\n", (fail == 0 && missing == 0)
           ? "RESULT: every field address resolves to the correct source bytes"
           : "RESULT: address/byte mismatch — layout is NOT trustworthy");

    gguf_box_close(&fld);
    gguf_box_close(&src);
    return (fail == 0 && missing == 0) ? 0 : 1;
}
