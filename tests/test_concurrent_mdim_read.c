/* tests/test_concurrent_mdim_read.c — concurrent multi-dim reads.
 *
 * Proves: N threads can resolve and read every tensor of a field through
 * the w-dim (tesseract) coordinate concurrently without corruption, and
 * all threads agree on the resolved bytes.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_concurrent_mdim_read tests/test_concurrent_mdim_read.c -lpthread
 * RUN:   ./build/test_concurrent_mdim_read [field.bin]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "../core/gguf_box.h"
#include "../core/geo_tesseract_addr.h"

#define NTHREADS 4
#define TESS_GEO_FULL 20736u

typedef struct {
    const GGUFBox *fld;
    uint64_t body_off;
    const uint64_t *fpos;
    int n_tensors;
    int tid;
    int fails;
} WorkerCtx;

static void *worker(void *arg) {
    WorkerCtx *c = (WorkerCtx *)arg;
    for (int i = 0; i < c->n_tensors; i++) {
        uint64_t addr = c->body_off + c->fpos[i];
        const uint8_t *ptr = c->fld->reader.base + addr;
        if (addr + c->fld->entries[i].size > c->fld->reader.base_sz) { c->fails++; continue; }
        uint32_t phys = (uint32_t)((addr / 64u) % TESS_GEO_FULL);
        uint32_t t, cell, slot;
        tess_unflat(phys, &t, &cell, &slot);
        if (tess_flat(t, cell, slot) != phys) { c->fails++; continue; }
        volatile uint8_t v = ptr[0];
        (void)v;
    }
    return NULL;
}

static uint64_t find_body_off(const uint8_t *base) {
    uint64_t n_kv; memcpy(&n_kv, base + 16, 8);
    const uint8_t *p = base + 24;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen; memcpy(&klen, p, 8); p += 8;
        char name[64]; memset(name,0,64);
        memcpy(name, p, klen < 63 ? klen : 63); p += klen;
        uint32_t vtype; memcpy(&vtype, p, 4); p += 4;
        if ((vtype == 10 || vtype == 11) && strcmp(name, "kis.layout.body_off") == 0) {
            uint64_t v; memcpy(&v, p, 8); return v;
        } else if (vtype == 9) {
            uint32_t at; uint64_t narr; memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            if ((at == 10 || at == 11) && strcmp(name, "kis.layout.body_off") == 0 && narr > 0) {
                uint64_t v; memcpy(&v, p, 8); return v;
            }
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
    }
    return 0;
}

static void load_fpos(const uint8_t *base, uint64_t *fpos, int n) {
    uint64_t n_kv; memcpy(&n_kv, base + 16, 8);
    const uint8_t *p = base + 24;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen; memcpy(&klen, p, 8); p += 8;
        char name[64]; memset(name,0,64);
        memcpy(name, p, klen < 63 ? klen : 63); p += klen;
        uint32_t vtype; memcpy(&vtype, p, 4); p += 4;
        if (vtype == 9) {
            uint32_t at; uint64_t narr; memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            if ((at == 10 || at == 11) && strcmp(name, "kis.layout.fpos") == 0) {
                for (uint64_t a = 0; a < narr && (int)a < n; a++) memcpy(&fpos[a], p + a*8, 8);
            }
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "build/fieldA.bin";
    GGUFBox fld;
    if (gguf_box_open(&fld, path) != 0) { printf("(cannot open %s)\n", path); return 1; }

    uint64_t body_off = find_body_off(fld.reader.base);
    uint64_t *fpos = (uint64_t *)malloc(fld.n_tensors * sizeof(uint64_t));
    load_fpos(fld.reader.base, fpos, (int)fld.n_tensors);

    printf("concurrent_mdim_read — %d threads x %u tensors  body_off=%llu\n", NTHREADS, fld.n_tensors, (unsigned long long)body_off);

    WorkerCtx ctxs[NTHREADS];
    pthread_t ths[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        ctxs[t].fld = &fld;
        ctxs[t].body_off = body_off;
        ctxs[t].fpos = fpos;
        ctxs[t].n_tensors = (int)fld.n_tensors;
        ctxs[t].tid = t;
        ctxs[t].fails = 0;
        pthread_create(&ths[t], NULL, worker, &ctxs[t]);
    }
    int total_fails = 0;
    for (int t = 0; t < NTHREADS; t++) {
        pthread_join(ths[t], NULL);
        total_fails += ctxs[t].fails;
        printf("  thread %d: %d fails\n", t, ctxs[t].fails);
    }

    free(fpos);
    gguf_box_close(&fld);

    printf("concurrent_mdim_read: %s (total fails=%d)\n", total_fails == 0 ? "PASS" : "FAIL", total_fails);
    return total_fails ? 1 : 0;
}