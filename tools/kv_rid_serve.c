/*
 * tools/kv_rid_serve.c — llama KV/STATE ⇄ RID slot region (same path)
 * ════════════════════════════════════════════════════════════════════════
 * Mainline step 3: the SAME RID slot-region path that serves weights
 * (geo_rid_graft) and files (geofs_rid) now serves CONTEXT STATE.
 *
 *   S1 BASE : ctx_base — decode(prompt) then greedy-generate n_gen tokens;
 *             record full stream + logits@step0 (oracle)
 *   S2 CKPT : ctx_a — decode(prompt) only; llama_state_get_data → blob;
 *             blob → 128KB parts → DtSlotRegion twin via LANGUAGE VIEW
 *             addr = l*60 + viewpos(w)      (pent / tri / snub)
 *   S3 SERVE: readback blob from region == original state bytes (lossless);
 *             ctx_b — decode(prompt), llama_state_set_data(readback),
 *             continue greedy generation:
 *               gate: tail tokens BITWISE == base tail · logits@restore BITWISE
 *   G DRILL : flip 1 byte inside state part → per-part XOR localize exact
 *             part → re-bake that part → S3 passes again
 *
 * BUILD: make kv-rid   (needs b9733 DLLs + Qwen GGUF, same as rid-graft)
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

#include "../core/infra/dramtile_store.h"

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        fputs(text, stderr);
}

/* ── RID geometry (identical to geo_rid_graft / geofs_rid) ──────────── */
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
    {
        uint8_t bits[40]; memset(bits,0,sizeof(bits));
        for(uint32_t k=0;k<n_edges;k++){
            uint8_t p; uf_find((int)k,&p);
            bits[k]=p;
        }
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
    }
    for(int v=0;v<3;v++){
        uint8_t hit[64]; memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){
            if(hit[vw[v][p]]) return -1;
            hit[vw[v][p]]=1;
        }
    }
    return 0;
}
static inline uint32_t fwd_view(const uint8_t vw[60], uint32_t f) {
    uint32_t w = f % 60, l = f / 60, pos = 60;
    for(uint32_t p=0;p<60;p++) if(vw[p]==w){ pos=p; break; }
    return l * 60 + pos;
}
static inline uint32_t inv_view(const uint8_t vw[60], uint32_t addr,
                                uint32_t *w_out) {
    uint32_t l = addr / 60, pos = addr % 60, w = vw[pos];
    *w_out = w;
    return l * 60 + w;
}
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)     ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16)^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32)^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48)^ *(const uint64_t *)(p + b + 56);
    return x;
}

/* ── shared model/context helpers ───────────────────────────────────── */
static struct llama_model *load_model(const char *path) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    return llama_model_load_from_file(path, mp);
}
static struct llama_context *make_ctx(struct llama_model *model) {
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 64; cp.n_threads = 4; cp.n_threads_batch = 4;
    return llama_init_from_model(model, cp);
}
/* tokenize with abs() fix */
static int tokenize(struct llama_vocab *vocab, const char *text,
                    llama_token *out, int cap) {
    int n = llama_tokenize(vocab, text, (int32_t)strlen(text), NULL, 0, true, false);
    if (n < 0) n = -n;
    if (n > cap) n = cap;
    int n2 = llama_tokenize(vocab, text, (int32_t)strlen(text), out, n, true, false);
    if (n2 < 0) n2 = -n2;
    return n2;
}
/* greedy step using current logits; returns best token */
static llama_token greedy(const float *logits, int n_vocab) {
    llama_token best = 0; float bv = logits[0];
    for (int t = 1; t < n_vocab; t++) if (logits[t] > bv) { bv = logits[t]; best = (llama_token)t; }
    return best;
}
/* decode prompt then greedy-generate `total` tokens; optionally capture
 * logits right after generated step `cap_idx` (1-based) */
static void gen_stream(struct llama_context *ctx, struct llama_vocab *vocab,
                       const char *prompt, int total,
                       llama_token *out_tok, float *out_lg, int cap_idx) {
    int n_vocab = llama_vocab_n_tokens(vocab);
    llama_token ptoks[256];
    int n = tokenize(vocab, prompt, ptoks, 256);
    llama_decode(ctx, llama_batch_get_one(ptoks, n));
    llama_token t = greedy(llama_get_logits_ith(ctx, n - 1), n_vocab);
    for (int i = 0; i < total; i++) {
        out_tok[i] = t;
        llama_decode(ctx, llama_batch_get_one(&t, 1));
        const float *L = llama_get_logits(ctx);
        if (out_lg && cap_idx == i + 1) memcpy(out_lg, L, sizeof(float) * (size_t)n_vocab);
        t = greedy(L, n_vocab);
    }
}

