// Whisper Mel Dump — uses pre-computed filters + Kokoro ground truth
// "The quick brown fox jumps over the lazy dog, TTS is working perfectly."

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t sz; char wave[4]; } WH;
typedef struct { char fmt[4]; uint32_t sz; uint16_t af,ch; uint32_t sr,br; uint16_t ba,bps; } FC;
typedef struct { char data[4]; uint32_t sz; } DH;
#pragma pack(pop)

static void fft(double *r, double *im, int n) {
    for(int i=1,j=0;i<n;i++){int b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;
        if(i<j){double t=r[i];r[i]=r[j];r[j]=t;t=im[i];im[i]=im[j];im[j]=t;}}
    for(int L=2;L<=n;L<<=1){
        double a=-2*M_PI/L,wr=cos(a),wi=sin(a);
        for(int i=0;i<n;i+=L){double cr=1,ci=0;
            for(int j=0;j<L/2;j++){double ur=r[i+j],ui=im[i+j],
                vr=r[i+j+L/2]*cr-im[i+j+L/2]*ci,
                vi=r[i+j+L/2]*ci+im[i+j+L/2]*cr;
                r[i+j]=ur+vr;im[i+j]=ui+vi;
                r[i+j+L/2]=ur-vr;im[i+j+L/2]=ui-vi;
                double t=cr*wr-ci*wi;ci=cr*wi+ci*wr;cr=t;}}}
}

