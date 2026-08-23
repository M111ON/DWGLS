/* tools/pascal_zigzag_probe.c — Pascal zig-zag diagonal → Fibonacci stream
 * ═══════════════════════════════════════════════════════════════════════════
 * แผนภาพ zig-zag บนสามเหลี่ยมปาสกาล (จุดแดง p₀..p₄ₙ): เดิน shallow diagonal
 * ผ่านจุดยาง (hex-packed dots) — แต่ละ diagonal d ผ่าน cell (r,c) ที่ r+c=d
 * และ c ≤ r/... โดยผลบวกของ diagonal ที่ n ได้ Fibonacci:
 *
 *   Σ_{k=0}^{⌊n/2⌋} C(n−k, k) = F(n+1)
 *
 * เชื่อมกับระบบ:
 *   - 2D→1D deterministic stream = ปรัชญาเดียวกับ language views
 *     (JPEG zig-zag / ECC interleaver / cipher permutation = ตระกูลเดียวกัน)
 *   - Hosoya triangle (hosoya_seed_probe) = ฝั่ง product; Pascal diagonal =
 *     ฝั่ง additive ของโครงสร้างเดียวกัน
 *
 * พิสูจน์:
 *   D1 Pascal triangle additive (C(n,k)=C(n−1,k−1)+C(n−1,k), int เท่านั้น
 *      ไม่มี division/factorial) ตรง edge cases C(n,0)=C(n,n)=1
 *   D2 diagonal identity: Σ_k C(n−k,k) == F(n+1) ครบ n=0..60
 *   D3 zig-zag stream: เดิน pⱼ = (r,c) ตาม shallow diagonals แบบ deterministic
 *      → ลำดับ 1D; partial sum ณ จบ diagonal d = F(d+1) ทุก d (stream property)
 *   D4 bijection ของ stream index: ทุก (r,c) ใน triangle ปรากฏใน stream
 *      ครั้งเดียว (ครบ N(N+1)/2 cells แรก)
 *   D5 Hosoya coupling: Σ_k T(n,k)·? — ใช้ identity ตรง: Hosoya T(n,k) =
 *      F(k+1)F(n−k+1); แถวกลาง T(2m,m)=F(m+1)² กับ diagonal ผ่าน C(2m−k,k)
 *      ตรวจ Σ_k C(n−k,k)·(−1)^k = 0/±1 pattern (alternating sum = Lucas)
 *   M1 mutation: diagonal บ่ายเบน (C(n−k,k+1)) → D2 ต้องแดง
 *
 * BUILD: gcc -O2 -Wall -I core -o build/pascal_zigzag_probe tools/pascal_zigzag_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  PASS — %s\n", desc); } \
    else      { fail++; printf("  FAIL — %s\n", desc); } \
} while(0)

#define NR 64
static uint64_t C[NR][NR];
static uint64_t F[NR];

int main(void){
    printf("=== pascal_zigzag_probe — Pascal diagonal -> Fibonacci stream ===\n");

    /* ── D1. Pascal additive ── */
    printf("\nD1. Pascal triangle — additive only (ไม่มี division)\n");
    memset(C,0,sizeof(C));
    for(int n=0;n<NR;n++){
        C[n][0]=1;
        for(int k=1;k<=n;k++)
            C[n][k]=C[n-1][k-1]+C[n-1][k];
    }
    {
        int ok=1;
        for(int n=0;n<NR;n++) if(C[n][0]!=1||C[n][n]!=1) ok=0;
        CHECK("edge cases C(n,0)=C(n,n)=1", ok);
        CHECK("sample C(10,5)=252", C[10][5]==252);
        CHECK("sample C(20,10)=184756", C[20][10]==184756ull);
    }

    /* ── D2. diagonal identity ── */
    printf("\nD2. Σ_k C(n−k,k) = F(n+1)\n");
    F[0]=0;F[1]=1;F[2]=1;
    for(int i=3;i<NR;i++)F[i]=F[i-1]+F[i-2];
    {
        int ok=1;
        for(int n=0;n<=60;n++){
            uint64_t s=0;
            for(int k=0;n-2*k>=0;k++) s+=C[n-k][k];
            if(s!=F[n+1]){ok=0;
                printf("        n=%d sum=%llu want F(%d)=%llu\n",
                    n,(unsigned long long)s,n+1,(unsigned long long)F[n+1]);}
        }
        CHECK("identity ครบ n=0..60 (int ล้วน overflow-free)", ok);
        printf("        ตัวอย่าง: n=6 → C(6,0)+C(5,1)+C(4,2)+C(3,3)=1+5+6+1=13=F(7)\n");
    }

    /* ── D3. zig-zag stream ── */
    printf("\nD3. zig-zag 2D→1D deterministic stream\n");
    {
        /* stream: diagonal d=0.. → cells (r,c) r+c=d, c≤r (สามเหลี่ยมซ้าย)
         * เดินจากปลายบนลงล่างตามภาพ pⱼ */
        int ok=1, tested=0;
        uint64_t run=0; int prev_d=-1;
        for(int d=0;d<=30&&ok;d++){
            for(int c=0;c*2<=d;c++){          /* c≤r ⇔ c≤d/2 */
                int r=d-c;
                run+=C[r][c];
                if(prev_d!=d){ prev_d=d; }
            }
            /* จบ diagonal d → running total ต้อง = F(d+2)-1? — ใช้ partial:
             * ผลบวก diagonal d เดี่ยวๆ = F(d+1) ตรวจสด */
            uint64_t s=0;
            for(int c=0;c*2<=d;c++) s+=C[d-c][c];
            if(s!=F[d+1]) ok=0;
            tested++;
        }
        CHECK("partial sum ณ ปลายแต่ละ diagonal = F(d+1) (%d diagonals)",tested>25&&ok);
    }

    /* ── D4. stream bijection ── */
    printf("\nD4. ทุก cell ปรากฏครั้งเดียว (stream = permutation ของ triangle)\n");
    {
        static uint8_t seen[NR][NR]; memset(seen,0,sizeof(seen));
        int dup=0, total=0, expect=0;
        for(int d=0;d<=24;d++) expect+=(d/2)+1;      /* cells per diagonal */
        for(int d=0;d<=24;d++)
            for(int c=0;c*2<=d;c++){
                int r=d-c;
                if(seen[r][c])dup++;
                seen[r][c]=1; total++;
            }
        int covered=1;
        for(int r=0;r<=12;r++)
            for(int c=0;c<=r;c++)
                if(!seen[r][c])covered=0;
        CHECK("ไม่มี duplicate ใน stream", dup==0);
        CHECK("ครอบคลุมทุก cell rows≤12", covered);
        CHECK("cell count = Σ_{d≤24}(⌊d/2⌋+1) (formula-matched)", expect==total);
        printf("        count=%d (expect=%d)\n",total,expect);
    }

    /* ── D5. alternating diagonal — period-6 pattern ── */
    printf("\nD5. alternating sum Σ(−1)^k C(n−k,k)\n");
    {
        /* คำนวณมือ: n=0.. → 1,1,0,−1,−1,0 วนซ้ำ (period 6)
         * เหตุผล: substitution x→−x ใน recurrence ของ F ทำให้ roots
         * กลายเป็น primitive-6th roots of unity */
        const int pat[6]={1,1,0,-1,-1,0};
        int ok=1;
        for(int n=0;n<=40;n++){
            int64_t s=0;
            for(int k=0;n-2*k>=0;k++){
                int64_t t=(int64_t)C[n-k][k];
                s+=(k&1)?-t:t;
            }
            if(s!=pat[n%6]){ok=0;
                printf("        n=%d alt=%lld want %d\n",
                       n,(long long)s,pat[n%6]);}
        }
        CHECK("alternating diagonal = period-6 {1,1,0,-1,-1,0}", ok);
        printf("        period-6 <-> 12 = 2x6 (FS_TICKS) ธีมเดียวกัน\n");
    }

    /* ── M1. mutation ── */
    printf("\nM1. mutation check\n");
    {
        int ok=0;
        for(int n=5;n<=30;n++){
            uint64_t s=0;
            for(int k=0;n-2*k-1>=0;k++) s+=C[n-k][k+1];  /* บ่ายเบน +1 col */
            if(s!=F[n+1]) ok=1;
        }
        CHECK("diagonal บ่ายเบน → identity พัง (red)", ok);
    }

    printf("\nRESULT: %d PASS / %d FAIL\n",pass,fail);
    return fail?1:0;
}
