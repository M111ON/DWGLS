/*
 * tools/geo_rid_graft.c — MAINLINE: RID slots → DtSlotRegion → llama.cpp
 * ════════════════════════════════════════════════════════════════════════
 * The direct pipe (docs/ARCHIMEDEAN-STOCK-2026-08-22.md §5):
 *
 *   A  RID-BAKE   : model bytes → DtSlotRegion (mmap twin), addressed by
 *                   RID slots: part f → layer l=f/60 · slot w=f%60, routed
 *                   through a LANGUAGE VIEW: addr = l*60 + viewpos(w)
 *                   gate: readback byte-identical vs source
 *   B  UNFOLD     : region → rebuilt GGUF byte-identical to ORIGINAL
 *                   (header verbatim + payload reassembled via inverse map)
 *                   gate: full-file memcmp == source · for ALL 3 languages
 *   C  INFERENCE  : real llama.cpp b9733 on rebuilt vs original
 *                   gate: token streams identical + logits BITWISE @step0
 *   D  DAMAGE     : flip 1 byte inside the REGION → detect via per-part
 *                   XOR · localize exact slot R(v,f)+layer · re-bake 1 part
 *                   → unfold lossless again
 *
 *   Language views over the 60 RID slots (from geo_rid_serve):
 *     pent : 12 faces × 5-cycle corners      (dodeca language)
 *     tri  : 20 vertex stars × 3 corners     (icosa language)
 *     snub : 30 squares × diagonal endpoints (chiral perfect matching)
 *
 * BUILD:
 *   make rid-graft
 * PATH needs I:/llama/llama-b9733-bin-win-vulkan-x64 at runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"
#include "ggml-backend.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/gguf_reader.h"
#include "../core/infra/dramtile_store.h"

/* ── silence llama.cpp info spam ────────────────────────────────────── */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

/* ── int dodecahedron (phi = 13/8 x104) + RID labeling ──────────────── */
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
static uint8_t radj[64][64];
static uint32_t sqc[40][4];

/* union-find with parity — snub diagonal solve (proven unique pair) */
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x, uint8_t *par){
    if(uf_p[x]<0){ *par=0; return x; }
    uint8_t pp; int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp; uf_p[x]=r; *par=uf_par[x]; return r;
}
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj; int ri=uf_find(i,&pi), rj=uf_find(j,&pj);
    if(ri==rj) return (pi^pj)==d;
    uf_p[ri]=rj; uf_par[ri]=(uint8_t)(pi^d^pj);
    return 1;
}

/* build RID base + solve snub diagonals; fills vw[3][60] language views.
 * returns 0 ok */
