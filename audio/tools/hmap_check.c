#include <stdio.h>
#include <stdint.h>
static inline uint32_t hilbert_idx(uint32_t x, uint32_t y, uint32_t n) {
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}
int main() {
    uint8_t h_map[4][64];
    for (int f = 0; f < 4; f++) {
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t x = i % 8, y = i / 8;
            uint32_t tx = x, ty = y;
            switch (f) {
                case 0: break;
                case 1: tx = 7 - y; ty = x; break;
                case 2: tx = 7 - x; ty = 7 - y; break;
                case 3: tx = y; ty = 7 - x; break;
            }
            h_map[f][i] = (uint8_t)hilbert_idx(tx, ty, 8);
        }
    }
    printf("Channel to Hilbert position per floor:\n");
    for (int c = 0; c < 8; c++) {
        printf("  ch%2d: f0=%3u f1=%3u f2=%3u f3=%3u\n",
               c, h_map[0][c], h_map[1][c], h_map[2][c], h_map[3][c]);
    }
    for (int f = 0; f < 4; f++) {
        int seen[64] = {0}, dup = 0;
        for (int i = 0; i < 64; i++) { if (seen[h_map[f][i]]) dup = 1; seen[h_map[f][i]] = 1; }
        int start_idx = h_map[f][0];
        printf("Floor %d: bijection %s, start(ch0)=%d\n",
               f, dup ? "FAIL" : "OK", start_idx);
    }
    return 0;
}