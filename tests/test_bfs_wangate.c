/* test_bfs_wangate.c — wang+tantrix block-edge gate.
 * Oracle: brittleness is the proof — 1 flipped encoded byte must TAMPER,
 * swapped block payloads must BREAK, clean chain reads back exact.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/bfs_wangate.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static BreathingFS fs;
static BWTape tape;
static int8_t wdata[500];
static int8_t rdata[500];

int main(void) {
    for (int i = 0; i < 500; i++) wdata[i] = (int8_t)((i * 37 + 11) & 0xFF);
    bfs_init(&fs);
    bwt_init(&tape);

    /* T1: write + seal + verify clean + gated read exact. */
    CHECK(bfs_write(&fs, "f", wdata, 500) == 0, "write");   /* 4 blocks */
    CHECK(bwt_seal(&fs, &tape, "f") == 4, "seal 4");
    CHECK(bwt_verify(&fs, &tape, "f") == 0, "clean");
    {
        uint32_t actual = 0;
        memset(rdata, 0, sizeof(rdata));
        CHECK(bwt_read(&fs, &tape, "f", rdata, sizeof(rdata), &actual) == 0, "gated read");
        CHECK(actual == 500 && memcmp(rdata, wdata, 500) == 0, "read exact");
    }

    /* T2: 1 flipped encoded byte → TAMPER, read refused (DROP). */
    {
        const BFSFileEntry *e = bwt_find(&fs, "f");
        uint32_t bi = e->home_block + 2;
        fs.block_encoded[bi][0] ^= 0xFF;
        CHECK(bwt_verify(&fs, &tape, "f") == -1, "tamper -1");
        uint32_t actual = 0;
        CHECK(bwt_read(&fs, &tape, "f", rdata, sizeof(rdata), &actual) == -11, "drop");
        fs.block_encoded[bi][0] ^= 0xFF;   /* restore */
        CHECK(bwt_verify(&fs, &tape, "f") == 0, "clean again");
    }

    /* T3: swapped block payloads → BREAK (exits travel with tiles, not bytes). */
    {
        const BFSFileEntry *e = bwt_find(&fs, "f");
        uint32_t a = e->home_block, b = e->home_block + 1;
        uint8_t tmp[2048];
        uint16_t na = fs.block_encoded_size[a], nb = fs.block_encoded_size[b];
        memcpy(tmp, fs.block_encoded[a], na);
        memcpy(fs.block_encoded[a], fs.block_encoded[b], nb);
        memcpy(fs.block_encoded[b], tmp, na);
        fs.block_encoded_size[a] = nb;
        fs.block_encoded_size[b] = na;
        int v = bwt_verify(&fs, &tape, "f");
        CHECK(v == -1 || v == -2, "swap detected");
        /* restore */
        memcpy(tmp, fs.block_encoded[a], nb);
        memcpy(fs.block_encoded[a], fs.block_encoded[b], na);
        memcpy(fs.block_encoded[b], tmp, nb);
        fs.block_encoded_size[a] = na;
        fs.block_encoded_size[b] = nb;
        CHECK(bwt_verify(&fs, &tape, "f") == 0, "restored clean");
    }

    /* T4: missing file / tape. */
    CHECK(bwt_verify(&fs, &tape, "nope") == -3, "missing -3");
    CHECK(bwt_verify(&fs, NULL, "f") == -3, "null tape -3");

    if (!fails) printf("bfs_wangate: ALL PASS\n");
    return fails ? 1 : 0;
}
