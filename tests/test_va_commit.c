#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#define RESERVE ((size_t)5244887552ULL)

int main(void) {
    fprintf(stderr, "=== MEM_RESERVE 4.9GB + incremental MEM_COMMIT ===\n");
    void *base = VirtualAlloc(NULL, RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!base) { fprintf(stderr, "FAIL: MEM_RESERVE\n"); return 1; }
    fprintf(stderr, "MEM_RESERVE OK: %p\n", base);

    size_t CHUNK = 40u * 1024u * 1024u; /* 40MB per commit */
    int committed = 0;
    for (size_t off = 0; off < RESERVE; off += CHUNK) {
        void *p = VirtualAlloc((uint8_t *)base + off, CHUNK, MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            fprintf(stderr, "commit failed at offset %zu MB (committed %d chunks = %d MB)\n",
                    off / 1048576, committed, committed * 40);
            break;
        }
        memset(p, 0xCD, CHUNK);
        committed++;
    }
    fprintf(stderr, "Committed: %d chunks = %d MB of %zu MB total\n",
            committed, committed * 40, (size_t)(RESERVE / 1048576));
    fprintf(stderr, "RESULT: %s\n", committed > 0 ? "partial commit works" : "FAIL");

    /* Verify first chunk is readable */
    uint8_t *p = (uint8_t *)base;
    int ok = 1;
    for (int i = 0; i < 1024; i++) if (p[i] != 0xCD) { ok = 0; break; }
    fprintf(stderr, "Data verify first chunk: %s\n", ok ? "PASS" : "FAIL");

    VirtualFree(base, 0, MEM_RELEASE);
    return 0;
}
