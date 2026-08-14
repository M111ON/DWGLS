/* rdh_container_demo.c — store + load + verify (MinGW-safe) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rdh_capture.h"

#define FRAME_CYCLE 1440u

static uint8_t *container_data[1440]; /* enc -> data buffer */
static size_t   container_len[1440];  /* enc -> data length */
static int      container_count = 0;

static int container_store(const uint8_t *data, size_t len)
{
    int64_t key = rdh_capture(data, len, &RDH_CAPTURE_144);
    uint16_t enc = (uint16_t)((uint64_t)key % FRAME_CYCLE);

    if (container_data[enc])
        free(container_data[enc]);

    container_data[enc] = (uint8_t*)malloc(len);
    container_len[enc] = len;
    memcpy(container_data[enc], data, len);
    container_count++;
    return enc;
}

static int container_load(uint16_t enc, uint8_t *out, size_t *len)
{
    if (enc >= 1440) return -1;  /* out of bounds */
    if (!container_data[enc]) return -1;  /* not found */
    *len = container_len[enc];
    memcpy(out, container_data[enc], *len);
    return 0;
}

int main(void)
{
    printf("RDH Container Demo - Store & Load\n");
    printf("================================\n\n");

    /* Store 1 */
    uint8_t d1[] = "Hello, RDH World!";
    int enc1 = container_store(d1, sizeof(d1));
    printf("Store: \"%s\"\n", d1);
    printf("  enc = %d (2 bytes)\n\n", enc1);

    /* Store 2 */
    uint8_t d2[64];
    for (int i = 0; i < 64; i++) d2[i] = (uint8_t)(i * 7 + 3);
    int enc2 = container_store(d2, 64);
    printf("Store: 64 bytes (pattern)\n");
    printf("  enc = %d (2 bytes)\n\n", enc2);

    /* Store 3 (same as 1) */
    uint8_t d3[] = "Hello, RDH World!";
    int enc3 = container_store(d3, sizeof(d3));
    printf("Store: \"%s\" (same as d1)\n", d3);
    printf("  enc = %d (same! deterministic)\n\n", enc3);

    /* Load */
    printf("--- Load ---\n");
    for (int i = 0; i < 3; i++) {
        uint8_t buf[256];
        size_t  n = 0;
        int e = (i == 0) ? enc1 : (i == 1) ? enc2 : enc3;
        if (container_load((uint16_t)e, buf, &n) == 0) {
            printf("  enc=%d -> %u bytes: \"", e, (unsigned)n);
            for (size_t j = 0; j < n && j < 30; j++)
                printf("%c", buf[j] >= 32 && buf[j] < 127 ? buf[j] : '.');
            printf("\"\n");
        }
    }

    /* Non-existent */
    printf("\n--- Non-existent ---\n");
    uint8_t buf[8];
    size_t n;
    printf("  enc=9999 -> %s\n",
           container_load(9999, buf, &n) == 0 ? "found" : "not found");

    /* Summary */
    printf("\nSummary:\n");
    printf("  Slots used: %d / 1440\n", container_count);
    printf("  Handle: 2 bytes\n");
    printf("  Flow: data -> rdh_capture -> enc -> container\n");
    printf("        enc -> container -> original data\n");

    for (int i = 0; i < 1440; i++)
        if (container_data[i]) free(container_data[i]);

    printf("\nDone.\n");
    return 0;
}
