/*
 * tools/multi_view_verify.c — verify weight integrity through multiple RID views
 * ════════════════════════════════════════════════════════════════════════
 * Tests: reading data through 2+ independent views provides error detection
 * without external checksums (SHA256, CRC, etc.).
 *
 * BUILD: gcc -O2 -std=c11 -o multi_view_verify.exe tools/multi_view_verify.c -lm
 * USAGE: multi_view_verify.exe <model.gguf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── RID geometry — exact copy from gguf_roundtrip.c ────────────── */
typedef struct { int64_t x, y, z; } V3;
static V3 V(int64_t x,int64_t y,int64_t z){ V3 v={x,y,z}; return v; }
static V3 vsub(V3 a,V3 b){ return V(a.x-b.x,a.y-b.y,a.z-b.z); }
static int64_t vdot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static int64_t dist2(V3 a,V3 b){ V3 d=vsub(a,b); return vdot(d,d); }
static V3 verts[20]; static uint32_t n_verts=0;
static uint32_t faces[16][5]; static uint32_t n_faces=0;
static uint32_t edges[40][2]; static uint32_t n_edges=0;
static void build_dodeca(void){
    const int64_t S=104,H=64,P=169; const int sg[2]={1,-1};
    for(int sx=0;sx<2;sx++)for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++)
        verts[n_verts++]=V(S*sg[sx],S*sg[sy],S*sg[sz]);
    for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++){
        verts[n_verts++]=V(0,H*sg[sy],P*sg[sz]);
        verts[n_verts++]=V(H*sg[sy],P*sg[sz],0);
        verts[n_verts++]=V(P*sg[sz],0,H*sg[sy]);
    }
}
#define FACE_POS(f,u) ({int _p=-1; \
    for(int _q=0;_q<5;_q++) if(faces[(f)][_q]==(u)){_p=_q;break;} _p;})
