/*
 * test_rdh_large.c — test RDH capture with large files (unlimited unlock)
 * Build: gcc -I../collection/rdh test_rdh_large.c -o test_rdh_large
 * Run:   test_rdh_large <file>
 *        or: test_rdh_large (generates 10MB test data)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "rdh_capture.h"

int main(int argc, char **argv)
{
    RDHConfig cfg = RDH_CAPTURE_144;
    
    if (argc > 1) {
        /* ── Read real file ── */
        const char *path = argv[1];
        FILE *fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 1; }
        
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        rewind(fp);
        
        printf("File: %s  (%ld bytes)\n", path, fsize);
        
        /* Process in large chunks */
        uint8_t *buf = (uint8_t*)malloc(fsize > 0 ? fsize : 1);
        size_t nread = fread(buf, 1, fsize, fp);
        fclose(fp);
        
        clock_t t0 = clock();
        int64_t key = rdh_capture(buf, nread, &cfg);
        clock_t t1 = clock();
        double ms = (double)(t1 - t0) * 1000 / CLOCKS_PER_SEC;
        
        uint16_t enc = (uint16_t)((uint64_t)key % 1440);
        
        printf("Key:  %lld\n", (long long)key);
        printf("Enc:  %u\n", (unsigned)enc);
        printf("Time: %.2f ms  (%.2f MB/s)\n", ms, nread / ms / 1000);
        
        free(buf);
    } else {
        /* ── Generate large test data ── */
        printf("No file specified. Generating 10MB test data...\n");
        
        size_t sz = 10 * 1024 * 1024;  /* 10 MB */
        uint8_t *buf = (uint8_t*)malloc(sz);
        
        srand(42);
        for (size_t i = 0; i < sz; i++)
            buf[i] = (uint8_t)rand();
        
        clock_t t0 = clock();
        int64_t key = rdh_capture(buf, sz, &cfg);
        clock_t t1 = clock();
        double ms = (double)(t1 - t0) * 1000 / CLOCKS_PER_SEC;
        
        printf("10MB random → key=%lld  enc=%llu  time=%.2fms\n",
               (long long)key, (unsigned long long)((uint64_t)key % 1440), ms);
        
        /* Verify against a second pass (deterministic) */
        int64_t key2 = rdh_capture(buf, sz, &cfg);
        printf("Deterministic check: %s\n", key == key2 ? "PASS" : "FAIL");
        
        free(buf);
    }
    
    return 0;
}
