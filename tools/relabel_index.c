/* tools/relabel_index.c — re-anchor an index under a GIVEN table (no training).
 *
 * THE re-label experiment (handoff 2026-10-10): entries carry TRAINED
 * integers, so any other table's walk is a label lottery. This tool assigns
 * every entry's anchor/anchor2 under the new table (anch_route top-2),
 * rebuilds the bucket-major placement perm (anch_perm), and rewrites the
 * index lines with the new labels — floats byte-identical (only the label
 * span is patched, vec text untouched).
 *
 * Refuses to relabel across growth (table ntr != index lines): entry-set
 * changes are ann-sync's job, not this tool's.
 *
 * Usage: relabel_index table.bin in.jsonl out.jsonl out.order
 * Receipt: n, K, bucket histogram -> stderr. EXIT 0 clean, 1 on any error.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/relabel_index.exe tools/relabel_index.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "anchor_route.h"

#ifndef ANCHR_MAXK
#define ANCHR_MAXK 64
#endif
#ifndef ANCHR_MAXD
#define ANCHR_MAXD 1024
#endif

static char *xgetline(FILE *f, size_t *len_out) {
    size_t cap = 8192, len = 0;
    char *b = (char *)malloc(cap);
    if (!b) return NULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(b, cap);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
        b[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) { free(b); return NULL; }
    b[len] = '\0';
    if (len_out) *len_out = len;
    return b;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s table.bin in.jsonl out.jsonl out.order\n", argv[0]);
        return 1;
    }
    const char *tpath = argv[1], *ipath = argv[2], *opath = argv[3], *ppath = argv[4];

    static float C[(size_t)ANCHR_MAXK * ANCHR_MAXD];
    int tdim = 0, tntr = 0;
    int K = anch_load(tpath, C, ANCHR_MAXK, ANCHR_MAXD, &tdim, &tntr);
    if (K <= 0 || tdim <= 0 || tdim > ANCHR_MAXD) {
        fprintf(stderr, "relabel: bad table %s (K=%d dim=%d)\n", tpath, K, tdim);
        return 1;
    }

    FILE *fi = fopen(ipath, "rb");
    if (!fi) { fprintf(stderr, "relabel: cannot open %s\n", ipath); return 1; }

    /* pass 1: count lines + label each (offsets recorded, lines re-read) */
    long *offs = NULL, *lens = NULL;
    size_t ncap = 0, n = 0;
    int *rows = NULL, *rows2 = NULL;
    float *vec = (float *)malloc((size_t)tdim * sizeof(float));
    if (!vec) { fclose(fi); return 1; }
    size_t pos = 0;
    for (;;) {
        long off = (long)pos;
        size_t ll = 0;
        char *line = xgetline(fi, &ll);
        if (!line) break;
        pos += ll;
        if (ll <= 2) { free(line); continue; } /* blank */
        if (n >= ncap) {
            size_t nc = ncap ? ncap * 2 : 65536;
            long *a = (long *)realloc(offs, nc * sizeof(long));
            long *b = (long *)realloc(lens, nc * sizeof(long));
            int *c = (int *)realloc(rows, nc * sizeof(int));
            int *d = (int *)realloc(rows2, nc * sizeof(int));
            if (!a || !b || !c || !d) {
                fprintf(stderr, "relabel: OOM at %u\n", (unsigned)n);
                free(line); fclose(fi); return 1;
            }
            offs = a; lens = b; rows = c; rows2 = d; ncap = nc;
        }
        /* parse vec */
        const char *vp = strstr(line, "\"vec\":[");
        if (!vp) { fprintf(stderr, "relabel: line %u has no vec\n", (unsigned)n); free(line); fclose(fi); return 1; }
        vp += 7;
        char *ep = NULL;
        int j;
        for (j = 0; j < tdim; j++) {
            double v = strtod(vp, &ep);
            if (ep == vp) break;
            vec[j] = (float)v;
            vp = ep;
            if (*vp == ',') vp++;
            else if (*vp == ']') { j++; break; }
            else break;
        }
        if (j != tdim) {
            fprintf(stderr, "relabel: line %u vec has %d want %d\n", (unsigned)n, j, tdim);
            free(line); fclose(fi); return 1;
        }
        int t2[64];
        int nb = anch_route(vec, C, K, tdim, 2, t2);
        offs[n] = off; lens[n] = (long)ll;
        rows[n] = nb > 0 ? t2[0] : -1;
        rows2[n] = nb > 1 ? t2[1] : -1;
        n++;
        free(line);
    }
    fprintf(stderr, "relabel: table K=%d dim=%d ntr=%d | index lines=%u\n", K, tdim, tntr, (unsigned)n);
    if (tntr != (int)n) {
        fprintf(stderr, "relabel: REFUSE — table ntr %d != index n %u (growth is sync's job)\n", tntr, (unsigned)n);
        fclose(fi); return 1;
    }

    uint32_t *perm = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    if (!perm) { fclose(fi); return 1; }
    if (anch_perm(rows, (int)n, K, perm) != 0) {
        fprintf(stderr, "relabel: anch_perm failed\n"); fclose(fi); return 1;
    }
    FILE *pf = fopen(ppath, "wb");
    if (!pf) { fprintf(stderr, "relabel: cannot write %s\n", ppath); fclose(fi); return 1; }
    if (n && fwrite(perm, sizeof(uint32_t), n, pf) != n) {
        fprintf(stderr, "relabel: order short write\n"); fclose(pf); fclose(fi); return 1;
    }
    fclose(pf);

    /* pass 2: patch label span, vec bytes untouched */
    FILE *fo = fopen(opath, "wb");
    if (!fo) { fprintf(stderr, "relabel: cannot write %s\n", opath); fclose(fi); return 1; }
    long hist[64] = {0};
    for (size_t i = 0; i < n; i++) {
        fseek(fi, offs[i], SEEK_SET);
        char *line = (char *)malloc((size_t)lens[i] + 64);
        if (!line) { fclose(fo); fclose(fi); return 1; }
        if (fread(line, 1, (size_t)lens[i], fi) != (size_t)lens[i]) {
            fprintf(stderr, "relabel: reread fail %u\n", (unsigned)i); fclose(fo); fclose(fi); return 1;
        }
        line[lens[i]] = '\0';
        char *dimpos = strstr(line, ",\"dim\":");
        if (!dimpos) { fprintf(stderr, "relabel: line %u has no dim\n", (unsigned)i); fclose(fo); fclose(fi); return 1; }
        char *ap = strstr(line, "\"anchor\":");
        int had_old = (ap && ap < dimpos);
        char *wstart = had_old ? ap : dimpos;
        /* prefix ends at wstart; old-label case leaves a trailing comma (good),
         * fresh-index case ends with the npast value (needs a comma). */
        *wstart = '\0';
        /* dimpos starts with ,"dim": — skip the comma since we add our own */
        if (had_old)
            fprintf(fo, "%s\"anchor\":%d,\"anchor2\":%d%s", line, rows[i], rows2[i], dimpos);
        else
            fprintf(fo, "%s,\"anchor\":%d,\"anchor2\":%d%s", line, rows[i], rows2[i], dimpos + 1);
        if (rows[i] >= 0 && rows[i] < 64) hist[rows[i]]++;
        free(line);
    }
    fclose(fo);
    fclose(fi);
    free(offs); free(lens); free(rows); free(rows2); free(perm); free(vec);
    fprintf(stderr, "relabel: wrote %s + %s buckets:", opath, ppath);
    for (int k = 0; k < K; k++) fprintf(stderr, " %ld", hist[k]);
    fprintf(stderr, "\nrelabel: OK\n");
    return 0;
}