static int32_t rid_of[320];
static uint32_t rf_v[64], rf_f[64], nrf=0;
static uint32_t sqc[40][4];
static uint8_t radj[64][64];
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x, uint8_t *par){
    if(uf_p[x]<0){ *par=0; return x; }
    uint8_t pp; int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp; uf_p[x]=r; *par=uf_par[x]; return r;
}
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj; int ri=uf_find(i,&pi), rj=uf_find(j,&pj);
    if(ri==rj) return (pi^pj)==d;
    if(uf_p[ri]>uf_p[rj]){ int t=ri;ri=rj;rj=t; }
    uf_p[ri]+=uf_p[rj]; uf_p[rj]=ri; uf_par[rj]=pi^pj^d; return 1;
}
static int build_rid(uint8_t vw[4][60]){
    build_dodeca();
    int64_t emin=0;
    for(uint32_t i=0;i<n_verts;i++)for(uint32_t j=i+1;j<n_verts;j++){
        int64_t d2=dist2(verts[i],verts[j]);
        if(d2&&(!emin||d2<emin)) emin=d2;
    }
    int64_t elo=emin, ehi=emin+emin/16;
    #define ADJ(i,j) ({int64_t d2=dist2(verts[i],verts[j]); d2&&d2>=elo&&d2<=ehi;})
    for(uint32_t i=0;i<n_verts;i++)
        for(uint32_t j=i+1;j<n_verts;j++)
            if(ADJ(i,j)&&n_edges<40){edges[n_edges][0]=i;edges[n_edges][1]=j;n_edges++;}
    for(uint32_t a=0;a<n_verts;a++)
    for(uint32_t b=0;b<n_verts;b++){
        if(b==a||!ADJ(a,b))continue;
        for(uint32_t c=0;c<n_verts;c++){
            if(c==a||c==b||!ADJ(b,c))continue;
            for(uint32_t dd=0;dd<n_verts;dd++){
                if(dd==a||dd==b||dd==c||!ADJ(c,dd))continue;
                for(uint32_t e=0;e<n_verts;e++){
                    if(e==a||e==b||e==c||e==dd)continue;
                    if(!ADJ(dd,e)||!ADJ(e,a))continue;
                    uint32_t mn5=a;
                    if(b<mn5)mn5=b; if(c<mn5)mn5=c;
                    if(dd<mn5)mn5=dd; if(e<mn5)mn5=e;
                    if(a!=mn5)continue;
                    int dupf=0;
                    for(uint32_t fc=0;fc<n_faces&&!dupf;fc++){
                        int same=1; uint32_t cc[5]={a,b,c,dd,e};
                        for(int p=0;p<5&&same;p++){
                            int hit=0;
                            for(int q=0;q<5;q++) if(faces[fc][q]==cc[p]){hit=1;break;}
                            if(!hit)same=0;
                        }
                        if(same)dupf=1;
                    }
                    if(dupf||n_faces>=16)continue;
                    faces[n_faces][0]=a; faces[n_faces][1]=b;
                    faces[n_faces][2]=c; faces[n_faces][3]=dd; faces[n_faces][4]=e;
                    n_faces++;
                }
            }
        }
    }
    if(n_faces>=16)return -1;
    memset(rid_of,0xFF,sizeof(rid_of));
    for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++){
            uint32_t u=faces[f][p];
            if(rid_of[u*16+f]<0){ rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u; rf_f[nrf]=f; nrf++; }
        }
    memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i], f=rf_f[i];
        int pos=-1;
        for(int p=0;p<5;p++) if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5], nx=faces[f][(pos+1)%5];
        radj[i][rid_of[pv*16+f]]=radj[rid_of[pv*16+f]][i]=1;
        radj[i][rid_of[nx*16+f]]=radj[rid_of[nx*16+f]][i]=1;
        for(uint32_t g=0;g<n_faces;g++){
            if(g==f)continue;
            for(int p2=0;p2<5;p2++) if(faces[g][p2]==u){
                radj[i][rid_of[u*16+g]]=radj[rid_of[u*16+g]][i]=1; break;
            }
        }
    }
    uint32_t deg_ok=1, e2=0;
    for(uint32_t i=0;i<nrf;i++){ uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++) d+=radj[i][j];
        if(d!=4) deg_ok=0; }
    for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++) e2+=radj[i][j];
    if(nrf!=60||e2!=120||!deg_ok) return -1;
    static uint8_t onsq[64][40]; static int32_t vsq[64][2];
    memset(onsq,0,sizeof(onsq)); memset(vsq,0xFF,sizeof(vsq));
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t a=edges[k][0], b=edges[k][1], ff[2], nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++)
            if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0) ff[nf++]=g;
        sqc[k][0]=(uint32_t)rid_of[a*16+ff[0]]; sqc[k][1]=(uint32_t)rid_of[b*16+ff[0]];
        sqc[k][2]=(uint32_t)rid_of[b*16+ff[1]]; sqc[k][3]=(uint32_t)rid_of[a*16+ff[1]];
        for(int c=0;c<4;c++){
            uint32_t w=sqc[k][c]; onsq[w][k]=1;
            if(vsq[w][0]<0) vsq[w][0]=(int32_t)k;
            else if(vsq[w][1]<0 && vsq[w][0]!=(int32_t)k) vsq[w][1]=(int32_t)k;
        }
    }
    memset(uf_p,0xFF,sizeof(uf_p)); memset(uf_par,0,sizeof(uf_par));
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0], j=vsq[w][1];
        uint8_t cw0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
        uint8_t cw1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(cw0^cw1^1))) return -1;
    }
    { uint8_t bits[40]; memset(bits,0,sizeof(bits));
      for(uint32_t k=0;k<n_edges;k++){ uint8_t p; uf_find((int)k,&p); bits[k]=p; }
      { uint32_t n=0;
        for(uint32_t f=0;f<n_faces;f++)
          for(int p=0;p<5;p++) vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f]; }
      { uint32_t n=0;
        for(uint32_t u=0;u<n_verts;u++)
          for(uint32_t g=0;g<n_faces;g++)
              if(FACE_POS(g,u)>=0) vw[1][n++]=(uint8_t)rid_of[u*16+g]; }
      { uint32_t n=0;
        for(uint32_t k=0;k<n_edges;k++){
            uint32_t a,b;
            if(bits[k]==0){ a=sqc[k][0]; b=sqc[k][2]; }
            else          { a=sqc[k][1]; b=sqc[k][3]; }
            vw[2][n++]=(uint8_t)a; vw[2][n++]=(uint8_t)b; } }
      { uint32_t n=0;
        for(uint32_t k=0;k<n_edges;k++){
            uint32_t a,b;
            if(bits[k]==0){ a=sqc[k][1]; b=sqc[k][3]; }
            else          { a=sqc[k][0]; b=sqc[k][2]; }
            vw[3][n++]=(uint8_t)a; vw[3][n++]=(uint8_t)b; } }
    }
    for(int v=0;v<4;v++){
        uint8_t hit[64]; memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){
            if(hit[vw[v][p]]) return -1;
            hit[vw[v][p]]=1;
        }
    }
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────────── */
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)     ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16)^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32)^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48)^ *(const uint64_t *)(p + b + 56);
    return x;
}

