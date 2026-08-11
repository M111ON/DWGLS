// JSID Scale → 20736 Address Map
// Maps musical scales (JSID) to the 20736 address space
// 12 chromatic notes × 12 keys × 12 octaves × 12 = 20736

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GEO_ADDR_SPACE 20736
#define GEO_GRID_SIDE 144
#define N_NOTES 12
#define N_KEYS 12
#define N_SCALES 2048  // 2^11 possible scales (root always on)
#define JSID_MIN 2048  // minimum JSID value
#define JSID_MAX 4095  // maximum JSID value

// Note names
static const char *note_names[] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

// Common scales with their JSID values
typedef struct {
    int jsid;
    const char *name;
    const char *binary;
} ScaleInfo;

static const ScaleInfo common_scales[] = {
    {2773, "Major (Ionian)",     "101011010101"},
    {2906, "Natural Minor",      "101101011010"},
    {2902, "Dorian",             "101101010110"},
    {3418, "Phrygian",           "110101011010"},
    {2741, "Lydian",             "101010110101"},
    {2774, "Mixolydian",         "101011010110"},
    {3434, "Locrian",            "110101101010"},
    {2901, "Melodic Minor ↑",    "101101010101"},
    {2905, "Harmonic Minor",     "101101011001"},
    {2644, "Major Pentatonic",   "101001010100"},
    {3289, "Double Harmonic",    "110011011001"},
    {3290, "Phrygian Dominant",  "110011011010"},
    {2730, "Whole Tone",         "101010101010"},
    {2925, "Octatonic WH",       "101101101101"},
    {3510, "Octatonic HW",       "110110110110"},
    {4095, "Chromatic (all)",    "111111111111"},
    {2048, "Single note",        "100000000000"},
    {2778, "No Name (GoT)",      "101011011010"},
};

// Address mapping:
// addr = key * 12^3 + scale_type * 12^2 + degree * 12 + octave
// where scale_type = which of the 12 scale patterns
// For simplicity: addr = (key * N_SCALES + scale_index) % 20736
// Better: addr = key * 144 + (scale_pattern mapped to 0-143)

// Map a 12-bit scale pattern to a value 0-143
// Use: count of active notes × 12 + position of first active note after root
int scale_to_index(int pattern) {
    int count = 0;
    int first = -1;
    for (int i = 1; i < N_NOTES; i++) {  // skip root (bit 0)
        if (pattern & (1 << i)) {
            count++;
            if (first < 0) first = i;
        }
    }
    // count: 0-11 notes (excluding root)
    // first: 1-11 (position of first non-root note)
    int idx = count * N_NOTES + (first > 0 ? first - 1 : 0);
    if (idx >= GEO_GRID_SIDE) idx = GEO_GRID_SIDE - 1;
    return idx;
}

// Generate all addresses for a given scale in a given key
int scale_addrs(int jsid, int key, int *addrs) {
    int pattern = jsid;  // 12-bit pattern
    int n = 0;
    
    for (int note = 0; note < N_NOTES; note++) {
        if (pattern & (1 << note)) {
            // This note is in the scale
            int absolute_note = (key + note) % N_NOTES;
            int octave = (key + note) / N_NOTES;
            
            // Map to address
            // addr = absolute_note * 144 + scale_index * 1 + octave * 12
            int scale_idx = scale_to_index(pattern);
            int addr = absolute_note * GEO_GRID_SIDE + scale_idx;
            if (addr < 0) addr = 0;
            if (addr >= GEO_ADDR_SPACE) addr = GEO_ADDR_SPACE - 1;
            
            addrs[n++] = addr;
        }
    }
    return n;
}

