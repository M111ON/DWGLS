/*
 * gguf_meta.c — Count KV pairs + largest KV values in GGUF
 * gcc -O2 -I.hermes/desktop-attachments -o build/gguf_meta tools/gguf_meta.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"

/* KV value types: 0=U8,1=I8,2=U16,3=I16,4=U32,5=I32,6=F32,7=BOOL,8=STR,9=ARR,10=U64,11=I64,12=F64 */
static const char *vtype_name(uint32_t t) {
    static const char *n[] = {"U8","I8","U16","I16","U32","I32","F32","BOOL","STR","ARR","U64","I64","F64"};
    return t <= 12 ? n[t] : "?";
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <file>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 1; }

    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);
    printf("version=%u tensors=%llu kv_pairs=%llu\n",
           version, (unsigned long long)n_tensors, (unsigned long long)n_kv);

    uint64_t kv_bytes = 0;
    uint64_t largest_arr = 0;
    uint64_t largest_str = 0;
    char largest_arr_key[128] = "";

    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen;
        fread(&klen, 8, 1, f);
        char key[256] = "";
        if (klen < sizeof(key)) fread(key, (size_t)klen, 1, f);
        else fseek(f, (long)klen, SEEK_CUR);
        key[klen < 255 ? klen : 255] = 0;
        kv_bytes += 8 + klen;

        uint32_t vtype;
        fread(&vtype, 4, 1, f);
        kv_bytes += 4;

        if (vtype == 9) { /* array */
            uint32_t atype;
            fread(&atype, 4, 1, f);
            uint64_t narr;
            fread(&narr, 8, 1, f);
            kv_bytes += 12;
            if (atype == 8) { /* array of strings */
                uint64_t bytes = 0;
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t slen;
                    fread(&slen, 8, 1, f);
                    fseek(f, (long)slen, SEEK_CUR);
                    bytes += 8 + slen;
                }
                if (bytes > largest_arr) { largest_arr = bytes; strncpy(largest_arr_key, key, 127); }
                kv_bytes += bytes;
            } else {
                static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
                uint64_t bytes = (atype < 13 ? esz[atype] : 0) * narr;
                fseek(f, (long)bytes, SEEK_CUR);
                if (bytes > largest_arr) { largest_arr = bytes; strncpy(largest_arr_key, key, 127); }
                kv_bytes += bytes;
            }
        } else if (vtype == 8) { /* string */
            uint64_t slen;
            fread(&slen, 8, 1, f);
            fseek(f, (long)slen, SEEK_CUR);
            if (slen > largest_str) largest_str = slen;
            kv_bytes += 8 + slen;
        } else {
            uint32_t sz = vtype <= 12 ? (1u << (vtype < 2 ? 0 : vtype < 4 ? 1 : vtype < 7 ? 2 : vtype < 8 ? 1 : vtype < 11 ? 3 : 3)) : 0;
            /* rough: U8/I8=1, U16/I16=2, U32/I32/F32=4, BOOL=1, U64/I64/F64=8 */
            sz = (vtype==0||vtype==1||vtype==7) ? 1 : (vtype==2||vtype==3) ? 2 : (vtype<=6) ? 4 : 8;
            fseek(f, (long)sz, SEEK_CUR);
            kv_bytes += sz;
        }
    }
    printf("kv_metadata_bytes=%llu\n", (unsigned long long)kv_bytes);
    printf("largest_array: %s = %llu bytes\n", largest_arr_key[0]?largest_arr_key:"(none)", (unsigned long long)largest_arr);
    printf("largest_str=%llu bytes\n", (unsigned long long)largest_str);

    /* tensor info section size */
    long pos = ftell(f);
    printf("tensor_info_section_start=%ld\n", pos);
    uint64_t tensor_info_bytes = 0;
    for (uint64_t i = 0; i < n_tensors && i < 5; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        char name[256] = ""; fread(name, (size_t)(nlen<255?nlen:255), 1, f);
        if (nlen >= 255) fseek(f, (long)(nlen-255), SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        fseek(f, (long)nd*8, SEEK_CUR);
        uint32_t ty; fread(&ty, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        tensor_info_bytes += 8 + nlen + 4 + nd*8 + 4 + 8;
        printf("  [%llu] %s dims=%u type=%u\n", (unsigned long long)i, name, nd, ty);
    }
    printf("~tensor_info_bytes_per_entry=%llu (x%llu tensors)\n",
           (unsigned long long)tensor_info_bytes, (unsigned long long)n_tensors);
    fclose(f);
    return 0;
}