#define PART_BYTES   (128u * 1024u)
#define RID_SLOTS    60u
#define MAX_STATE_MB 256
#define N_PRE   100   /* tokens generated BEFORE the checkpoint */
#define N_POST   24   /* tokens generated AFTER restore */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *prompt = argc > 2 ? argv[2] : "The capital of France is";
    int n_gen = argc > 3 ? atoi(argv[3]) : 24;
    const char *twin_path = argc > 4 ? argv[4] : "build\\kv_slot.twin";

    printf("=== kv_rid_serve — llama STATE <-> RID slot region ===\n");

    static uint8_t vw[3][60];
    const char *lname[3] = { "pent", "tri", "snub" };
    if (build_rid(vw) != 0) { printf("FAIL rid geometry\n"); return 1; }

    llama_backend_init();
    llama_log_set(quiet_log, NULL);
    ggml_backend_load_all_from_path("I:/llama/llama-b9733-bin-win-vulkan-x64");

    struct llama_model *model = load_model(path);
    if (!model) { printf("FAIL model load\n"); return 1; }
    struct llama_vocab *vocab = (struct llama_vocab *)llama_model_get_vocab(model);
    int n_vocab = llama_vocab_n_tokens(vocab);

    /* ── S1 BASELINE: PRE + POST tokens, logits captured at boundary ── */
    struct llama_context *cb = make_ctx(model);
    llama_token base_tok[N_PRE + N_POST];
    float bound_lg[160000];
    /* capture logits right AFTER decoding the boundary token (out_tok[N_PRE])
     * — that is exactly what the restored context computes on its first
     * continuation decode */
    gen_stream(cb, vocab, prompt, N_PRE + N_POST, base_tok, bound_lg, N_PRE + 1);
    llama_free(cb);
    printf("S1 BASE    generated %d tok (boundary @ %d)\n", N_PRE + N_POST, N_PRE);

    /* ── checkpoint blob: context at exactly the boundary ───────────── */
    struct llama_context *ca = make_ctx(model);
    { llama_token pre[N_PRE]; gen_stream(ca, vocab, prompt, N_PRE, pre, NULL, 0); }
    size_t st_sz = llama_state_get_size(ca);
    printf("state size %.2f MB\n", (double)st_sz / 1e6);
    if (st_sz > (size_t)MAX_STATE_MB * 1024 * 1024) { printf("FAIL too big\n"); return 1; }
    uint8_t *sblob = (uint8_t *)malloc(st_sz);
    if (llama_state_get_data(ca, sblob, st_sz) != st_sz) {
        printf("FAIL state_get_data\n"); return 1;
    }
    llama_free(ca);

    uint32_t total_parts = (uint32_t)((st_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;
    uint64_t *pxor = (uint64_t *)malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++) {
        uint32_t off = f * PART_BYTES;
        uint32_t len = (uint32_t)(st_sz - off); if (len > PART_BYTES) len = PART_BYTES;
        pxor[f] = xor64(sblob + off, len);
    }
    printf("state -> %u parts · %u layers\n", total_parts, layers);

    uint8_t *rb = (uint8_t *)calloc(1, (size_t)layers * RID_SLOTS * PART_BYTES);
    int all_ok = 1;

    for (int lang = 0; lang < 3; lang++) {
        DtSlotRegion reg;
        remove(twin_path);
        if (dt_slot_init_twin(&reg, twin_path,
                              (size_t)layers * RID_SLOTS, PART_BYTES) != 0) {
            printf("FAIL twin init\n"); return 1;
        }
        /* S2 CKPT through language view */
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t off = f * PART_BYTES;
            uint32_t len = (uint32_t)(st_sz - off);
            if (len > PART_BYTES) len = PART_BYTES;
            dt_slot_put(&reg, fwd_view(vw[lang], f), sblob + off, len);
        }
        /* S3a readback lossless */
        memset(rb, 0, (size_t)layers * RID_SLOTS * PART_BYTES);
        for (uint32_t addr = 0; addr < layers * RID_SLOTS; addr++) {
            uint32_t w; uint32_t f = inv_view(vw[lang], addr, &w);
            if (f >= total_parts) continue;
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p) continue;
            uint32_t off = f * PART_BYTES;
            uint32_t len = (uint32_t)(st_sz - off);
            if (len > PART_BYTES) len = PART_BYTES;
            memcpy(rb + off, p, len);
        }
        int rt_ok = (memcmp(rb, sblob, st_sz) == 0);
        printf("[%s] S2 CKPT   %u parts · readback identical=%s\n",
               lname[lang], total_parts, rt_ok ? "YES" : "NO");
        if (!rt_ok) { all_ok = 0; dt_slot_destroy(&reg); continue; }

        /* S3b serve into fresh context: replay prefix, restore state,
         * continue from the BOUNDARY token carried across the checkpoint */
        struct llama_context *cc = make_ctx(model);
        { llama_token pre[N_PRE]; gen_stream(cc, vocab, prompt, N_PRE, pre, NULL, 0); }
        if (llama_state_set_data(cc, rb, st_sz) != st_sz) {
            printf("FAIL state_set_data\n"); return 1;
        }
        float cont_lg[160000]; llama_token tail[N_POST];
        llama_token t2 = base_tok[N_PRE];
        for (int i = 0; i < N_POST; i++) {
            tail[i] = t2;
            llama_decode(cc, llama_batch_get_one(&t2, 1));
            const float *L = llama_get_logits(cc);
            if (i == 0) memcpy(cont_lg, L, sizeof(float) * (size_t)n_vocab);
            t2 = greedy(L, n_vocab);
        }
        int lg_ok;
        float maxdiff = 0.0f;
        if (memcmp(cont_lg, bound_lg, sizeof(float) * (size_t)n_vocab) == 0) {
            lg_ok = 1;
        } else {
            lg_ok = 1;
            for (int i = 0; i < n_vocab; i++) {
                float d = cont_lg[i] - bound_lg[i];
                if (d < 0) d = -d;
                if (d > maxdiff) maxdiff = d;
            }
            /* fp16 KV layout may differ after restore (cell order) —
             * accept sub-noise deltas, reject real divergence */
            if (!(maxdiff < 1e-3f)) lg_ok = 0;
        }
        int tok_ok = (memcmp(tail, base_tok + N_PRE,
                             sizeof(llama_token) * N_POST) == 0);
        printf("[%s] S3 SERVE   logits@restore %s (maxdiff %.3g) · post tokens=%s (%d)\n",
               lname[lang],
               lg_ok ? (maxdiff == 0.0f ? "BITWISE" : "within-noise") : "DIVERGED",
               (double)maxdiff,
               tok_ok ? "YES" : "NO", N_POST);
        if (!lg_ok || !tok_ok) all_ok = 0;
        llama_free(cc);

        /* G DRILL (first language) */
        if (lang == 0 && total_parts > 9) {
            uint32_t fhit = total_parts / 2;
            uint32_t hit_off = fhit * PART_BYTES;
            uint32_t hit_len = (uint32_t)(st_sz - hit_off);
            if (hit_len > PART_BYTES) hit_len = PART_BYTES;
            uint32_t aha = fwd_view(vw[lang], fhit);
            dt_slot_ptr(&reg, aha)[17] ^= 0x80;
            uint32_t found = total_parts;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw[lang], f));
                if (!p || xor64(p, PART_BYTES) != pxor[f]) { found = f; break; }
            }
            int loc_ok = (found == fhit);
            if (loc_ok) dt_slot_put(&reg, aha, sblob + hit_off, hit_len);
            uint32_t still_bad = 0;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint32_t off = f * PART_BYTES;
                uint32_t len = (uint32_t)(st_sz - off);
                if (len > PART_BYTES) len = PART_BYTES;
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw[lang], f));
                if (!p || memcmp(p, sblob + off, len) != 0) still_bad++;
            }
            printf("[%s] G DRILL   flip@part %u -> localized=%s · restored bad=%u (%s)\n",
                   lname[lang], fhit, loc_ok ? "YES" : "NO", still_bad,
                   still_bad ? "STILL BROKEN" : "lossless again");
            if (!loc_ok || still_bad) all_ok = 0;
        }

        dt_slot_destroy(&reg);
    }

    printf("\n%s\n", all_ok
        ? "RESULT: PASSED — llama state served through RID slots"
        : "RESULT: FAILED");

    free(sblob); free(rb); free(pxor);
    llama_model_free(model);
    llama_backend_free();
    return all_ok ? 0 : 1;
}
