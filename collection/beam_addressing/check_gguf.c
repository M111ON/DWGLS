#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static float fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t h_exp = (h >> 10) & 0x1f;
    uint32_t h_mant = h & 0x3ff;
    uint32_t f32_exp, f32_mant;
    if (h_exp == 0) {
        if (h_mant == 0) return 0.0f;
        int shift = 0;
        while (!(h_mant & 0x400)) { h_mant <<= 1; shift++; }
        f32_exp = 113u - (uint32_t)shift;
        f32_mant = (h_mant & 0x3ff) << 13;
    } else if (h_exp == 31) {
        f32_exp = 255;
        f32_mant = (uint32_t)h_mant << 13;
    } else {
        f32_exp = h_exp + 112;
        f32_mant = (uint32_t)h_mant << 13;
    }
    uint32_t bits = sign | (f32_exp << 23) | f32_mant;
    float result;
    memcpy(&result, &bits, 4);
    return result;
}

int main(void) {
    FILE *fp = fopen("I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf", "rb");
    if (!fp) { printf("FAIL open\n"); return 1; }

    uint32_t magic; fread(&magic,4,1,fp);
    uint32_t ver;   fread(&ver,4,1,fp);
    uint64_t n_t;   fread(&n_t,8,1,fp);
    uint64_t n_kv;  fread(&n_kv,8,1,fp);
    printf("GGUF v%u %llu tensors %llu KV\n", ver, (unsigned long long)n_t, (unsigned long long)n_kv);

    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t slen; fread(&slen,8,1,fp); fseek(fp, slen, SEEK_CUR);
        uint32_t vt; fread(&vt,4,1,fp);
        if (vt <= 7) {
            int sizes[] = {1,1,2,2,4,4,4,1};
            fseek(fp, sizes[vt], SEEK_CUR);
        } else if (vt == 8) {
            uint64_t sl; fread(&sl,8,1,fp); fseek(fp, sl, SEEK_CUR);
        } else if (vt == 9) {
            uint32_t at; fread(&at,4,1,fp);
            uint64_t al; fread(&al,8,1,fp);
            for (uint64_t j=0;j<al;j++) {
                if (at == 8) { uint64_t sl; fread(&sl,8,1,fp); fseek(fp, sl, SEEK_CUR); }
                else { fseek(fp, 4, SEEK_CUR); }
            }
        } else {
            uint64_t tmp; fread(&tmp,8,1,fp);
        }
    }

    for (uint64_t i = 0; i < n_t; i++) {
        uint64_t nl; fread(&nl,8,1,fp);
        char name[256]; fread(name,1,nl,fp); name[nl]=0;
        uint32_t nd; fread(&nd,4,1,fp);
        uint64_t dims[4]; for(int d=0;d<nd;d++) fread(&dims[d],8,1,fp);
        uint32_t tt; fread(&tt,4,1,fp);
        uint64_t off; fread(&off,8,1,fp);

        if (strstr(name, "blk.0.ffn_gate.weight")) {
            printf("FOUND: %s offset=%llu\n", name, (unsigned long long)off);
            long tds = ftell(fp);
            printf("tensor_data_start = %ld\n", tds);

            printf("\nBlocks 30-40 scales at gate data:\n");
            for (int b = 30; b <= 40; b++) {
                fseek(fp, tds + off + b * 34, SEEK_SET);
                uint16_t sraw; fread(&sraw,2,1,fp);
                float sf = fp16_to_fp32(sraw);
                printf("  block %2d: 0x%04x -> %f", b, (unsigned)sraw, (double)sf);
                if (isnan(sf)) printf(" *** NAN ***");
                printf("\n");
            }

            printf("\nBlocks 30-40 scales READING RAW from ABSOLUTE offset:\n");
            for (int b = 30; b <= 40; b++) {
                fseek(fp, off + b * 34, SEEK_SET);
                uint16_t sraw; fread(&sraw,2,1,fp);
                float sf = fp16_to_fp32(sraw);
                printf("  block %2d: 0x%04x -> %f", b, (unsigned)sraw, (double)sf);
                if (isnan(sf)) printf(" *** NAN ***");
                printf("\n");
            }
            break;
        }
    }
    fclose(fp);
    return 0;
}
