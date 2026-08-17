/* tools/wave_delta_probe.c — พิสูจน์แนวคิด waveform (แค่ตัวอย่าง ไม่เอา 71 ไฟล์)
 * ═══════════════════════════════════════════════════════════════════════════
 * คำถาม: raw PCM waveform มีโครงสร้างให้ delta/residual บีบได้จริงไหม
 * (ต่างจาก Q8 weights ที่ pseudorandom ถึง bound 96%)
 *
 * วัดบน sample จริง (cap 2M samples/ไฟล์ — พอให้ entropy ลู่):
 *   H(raw)    — entropy ของ sample 16-bit ตรงๆ
 *   H(delta)  — sample[i] − sample[i−1]   (correlation ตัวแรก)
 *   H(delta2) — delta[i] − delta[i−1]     (2nd-order)
 *   ratio     = 16 bits / H — ตัวเลขที่บอกว่า waveform codec ได้กี่ ×
 *
 * BUILD: gcc -O2 -Wall -I. -o build/wave_delta_probe tools/wave_delta_probe.c -lm
 * RUN:   ./build/wave_delta_probe [wav ...]   (default = 3 ไฟล์ใน notebookLM)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SAMPLE_CAP  2000000u    /* พอให้ entropy ลู่ (4 MB PCM/ไฟล์) */

static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

/* อ่าน PCM 16-bit จาก RIFF (หา "data" chunk, เก็บช่อง 0 เท่านั้น) */
static long read_wav_pcm(const char *path, int16_t **out, uint64_t *n_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(fp); return -2; }

    uint32_t channels = 1;
    int found_fmt = 0, found_data = 0;
    long data_off = 0;
    uint32_t data_sz = 0;

    while (1) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) break;
        uint32_t sz;
        memcpy(&sz, ch + 4, 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (fread(fb, 1, sz > 16 ? 16 : sz, fp) != (sz > 16 ? 16 : sz)) break;
            uint16_t audio_fmt, chans, bits;
            memcpy(&audio_fmt, fb, 2);
            memcpy(&chans, fb + 2, 2);
            memcpy(&bits, fb + 14, 2);
            if (audio_fmt != 1 || bits != 16) { fclose(fp); return -3; }   /* PCM 16-bit only */
            channels = chans;
            found_fmt = 1;
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_off = ftell(fp);
            data_sz = sz;
            found_data = 1;
            break;
        } else {
            fseek(fp, sz, SEEK_CUR);
        }
    }
    if (!found_fmt || !found_data) { fclose(fp); return -4; }

    uint64_t want = (uint64_t)SAMPLE_CAP * channels;
    uint32_t avail = data_sz / 2;
    if (want > avail) want = avail;
    int16_t *buf = (int16_t *)malloc((size_t)want * 2);
    if (!buf) { fclose(fp); return -5; }
    if (fseek(fp, data_off, SEEK_SET) != 0 ||
        fread(buf, 2, (size_t)want, fp) != want) { free(buf); fclose(fp); return -6; }
    fclose(fp);

    /* de-interleave: เก็บช่อง 0 */
    uint64_t n = want / channels;
    int16_t *mono = (int16_t *)malloc((size_t)n * 2);
    for (uint64_t i = 0; i < n; i++) mono[i] = buf[i * channels];
    free(buf);
    *out = mono;
    *n_out = n;
    return 0;
}

static void probe_file(const char *path) {
    printf("\n═ %s ═\n", path);
    int16_t *s = NULL;
    uint64_t n = 0;
    long rc = read_wav_pcm(path, &s, &n);
    if (rc != 0) { printf("  (skip — rc=%ld: ไม่ใช่ PCM16 / อ่านไม่ได้)\n", rc); return; }
    printf("  samples: %llu (%llu KB PCM ช่องเดียว)\n",
           (unsigned long long)n, (unsigned long long)(n * 2 >> 10));

    uint64_t h_raw[65536] = {0};
    uint64_t *h_d  = (uint64_t *)calloc(131072, sizeof(uint64_t));
    uint64_t *h_d2 = (uint64_t *)calloc(131072, sizeof(uint64_t));
    uint64_t z_d = 0, z_d2 = 0, small7 = 0, small31 = 0;

    h_raw[(uint16_t)s[0]]++;
    for (uint64_t i = 1; i < n; i++) {
        int32_t d = (int32_t)s[i] - (int32_t)s[i - 1];
        h_raw[(uint16_t)s[i]]++;
        h_d[d + 65536]++;
        if (d == 0) z_d++;
        if (d >= -7 && d <= 7) small7++;
        if (d >= -31 && d <= 31) small31++;
        if (i > 1) {
            int32_t d2 = d - ((int32_t)s[i - 1] - (int32_t)s[i - 2]);
            h_d2[d2 + 65536]++;
            if (d2 == 0) z_d2++;
        }
    }
    uint64_t nd = n - 1, nd2 = n - 2;
    double hr  = entropy_of(h_raw, 65536, n);
    double hd  = entropy_of(h_d, 131072, nd);
    double hd2 = entropy_of(h_d2, 131072, nd2);

    printf("  H(raw)=%.2f/16b | H(delta)=%.2f/16b | H(delta2)=%.2f/16b\n",
           hr, hd, hd2);
    printf("  ratio: delta %.1f×  delta2 %.1f×  (16 bits / H)\n",
           16.0 / (hd < 0.01 ? 0.01 : hd), 16.0 / (hd2 < 0.01 ? 0.01 : hd2));
    printf("  delta: %s zero, |d|≤7 = %.1f%%, |d|≤31 = %.1f%%\n",
           hd > 6.0 ? "0%" : ">0%",
           100.0 * (double)small7 / (double)nd,
           100.0 * (double)small31 / (double)nd);
    (void)z_d2;
    free(h_d); free(h_d2); free(s);
}

static void self_test(void) {
    /* sine: delta เป็น deterministic → H(delta) ≈ 0 */
    uint64_t n = 200000;
    int16_t *s = (int16_t *)malloc((size_t)n * 2);
    for (uint64_t i = 0; i < n; i++)
        s[i] = (int16_t)(30000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / 44100.0));
    uint64_t h_d[131072] = {0};
    for (uint64_t i = 1; i < n; i++) h_d[(int32_t)s[i] - (int32_t)s[i - 1] + 65536]++;
    double hd = entropy_of(h_d, 131072, n - 1);
    printf("self-test sine: H(delta)=%.3f bits (expect < 1) — ratio %.0f×\n",
           hd, 16.0 / (hd < 0.01 ? 0.01 : hd));
    free(s);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Waveform delta probe — พิสูจน์ว่า raw PCM มีโครงสร้าง (ต่างจาก Q8)\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    self_test();

    const char *def[3] = {
        "F:/notebookLM/Latent-to-Latent Architecture_ Optimization and Attention Masking Strategies/Artifacts/ltn2ltn เจนภาพไวขึ้น 10 เท่า.wav",
        "F:/notebookLM/Temporal Space_ Geometry of the Wait Moment/Artifacts/หยุดเวลาในคอมพิวเตอร์ด้วยตรรกะเรขาคณิต.wav",
        "F:/notebookLM/Electric Hilbert Color Pallets/Artifacts/Replacing Software Logic with Geometric Gravity.wav"
    };
    uint32_t n = (argc > 1) ? (uint32_t)argc - 1 : 3;
    for (uint32_t i = 0; i < n; i++) probe_file((argc > 1) ? argv[i + 1] : def[i]);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    return 0;
}