static int build_rid(uint8_t vw[3][60]) {
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
                    if(b<mn5)mn5=b;
                    if(c<mn5)mn5=c;
                    if(dd<mn5)mn5=dd;
                    if(e<mn5)mn5=e;
                    if(a!=mn5||b>e)continue;
                    uint32_t cl[5]={a,b,c,dd,e};
                    V3 u=vsub(verts[cl[1]],verts[cl[0]]);
                    V3 w=vsub(verts[cl[2]],verts[cl[0]]);
                    int planar=1;
                    for(int k=3;k<5&&planar;k++){
                        V3 r2=vsub(verts[cl[k]],verts[cl[0]]);
                        int64_t dx=u.y*w.z-u.z*w.y, dy=u.z*w.x-u.x*w.z,
                                dz=u.x*w.y-u.y*w.x;
                        if(labs((long)(r2.x*dx+r2.y*dy+r2.z*dz))>100000L) planar=0;
                    }
                    if(!planar)continue;
                    int dupf=0;
                    for(uint32_t fc=0;fc<n_faces&&!dupf;fc++){
                        int same=1;
                        for(int p=0;p<5&&same;p++){
                            int hit=0;
                            for(int q=0;q<5;q++)
                                if(faces[fc][q]==cl[p]){hit=1;break;}
                            if(!hit)same=0;
                        }
                        if(same)dupf=1;
                    }
                    if(dupf||n_faces>=16)continue;
                    memcpy(faces[n_faces],cl,sizeof(cl));
                    n_faces++;
                }
                if(n_faces>=16)break;
            }
            if(n_faces>=16)break;
        }
        if(n_faces>=16)break;
    }
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

    /* squares + membership */
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
    /* snab diagonal parity solve */
    memset(uf_p,0xFF,sizeof(uf_p)); memset(uf_par,0,sizeof(uf_par));
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0], j=vsq[w][1];
        uint8_t cw0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
        uint8_t cw1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(cw0^cw1^1))) return -1;
    }
    uint8_t bits[40], par_of[40];
    { int32_t seen[40]; memset(seen,0xFF,sizeof(seen)); uint32_t nc=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint8_t p; int r=uf_find((int)k,&p);
          if(seen[r]<0) seen[r]=(int32_t)nc++;
          par_of[k]=p;
      }
      memset(bits,0,sizeof(bits));
      for(uint32_t k=0;k<n_edges;k++) bits[k]=par_of[k];
    }
    /* views */
    { uint32_t n=0;
      for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++) vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f];
    }
    { uint32_t n=0;
      for(uint32_t u=0;u<n_verts;u++)
        for(uint32_t g=0;g<n_faces;g++)
            if(FACE_POS(g,u)>=0) vw[1][n++]=(uint8_t)rid_of[u*16+g];
    }
    { uint32_t n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){ a=sqc[k][0]; b=sqc[k][2]; }
          else          { a=sqc[k][1]; b=sqc[k][3]; }
          vw[2][n++]=(uint8_t)a; vw[2][n++]=(uint8_t)b;
      }
    }
    /* bijection check */
    for(int v=0;v<3;v++){
        uint8_t hit[64]; memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){
            if(hit[vw[v][p]]) return -1;
            hit[vw[v][p]]=1;
        }
    }
    return 0;
}

/* inverse of "addr = l*60 + pos(w)": given addr and language, part id */
static inline uint32_t inv_view(const uint8_t vw[60], uint32_t addr,
                                uint32_t *w_out) {
    uint32_t l = addr / 60, pos = addr % 60, w = vw[pos];
    *w_out = w;
    return l * 60 + w;
}
/* forward: part id → addr through language view */
static inline uint32_t fwd_view(const uint8_t vw[60], uint32_t f) {
    uint32_t w = f % 60, l = f / 60, pos = 60;
    for(uint32_t p=0;p<60;p++) if(vw[p]==w){ pos=p; break; }
    return l * 60 + pos;
}

/* XOR fold over 64-byte chunks */
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)     ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16)^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32)^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48)^ *(const uint64_t *)(p + b + 56);
    return x;
}

/* ── greedy generation (identical harness to gguf_graft_field) ──────── */
static llama_token *generate(const char *gguf_path, const char *prompt,
                             int n_gen, int *n_out,
                             float *logits0, int n_vocab_cap, int *nv_out) {
    *n_out = 0; *nv_out = 0;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (!model) return NULL;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 64; cp.n_threads = 4; cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); return NULL; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);
    *nv_out = n_vocab > n_vocab_cap ? n_vocab_cap : n_vocab;
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), NULL, 0, true, false);
    if (n_prompt < 0) n_prompt = -n_prompt;
    llama_token *toks = (llama_token *)malloc((size_t)(n_prompt + 1) * sizeof(llama_token));
    int n2 = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks, n_prompt, true, false);
    if (n2 < 0) n2 = -n2;
    n_prompt = n2;
    llama_token *out = (llama_token *)malloc((size_t)(n_gen + 1) * sizeof(llama_token));
    if (llama_decode(ctx, llama_batch_get_one(toks, n_prompt)) != 0) {
        free(toks); free(out); llama_free(ctx); llama_model_free(model); return NULL;
    }
    for (int i = 0; i < n_gen; i++) {
        const float *logits = (i == 0) ? llama_get_logits_ith(ctx, n_prompt - 1)
                                       : llama_get_logits(ctx);
        if (i == 0 && logits0)
            memcpy(logits0, logits, sizeof(float) * (size_t)*nv_out);
        llama_token best = 0; float best_v = logits[0];
        for (int t = 1; t < n_vocab; t++)
            if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token)t; }
        out[i] = best; (*n_out)++;
        if (llama_decode(ctx, llama_batch_get_one(&best, 1)) != 0) break;
    }
    free(toks); llama_free(ctx); llama_model_free(model);
    return out;
}