int main(int argc, char **argv) {
    const char *wav = (argc > 1) ? argv[1] : "I:/model/kokoro_test2.wav";
    const char *flt = (argc > 2) ? argv[2] : "I:/DWGLS/tools/mel_filters.bin";
    
    // Load pre-computed filters
    FILE *ff = fopen(flt, "rb");
    if (!ff) { fprintf(stderr, "Cannot open filters: %s\n", flt); return 1; }
    int n_mel, n_fft_f;
    fread(&n_mel, 4, 1, ff);
    fread(&n_fft_f, 4, 1, ff);
    float *filters = malloc(n_mel * n_fft_f * sizeof(float));
    fread(filters, sizeof(float), n_mel * n_fft_f, ff);
    fclose(ff);
    fprintf(stderr, "Filters: %d x %d\n", n_mel, n_fft_f);
    
    // Read audio
    FILE *af = fopen(wav, "rb");
    if (!af) { fprintf(stderr, "Cannot open: %s\n", wav); return 1; }
    WH wh; fread(&wh, sizeof(wh), 1, af);
    FC fc; while(1){fread(&fc,sizeof(fc),1,af);if(memcmp(fc.fmt,"fmt ",4)==0)break;fseek(af,fc.sz,SEEK_CUR);}
    DH dd; while(1){fread(&dd,sizeof(dd),1,af);if(memcmp(dd.data,"data",4)==0)break;fseek(af,dd.sz,SEEK_CUR);}
    int total = dd.sz / (fc.bps / 8);
    int16_t *raw = malloc(total * sizeof(int16_t));
    fread(raw, sizeof(int16_t), total, af);
    fclose(af);
    
    // Resample to 16kHz
    int sr = 16000;
    int n = total * sr / fc.sr;
    float *pcm = malloc(n * sizeof(float));
    for (int i = 0; i < n; i++) pcm[i] = raw[(long)i * fc.sr / sr] / 32768.0f;
    free(raw);
    fprintf(stderr, "Audio: %d samples (%.2fs)\n", n, (double)n/sr);
    
    // Compute mel spectrogram
    int frame_sz = 400, hop = 160;
    int n_frames = (n - frame_sz) / hop + 1;
    float *mel = calloc(n_frames * n_mel, sizeof(float));
    float hann[400];
    for (int i = 0; i < 400; i++) hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / 400));
    double *re = calloc(400, sizeof(double));
    double *im = calloc(400, sizeof(double));
    
    for (int f = 0; f < n_frames; f++) {
        int off = f * hop;
        for (int j = 0; j < frame_sz; j++) { re[j] = hann[j] * pcm[off+j]; im[j] = 0; }
        fft(re, im, frame_sz);
        float mag[201];
        for (int j = 0; j < n_fft_f; j++) mag[j] = sqrtf(re[j]*re[j] + im[j]*im[j]);
        for (int m = 0; m < n_mel; m++) {
            float s = 0;
            for (int j = 0; j < n_fft_f; j++) s += filters[m*n_fft_f+j] * mag[j];
            mel[f*n_mel+m] = logf(fmaxf(s, 1e-10f));
        }
    }
    
    // Normalize (whisper-style)
    float mx = -1e20f;
    for (int i = 0; i < n_frames*n_mel; i++) if(mel[i]>mx) mx=mel[i];
    float mx8 = mx - 8.0f;
    for (int i = 0; i < n_frames*n_mel; i++) {
        if(mel[i]<mx8) mel[i]=mx8;
        mel[i] = (mel[i]+4.0f)/4.0f;
    }
    fprintf(stderr, "Mel: %d frames x %d bins\n", n_frames, n_mel);
    
    // === OUTPUT ===
    printf("=== WHAT WHISPER TRANSFORMER SEES ===\n");
    printf("Shape: [%d, %d] float32\n", n_frames, n_mel);
    printf("Ground truth: The quick brown fox jumps over the lazy dog, TTS is working perfectly.\n\n");
    
    // Heatmap
    int show = n_frames < 330 ? n_frames : 330;
    printf("MEL HEATMAP (%d frames x %d bins)\n", show, n_mel);
    printf(" .=<0  :=0-0.25  -=0.25-0.5  +=0.5-0.75  *=0.75+\n\n");
    for (int m = n_mel-1; m >= 0; m -= 2) {
        printf("%2d|", m);
        for (int t = 0; t < show; t++) {
            float v = mel[t*n_mel+m];
            printf("%c", v<0?'.' : v<0.25f?':' : v<0.5f?'-' : v<0.75f?'+':'*');
        }
        printf("\n");
    }
    printf("  +"); for(int t=0;t<show;t++) printf("-"); printf("\n");
    printf("   0s       1s       2s       3s       4s       5s\n\n");
    
    // Token alignment
    typedef struct { const char *w; float t0,t1,s; } Tok;
    Tok T[] = {
        {"The",0.00f,0.36f,0.957f}, {"quick",0.36f,0.68f,0.943f}, {"brown",0.68f,0.98f,0.947f},
        {"fox",0.98f,1.30f,0.947f}, {"jumps",1.30f,1.68f,0.993f}, {"over",1.68f,1.98f,0.995f},
        {"the",1.98f,2.18f,0.995f}, {"lazy",2.18f,2.56f,0.968f}, {"dog",2.56f,2.96f,0.981f},
        {",",2.96f,3.06f,0.255f}, {"TTS",3.06f,3.46f,0.755f}, {"is",3.46f,3.66f,0.964f},
        {"working",3.66f,4.06f,0.997f}, {"perfectly",4.06f,4.80f,0.995f}, {".",4.80f,5.02f,0.925f},
    };
    int nt = sizeof(T)/sizeof(T[0]);
    
    printf("=== TOKEN -> MEL ALIGNMENT ===\n");
    printf("%-12s %6s-%6s  %5s  Frame    AvgMel  Peak\n", "Token","From","To","Score");
    printf("%-12s %6s-%6s  %5s  -----    ------  ----\n", "-----","----","--","-----");
    
    for (int i = 0; i < nt; i++) {
        int f0 = (int)(T[i].t0*sr/hop);
        int f1 = (int)(T[i].t1*sr/hop);
        if(f0<0) f0=0;
        if(f1>n_frames) f1=n_frames;
        float en=0, pk=-1e20f; int pkb=0, cnt=0;
        for (int t=f0; t<f1; t++) {
            for (int m=0; m<n_mel; m++) {
                float v = mel[t*n_mel+m];
                en += v;
                if(v>pk){pk=v;pkb=m;}
                cnt++;
            }
        }
        if(cnt>0) en/=cnt;
        printf("%-12s %5.2fs-%5.2fs  %.3f  %3d-%3d  %6.3f  bin%2d\n",
               T[i].w, T[i].t0, T[i].t1, T[i].s, f0, f1, en, pkb);
    }
    
    printf("\n=== KEY INSIGHTS ===\n");
    printf("Total: %d mel values (%.1f KB)\n", n_frames*n_mel, n_frames*n_mel*4.0f/1024);
    printf("Per token: ~%d mel values\n", n_frames*n_mel/nt);
    printf("\nEach word has a UNIQUE mel signature (pattern of 80 freq bins over time).\n");
    printf("Whisper encoder learns these signatures -> decoder maps to tokens.\n");
    printf("Geometric audio codec = same thing, but map to 20736 address space.\n");
    
    free(filters); free(pcm); free(mel); free(re); free(im);
    return 0;
}