int main(int argc, char *argv[]) {
    printf("=== JSID SCALE → 20736 ADDRESS MAP ===\n\n");
    
    // Track address usage
    static int addr_count[GEO_ADDR_SPACE];
    static int addr_scales[GEO_ADDR_SPACE];  // how many scales use this addr
    memset(addr_count, 0, sizeof(addr_count));
    memset(addr_scales, 0, sizeof(addr_scales));
    
    // Process common scales
    int n_common = sizeof(common_scales) / sizeof(common_scales[0]);
    
    printf("=== COMMON SCALES ===\n");
    printf("%-20s %-6s %-12s %s\n", "Scale", "JSID", "Binary", "Addresses (key=C)");
    printf("%-20s %-6s %-12s %s\n", "-----", "----", "------", "------------------");
    
    for (int s = 0; s < n_common; s++) {
        int addrs[12];
        int n = scale_addrs(common_scales[s].jsid, 0, addrs);  // key=C
        
        printf("%-20s %-6d %-12s ", 
               common_scales[s].name, common_scales[s].jsid, common_scales[s].binary);
        for (int i = 0; i < n && i < 6; i++) printf("%d ", addrs[i]);
        if (n > 6) printf("...");
        printf(" (%d addrs)\n", n);
        
        // Track across all 12 keys
        for (int key = 0; key < N_KEYS; key++) {
            int ka[12];
            int kn = scale_addrs(common_scales[s].jsid, key, ka);
            for (int i = 0; i < kn; i++) {
                addr_scales[ka[i]]++;
            }
        }
    }
    
    // Address space coverage analysis
    printf("\n=== ADDRESS SPACE COVERAGE ===\n");
    int filled = 0, max_scales = 0;
    double avg_scales = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_scales[i] > 0) filled++;
        if (addr_scales[i] > max_scales) max_scales = addr_scales[i];
        avg_scales += addr_scales[i];
    }
    avg_scales /= GEO_ADDR_SPACE;
    
    printf("Addresses used by common scales: %d / %d (%.1f%%)\n", 
           filled, GEO_ADDR_SPACE, filled * 100.0 / GEO_ADDR_SPACE);
    printf("Max scales per address: %d\n", max_scales);
    printf("Avg scales per address: %.1f\n", avg_scales);
    
    // Heat map (24x24)
    printf("\n=== SCALE DENSITY HEAT MAP (24×24) ===\n");
    printf("  (brightness = how many scales use this address)\n");
    for (int ay = 0; ay < 24; ay++) {
        printf("  ");
        for (int ax = 0; ax < 24; ax++) {
            int sum = 0;
            for (int dy = 0; dy < 6; dy++) {
                for (int dx = 0; dx < 6; dx++) {
                    int a = (ay * 6 + dy) * GEO_GRID_SIDE + (ax * 6 + dx);
                    sum += addr_scales[a];
                }
            }
            int level = sum / 20;
            if (level > 8) level = 8;
            printf("%c", " .:-=+*#%@"[level]);
        }
        printf("\n");
    }
    
    // Now process ALL 2048 scales for a single key
    printf("\n=== FULL SCALE SPACE (key=C) ===\n");
    memset(addr_scales, 0, sizeof(addr_scales));
    
    int scale_fill[13] = {0};  // count scales by number of notes
    for (int jsid = JSID_MIN; jsid <= JSID_MAX; jsid++) {
        int addrs[12];
        int n = scale_addrs(jsid, 0, addrs);
        scale_fill[n]++;
        
        for (int i = 0; i < n; i++) {
            addr_scales[addrs[i]]++;
        }
    }
    
    printf("Scales by number of notes (excluding root):\n");
    for (int i = 0; i <= 11; i++) {
        if (scale_fill[i] > 0) {
            printf("  %2d notes: %5d scales\n", i, scale_fill[i]);
        }
    }
    
    // Full coverage
    filled = 0;
    max_scales = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_scales[i] > 0) filled++;
        if (addr_scales[i] > max_scales) max_scales = addr_scales[i];
    }
    printf("\nAll 2048 scales (key=C): %d / %d addresses used (%.1f%%)\n",
           filled, GEO_ADDR_SPACE, filled * 100.0 / GEO_ADDR_SPACE);
    printf("Max scales per address: %d\n", max_scales);
    
    // Show most contested addresses
    printf("\n=== MOST CONTESTED ADDRESSES ===\n");
    printf("(addresses used by the most scales)\n");
    typedef struct { int addr; int count; } AddrCount;
    static AddrCount top[20];
    for (int i = 0; i < 20; i++) { top[i].addr = -1; top[i].count = 0; }
    
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_scales[i] > top[19].count) {
            top[19].addr = i;
            top[19].count = addr_scales[i];
            // Sort (insertion)
            for (int j = 18; j >= 0; j--) {
                if (top[j+1].count > top[j].count) {
                    AddrCount tmp = top[j]; top[j] = top[j+1]; top[j+1] = tmp;
                } else break;
            }
        }
    }
    
    for (int i = 0; i < 10 && top[i].addr >= 0; i++) {
        int note = top[i].addr / GEO_GRID_SIDE;
        int idx = top[i].addr % GEO_GRID_SIDE;
        printf("  addr %5d (note=%s, idx=%2d): %d scales\n",
               top[i].addr, note_names[note % 12], idx, top[i].count);
    }
    
    // Fibonacci connection
    printf("\n=== FIBONACCI CONNECTION ===\n");
    printf("Fibonacci mod 7 → scale degrees:\n");
    int fib[] = {1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987};
    printf("  F(n):      ");
    for (int i = 0; i < 16; i++) printf("%3d ", fib[i]);
    printf("\n  F(n) mod 7:");
    for (int i = 0; i < 16; i++) printf("%3d ", fib[i] % 7);
    printf("\n\n");
    
    printf("144 = F(12) = address grid side\n");
    printf("20736 = 144² = full address space\n");
    printf("12 notes → 12^4 = 20736 (4D chromatic space)\n");
    
    return 0;
}