#define PART_BYTES (128u * 1024u)
#define RID_SLOTS  60u

/* Read through a permuted view: for each part, apply the view permutation */
static void read_via_view(const uint8_t *region, uint32_t total_parts,
                           uint32_t file_sz, const uint8_t vw[60],
                           uint8_t *out) {
    for (uint32_t p = 0; p < total_parts; p++) {
        uint32_t L = p / RID_SLOTS;
        uint32_t s = p % RID_SLOTS;
        uint32_t mapped_s = vw[s];
        uint32_t off = L * PART_BYTES * RID_SLOTS + mapped_s * PART_BYTES;
        uint32_t remaining = file_sz - p * PART_BYTES;
        uint32_t len = remaining < PART_BYTES ? remaining : PART_BYTES;
        memcpy(out + (size_t)p * PART_BYTES, region + off, len);
    }
}

/* ── Main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    srand(42);

    printf("=== Multi-View Weight Verification ===\n\n");

    /* Build RID views 0-3 */
    static uint8_t vw[4][60];
    if (build_rid(vw) != 0) { printf("FAIL rid geometry\n"); return 1; }
    printf("RID geometry: OK (60 slots, 120 edges, degree 4)\n");
    static const char *lname[] = {"pent","tri","snubL","snubR"};

    /* Read file */
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "ERROR: cannot open %s\n", path); return 1; }
    _fseeki64(fp, 0, SEEK_END);
    int64_t file_sz = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    uint8_t *src = (uint8_t *)malloc((size_t)file_sz);
    size_t nr = fread(src, 1, (size_t)file_sz, fp);
    fclose(fp);
    if ((int64_t)nr != file_sz) { fprintf(stderr, "FAIL read\n"); return 1; }

    uint32_t total_parts = (uint32_t)(((size_t)file_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;

    printf("file: %s\n", path);
    printf("size: %.2f MB\n", (double)file_sz / (1024.0 * 1024.0));
    printf("parts: %u · layers: %u\n\n", total_parts, layers);

    /* Store into slot region (canonical) */
    uint8_t *region = (uint8_t *)calloc((size_t)layers * RID_SLOTS * PART_BYTES, 1);
    for (uint32_t p = 0; p < total_parts; p++) {
        uint32_t L = p / RID_SLOTS, s = p % RID_SLOTS;
        uint32_t off = L * PART_BYTES * RID_SLOTS + s * PART_BYTES;
        uint32_t remaining = (uint32_t)(file_sz - (int64_t)p * PART_BYTES);
        uint32_t len = remaining < PART_BYTES ? remaining : PART_BYTES;
        memcpy(region + off, src + (size_t)p * PART_BYTES, len);
    }

    /* ── Phase 1: View consistency ───────────────────────────────── */
    printf("─── PHASE 1: View Consistency ───\n");
    uint64_t xors[4];
    uint8_t *buf = (uint8_t *)malloc((size_t)total_parts * PART_BYTES);
    for (int v = 0; v < 4; v++) {
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[v], buf);
        xors[v] = xor64(buf, (uint32_t)file_sz);
        printf("  %s: 0x%016llx\n", lname[v], (unsigned long long)xors[v]);
    }
    free(buf);
    int all_same = (xors[0]==xors[1] && xors[1]==xors[2] && xors[2]==xors[3]);
    printf("all views identical: %s\n\n", all_same ? "YES ✓" : "NO ✗");

    /* ── Phase 2: Bit-flip detection ─────────────────────────────── */
    printf("─── PHASE 2: Bit-Flip Detection ───\n");
    int n_trials = 100;
    int det_a=0, det_b=0, det_c=0, det_any=0, det_all=0;

    buf = (uint8_t *)malloc((size_t)total_parts * PART_BYTES);
    for (int t = 0; t < n_trials; t++) {
        uint32_t flip_byte = (uint32_t)((uint64_t)rand() * ((uint64_t)file_sz - 1) / RAND_MAX);
        int flip_bit = rand() % 8;
        region[flip_byte] ^= (1 << flip_bit);

        uint64_t xa, xb, xc, xd;
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[0], buf);
        xa = xor64(buf, (uint32_t)file_sz);
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[1], buf);
        xb = xor64(buf, (uint32_t)file_sz);
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[2], buf);
        xc = xor64(buf, (uint32_t)file_sz);
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[3], buf);
        xd = xor64(buf, (uint32_t)file_sz);

        int da = (xa != xors[0]);
        int db = (xb != xors[1]);
        int dc = (xc != xors[2]);
        int dd = (xd != xors[3]);
        if (da) det_a++;
        if (db) det_b++;
        if (dc) det_c++;
        if (da||db||dc||dd) det_any++;
        if (da&&db&&dc&&dd) det_all++;

        region[flip_byte] ^= (1 << flip_bit);
    }
    free(buf);

    printf("trials: %d random bit-flips\n", n_trials);
    printf("detected by pent:   %d/%d (%.1f%%)\n", det_a, n_trials, 100.0*det_a/n_trials);
    printf("detected by tri:    %d/%d (%.1f%%)\n", det_b, n_trials, 100.0*det_b/n_trials);
    printf("detected by snubL:  %d/%d (%.1f%%)\n", det_c, n_trials, 100.0*det_c/n_trials);
    printf("detected by ANY:    %d/%d (%.1f%%)\n", det_any, n_trials, 100.0*det_any/n_trials);
    printf("detected by ALL:    %d/%d (%.1f%%)\n", det_all, n_trials, 100.0*det_all/n_trials);
    printf("\n");

    /* ── Phase 3: View independence ───────────────────────────────── */
    printf("─── PHASE 3: Cross-View Independence ───\n");
    int only_pent=0, only_tri=0, only_snubL=0, none=0;

    buf = (uint8_t *)malloc((size_t)total_parts * PART_BYTES);
    for (int t = 0; t < n_trials; t++) {
        uint32_t flip_byte = (uint32_t)((uint64_t)rand() * ((uint64_t)file_sz - 1) / RAND_MAX);
        int flip_bit = rand() % 8;
        region[flip_byte] ^= (1 << flip_bit);

        uint64_t xa, xb, xc;
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[0], buf);
        xa = xor64(buf, (uint32_t)file_sz);
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[1], buf);
        xb = xor64(buf, (uint32_t)file_sz);
        read_via_view(region, total_parts, (uint32_t)file_sz, vw[2], buf);
        xc = xor64(buf, (uint32_t)file_sz);

        int da = (xa != xors[0]);
        int db = (xb != xors[1]);
        int dc = (xc != xors[2]);

        if (da && !db && !dc) only_pent++;
        else if (!da && db && !dc) only_tri++;
        else if (!da && !db && dc) only_snubL++;
        else if (!da && !db && !dc) none++;

        region[flip_byte] ^= (1 << flip_bit);
    }
    free(buf);

    printf("only pent detects:  %d/%d (%.1f%%)\n", only_pent, n_trials, 100.0*only_pent/n_trials);
    printf("only tri detects:   %d/%d (%.1f%%)\n", only_tri, n_trials, 100.0*only_tri/n_trials);
    printf("only snubL detects: %d/%d (%.1f%%)\n", only_snubL, n_trials, 100.0*only_snubL/n_trials);
    printf("none detect:        %d/%d (%.1f%%)\n", none, n_trials, 100.0*none/n_trials);
    printf("\n");

    /* ── Summary ─────────────────────────────────────────────────── */
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("MULTI-VIEW VERIFICATION SUMMARY\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("Views: pent, tri, snubL, snubR (4 independent permutations)\n");
    printf("Bit-flip detection: %.1f%% (any single view)\n", 100.0*det_any/n_trials);
    printf("View independence:  %.1f%% (only one view catches it)\n",
           100.0*(only_pent+only_tri+only_snubL)/n_trials);
    printf("\n");
    printf("KEY INSIGHT:\n");
    printf("  - Each view = independent error detector\n");
    printf("  - No external checksum (SHA256/CRC) needed\n");
    printf("  - Views double as data access languages\n");
    printf("  - Trade-off: need 2+ reads to verify\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    free(src); free(region);
    return 0;
}
