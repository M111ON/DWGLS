#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    FILE *fp = fopen("I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf", "rb");
    if (!fp) { printf("FAIL\n"); return 1; }
    
    uint32_t magic, version;
    uint64_t tensor_count, kv_count;
    fread(&magic, 4, 1, fp); fread(&version, 4, 1, fp);
    fread(&tensor_count, 8, 1, fp); fread(&kv_count, 8, 1, fp);
    printf("Header: tensors=%llu kv=%llu\n", (unsigned long long)tensor_count, (unsigned long long)kv_count);
    
    for (uint64_t i = 0; i < kv_count; i++) {
        long before = ftell(fp);
        uint64_t klen; fread(&klen, 8, 1, fp); fseek(fp, (long)klen, SEEK_CUR);
        uint32_t vtype; fread(&vtype, 4, 1, fp);
        switch (vtype) {
            case 0:case 1:fseek(fp,1,SEEK_CUR);break;
            case 2:case 3:fseek(fp,2,SEEK_CUR);break;
            case 4:case 5:case 6:fseek(fp,4,SEEK_CUR);break;
            case 7:fseek(fp,1,SEEK_CUR);break;
            case 8:{uint64_t sl;fread(&sl,8,1,fp);fseek(fp,(long)sl,SEEK_CUR);break;}
            case 9:{uint32_t at;uint64_t al;fread(&at,4,1,fp);fread(&al,8,1,fp);
                for(uint64_t a=0;a<al;a++)switch(at){
                    case 0:case 1:fseek(fp,1,SEEK_CUR);break;
                    case 2:case 3:fseek(fp,2,SEEK_CUR);break;
                    case 4:case 5:case 6:fseek(fp,4,SEEK_CUR);break;
                    case 7:fseek(fp,1,SEEK_CUR);break;
                    case 8:{uint64_t s2;fread(&s2,8,1,fp);fseek(fp,(long)s2,SEEK_CUR);break;}
                    case 10:case 11:case 12:fseek(fp,8,SEEK_CUR);break;
                }break;}
            case 10:case 11:case 12:fseek(fp,8,SEEK_CUR);break;
        }
        printf("  KV[%llu]: type=%d bytes=%ld\n",(unsigned long long)i,vtype,ftell(fp)-before);
    }
    
    long pos_after_kv = ftell(fp);
    printf("After KV: %ld\n", pos_after_kv);
    
    for (uint64_t i = 0; i < tensor_count; i++) {
        long before = ftell(fp);
        uint64_t nlen; fread(&nlen, 8, 1, fp);
        char name[256]; fread(name, 1, (size_t)nlen, fp); name[nlen]=0;
        uint32_t nd; fread(&nd, 4, 1, fp);
        for (int d = 0; d < (int)nd; d++) { uint64_t dim; fread(&dim, 8, 1, fp); }
        uint32_t tt; fread(&tt, 4, 1, fp);
        uint64_t off; fread(&off, 8, 1, fp);
        if (i < 5 || strstr(name,"ffn_gate") || strstr(name,"ffn_up") || strstr(name,"ffn_down") || i == tensor_count-1)
            printf("  T[%llu]: %s nd=%u type=%u off=%llu bytes=%ld\n",
                   (unsigned long long)i, name, nd, tt, (unsigned long long)off, ftell(fp)-before);
    }
    
    long tds = ftell(fp);
    printf("\ntensor_data_start = %ld\n", tds);
    
    /* Now re-scan to find gate offset */
    fseek(fp, pos_after_kv, SEEK_SET);
    uint64_t gate_off = 0, up_off = 0, down_off = 0;
    for (uint64_t i = 0; i < tensor_count; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, fp);
        char name[256]; fread(name, 1, (size_t)nlen, fp); name[nlen]=0;
        uint32_t nd; fread(&nd, 4, 1, fp);
        for (int d = 0; d < (int)nd; d++) { uint64_t dim; fread(&dim, 8, 1, fp); }
        uint32_t tt; fread(&tt, 4, 1, fp);
        uint64_t off; fread(&off, 8, 1, fp);
        if (strstr(name,"ffn_gate")) gate_off = off;
        if (strstr(name,"ffn_up"))   up_off = off;
        if (strstr(name,"ffn_down")) down_off = off;
    }
    printf("Gate: off=%llu  Up: off=%llu  Down: off=%llu\n",
           (unsigned long long)gate_off, (unsigned long long)up_off, (unsigned long long)down_off);
    
    /* Read Q8_0 blocks at correct position */
    fseek(fp, tds + (long)gate_off, SEEK_SET);
    printf("Reading 5 Q8_0 blocks from tds+gate_off (%ld+%llu=%ld):\n",
           tds, (unsigned long long)gate_off, tds + (long)gate_off);
    for (int b = 0; b < 5; b++) {
        uint16_t scale; fread(&scale, 2, 1, fp);
        uint8_t w[32]; fread(w, 1, 32, fp);
        int nz = 0; for(int k=0;k<32;k++) if(w[k]) nz++;
        printf("  Block %d: scale=0x%04x exp=%d nz=%d/32\n", b, scale, (scale>>10)&0x1f, nz);
    }
    fclose(fp);
    return 0;
}
