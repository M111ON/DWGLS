/* kis_codec_v5.h — Angular Wavelet: sparse residual only */
#ifndef KIS_V5_H
#define KIS_V5_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define V5_MAGIC  0x4B435635
#define V5_GRID   144
#define V5_SLOTS  (V5_GRID*V5_GRID)

/* Angular map: code → (θ, φ) */
static inline void v5_angular(uint8_t code, uint32_t *th, uint32_t *ph) {
    uint32_t t = (uint32_t)code * 1440 / 256;
    uint32_t frame = (t * 37) % 1440;
    *th = frame % V5_GRID;
    *ph = ((uint32_t)code * 137 + 50) % V5_GRID;
}

/* Codebook: histogram only (~550B) */
typedef struct { uint32_t h[256]; uint8_t act[32]; uint32_t na,nw; } V5CB;
static void cb_build(V5CB *cb, const int8_t *w, uint32_t n) {
    memset(cb,0,sizeof(*cb)); cb->nw=n;
    for(uint32_t i=0;i<n;i++) cb->h[(uint8_t)w[i]]++;
    for(int v=0;v<256;v++) if(cb->h[v]>0){cb->act[v>>3]|=(1u<<(v&7));cb->na++;}
}
static uint32_t cb_enc(const V5CB *cb, uint8_t *o, uint32_t cap) {
    if(cap<36)return 0; uint32_t off=0;
    memcpy(o,&cb->nw,4);off+=4; memcpy(o+off,cb->act,32);off+=32;
    for(int v=0;v<256;v++){uint32_t c=cb->h[v];if(!c)continue;
        while(c>=0x80){if(off<cap)o[off++]=(c&0x7F)|0x80u;c>>=7;}
        if(off<cap)o[off++]=(uint8_t)c;} return off;
}
static int cb_dec(const uint8_t *d,uint32_t len,V5CB *cb) {
    if(!d||!cb||len<36)return -1; uint32_t off=0;
    memcpy(&cb->nw,d,4);off+=4; memcpy(cb->act,d+off,32);off+=32;
    memset(cb->h,0,sizeof(cb->h));cb->na=0;
    for(int v=0;v<256&&off<len;v++){
        if(!(cb->act[v>>3]&(1u<<(v&7))))continue;
        uint32_t c=0,s=0;
        while(off<len){uint8_t b=d[off++];c|=(b&0x7Fu)<<s;s+=7;if(!(b&0x80))break;}
        cb->h[v]=c;cb->na++;} return 0;
}
static void cb_recon(const V5CB *cb, int8_t *o, uint32_t n) {
    uint32_t p=0;
    for(int v=0;v<256&&p<n;v++)
        for(uint32_t i=0;i<cb->h[v]&&p<n;i++) o[p++]=(int8_t)(uint8_t)v;
}

/* Varint */
static uint32_t vi_enc(uint32_t v,uint8_t *o,uint32_t cap,uint32_t off){
    while(v>=0x80&&off<cap){o[off++]=(v&0x7F)|0x80u;v>>=7;}
    if(off<cap)o[off++]=(uint8_t)v; return off;
}
static void vi_dec(const uint8_t *d,uint32_t len,uint32_t *off,uint32_t *v){
    *v=0;uint32_t s=0;
    while(*off<len){uint8_t b=d[*off];*v|=(b&0x7Fu)<<s;s+=7;(*off)++;if(!(b&0x80))break;}
}

