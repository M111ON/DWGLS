/*
 * tests/test_shell.c — DWGLS Shell tests
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * T1  shell_init + shell_validate — basic header operations
 * T2  shell_total_size — correct total size computation
 * T3  shell_codec_name — all registered codecs have names
 * T4  shell_compute_checksum / shell_verify_integrity — CRC64 integrity
 * T5  bad magic detection
 * T6  bad version detection
 * T7  checksum mismatch detection
 * T8  buffer too short detection
 * T9  roundtrip: init → compute checksum → verify
 * T10 shell size is exactly 32 bytes (compile-time check)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test_shell tests/test_shell.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/dwgls_shell.h"
#include "../core/dwgls_codec.h"
#include "../core/codec_tess.h"

static int pass = 0, fail = 0;

#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  [PASS] %s\n", desc); } \
    else { fail++; printf("  [FAIL] %s\n", desc); } \
} while(0)

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  DWGLS Shell Tests\n");
    printf("═══════════════════════════════════════\n\n");

    /* ── T1: shell_init + shell_validate ───────────────────────────── */
    {
        DWGLS_Shell s;
        dwgls_shell_init(&s, CODEC_TESS, 20736, 65536, 1024, 4, INTEGRITY_CRC64);
        CHECK("T1a: shell_init sets magic", s.magic == DWGLS_SHELL_MAGIC);
        CHECK("T1b: shell_init sets version", s.version == DWGLS_SHELL_VERSION);
        CHECK("T1c: shell_init sets codec_id", s.codec_id == CODEC_TESS);
        CHECK("T1d: shell_init sets total_slots", s.total_slots == 20736);
        CHECK("T1e: shell_init sets scale_factor", s.scale_factor == 65536);
        CHECK("T1f: shell_init sets payload_size", s.payload_size == 1024);
        CHECK("T1g: shell_init sets cell_size", s.cell_size == 4);
        CHECK("T1h: shell_init sets integrity", s.integrity == INTEGRITY_CRC64);
        CHECK("T1i: shell_init zeroes checksum", s.checksum == 0);
        CHECK("T1j: shell_validate passes", dwgls_shell_validate(&s) == 0);
    }

    /* ── T2: shell_total_size ──────────────────────────────────────── */
    {
        DWGLS_Shell s;
        dwgls_shell_init(&s, CODEC_TESS, 20736, 65536, 1024, 4, INTEGRITY_CRC64);
        CHECK("T2: total_size = 32 + payload", dwgls_shell_total_size(&s) == 1056);
    }

    /* ── T3: shell_codec_name ──────────────────────────────────────── */
    {
        CHECK("T3a: CODEC_NONE name", strcmp(dwgls_shell_codec_name(CODEC_NONE), "raw") == 0);
        CHECK("T3b: CODEC_KIS_FRAME name", strcmp(dwgls_shell_codec_name(CODEC_KIS_FRAME), "kis_frame") == 0);
        CHECK("T3c: CODEC_KIS_4D name", strcmp(dwgls_shell_codec_name(CODEC_KIS_4D), "kis_4d") == 0);
        CHECK("T3d: CODEC_TESSERACT name", strcmp(dwgls_shell_codec_name(CODEC_TESSERACT), "tesseract") == 0);
        CHECK("T3e: CODEC_GCUBE name", strcmp(dwgls_shell_codec_name(CODEC_GCUBE), "gcube") == 0);
        CHECK("T3f: CODEC_BEAM_ENTROPY name", strcmp(dwgls_shell_codec_name(CODEC_BEAM_ENTROPY), "beam_entropy") == 0);
        CHECK("T3g: CODEC_TESS name", strcmp(dwgls_shell_codec_name(CODEC_TESS), "tess") == 0);
        CHECK("T3h: CODEC_KIS_CODEC_V6 name", strcmp(dwgls_shell_codec_name(CODEC_KIS_CODEC_V6), "kis_v6") == 0);
        CHECK("T3i: CODEC_DIAMOND_FIELD name", strcmp(dwgls_shell_codec_name(CODEC_DIAMOND_FIELD), "diamond_field") == 0);
        CHECK("T3j: unknown codec name", strcmp(dwgls_shell_codec_name(99), "user_defined") == 0);
    }

    /* ── T4: checksum computation and verification ─────────────────── */
    {
        DWGLS_Shell s;
        uint8_t payload[64] = {0};
        for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i * 7);

        dwgls_shell_init(&s, CODEC_TESS, 20736, 65536, 64, 4, INTEGRITY_CRC64);

        /* Compute checksum */
        uint64_t crc = dwgls_shell_compute_checksum(&s, payload);
        CHECK("T4a: checksum non-zero", crc != 0);

        s.checksum = crc;

        /* Verify integrity */
        int v = dwgls_shell_verify_integrity(&s, payload, 64);
        CHECK("T4b: verify_integrity passes", v == 0);
    }

    /* ── T5: bad magic detection ───────────────────────────────────── */
    {
        DWGLS_Shell s = {0};
        s.magic = 0xDEADBEEF;
        s.version = DWGLS_SHELL_VERSION;
        CHECK("T5: bad magic detected", dwgls_shell_validate(&s) == -1);
    }

    /* ── T6: bad version detection ─────────────────────────────────── */
    {
        DWGLS_Shell s = {0};
        s.magic = DWGLS_SHELL_MAGIC;
        s.version = 99;
        CHECK("T6: bad version detected", dwgls_shell_validate(&s) == -2);
    }

    /* ── T7: checksum mismatch detection ───────────────────────────── */
    {
        DWGLS_Shell s;
        uint8_t payload[64] = {0};
        for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i * 7);

        dwgls_shell_init(&s, CODEC_TESS, 20736, 65536, 64, 4, INTEGRITY_CRC64);
        uint64_t crc = dwgls_shell_compute_checksum(&s, payload);
        s.checksum = crc;

        /* Corrupt payload */
        payload[0] ^= 0xFF;

        int v = dwgls_shell_verify_integrity(&s, payload, 64);
        CHECK("T7: checksum mismatch detected", v == -3);
    }

    /* ── T8: buffer too short detection ────────────────────────────── */
    {
        DWGLS_Shell s;
        uint8_t payload[32] = {0};  /* shorter than declared payload_size */

        dwgls_shell_init(&s, CODEC_TESS, 20736, 65536, 64, 4, INTEGRITY_CRC64);
        uint64_t crc = dwgls_shell_compute_checksum(&s, payload);
        s.checksum = crc;

        int v = dwgls_shell_verify_integrity(&s, payload, 32);
        CHECK("T8: buffer too short detected", v == -4);
    }

    /* ── T9: full roundtrip ────────────────────────────────────────── */
    {
        DWGLS_Shell s;
        uint8_t payload[128];
        for (int i = 0; i < 128; i++) payload[i] = (uint8_t)(i * 13 + 7);

        dwgls_shell_init(&s, CODEC_GCUBE, 20736, 65536, 128, 2, INTEGRITY_CRC64);
        uint64_t crc = dwgls_shell_compute_checksum(&s, payload);
        s.checksum = crc;

        int v = dwgls_shell_verify_integrity(&s, payload, 128);
        CHECK("T9: full roundtrip passes", v == 0);
    }

    /* ── T10: shell size is exactly 32 bytes ───────────────────────── */
    {
        CHECK("T10: DWGLS_Shell is 32 bytes", sizeof(DWGLS_Shell) == 32);
    }

    /* ── Bonus: codec registry ─────────────────────────────────────── */
    {
        const DWGLS_CodecVtable *c;

        c = dwgls_codec_find(CODEC_NONE);
        CHECK("Bonus: codec_find RAW", c != NULL && c->info().codec_id == CODEC_NONE);

        c = dwgls_codec_find(CODEC_TESS);
        CHECK("Bonus: codec_find TESS", c != NULL && c->info().codec_id == CODEC_TESS);

        c = dwgls_codec_find(CODEC_GCUBE);
        CHECK("Bonus: codec_find GCUBE (not yet implemented, returns NULL)", c == NULL);

        c = dwgls_codec_find(99);
        CHECK("Bonus: codec_find unknown returns NULL", c == NULL);
    }

    printf("\n═══════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}