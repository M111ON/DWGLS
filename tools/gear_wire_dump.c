/*
 * gear_wire_dump.c — validate + dump GHST+gear-wire containers
 * ══════════════════════════════════════════════════════════════
 *
 * SELF-CONTAINMENT PROOF: includes ONLY gear_wire_bridge.h
 * (plus <stdio.h> / <stdlib.h>).  Building this file proves
 * that the bridge works as a standalone interop header.
 *
 * Usage: gear_wire_dump <file> [--json]
 *   no flags  → human-readable report
 *   --json    → canonical JSON output (machine-parseable)
 *
 * BUILD: make gear-dump
 *   gcc -O2 -Wall -Icore -o build/gear_wire_dump tools/gear_wire_dump.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SELF-CONTAINED: only the bridge header (no DWGLS deps) ──────────── */
#include "../core/gear_wire_bridge.h"

static const char *errstr(int rc) {
    switch (rc) {
    case GWB_OK:     return "ok";
    case GWB_E_BADARG: return "bad argument";
    case GWB_E_SMALL:  return "buffer too small";
    case GWB_E_MAGIC:  return "bad magic (not GHST)";
    case GWB_E_VER:    return "unsupported version";
    case GWB_E_COUNT:  return "entry count out of range";
    case GWB_E_TRUNC:  return "truncated (entries past buffer)";
    case GWB_E_WIRE:   return "seal-accounting mismatch";
    default:           return "unknown error";
    }
}

int main(int argc, char **argv) {
    int do_json = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) do_json = 1;
        else if (argv[i][0] != '-') path = argv[i];
        else { fprintf(stderr, "Unknown flag: %s\n", argv[i]); return 1; }
    }
    if (!path) {
        fprintf(stderr, "Usage: gear_wire_dump <file> [--json]\n");
        return 1;
    }

    /* read file */
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fprintf(stderr, "%s: empty\n", path); fclose(f); return 1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fprintf(stderr, "out of memory\n"); fclose(f); return 1; }
    if ((long)fread(buf, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "%s: read error\n", path); free(buf); fclose(f); return 1;
    }
    fclose(f);

    /* parse */
    gwb_view v;
    int rc = gwb_parse(buf, (uint64_t)fsize, &v);
    if (rc != GWB_OK) {
        fprintf(stderr, "%s: parse error: %s (%d)\n", path, errstr(rc), rc);
        free(buf);
        return 1;
    }

    /* validate */
    int vrc = gwb_validate(&v);

    if (do_json) {
        /* JSON output */
        uint64_t cap = 1u << 20;  /* 1 MB */
        char *jbuf = (char *)malloc(cap);
        if (!jbuf) { fprintf(stderr, "out of memory\n"); free(buf); return 1; }
        uint64_t jlen = gwb_json(&v, jbuf, cap);
        if (jlen == 0) {
            fprintf(stderr, "%s: JSON output too large for buffer\n", path);
            free(jbuf); free(buf);
            return 1;
        }
        fwrite(jbuf, 1, jlen, stdout);
        putchar('\n');
        free(jbuf);
    } else {
        /* human-readable report */
        printf("File: %s  (%ld bytes)\n", path, fsize);
        printf("Format: GHST+gear-wire v1\n");
        printf("Entries: %u  (geared: ", v.count);
        uint32_t geared = 0, seals = 0;
        for (uint32_t i = 0; i < v.count; i++) {
            gwb_entry e;
            gwb_entry_get(&v, i, &e);
            if (e.flags & GWB_FLAG_GEAR) geared++;
        }
        printf("%u, seals: ", geared);
        for (uint32_t k = 0; k < v.wire_len; k++)
            if (v.wire[k] == GWB_SEAL) seals++;
        printf("%u)\n", seals);
        printf("Wire: %u bytes\n", v.wire_len);
        printf("Validation: %s (%d)\n", errstr(vrc), vrc);

        /* per-block chains */
        printf("\n--- Records ---\n");
        for (uint32_t i = 0; i < v.count; i++) {
            gwb_entry e;
            gwb_entry_get(&v, i, &e);
            printf("  [%u] block=%u  from=%u→to=%u  flags=",
                   i, e.block_id, e.from_scale, e.to_scale);
            if (e.flags & GWB_FLAG_GEAR) printf("G");
            if (e.flags & 0x01) printf("L");
            if (e.flags & 0x02) printf("E");
            if (e.flags & 0x04) printf("D");
            if (!(e.flags & 0x0F)) printf(".");
            putchar('\n');
        }

        /* decoded wire events */
        printf("\n--- Wire events ---\n");
        for (uint32_t k = 0; k < v.wire_len; k++) {
            uint8_t b = v.wire[k];
            int own = gwb_wire_owner(&v, k);
            printf("  [%u] byte=0x%02X  owner_block=", k, b);
            if (own < 0) printf("??");
            else printf("%d", own);
            if (b == GWB_SEAL) {
                printf("  SEAL\n");
            } else {
                uint8_t q, dc, dx;
                gwb_decode(b, &q, &dc, &dx);
                printf("  q=%u dc=%u dx=%u  delta=%u\n",
                       q, dc, dx, gwb_delta(q, dc, dx));
            }
        }
    }

    free(buf);
    return (vrc != GWB_OK) ? 1 : 0;
}
