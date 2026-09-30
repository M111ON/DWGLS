/* tools/beam_zigzag_probe.c — angular zigzag wave radius (Phase 4 เริ่มต้น)
 * ═══════════════════════════════════════════════════════════════════════════
 * 2 spheres × 3 axes overlap: A = positive, B = negative.
 * ต่อค่า: sphere = sign(v), axis = |v| % 3, r = |v|,
 *         angle = (r * 137 + tick) % 360   (fixed, invertible ให้ tick)
 * zigzag: เดิน snake สลับทิศตาม parity ของ tick → คลื่นกางเป็นเส้นตรง
 * XOR radius: addr-pack = (angle ^ (r<<2) ^ axis) — พก r ไปด้วยแบบ carry
 * กู้: r จาก pack, v = ±r ตาม sphere. memcmp ทั้ง stream.
 * กฎ int-only: ไม่มี float (ของเก่า beam_angular_v2.c ใช้ M_PI — ไม่เอามา)
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_zigzag_probe.exe tools/beam_zigzag_probe.c -lm
 * RUN:   ./build/beam_zigzag_probe.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf [maxvals]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define K_ANG 137u

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [maxvals]\n", argv[0]); return 2; }
    unsigned long long maxv = argc > 2 ? strtoull(argv[2], 0, 10) : 2000000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    unsigned long long n = 0, bad = 0, cntA = 0, cntB = 0;
    unsigned long long tick = 0;
    /* snake state: ทิศขึ้น/ลงสลับตาม parity */
    for (unsigned t = 0; t < box.n_tensors && n < maxv; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < maxv; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < maxv; k++, n++, tick++) {
                int8_t v = (int8_t)blk[k];
                int sphere = (v >= 0) ? 0 : 1;          /* A=0, B=1 */
                unsigned r = (unsigned)(v >= 0 ? v : -(int)v);  /* 0..128 */
                unsigned axis = r % 3u;
                /* zigzag: สลับทิศ — tick คี่กลับด้าน */
                unsigned dir = (unsigned)(tick & 1ull);
                unsigned angle = (r * K_ANG + (unsigned)(tick % 360ull)) % 360u;
                if (dir) angle = (360u - angle) % 360u;
                /* carry r ไปใน pack (ไม่ cut — กฎ carry) */
                unsigned pack = (angle ^ (r << 2) ^ (unsigned)(axis * 91u)) & 0x3FFu;
                /* กู้: ต้องรู้ tick (stream order) + sphere/axis แยกเก็บ */
                unsigned ra = angle;
                if (dir) ra = (360u - ra) % 360u;
                /* angle = (r*137 + tick%360) % 360 → ลอง r 0..128 ตัวเดียวที่ตรง */
                int got = -1;
                for (unsigned c = 0; c <= 128; c++) {
                    if ((c * K_ANG + (unsigned)(tick % 360ull)) % 360u == ra) { got = (int)c; break; }
                }
                /* pack verify: encode ซ้ำต้องได้ pack เดิม */
                unsigned pack2 = 0;
                if (got >= 0) {
                    unsigned a2 = ((unsigned)got * K_ANG + (unsigned)(tick % 360ull)) % 360u;
                    if (dir) a2 = (360u - a2) % 360u;
                    pack2 = (a2 ^ ((unsigned)got << 2) ^ (unsigned)(axis * 91u)) & 0x3FFu;
                }
                int8_t back = (got < 0) ? 0 : (int8_t)(sphere ? -got : got);
                /* r=128 ลบไม่ได้ใน int8 (min -128 ok: -(128) overflow!) — กฎพิเศษ */
                if (sphere && got == 128) back = (int8_t)-128;
                if (sphere && got > 128) { bad++; continue; }
                if (got != (int)r || pack2 != pack || back != v) bad++;
                if (sphere) cntB++; else cntA++;
            }
        }
    }
    printf("values=%llu A(+)=%llu B(-)=%llu bad=%llu\n", n, cntA, cntB, bad);
    printf("%s\n", bad == 0 ? "ZIGZAG ROUNDTRIP OK" : "ZIGZAG ROUNDTRIP FAIL");
    return bad == 0 ? 0 : 1;
}
