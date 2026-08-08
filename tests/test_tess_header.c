/* test_tess_header.c — Verify .tess header struct sizes and invariants
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_header.exe tests/test_tess_header.c -lm
 * RUN:   build/test_tess_header.exe
 */

#include <stdio.h>
#include <assert.h>
#include "geo_tess_container.h"

int main(void) {
    int pass = 0, fail = 0;

    /* T1: Header is exactly 64 bytes */
    {
        int ok = ( (unsigned)sizeof(TESS_Header) == 64);
        printf("[%s] T1:  (unsigned)sizeof(TESS_Header) = %u (expect 64)\n",
               ok ? "PASS" : "FAIL",  (unsigned)sizeof(TESS_Header));
        ok ? pass++ : fail++;
    }

    /* T2: FormulaBlock is exactly 64 bytes */
    {
        int ok = ( (unsigned)sizeof(TESS_Formula) == 64);
        printf("[%s] T2:  (unsigned)sizeof(TESS_Formula) = %u (expect 64)\n",
               ok ? "PASS" : "FAIL",  (unsigned)sizeof(TESS_Formula));
        ok ? pass++ : fail++;
    }

    /* T3: Section header is exactly 8 bytes */
    {
        int ok = ( (unsigned)sizeof(TESS_SectionHdr) == 8);
        printf("[%s] T3:  (unsigned)sizeof(TESS_SectionHdr) = %u (expect 8)\n",
               ok ? "PASS" : "FAIL",  (unsigned)sizeof(TESS_SectionHdr));
        ok ? pass++ : fail++;
    }

    /* T4: Magic constant */
    {
        int ok = (TESS_MAGIC == 0x54455353u);
        printf("[%s] T4: TESS_MAGIC = 0x%08X (expect 0x54455353)\n",
               ok ? "PASS" : "FAIL", TESS_MAGIC);
        ok ? pass++ : fail++;
    }

    /* T5: Total slots is sacred number */
    {
        int ok = (TESS_TOTAL_SLOTS == 20736);
        printf("[%s] T5: TESS_TOTAL_SLOTS = %u (expect 20736)\n",
               ok ? "PASS" : "FAIL", TESS_TOTAL_SLOTS);
        ok ? pass++ : fail++;
    }

    /* T6: Axis partition sums to total */
    {
        int ok = (TESS_X_SLOTS + TESS_Y_SLOTS + TESS_Z_SLOTS == TESS_TOTAL_SLOTS);
        printf("[%s] T6: axes %u + %u + %u = %u (expect %u)\n",
               ok ? "PASS" : "FAIL",
               TESS_X_SLOTS, TESS_Y_SLOTS, TESS_Z_SLOTS,
               TESS_X_SLOTS + TESS_Y_SLOTS + TESS_Z_SLOTS,
               TESS_TOTAL_SLOTS);
        ok ? pass++ : fail++;
    }

    /* T7: Stride-37 scatter is bijection */
    {
        uint8_t seen[20736];
        memset(seen, 0, sizeof(seen));
        int unique = 1;
        for (uint32_t i = 0; i < 20736; i++) {
            uint32_t cell = tess_stride_scatter(i);
            if (cell >= 20736 || seen[cell]) { unique = 0; break; }
            seen[cell] = 1;
        }
        printf("[%s] T7: stride-37 scatter bijection (20736 unique cells)\n",
               unique ? "PASS" : "FAIL");
        unique ? pass++ : fail++;
    }

    /* T8: Stride-37 gather inverts scatter */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 20736; i++) {
            uint32_t cell = tess_stride_scatter(i);
            uint32_t back = tess_stride_gather(cell);
            if (back != i) { ok = 0; printf("  FAIL at %u: cell=%u back=%u\n", i, cell, back); break; }
        }
        printf("[%s] T8: stride-37 gather(inverse) roundtrip\n",
               ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* T9: Header init defaults */
    {
        TESS_Header h;
        tess_header_init(&h, TESS_GGML_Q8_0, TESS_CELL_Q8_0);
        int ok = (h.magic == TESS_MAGIC) &&
                 (h.version == 1) &&
                 (h.total_slots == 20736) &&
                 (h.cell_size == 34) &&
                 (h.scale_factor == 65536) &&
                 (h.x_slots == 6912) &&
                 (h.y_slots == 6912) &&
                 (h.z_slots == 6912) &&
                 (h.gguf_type == TESS_GGML_Q8_0);
        printf("[%s] T9: tess_header_init defaults correct\n",
               ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* T10: Header validation */
    {
        TESS_Header h;
        tess_header_init(&h, TESS_GGML_Q8_0, TESS_CELL_Q8_0);
        int v1 = tess_header_validate(&h);

        h.magic = 0xDEADBEEF;
        int v2 = tess_header_validate(&h);

        printf("[%s] T10: header validate (valid=%d, bad_magic=%d)\n",
               (v1 == 0 && v2 == -1) ? "PASS" : "FAIL", v1, v2);
        (v1 == 0 && v2 == -1) ? pass++ : fail++;
    }

    /* T11: Octant roundtrip identity (mirror is self-inverse) */
    {
        TESS_Header h;
        tess_header_init(&h, TESS_GGML_Q8_0, TESS_CELL_Q8_0);
        int ok = 1;
        for (uint32_t slot = 0; slot < TESS_TOTAL_SLOTS; slot++) {
            for (uint8_t oct = 0; oct < TESS_NUM_OCTANTS; oct++) {
                uint32_t addr = tess_resolve_octant(slot, oct, &h);
                uint32_t back = tess_resolve_octant(addr, oct, &h);
                if (back != slot) {
                    ok = 0;
                    printf("  FAIL: slot=%u oct=%u addr=%u back=%u\n",
                           slot, oct, addr, back);
                    break;
                }
            }
            if (!ok) break;
        }
        printf("[%s] T11: octant roundtrip (self-inverse, 20736×8)\n",
               ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* T12: Octant 0 is identity */
    {
        TESS_Header h;
        tess_header_init(&h, TESS_GGML_Q8_0, TESS_CELL_Q8_0);
        int ok = 1;
        for (uint32_t s = 0; s < 20736; s++) {
            if (tess_resolve_octant(s, 0, &h) != s) { ok = 0; break; }
        }
        printf("[%s] T12: octant 0 = identity\n", ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* T13: CRC-64 basic test */
    {
        uint64_t crc = tess_crc64((const uint8_t *)"123456789", 9);
        int ok = (crc == 0x62EC59E3F1A4F00AULL);
        printf("[%s] T13: CRC-64 ECMA-182 test vector (0x%016I64X)\n",
               ok ? "PASS" : "FAIL", (unsigned long long)crc);
        ok ? pass++ : fail++;
    }

    /* T14: GGUF type → cell size mapping */
    {
        int ok = (tess_gguf_type_to_cell_size(TESS_GGML_Q8_0) == 34) &&
                 (tess_gguf_type_to_cell_size(TESS_GGML_F32) == 4) &&
                 (tess_gguf_type_to_cell_size(TESS_GGML_BF16) == 2) &&
                 (tess_gguf_type_to_cell_size(99) == 0);
        printf("[%s] T14: gguf_type → cell_size mapping\n",
               ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    printf("\n═══════════════════════════════════════════════\n");
    printf("Results: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════\n");

    return fail > 0 ? 1 : 0;
}