#define PART_BYTES   (128u * 1024u)
#define RID_SLOTS    60u

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    int n_gen = argc > 3 ? atoi(argv[3]) : 40;
    const char *twin_path = argc > 4 ? argv[4] : "build\\rid_slot.twin";

    printf("=== geo_rid_graft — RID slots -> DtSlotRegion -> llama ===\n");

    /* ── RID geometry + language views ──────────────────────────────── */
    static uint8_t vw[3][60];
    const char *lname[3] = { "pent", "tri", "snub" };
    if (build_rid(vw) != 0) { printf("FAIL rid geometry\n"); return 1; }
    printf("RID geometry: 60 slots · 3 languages bijective · snub solved\n\n");

    /* ── open source ────────────────────────────────────────────────── */
    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open %s\n", path); return 1; }
    printf("source: %s · %u tensors · %.1f MB\n",
           path, r.n_tensors, (double)r.base_sz / 1e6);

    /* part table */
    uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++)
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;
    typedef struct { const uint8_t *src; uint32_t len; size_t file_off; } Job;
    Job *jobs = (Job *)malloc(sizeof(Job) * total_parts);
    if (!jobs) return 1;
    { uint32_t f = 0;
      for (uint32_t i = 0; i < r.n_tensors; i++) {
          const uint8_t *src = r.base + r.data_offset + r.offsets[i];
          uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
          for (uint32_t p = 0; p < np && f < total_parts; p++, f++) {
              uint32_t off = p * PART_BYTES;
              uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
              jobs[f].src = src + off;
              jobs[f].len = len;
              jobs[f].file_off = (size_t)r.data_offset + r.offsets[i] + off;
          }
      }
    }
    printf("parts: %u · layers: %u · region %.1f MB\n\n",
           total_parts, layers, (double)layers * RID_SLOTS * PART_BYTES / 1e6);

    /* per-language pipes */
    int all_ok = 1;
    static float lg_a[160000], lg_b[160000];
    int ran_inference = 0;

    for (int lang = 0; lang < 3; lang++) {
        /* A. BAKE into twin region through language view */
        DtSlotRegion reg;
        remove(twin_path);
        if (dt_slot_init_twin(&reg, twin_path,
                              (size_t)layers * RID_SLOTS, PART_BYTES) != 0) {
            printf("FAIL dt_slot_init_twin\n"); return 1;
        }
        int bake_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t addr = fwd_view(vw[lang], f);
            if (!dt_slot_put(&reg, addr, jobs[f].src, jobs[f].len)) bake_bad++;
        }
        uint32_t rb_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t addr = fwd_view(vw[lang], f);
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p || memcmp(p, jobs[f].src, jobs[f].len) != 0) rb_bad++;
        }
        printf("[%s] A BAKE  %u parts · readback bad=%u (%s)\n",
               lname[lang], total_parts, rb_bad,
               (bake_bad || rb_bad) ? "FAIL" : "lossless");
        if (bake_bad || rb_bad) { all_ok = 0; dt_slot_destroy(&reg); continue; }

        /* B. UNFOLD — rebuilt file must be BYTE-IDENTICAL to original */
        uint8_t *out = (uint8_t *)malloc(r.base_sz);
        if (!out) { dt_slot_destroy(&reg); return 1; }
        memcpy(out, r.base, r.data_offset);           /* header verbatim */
        for (uint32_t addr = 0; addr < (uint32_t)layers * RID_SLOTS; addr++) {
            uint32_t w; uint32_t f = inv_view(vw[lang], addr, &w);
            if (f >= total_parts) continue;
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p) continue;
            memcpy(out + jobs[f].file_off, p, jobs[f].len);
        }
        int unfold_ok = (memcmp(out, r.base, r.base_sz) == 0);
        printf("[%s] B FOLD  rebuilt %zu MB · byte-identical=%s\n",
               lname[lang], (size_t)(r.base_sz / (1024*1024)),
               unfold_ok ? "YES" : "NO");
        if (!unfold_ok) { all_ok = 0; free(out); dt_slot_destroy(&reg); continue; }

        /* C. INFERENCE — once (bytes identical ⇒ equivalent, prove once end-to-end) */
        if (!ran_inference) {
            const char *graft = "build\\rid_graft.gguf";
            FILE *fp = fopen(graft, "wb");
            fwrite(out, 1, r.base_sz, fp);
            fclose(fp);
            llama_backend_init();
            llama_log_set(quiet_log, NULL);
            /* backends live beside the llama DLLs — load explicitly */
            ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");
            int na = 0, nb = 0, nv = 0;
            llama_token *ga = generate(graft, prompt, n_gen, &na, lg_a, 160000, &nv);
            llama_token *gb = generate(path,   prompt, n_gen, &nb, lg_b, 160000, &nv);
            int tok_ok = (ga && gb && na == nb && na > 0 &&
                          memcmp(ga, gb, sizeof(llama_token) * (size_t)na) == 0);
            int log_ok = (memcmp(lg_a, lg_b, sizeof(float) * (size_t)nv) == 0);
            printf("[%s] C INFER tokens identical=%s (%d) · logits@0 bitwise=%s (%d dims)\n",
                   lname[lang], tok_ok ? "YES" : "NO", na,
                   log_ok ? "YES" : "NO", nv);
            free(ga); free(gb);
            llama_backend_free();
            if (!tok_ok || !log_ok) all_ok = 0;
            ran_inference = 1;
        }
        free(out);

        /* D. DAMAGE DRILL (first language only — semantics are language-invariant) */
        if (lang == 0 && total_parts > 9) {
            uint32_t fhit = total_parts / 2;
            uint32_t aha = fwd_view(vw[lang], fhit);
            uint8_t *pbyte = dt_slot_ptr(&reg, aha) + 17;
            *pbyte ^= 0x80;
            /* detect + localize via per-part XOR against source */
            uint32_t found = total_parts;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint32_t ad = fwd_view(vw[lang], f);
                uint8_t *p = dt_slot_ptr(&reg, ad);
                if (xor64(p, jobs[f].len) != xor64(jobs[f].src, jobs[f].len)) {
                    found = f; break;
                }
            }
            int loc_ok = (found == fhit);
            if (loc_ok) {  /* restore exactly one part */
                dt_slot_put(&reg, aha, jobs[found].src, jobs[found].len);
            }
            uint32_t still_bad = 0;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint32_t ad = fwd_view(vw[lang], f);
                uint8_t *p = dt_slot_ptr(&reg, ad);
                if (memcmp(p, jobs[f].src, jobs[f].len) != 0) still_bad++;
            }
            printf("[%s] D DRILL  flip@part %u → localized=%s · restored bad=%u (%s)\n",
                   lname[lang], fhit, loc_ok ? "YES" : "NO", still_bad,
                   still_bad ? "STILL BROKEN" : "lossless again");
            if (!loc_ok || still_bad) all_ok = 0;
        }

        dt_slot_destroy(&reg);
        printf("\n");
    }

    /* keep the last unfolded graft for inspection */
    /* (rebuilt files were freed; regenerate pent graft quickly for artifact) */

    printf("%s\n", all_ok ? "RESULT: PASSED — RID slots serve llama lossless"
                          : "RESULT: FAILED");
    free(jobs);
    gguf_close(&r);
    return all_ok ? 0 : 1;
}
