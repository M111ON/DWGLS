/* tools/mel_delta_probe.c — พิสูจน์: mel spectrogram (representation ที่
 * audio branch ใช้) มีโครงสร้างให้ delta/residual บีบ lossless ได้จริง
 * ═══════════════════════════════════════════════════════════════════════════
 * ข้อมูล: whisper_mel_real.json (real whisper mel — 80 bins/frame, normalized)
 *
 * วัด:
 *   H(raw)      — entropy ของ mel value ตรงๆ
 *   H(dt)       — delta ตามเวลา  v[f][m] − v[f−1][m]  (frames ห่างกัน 10ms)
 *   H(df)       — delta ตามความถี่ v[f][m] − v[f][m−1]
 *   ratio       = H(raw)/H(delta) — mel มี correlation จริง (ต่างจาก Q8)
 *
 * BUILD: gcc -O2 -Wall -I. -o build/mel_delta_probe tools/mel_delta_probe.c -lm
 * RUN:   ./build/mel_delta_probe <mel.json> [max_frames]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BINS 1024

static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

/* bin ตัวเลขจริงลง BINS buckets ตามช่วง [lo,hi] */
static inline int bin_of(double v, double lo, double hi) {
    if (v <= lo) return 0;
    if (v >= hi) return BINS - 1;
    return (int)((v - lo) / (hi - lo) * (BINS - 1));
}

/* parse {"frames": [[...],[...]], ...} → flat buffer frames×mels
   รับเฉพาะตัวเลขล้วน — ไม่มี string ใน frames array */
static int parse_frames(const char *path, double **out, int max_frames,
                        int *n_frames_out, int *mels_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -2; }
    fclose(fp);
    buf[sz] = 0;

    /* หา "frames" key → '[' เปิด array ใหญ่ */
    char *p = strstr(buf, "\"frames\"");
    if (!p) { free(buf); return -3; }
    p = strchr(p, '[');
    if (!p) { free(buf); return -4; }
    p++;   /* ข้าม '[' ตัวนอก */

    double *frames = (double *)malloc((size_t)max_frames * 256 * sizeof(double));
    int n_frames = 0, mels = 0, first_mels = 0, cap = max_frames * 256;
    int in_frame = 0, seen_any = 0;
    char *q = p;

    while (n_frames < max_frames) {
        char c = *q;
        if (!c) break;
        if (c == '[') { in_frame = 1; seen_any = 1; q++; continue; }
        if (c == ']') {
            if (in_frame) {          /* ปิด frame → frame ถัดไป */
                if (n_frames == 0) first_mels = mels;
                n_frames++;
                in_frame = 0;
                mels = 0;
            } else {
                break;               /* ปิด array ใหญ่ */
            }
            q++;
            continue;
        }
        if (c == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t') {
            q++;
            continue;
        }
        if (!in_frame) { q++; continue; }   /* stray token นอก frame */
        char *end = NULL;
        double v = strtod(q, &end);
        if (end == q) { q++; continue; }
        if (mels < 256) frames[n_frames * 256 + mels] = v;
        mels++;
        q = end;
    }
    if (n_frames == 0 && !seen_any) { free(frames); free(buf); return -5; }

    *out = frames;
    *n_frames_out = n_frames;
    *mels_out = first_mels;
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = (argc > 1) ? argv[1] : "build/mel_real.json";
    int max_frames = (argc > 2) ? atoi(argv[2]) : 4000;

    printf("Mel delta probe — mel spectrogram มี correlation จริงไหม\n");
    printf("═════════════════════════════════════════════════════════\n");
    double *frames = NULL;
    int nf = 0, mels = 0;
    int rc = parse_frames(path, &frames, max_frames, &nf, &mels);
    if (rc != 0) { printf("(parse fail rc=%d — ใช้ path ที่เข้าถึงได้ เช่น build/mel_real.json)\n", rc); return 1; }
    printf("  frames: %d × %d mels (sample cap %d)\n", nf, mels, max_frames);

    /* ช่วงค่าจากข้อมูล */
    double lo = frames[0], hi = frames[0];
    for (int i = 0; i < nf * mels; i++) {
        if (frames[i] < lo) lo = frames[i];
        if (frames[i] > hi) hi = frames[i];
    }
    double dmax = (hi - lo) * 0.5 + 1e-6;   /* delta range คร่าวๆ */

    uint64_t h_raw[BINS] = {0}, h_dt[BINS] = {0}, h_df[BINS] = {0}, h_dt2[BINS] = {0};
    for (int f = 0; f < nf; f++) {
        for (int m = 0; m < mels; m++) {
            double v = frames[f * mels + m];
            h_raw[bin_of(v, lo, hi)]++;
            if (f > 0) {
                double dt = v - frames[(f - 1) * mels + m];
                h_dt[bin_of(dt, -dmax, dmax)]++;
                if (f > 1) {
                    double dt2 = v - 2.0 * frames[(f - 1) * mels + m]
                                     + frames[(f - 2) * mels + m];
                    h_dt2[bin_of(dt2, -dmax, dmax)]++;
                }
            }
            if (m > 0) {
                double df = v - frames[f * mels + m - 1];
                h_df[bin_of(df, -dmax, dmax)]++;
            }
        }
    }
    uint64_t n = (uint64_t)nf * mels;
    double hr  = entropy_of(h_raw, BINS, n);
    double hdt = entropy_of(h_dt, BINS, n - (uint64_t)mels);
    double hdf = entropy_of(h_df, BINS, n - (uint64_t)nf);
    double hd2 = entropy_of(h_dt2, BINS, n - 2 * (uint64_t)mels);

    printf("  val range: %.3f .. %.3f (normalized log-mel)\n", lo, hi);
    printf("  H(raw)=%.3f b | H(dt)=%.3f b | H(df)=%.3f b | H(dt²)=%.3f b\n",
           hr, hdt, hdf, hd2);
    printf("  ratio: time-delta %.2f× | freq-delta %.2f× | 2nd-time %.2f×  (H(raw)/H(delta))\n",
           hr / (hdt < 0.01 ? 0.01 : hdt),
           hr / (hdf < 0.01 ? 0.01 : hdf),
           hr / (hd2 < 0.01 ? 0.01 : hd2));

    free(frames);
    printf("═════════════════════════════════════════════════════════\n");
    return 0;
}
