/* tools/p3_view_roundtrip.c - bake/unfold per view for cloud P-gates
 * ====================================================================
 * subcommands:
 *   p3 bake   <model> <twin> <viewidx>          file -> slot region
 *   p3 unfold <twin> <out> <viewidx> <nparts>   region -> file
 * views: pent/tri/snubL/snubR/hosoya/zeck
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "../core/infra/dramtile_store.h"

typedef struct{int64_t x,y,z;}V3;
static V3 V(int64_t x,int64_t y,int64_t z){V3 v={x,y,z};return v;}
static V3 vsub(V3 a,V3 b){return V(a.x-b.x,a.y-b.y,a.z-b.z);}
static int64_t dist2(V3 a,V3 b){V3 d=vsub(a,b);
    return d.x*d.x+d.y*d.y+d.z*d.z;}
static V3 verts[20]; static uint32_t n_verts=0;
static uint32_t faces[16][5]; static uint32_t n_faces=0;
static uint32_t edges[40][2]; static uint32_t n_edges=0;
static void build_dodeca(void){
    const int64_t S=104,H=64,P=169;const int sg[2]={1,-1};
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
static uint32_t rf_v[64],rf_f[64],nrf=0;
static uint32_t sqc[40][4];
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x,uint8_t*par){
    if(uf_p[x]<0){*par=0;return x;}
    uint8_t pp;int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp;uf_p[x]=r;*par=uf_par[x];return r;
}
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj;int ri=uf_find(i,&pi),rj=uf_find(j,&pj);
    if(ri==rj)return (pi^pj)==d;
    uf_p[ri]=rj;uf_par[ri]=(uint8_t)(pi^d^pj);return 1;
}
static int build_rid(uint8_t vw[4][60]){
    build_dodeca();
    int64_t emin=0;
    for(uint32_t i=0;i<n_verts;i++)for(uint32_t j=i+1;j<n_verts;j++){
        int64_t d2=dist2(verts[i],verts[j]);
        if(d2&&(!emin||d2<emin))emin=d2;}
    int64_t elo=emin,ehi=emin+emin/16;
#define ADJ(i,j) ({int64_t d2=dist2(verts[i],verts[j]);d2&&d2>=elo&&d2<=ehi;})
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
                    uint32_t mn=a;if(b<mn)mn=b;if(c<mn)mn=c;
                    if(dd<mn)mn=dd;if(e<mn)mn=e;
                    if(a!=mn||b>e)continue;
                    uint32_t cl[5]={a,b,c,dd,e};
                    V3 u=vsub(verts[cl[1]],verts[cl[0]]);
                    V3 w=vsub(verts[cl[2]],verts[cl[0]]);
                    int planar=1;
                    for(int k=3;k<5&&planar;k++){
                        V3 r2=vsub(verts[cl[k]],verts[cl[0]]);
                        int64_t dx=u.y*w.z-u.z*w.y,dy=u.z*w.x-u.x*w.z,
                                dz=u.x*w.y-u.y*w.x;
                        if(labs((long)(r2.x*dx+r2.y*dy+r2.z*dz))>100000L)planar=0;}
                    if(!planar)continue;
                    int dupf=0;
                    for(uint32_t fc=0;fc<n_faces&&!dupf;fc++){
                        int same=1;
                        for(int p=0;p<5&&same;p++){
                            int hit=0;
                            for(int q=0;q<5;q++)
                                if(faces[fc][q]==cl[p]){hit=1;break;}
                            if(!hit)same=0;}
                        if(same)dupf=1;}
                    if(dupf||n_faces>=16)continue;
                    memcpy(faces[n_faces],cl,sizeof(cl));n_faces++;
                }
                if(n_faces>=16)break;}
            if(n_faces>=16)break;}
        if(n_faces>=16)break;}
    memset(rid_of,0xFF,sizeof(rid_of));
    for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++){
            uint32_t u=faces[f][p];
            if(rid_of[u*16+f]<0){rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u;rf_f[nrf]=f;nrf++;}}
    static uint8_t radj[64][64];memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i],f=rf_f[i];int pos=-1;
        for(int p=0;p<5;p++)if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5],nx=faces[f][(pos+1)%5];
        radj[i][rid_of[pv*16+f]]=radj[rid_of[pv*16+f]][i]=1;
        radj[i][rid_of[nx*16+f]]=radj[rid_of[nx*16+f]][i]=1;
        for(uint32_t g=0;g<n_faces;g++){
            if(g==f)continue;
            for(int p2=0;p2<5;p2++)if(faces[g][p2]==u){
                radj[i][rid_of[u*16+g]]=radj[rid_of[u*16+g]][i]=1;break;}}}
    uint32_t deg_ok=1,e2=0;
    for(uint32_t i=0;i<nrf;i++){uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++)d+=radj[i][j];
        if(d!=4)deg_ok=0;}
    for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++)e2+=radj[i][j];
    if(nrf!=60||e2!=120||!deg_ok)return -1;
    {static int32_t vsq[64][2];memset(vsq,0xFF,sizeof(vsq));
     for(uint32_t k=0;k<n_edges;k++){
         uint32_t a=edges[k][0],b=edges[k][1],ff[2],nf=0;
         for(uint32_t g=0;g<n_faces&&nf<2;g++)
             if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0)ff[nf++]=g;
         sqc[k][0]=(uint32_t)rid_of[a*16+ff[0]];
         sqc[k][1]=(uint32_t)rid_of[b*16+ff[0]];
         sqc[k][2]=(uint32_t)rid_of[b*16+ff[1]];
         sqc[k][3]=(uint32_t)rid_of[a*16+ff[1]];
         for(int c=0;c<4;c++){
             uint32_t w=sqc[k][c];
             if(vsq[w][0]<0)vsq[w][0]=(int32_t)k;
             else if(vsq[w][1]<0&&vsq[w][0]!=(int32_t)k)vsq[w][1]=(int32_t)k;}}
     memset(uf_p,0xFF,sizeof(uf_p));memset(uf_par,0,sizeof(uf_par));
     for(uint32_t w=0;w<nrf;w++){
         int32_t i=vsq[w][0],j=vsq[w][1];
         uint8_t c0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
         uint8_t c1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
         if(!uf_merge((int)i,(int)j,(uint8_t)(c0^c1^1)))return -1;}
     uint8_t bits[40];memset(bits,0,sizeof(bits));
     for(uint32_t k=0;k<n_edges;k++){uint8_t p;uf_find((int)k,&p);bits[k]=p;}
     {uint32_t n=0;
      for(uint32_t f=0;f<n_faces;f++)
          for(int p=0;p<5;p++)vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f];}
     {uint32_t n=0;
      for(uint32_t u=0;u<n_verts;u++)
          for(uint32_t g=0;g<n_faces;g++)
              if(FACE_POS(g,u)>=0)vw[1][n++]=(uint8_t)rid_of[u*16+g];}
     {uint32_t n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){a=sqc[k][0];b=sqc[k][2];}
          else{a=sqc[k][1];b=sqc[k][3];}
          vw[2][n++]=(uint8_t)a;vw[2][n++]=(uint8_t)b;}
      n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){a=sqc[k][1];b=sqc[k][3];}
          else{a=sqc[k][0];b=sqc[k][2];}
          vw[3][n++]=(uint8_t)a;vw[3][n++]=(uint8_t)b;}
      (void)n;}
    for(int v=0;v<4;v++){
        uint8_t hit[64];memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){if(hit[vw[v][p]])return -1;hit[vw[v][p]]=1;}}
    return 0;
}
}
static inline uint32_t fwd_view(const uint8_t vw[60],uint32_t f){
    uint32_t w=f%60,l=f/60,pos=60;
    for(uint32_t p=0;p<60;p++)if(vw[p]==w){pos=p;break;}
    return l*60+pos;
}
#define PART_BYTES (128u*1024u)
int main(int argc,char**argv){
    fprintf(stderr,"dbg-main argc=%d\n",argc);
    if(argc<4){printf("usage: %s bake <model> <twin> <view>\n"
                      "       %s unfold <twin> <out> <view> <nparts>\n",
                      argv[0],argv[0]);return 2;}
    static uint8_t vw[4][60];
    if(build_rid(vw)!=0){printf("FAIL rid\n");return 1;}
    uint8_t vws[6][60];
    for(int i=0;i<4;i++)memcpy(vws[i],vw[i],60);
    for(uint32_t p=0;p<60;p++)vws[4][p]=(uint8_t)((p*13u)%60u);
    {static const uint64_t ZF[12]={0,1,1,2,3,5,8,13,21,34,55,89};
     uint32_t key[60],ord[60];
     for(int n=0;n<60;n++){
         uint64_t rem=(uint64_t)(n+1),mask=0;
         for(int i=9;i>=0;i--)
             if(ZF[i+2]<=rem){mask|=1ull<<i;rem-=ZF[i+2];}
         uint64_t rev=0;
         for(int b=0;b<10;b++)if(mask&(1ull<<b))rev|=1ull<<(9-b);
         key[n]=(uint32_t)rev;ord[n]=(uint32_t)n;}
     for(int i=1;i<60;i++){uint32_t v=ord[i];int j=i-1;
         while(j>=0&&key[ord[j]]>key[v]){ord[j+1]=ord[j];j--;}
         ord[j+1]=v;}
     for(int i=0;i<60;i++)vws[5][i]=(uint8_t)ord[i];}

    if(strcmp(argv[1],"bake")==0){
        const char*model=argv[2];const char*twin=argv[3];
        int v=atoi(argv[4]);
        FILE*fp=fopen(model,"rb");
        if(!fp){printf("FAIL open model\n");return 1;}
        fseek(fp,0,SEEK_END);long fsz=ftell(fp);fseek(fp,0,SEEK_SET);
        uint32_t total=(uint32_t)(((size_t)fsz+PART_BYTES-1)/PART_BYTES);
        uint32_t nslots=((total+59)/60)*60;
        remove(twin);
        DtSlotRegion reg;
        if(dt_slot_init_twin(&reg,twin,nslots,PART_BYTES)!=0){
            printf("FAIL twin\n");return 1;}
        uint8_t*buf=malloc(PART_BYTES);
        for(uint32_t f=0;f<total;f++){
            uint32_t len=PART_BYTES;
            if((size_t)f*len+len>(size_t)fsz)
                len=(uint32_t)((size_t)fsz-(size_t)f*PART_BYTES);
            fseek(fp,(long)f*PART_BYTES,SEEK_SET);
            if(fread(buf,1,len,fp)!=(size_t)len){printf("FAIL read\n");return 1;}
            dt_slot_put(&reg,fwd_view(vws[v],f),buf,len);
        }
        free(buf);fclose(fp);dt_slot_destroy(&reg);
        printf("baked %u parts via view %d -> %s\n",total,v,twin);
        return 0;
    }
    if(strcmp(argv[1],"unfold")==0){
        const char*twin=argv[2];const char*out=argv[3];
        int v=atoi(argv[4]);
        uint32_t total=(uint32_t)atoi(argv[5]);
        long fsz=(argc>6)?atol(argv[6]):-1L;
        uint32_t nslots=((total+59)/60)*60;   /* round up to layer */
        fprintf(stderr,"dbg0 total=%u fsz=%ld nslots=%u v=%d\n",total,fsz,nslots,v);
        DtSlotRegion reg;
        if(dt_slot_init_twin(&reg,twin,nslots,PART_BYTES)!=0){
            printf("FAIL reopen twin\n");return 1;}
        fprintf(stderr,"dbg1 reopened\n");
        uint8_t inv[60];
        /* position p holds part l*60+vw[p]; emit to offset f*PART */
        memcpy(inv,vws[v],60);
        FILE*fo=fopen(out,"wb");
        if(!fo){printf("FAIL out\n");return 1;}
        uint8_t*buf=malloc(PART_BYTES);
        /* every slot emits its part at the RIGHT offset with TRUE
         * per-part length; no sequential-stop assumptions */
        for(uint32_t l=0;l<nslots/60;l++)
            for(uint32_t p=0;p<60;p++){
                uint32_t addr=l*60+p,f=l*60+vws[v][p];
                if(f>=total) continue;              /* padding slot */
                long off=(long)((uint64_t)f*PART_BYTES);
                uint32_t len=PART_BYTES;
                if(fsz>=0 && off+len>fsz) len=(uint32_t)(fsz-off);
                if(len==0) continue;
                uint8_t*p2=dt_slot_ptr(&reg,addr);
                if(!p2){printf("FAIL ptr %u\n",addr);return 1;}
                memcpy(buf,p2,len);
                if(fseek(fo,off,SEEK_SET)!=0){printf("FAIL seek\n");return 1;}
                fwrite(buf,1,len,fo);
            }
        free(buf);fclose(fo);dt_slot_destroy(&reg);
        printf("unfolded via view %d -> %s (%ld bytes)\n",v,out,
               fsz>=0?fsz:(long)nslots*PART_BYTES);
        return 0;
    }
    printf("unknown subcommand\n");return 2;
}
