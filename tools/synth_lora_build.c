// synth_lora_build.c — build synthetic rank-r LoRA adapter GGUF (v3) for a base model tensor
// usage: synth_lora_build <base.gguf> <out_lora.gguf> [rank=8] [layer=0]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"

static uint64_t rng = 0x123456789ABCDEFULL;
static double frand(void) { // xorshift64* -> [-0.1, 0.1)
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return ((double)(rng * 0x2545F4914F6CDD1DULL >> 11) / 9007199254740992.0 - 0.5) * 0.2;
}
static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static int r32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1 ? 0 : -1; }
static int r64(FILE *f, uint64_t *v) { return fread(v, 8, 1, f) == 1 ? 0 : -1; }
static int skip_val(FILE *f, uint32_t t) {
    static const int sz[] = { 1, 1, 2, 2, 4, 4, 4, 1, -1, -2, 8, 8, 8 };
    if (t > 12) return -1;
    if (sz[t] >= 0) return fseek(f, sz[t], SEEK_CUR);
    if (t == 8) { uint64_t n; if (r64(f, &n)) return -1; return fseek(f, (long)n, SEEK_CUR); }
    uint32_t et; uint64_t n; // array
    if (r32(f, &et) || r64(f, &n) || et > 12) return -1;
    if (et == 8) { for (uint64_t i = 0; i < n; i++) { uint64_t m; if (r64(f, &m)) return -1; if (fseek(f, (long)m, SEEK_CUR)) return -1; } return 0; }
    return fseek(f, (long)(n * (uint64_t)sz[et]), SEEK_CUR);
}
// find string KV in base gguf; returns 0 + fills out on success
static int find_kv_str(const char *path, const char *key, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, ver, n_d; uint64_t n_t, n_kv;
    if (r32(f, &magic) || r32(f, &ver) || r64(f, &n_t) || r64(f, &n_kv)) { fclose(f); return -1; }
    (void)n_d;
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t kl; uint32_t ty;
        if (r64(f, &kl) || kl > 256) { fclose(f); return -1; }
        char kb[257]; if (fread(kb, 1, (size_t)kl, f) != (size_t)kl) { fclose(f); return -1; }
        kb[kl] = 0;
        if (r32(f, &ty)) { fclose(f); return -1; }
        if (strcmp(kb, key) == 0 && ty == 8) {
            uint64_t vl; if (r64(f, &vl) || vl >= cap) { fclose(f); return -1; }
            if (fread(out, 1, (size_t)vl, f) != (size_t)vl) { fclose(f); return -1; }
            out[vl] = 0; fclose(f); return 0;
        }
        if (skip_val(f, ty)) { fclose(f); return -1; }
    }
    fclose(f); return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s <base.gguf> <out.gguf> [rank=8] [layer=0]\n", argv[0]); return 2; }
    int rank = argc > 3 ? atoi(argv[3]) : 8;
    int layer = argc > 4 ? atoi(argv[4]) : 0;
    char tname[128];
    snprintf(tname, sizeof tname, "blk.%d.attn_output.weight", layer);

    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("base read fail\n"); return 1; }
    int idx = -1;
    for (uint32_t i = 0; i < r.n_tensors; i++)
        if (strcmp(r.names[i], tname) == 0) { idx = (int)i; break; }
    if (idx < 0) { printf("tensor %s not found\n", tname); return 1; }
    if (r.n_dims[idx] != 2) { printf("expect 2D, got %d\n", r.n_dims[idx]); return 1; }
    uint64_t d0 = r.dims[(size_t)idx * 4], d1 = r.dims[(size_t)idx * 4 + 1];
    printf("base %s: [%llu x %llu]\n", tname, (unsigned long long)d0, (unsigned long long)d1);

    char na[160], nb[160];
    snprintf(na, sizeof na, "%s.lora_a", tname);
    snprintf(nb, sizeof nb, "%s.lora_b", tname);
    uint64_t adims[2] = { d0, (uint64_t)rank };
    uint64_t bdims[2] = { (uint64_t)rank, d1 };
    uint64_t asz = (uint64_t)rank * d0, bsz = d1 * (uint64_t)rank;

    FILE *f = fopen(argv[2], "wb");
    if (!f) { printf("open out fail\n"); return 1; }
    char arch[64] = "unknown";
    if (find_kv_str(argv[1], "general.architecture", arch, sizeof arch) != 0)
        printf("warn: no general.architecture in base, using 'unknown'\n");
    w32(f, 0x46554747u); w32(f, 3); w64(f, 2); w64(f, 3); // magic, v3, 2 tensors, 3 kv
    { const char *k = "general.type", *v = "adapter";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(v)); fwrite(v, 1, strlen(v), f);
      k = "adapter.type"; v = "lora";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(v)); fwrite(v, 1, strlen(v), f);
      k = "general.architecture";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(arch)); fwrite(arch, 1, strlen(arch), f); }
    // tensor a info
    w64(f, (uint64_t)strlen(na)); fwrite(na, 1, strlen(na), f);
    w32(f, 2); w64(f, adims[0]); w64(f, adims[1]); w32(f, 0); w64(f, 0);
    // tensor b info
    w64(f, (uint64_t)strlen(nb)); fwrite(nb, 1, strlen(nb), f);
    w32(f, 2); w64(f, bdims[0]); w64(f, bdims[1]); w32(f, 0); w64(f, 32); // placeholder, fix below
    long hdr_end = ftell(f);
    long data_start = (hdr_end + 31) & ~31L;
    // fix b offset = 32-aligned a size
    uint64_t a_bytes = asz * 4, b_off = (a_bytes + 31) & ~31ULL;
    fseek(f, 0, SEEK_SET);
    // rewalk: easier to rewrite header fully — recompute positions
    // (offsets: a=0, b=b_off; both relative to data_start)
    fclose(f);
    f = fopen(argv[2], "wb");
    w32(f, 0x46554747u); w32(f, 3); w64(f, 2); w64(f, 3);
    { const char *k = "general.type", *v = "adapter";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(v)); fwrite(v, 1, strlen(v), f);
      k = "adapter.type"; v = "lora";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(v)); fwrite(v, 1, strlen(v), f);
      k = "general.architecture";
      w64(f, (uint64_t)strlen(k)); fwrite(k, 1, strlen(k), f);
      w32(f, 8); w64(f, (uint64_t)strlen(arch)); fwrite(arch, 1, strlen(arch), f); }
    w64(f, (uint64_t)strlen(na)); fwrite(na, 1, strlen(na), f);
    w32(f, 2); w64(f, adims[0]); w64(f, adims[1]); w32(f, 0); w64(f, 0);
    w64(f, (uint64_t)strlen(nb)); fwrite(nb, 1, strlen(nb), f);
    w32(f, 2); w64(f, bdims[0]); w64(f, bdims[1]); w32(f, 0); w64(f, b_off);
    hdr_end = ftell(f);
    data_start = (hdr_end + 31) & ~31L;
    while (ftell(f) < data_start) fputc(0, f);
    float *buf = (float *)malloc((size_t)(asz > bsz ? asz : bsz) * 4);
    for (uint64_t i = 0; i < asz; i++) buf[i] = (float)frand();
    fwrite(buf, 4, (size_t)asz, f);
    long pos = ftell(f), want = data_start + (long)b_off;
    while (pos++ < want) fputc(0, f);
    for (uint64_t i = 0; i < bsz; i++) buf[i] = (float)frand();
    fwrite(buf, 4, (size_t)bsz, f);
    free(buf);
    fclose(f);
    gguf_close(&r);
    printf("wrote %s (a=[%llu,%d] b=[%d,%llu])\n", argv[2],
        (unsigned long long)d0, rank, rank, (unsigned long long)d1);
    return 0;
}