/* Encode: codebook + sparse residual */
static uint32_t v5_encode(const int8_t *w, uint32_t n, uint8_t *o, uint32_t cap) {
    if(!w||!o||cap<64||n==0)return 0;
    V5CB cb; cb_build(&cb,w,n);
    int8_t *sorted=(int8_t*)malloc(n); if(!sorted)return 0;
    cb_recon(&cb,sorted,n);

    /* Original grid: grid[slot] = original_index+1 (1-based, 0=empty) */
    uint32_t *grid=(uint32_t*)calloc(V5_SLOTS,sizeof(uint32_t));
    uint32_t *expected=(uint32_t*)calloc(V5_SLOTS,sizeof(uint32_t));
    if(!grid||!expected){free(sorted);free(grid);free(expected);return 0;}

    for(uint32_t i=0;i<n;i++){
        uint8_t c=(uint8_t)w[i]; uint32_t th,ph;
        v5_angular(c,&th,&ph); uint32_t s=th*V5_GRID+ph;
        while(grid[s]&&grid[s]<=n) s=(s+1)%V5_SLOTS;
        grid[s]=i+1;
    }
    for(uint32_t j=0;j<n;j++){
        uint8_t c=(uint8_t)sorted[j]; uint32_t th,ph;
        v5_angular(c,&th,&ph); uint32_t s=th*V5_GRID+ph;
        while(expected[s]&&expected[s]<=n) s=(s+1)%V5_SLOTS;
        expected[s]=j+1;
    }
    free(sorted);

    /* Count nonzero residuals */
    uint32_t nz=0;
    for(uint32_t s=0;s<V5_SLOTS;s++) if(grid[s]!=expected[s]) nz++;

    /* Encode */
    uint32_t off=0;
    uint32_t magic=V5_MAGIC; memcpy(o+off,&magic,4);off+=4;
    memcpy(o+off,&n,4);off+=4;
    memcpy(o+off,&nz,4);off+=4;
    uint32_t cb_len=cb_enc(&cb,o+off,cap-off); off+=cb_len;

    uint32_t prev=0;
    for(uint32_t s=0;s<V5_SLOTS;s++){
        if(grid[s]==expected[s])continue;
        off=vi_enc(s-prev,o,cap,off);
        int32_t diff=(int32_t)grid[s]-(int32_t)expected[s];
        uint32_t z=(uint32_t)((diff<<1)^(diff>>31));
        off=vi_enc(z,o,cap,off);
        prev=s;
    }
    free(grid);free(expected);
    return off;
}
#define kis_v5_encode v5_encode

/* Decode */
static int v5_decode(const uint8_t *d,uint32_t dlen,int8_t *out,uint32_t n) {
    if(!d||!out||dlen<12||n==0)return -1;
    uint32_t off=0,magic; memcpy(&magic,d,4);off+=4;
    if(magic!=V5_MAGIC)return -2;
    uint32_t nn; memcpy(&nn,d+off,4);off+=4;
    if(nn!=n)return -3;
    uint32_t nz; memcpy(&nz,d+off,4);off+=4;

    V5CB cb;
    if(cb_dec(d+off,dlen-off,&cb)!=0)return -4;
    off+=36;
    for(int v=0;v<256;v++){if(!cb.h[v])continue;uint32_t c=cb.h[v];
        while(c>=0x80){off++;c>>=7;}off++;}

    int8_t *sorted=(int8_t*)malloc(n); if(!sorted)return -5;
    cb_recon(&cb,sorted,n);

    uint32_t *expected=(uint32_t*)calloc(V5_SLOTS,sizeof(uint32_t));
    if(!expected){free(sorted);return -6;}
    for(uint32_t j=0;j<n;j++){
        uint8_t c=(uint8_t)sorted[j]; uint32_t th,ph;
        v5_angular(c,&th,&ph); uint32_t s=th*V5_GRID+ph;
        while(expected[s]&&expected[s]<=n) s=(s+1)%V5_SLOTS;
        expected[s]=j+1;
    }

    uint32_t prev=0;
    for(uint32_t i=0;i<nz&&off<dlen;i++){
        uint32_t delta; vi_dec(d,dlen,&off,&delta);
        uint32_t s=prev+delta;
        uint32_t z; vi_dec(d,dlen,&off,&z);
        int32_t diff=(int32_t)((z>>1)^-(z&1));
        expected[s]=(uint32_t)((int32_t)expected[s]+diff);
        prev=s;
    }

    memset(out,0,n);
    uint32_t si=0;
    for(uint32_t s=0;s<V5_SLOTS&&si<n;s++){
        if(expected[s]>0&&expected[s]<=n){
            uint32_t pos=expected[s]-1;
            if(pos<n) out[pos]=sorted[si];
            si++;
        }
    }
    free(sorted);free(expected);
    return 0;
}
#define kis_v5_decode v5_decode
#endif
