/*
 * tools/geo_field_query.c — GeoStore-style POSITION query over a DWGLS field.
 *
 * Merge of two lines:
 *   DWGLS field (build/fieldA.bin): self-contained [index header][tensor
 *     chain in inference order][tokenizer windows] — values live here,
 *     source GGUF deletable after bake.
 *   FGLS GeoStore (.gsidx): position-only index — "GGUF stores VALUES,
 *     geometry stores POSITIONS" (FGLS_new session_notes_2026-07-24).
 *
 * Query path: tensor name -> chain position -> bytes straight out of the
 * field mmap. NO llama.cpp, NO source GGUF, NO rebuild.
 *
 * MAP not COMPRESS: positions only, values never transformed.
 * Test integrity: every result memcmp'd against the SOURCE GGUF bytes
 * (independent oracle). Change one byte in the field -> FAIL.
 *
 * BUILD: gcc -O2 -Icore -o build/geo_field_query tools/geo_field_query.c
 * RUN:   ./build/geo_field_query <field.bin> <source.gguf> [substr-filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"

#define ALIGN 32u
#define align32(x) (((x) + (ALIGN - 1)) & ~((uint64_t)(ALIGN - 1)))

/* ── KV walk (verbatim from dual_lazy_serve.c — proven on these files) ── */
typedef struct { size_t start, end, val_start; char name[64]; int is_tok;
                 uint32_t arr_type; uint64_t arr_count; } KVInfo;
static int kv_walk(const uint8_t *base, KVInfo *infos, int cap, uint32_t *n_out) {
    uint64_t n_kv;
    memcpy(&n_kv, base + 16, 8);
    if (n_kv > (uint64_t)cap) return -1;
    const uint8_t *p = base + 24;
    uint32_t n = 0;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        KVInfo *kv = &infos[n];
        kv->start = (size_t)(p - base);
        uint64_t klen; uint32_t vtype;
        memcpy(&klen, p, 8); p += 8;
        memcpy(kv->name, p, klen < 63 ? klen : 63); kv->name[klen < 63 ? klen : 63] = 0;
        p += klen;
        memcpy(&vtype, p, 4); p += 4;
        kv->val_start = (size_t)(p - base);
        kv->is_tok = 0;
        kv->arr_type = 0; kv->arr_count = 0;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            kv->arr_type = at; kv->arr_count = narr;
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
            else return -1;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
        else return -1;
        kv->end = (size_t)(p - base);
        n++;
    }
    *n_out = n;
    return 0;
}

/* ── inference order (verbatim from dual_lazy_serve.c) ── */
static int cat_of(const char *name, unsigned *block) {
    *block = 0;
    if (strncmp(name, "token_embd", 10) == 0) return 0;
    if (strncmp(name, "blk.", 4) == 0) { *block = (unsigned)atoi(name + 4); return 1; }
    if (strncmp(name, "output_norm", 11) == 0) return 2;
    return 3;
}
static void sort_inference(const GGUFBox *box, uint32_t *order, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            unsigned ba = 0, bb = 0;
            int ca = cat_of(box->entries[order[i]].name, &ba);
            int cb = cat_of(box->entries[order[j]].name, &bb);
            int less = (ca < cb) || (ca == cb && (ba < bb || (ba == bb && order[i] < order[j])));
            if (!less) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }
        }
}

int main(int argc, char **argv) {
    const char *field_path = argc > 1 ? argv[1] : "build/fieldA.bin";
    const char *src_path = argc > 2 ? argv[2] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *filter = argc > 3 ? argv[3] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("geo_field_query — GeoStore position query over field windows\n");

    /* source = oracle (values + sizes + offsets from gguf_box reader) */
    GGUFBox src;
    if (gguf_box_open(&src, src_path) != 0) { printf("(cannot open source %s)\n", src_path); return 1; }
    /* field opened as GGUF too (it is GGUF-valid: rebuilt index header) */
    GGUFBox fld;
    if (gguf_box_open(&fld, field_path) != 0) { printf("(cannot open field %s)\n", field_path); return 1; }
    if (src.n_tensors != fld.n_tensors) {
        printf("(tensor count mismatch src=%u field=%u)\n", src.n_tensors, fld.n_tensors);
        return 1;
    }
    uint32_t N = src.n_tensors;

    /* field body_off from kis.layout.body_off */
    uint64_t body_off = 0;
    {
        KVInfo fk[64]; uint32_t nfk = 0;
        if (kv_walk(fld.reader.base, fk, 64, &nfk) != 0) { printf("(field kv walk failed)\n"); return 1; }
        for (uint32_t i = 0; i < nfk; i++)
            if (strcmp(fk[i].name, "kis.layout.body_off") == 0)
                memcpy(&body_off, fld.reader.base + fk[i].val_start, 8);
    }
    if (!body_off) { printf("(no kis.layout.body_off)\n"); return 1; }

    /* chain positions in inference order (same sort as bake) */
    uint32_t *order = (uint32_t *)calloc(N, sizeof(uint32_t));
    uint64_t *fpos = (uint64_t *)calloc(N, sizeof(uint64_t));
    sort_inference(&src, order, N);
    {
        uint64_t cur = 0;
        for (uint32_t r = 0; r < N; r++) {
            uint32_t fi = order[r];
            fpos[fi] = cur;
            cur += align32(src.entries[fi].size);
        }
    }

    /* every tensor: field window bytes vs source oracle bytes */
    int pass = 0, fail = 0, shown = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (filter[0] && !strstr(src.entries[i].name, filter)) continue;
        const GGUFBoxEntry *se = &src.entries[i];
        const uint8_t *fp = fld.reader.base + body_off + fpos[i];
        int ok = se->data &&
                 (body_off + fpos[i] + se->size <= fld.reader.base_sz) &&
                 memcmp(fp, se->data, se->size) == 0;
        if (ok) pass++; else fail++;
        if (shown < 8 || !ok)
            printf("  %s %-48s %u B @+%llu\n", ok ? "ok " : "FAIL", se->name, se->size,
                   (unsigned long long)(body_off + fpos[i]));
        shown++;
    }
    printf("geo_field_query: %s %d/%d tensors field==source%s%s\n",
           fail ? "MISMATCH" : "IDENTICAL", pass, pass + fail,
           filter[0] ? " filter=" : "", filter[0] ? filter : "");
    printf("  positions live in chain order; values untouched (MAP not COMPRESS)\n");
    gguf_box_close(&src);
    gguf_box_close(&fld);
    free(order); free(fpos);
    return fail ? 1 : 0;
}
