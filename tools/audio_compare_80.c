#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t size; char wave[4]; } WavHeader;
typedef struct { char fmt[4]; uint32_t size; uint16_t af; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; } FmtChunk;
typedef struct { char data[4]; uint32_t size; } DataHeader;
#pragma pack(pop)

void fft(double *r, double *im, int n) {
    for (int i=1,j=0;i<n;i++) { int b=n>>1; for(;j&b;b>>=1) j^=b; j^=b; if(i<j){double t=r[i];r[i]=r[j];r[j]=t;t=im[i];im[i]=im[j];im[j]=t;} }
    for (int len=2;len<=n;len<<=1) { double a=-2*M_PI/len,wr=cos(a),wi=sin(a); for(int i=0;i<n;i+=len){double cr=1,ci=0; for(int j=0;j<len/2;j++){double ur=r[i+j],ui=im[i+j],vr=r[i+j+len/2]*cr-im[i+j+len/2]*ci,vi=r[i+j+len/2]*ci+im[i+j+len/2]*cr; r[i+j]=ur+vr;im[i+j]=ui+vi;r[i+j+len/2]=ur-vr;im[i+j+len/2]=ui-vi;double t=cr*wr-ci*wi;ci=cr*wi+ci*wr;cr=t;}} }
}

int main(int argc, char *argv[]) {
    if(argc<2)return 1;
    FILE *fp=fopen(argv[1],"rb"); if(!fp)return 1;
    WavHeader h;fread(&h,sizeof(h),1,fp);
    FmtChunk f;while(1){fread(&f,sizeof(f),1,fp);if(memcmp(f.fmt,"fmt ",4)==0)break;fseek(fp,f.size,SEEK_CUR);}
    DataHeader d;while(1){fread(&d,sizeof(d),1,fp);if(memcmp(d.data,"data",4)==0)break;fseek(fp,d.size,SEEK_CUR);}
    int total=d.size/(f.bps/8), n=f.sr;
    fseek(fp,(total/2)*(f.bps/8),SEEK_CUR);
    int16_t *s=malloc(n*2);fread(s,2,n,fp);fclose(fp);

    int ab[80]={0};
    for(int i=0;i<n;i++){double a=(s[i]+32768.0)*80.0/65536.0;ab[(int)a%80]++;}

    int fsz=1024;double*re=calloc(fsz,sizeof(double));double*im=calloc(fsz,sizeof(double));
    for(int i=0;i<fsz&&i<n;i++)re[i]=s[i]/32768.0;
    fft(re,im,fsz);
    double fm[80];for(int i=0;i<80;i++)fm[i]=sqrt(re[i]*re[i]+im[i]*im[i]);

    double at[80];int st=n/80;for(int i=0;i<80;i++)at[i]=(s[i*st]+32768.0)*360.0/65536.0;

    printf("{\"angular_hist\":[");for(int i=0;i<80;i++){if(i)printf(",");printf("%d",ab[i]);}
    printf("],\"fft_mag\":[");for(int i=0;i<80;i++){if(i)printf(",");printf("%.1f",fm[i]);}
    printf("],\"angular_time\":[");for(int i=0;i<80;i++){if(i)printf(",");printf("%.1f",at[i]);}
    printf("]}\n");
    free(re);free(im);free(s);
}
