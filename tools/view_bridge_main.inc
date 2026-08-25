static inline uint32_t fwd_view(const uint8_t *vw, uint32_t f) {
    uint32_t w = f % 60, l = f / 60;
    return l * 60 + vw[w];
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

#define PART_BYTES   (128u * 1024u)
#define RID_SLOTS    60u
#define NLANG        9

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *twin_path = argc > 2 ? argv[2] : "build\\view_bridge.twin";

    printf("=== view_bridge — bake ONCE (pent), serve all languages ===\n");

    static uint8_t vw[NLANG][60];
    const char *lname[NLANG] = { "pent", "tri", "snubL", "snubR",
                                 "hosoya", "zeck", "pascal", "hexagram",
                                 "limacon" };
    if (build_rid(vw) != 0) { printf("FAIL rid geometry\n"); return 1; }

    /* derived views 4..8: strides / orders / limacon grammar (int-only) */
    for (uint32_t p = 0; p < 60; p++) vw[4][p] = (uint8_t)((p * 13u) % 60u);
    { static const uint64_t ZF[12] = { 0,1,1,2,3,5,8,13,21,34,55,89 };
      uint32_t zkey[60], zord[60];
      for (int n = 0; n < 60; n++) {
          uint64_t rem = (uint64_t)(n + 1), mask = 0;
          for (int i = 9; i >= 0; i--)
              if (ZF[i + 2] <= rem) { mask |= 1ull << i; rem -= ZF[i + 2]; }
          uint64_t rev = 0;
          for (int b = 0; b < 10; b++)
              if (mask & (1ull << b)) rev |= 1ull << (9 - b);
          zkey[n] = (uint32_t)rev; zord[n] = (uint32_t)n;
      }
      for (int i = 1; i < 60; i++) {
          uint32_t v2 = zord[i]; int j = i - 1;
          while (j >= 0 && zkey[zord[j]] > zkey[v2]) { zord[j+1]=zord[j]; j--; }
          zord[j + 1] = v2;
      }
      for (int i = 0; i < 60; i++) vw[5][i] = (uint8_t)zord[i]; }
    { int32_t A[60];
      for(int n=0;n<60;n++){ int64_t s=0;
          for(int k=0;k<=n/2;k++){ int NK=n-k,K=k;
              if(K>NK-K)K=NK-K;
              int64_t c=1; for(int i=0;i<K;i++) c=c*(NK-i)/(i+1);
              if(k&1)s-=c; else s+=c; }
          A[n]=(int32_t)s; }
      uint32_t ord[60]; for(int i=0;i<60;i++) ord[i]=(uint32_t)i;
      for(int i=1;i<60;i++){uint32_t v2=ord[i];int j=i-1;
          while(j>=0&&(A[ord[j]]>A[v2]||(A[ord[j]]==A[v2]&&(int)ord[j]>(int)v2))){
              ord[j+1]=ord[j];j--; }
          ord[j+1]=v2; }
      for(int i=0;i<60;i++) vw[6][i]=(uint8_t)ord[i]; }
    { int32_t dist[60], q0a[60], r0a[60];
      for(int i=0;i<60;i++){ int q=(i%10)-5, r=(i/10)-2;
          q0a[i]=q; r0a[i]=r;
          int aq=q<0?-q:q, ar=r<0?-r:r, as=(q+r)<0?-(q+r):(q+r);
          dist[i]=(aq+ar+as)/2; }
      uint32_t ord[60]; for(int i=0;i<60;i++) ord[i]=(uint32_t)i;
      for(int i=1;i<60;i++){uint32_t v2=ord[i];int j=i-1;
          while(j>=0&&(dist[ord[j]]>dist[v2]||
                 (dist[ord[j]]==dist[v2]&&q0a[ord[j]]>q0a[v2])||
                 (dist[ord[j]]==dist[v2]&&q0a[ord[j]]==q0a[v2]&&r0a[ord[j]]>r0a[v2]))){
              ord[j+1]=ord[j];j--; }
          ord[j+1]=v2; }
      for(int i=0;i<60;i++) vw[7][i]=(uint8_t)ord[i]; }
    for (int p = 0; p < 60; p++) {
        int w;
        if (p == 0) w = 30;
        else if (p == 59) w = 0;
        else w = (p & 1) ? 30 + (p + 1) / 2 : 30 - p / 2;
        vw[8][p] = (uint8_t)w;
    }
    /* bijection sanity on every view */
    for (int L = 0; L < NLANG; L++) {
        uint8_t hit[60]; memset(hit, 0, sizeof(hit));
        for (int p = 0; p < 60; p++) {
            if (hit[vw[L][p]]) { printf("FAIL %s not bijective\n", lname[L]); return 1; }
            hit[vw[L][p]] = 1;
        }
    }

    /* ── read source file ───────────────────────────────────────────── */
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long file_sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *src = (uint8_t *)malloc((size_t)file_sz);
    size_t nr = fread(src, 1, (size_t)file_sz, fp);
    fclose(fp);
    if ((long)nr != file_sz) { printf("FAIL read\n"); return 1; }

    uint32_t total_parts = (uint32_t)(((size_t)file_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;
    printf("file: %.1f MB · %u parts · %u layers · twin %s\n\n",
           (double)file_sz / 1e6, total_parts, layers, twin_path);

    /* per-part XOR oracle (clamped length) */
    uint32_t *plen = malloc(sizeof(uint32_t) * total_parts);
    uint64_t *pxor = malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++) {
        uint32_t off = f * PART_BYTES;
        uint32_t len = (uint32_t)((size_t)file_sz - off);
        if (len > PART_BYTES) len = PART_BYTES;
        plen[f] = len;
        pxor[f] = xor64(src + off, len);
    }

    DtSlotRegion reg;
    remove(twin_path);
    if (dt_slot_init_twin(&reg, twin_path,
                          (size_t)layers * RID_SLOTS, PART_BYTES) != 0) {
        printf("FAIL twin init\n"); return 1;
    }

    /* ── B1: SINGLE bake through pent only (last part clamped) ──────── */
    uint32_t writes = 0;
    for (uint32_t f = 0; f < total_parts; f++) {
        uint32_t off = f * PART_BYTES;
        uint32_t len = (uint32_t)((size_t)file_sz - off);
        if (len > PART_BYTES) len = PART_BYTES;
        if (dt_slot_put(&reg, fwd_view(vw[0], f),
                        src + off, len)) writes++;
    }
    printf("[B1] SINGLE BAKE   writes=%u / parts=%u (%s)\n",
           writes, total_parts, writes == total_parts ? "once each" : "FAIL");
    int fails = 0;
    if (writes != total_parts) fails++;

    /* bridge table: sigma_L maps lang-L slot word -> pent slot word */
    uint8_t sig[NLANG][60];
    memset(sig, 0xFF, sizeof(sig));
    for (int L = 1; L < NLANG; L++)
        for (uint32_t w = 0; w < 60; w++)
            sig[L][vw[L][w]] = vw[0][w];

    /* ── B2+B3: cross-read every language by translation ────────────── */
    uint8_t *buf = malloc(PART_BYTES);
    for (int L = 1; L < NLANG; L++) {
        uint32_t bad = 0;
        uint8_t consensus = 1;
        for (uint32_t f = 0; f < total_parts && bad == 0; f++) {
            uint32_t w = f % 60, l = f / 60;
            uint32_t addr = l * 60 + sig[L][vw[L][w]];
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p || memcmp(p, src + f * PART_BYTES, plen[f]) != 0) bad = f + 1;
            else if (xor64(p, plen[f]) != pxor[f]) consensus = 0;
        }
        printf("[B2/B3] %-9s cross-read %u parts · mismatch@%u · xor-consensus=%s\n",
               lname[L], total_parts, bad ? bad - 1 : 0,
               consensus ? "YES" : "NO");
        if (bad || !consensus) fails++;
    }

    /* ── B4: damage drill THROUGH a foreign language's ordering ─────── */
    {
        uint32_t fhit = total_parts / 2;
        uint32_t w = fhit % 60, l = fhit / 60;
        uint32_t addr_pent = l * 60 + vw[0][w];
        dt_slot_ptr(&reg, addr_pent)[17] ^= 0x80;
        int all_localize = 1;
        for (int L = 1; L < NLANG; L++) {
            uint32_t found = total_parts;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint32_t ww = f % 60, ll = f / 60;
                uint32_t addr = ll * 60 + sig[L][vw[L][ww]];
                uint8_t *p = dt_slot_ptr(&reg, addr);
                if (!p || xor64(p, plen[f]) != pxor[f]) { found = f; break; }
            }
            if (found != fhit) { all_localize = 0;
                printf("        [B4] %s localized part %u != %u\n",
                       lname[L], found, fhit); }
        }
        dt_slot_put(&reg, addr_pent, src + (size_t)fhit * PART_BYTES,
                    plen[fhit]);
        uint8_t *p = dt_slot_ptr(&reg, addr_pent);
        int restored = p && memcmp(p, src + (size_t)fhit * PART_BYTES,
                                   plen[fhit]) == 0;
        printf("[B4] DAMAGE X-VIEW flip@pent-part %u → all 8 langs localize=%s · restore-once=%s\n",
               fhit, all_localize ? "YES" : "NO", restored ? "YES" : "NO");
        if (!all_localize || !restored) fails++;
    }

    /* ── B5: cycle census of each sigma_L ───────────────────────────── */
    printf("[B5] cycle census of sigma_L (fixed points = shared words):\n");
    for (int L = 1; L < NLANG; L++) {
        uint8_t seen[60]; memset(seen, 0, sizeof(seen));
        int ncyc = 0, fixed = 0, maxlen = 0;
        printf("        %-9s", lname[L]);
        for (uint32_t w = 0; w < 60; w++) {
            if (seen[w]) continue;
            int len = 0; uint32_t j = w;
            while (!seen[j]) { seen[j] = 1; len++; j = sig[L][j]; }
            if (len == 1) fixed++;
            if (len > maxlen) maxlen = len;
            ncyc++;
        }
        printf(" cycles=%d fixed=%d longest=%d\n", ncyc, fixed, maxlen);
    }

    /* ── MUT: corrupted sigma must fail cross-read ──────────────────── */
    {
        uint8_t save = sig[4][vw[4][7]];         /* hosoya slot 7 -> wrong */
        sig[4][vw[4][7]] = (uint8_t)((sig[4][vw[4][7]] + 1) % 60);
        uint32_t f = 86 * 0 + 7;                 /* layer 0, word 7 */
        uint32_t addr = sig[4][vw[4][f]];
        uint8_t *p = dt_slot_ptr(&reg, addr);
        int caught = !p || memcmp(p, src + f * PART_BYTES, PART_BYTES) != 0;
        sig[4][vw[4][7]] = save;                 /* undo */
        printf("[MUT] corrupted hosoya-sigma → cross-read %s\n",
               caught ? "caught (RED ok)" : "UNDETECTED — SUITE BROKEN");
        if (!caught) fails++;
    }

    dt_slot_destroy(&reg);
    free(src); free(pxor); free(plen); free(buf);

    printf("\n%s\n", fails == 0
        ? "RESULT: PASSED — one bake serves every language lossless"
        : "RESULT: FAILED");
    return fails ? 1 : 0;
}
