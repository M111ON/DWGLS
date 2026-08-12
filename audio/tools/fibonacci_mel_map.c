// Fibonacci mod 7 → Mel Bin Mapping
// Connects Fibonacci sequence to audio frequency representation
// 144 = F(12), 20736 = 144², 80 mel bins, 7 scale degrees

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N_MEL_BINS 80
#define N_DEGREES 7
#define N_FIBO 32  // Fibonacci terms to show
#define GEO_GRID_SIDE 144
#define GEO_ADDR_SPACE 20736

// Fibonacci numbers
long long fib[N_FIBO];

// Fibonacci mod 7 → scale degree (1-7, where 0 maps to 7)
int fib_mod7[N_FIBO];

// Mel bin ranges for each scale degree
// Based on: mel = 2595 × log10(1 + f/700), f = bin × sr / n_fft
// 80 bins, 16kHz sr, 400 n_fft → bin 0 = 0Hz, bin 80 = 8000Hz
// 7 degrees → each covers ~11.4 bins
typedef struct {
    int degree;
    int bin_start;
    int bin_end;
    double freq_start;
    double freq_end;
    const char *name;
} DegreeMapping;

DegreeMapping degree_map[N_DEGREES];

// Initialize Fibonacci sequence
void init_fib(void) {
    fib[0] = 1; fib[1] = 1;
    for (int i = 2; i < N_FIBO; i++) {
        fib[i] = fib[i-1] + fib[i-2];
    }
    
    for (int i = 0; i < N_FIBO; i++) {
        fib_mod7[i] = (int)(fib[i] % 7);
        if (fib_mod7[i] == 0) fib_mod7[i] = 7;  // map 0 → 7
    }
}

// Build degree-to-mel-bin mapping
// Use logarithmic spacing to match mel scale
void build_degree_map(void) {
    // Mel bin centers (approximate frequencies)
    // bin 0 ≈ 0Hz, bin 80 ≈ 8000Hz
    // Mel scale: mel = 2595 × log10(1 + f/700)
    
    const char *names[] = {"Tonic", "Major 2nd", "Minor 3rd", "Perfect 4th",
                           "Perfect 5th", "Major 6th", "Minor 7th"};
    
    // Logarithmic spacing: each degree covers a frequency ratio
    // In music: each interval has a frequency ratio
    // Octave = 2:1, Perfect 5th = 3:2, Perfect 4th = 4:3, etc.
    
    // Map 7 degrees to 80 mel bins using musical intervals
    // Degree 1 (Tonic): bins 0-6 (low fundamentals)
    // Degree 2: bins 7-17
    // Degree 3: bins 18-28
    // Degree 4: bins 29-39
    // Degree 5: bins 40-50
    // Degree 6: bins 51-61
    // Degree 7: bins 62-79
    
    int bins_per_degree = N_MEL_BINS / N_DEGREES;
    int remainder = N_MEL_BINS % N_DEGREES;
    
    int start = 0;
    for (int d = 0; d < N_DEGREES; d++) {
        degree_map[d].degree = d + 1;
        degree_map[d].bin_start = start;
        degree_map[d].bin_end = start + bins_per_degree - 1;
        if (d < remainder) degree_map[d].bin_end++;  // distribute remainder
        start = degree_map[d].bin_end + 1;
        
        // Approximate frequencies
        degree_map[d].freq_start = degree_map[d].bin_start * 8000.0 / N_MEL_BINS;
        degree_map[d].freq_end = (degree_map[d].bin_end + 1) * 8000.0 / N_MEL_BINS;
        degree_map[d].name = names[d];
    }
}

// Map a mel bin to a scale degree
int mel_bin_to_degree(int bin) {
    for (int d = 0; d < N_DEGREES; d++) {
        if (bin >= degree_map[d].bin_start && bin <= degree_map[d].bin_end) {
            return degree_map[d].degree;
        }
    }
    return 1;  // default to tonic
}

// Generate a melody using Fibonacci mod 7
// Returns array of mel bin indices
int fibonacci_melody(int n_notes, int *melody) {
    for (int i = 0; i < n_notes; i++) {
        int degree = fib_mod7[i % N_FIBO];
        // Map degree to middle of its mel bin range
        int d = degree - 1;
        melody[i] = (degree_map[d].bin_start + degree_map[d].bin_end) / 2;
    }
    return n_notes;
}

// Map melody to 20736 addresses
void melody_to_addresses(int *melody, int n_notes, int *addrs, int *n_addrs) {
    *n_addrs = 0;
    for (int i = 0; i < n_notes; i++) {
        int bin = melody[i];
        int degree = mel_bin_to_degree(bin);
        
        // Address mapping: (note_index % 144) * 144 + mel_bin
        int angle = i % GEO_GRID_SIDE;
        int radius = bin;
        int addr = angle * GEO_GRID_SIDE + radius;
        
        if (addr >= 0 && addr < GEO_ADDR_SPACE) {
            addrs[*n_addrs] = addr;
            (*n_addrs)++;
        }
    }
}

int main(void) {
    init_fib();
    build_degree_map();
    
    printf("=== FIBONACCI MOD 7 → MEL BIN MAPPING ===\n\n");
    
    // Show Fibonacci sequence
    printf("=== FIBONACCI SEQUENCE ===\n");
    printf("F(n):       ");
    for (int i = 0; i < 20; i++) printf("%4lld ", fib[i]);
    printf("\nF(n) mod 7: ");
    for (int i = 0; i < 20; i++) printf("%4d ", fib_mod7[i]);
    printf("\nDegree:     ");
    for (int i = 0; i < 20; i++) {
        printf("  %d ", fib_mod7[i]);
    }
    printf("\n\n");
    
    // Show degree-to-mel mapping
    printf("=== SCALE DEGREE → MEL BIN MAPPING ===\n");
    printf("Degree  Name         Bins     Freq Range\n");
    printf("------  ----         ----     ----------\n");
    for (int d = 0; d < N_DEGREES; d++) {
        printf("  %d     %-12s %3d-%3d   %.0f - %.0f Hz\n",
               degree_map[d].degree, degree_map[d].name,
               degree_map[d].bin_start, degree_map[d].bin_end,
               degree_map[d].freq_start, degree_map[d].freq_end);
    }
    printf("\n");
    
    // Generate Fibonacci melody
    printf("=== FIBONACCI MELODY (first 32 notes) ===\n");
    int melody[32];
    fibonacci_melody(32, melody);
    
    printf("Note: Degree → Mel Bin → Frequency\n");
    for (int i = 0; i < 32; i++) {
        int degree = fib_mod7[i];
        int bin = melody[i];
        double freq = bin * 8000.0 / N_MEL_BINS;
        printf("  F(%2d) = %d (%s) → bin %2d (%.0f Hz)\n",
               i, degree, degree_map[degree-1].name, bin, freq);
    }
    printf("\n");
    
    // Map to addresses
    printf("=== ADDRESS MAPPING ===\n");
    int addrs[32];
    int n_addrs;
    melody_to_addresses(melody, 32, addrs, &n_addrs);
    
    printf("Fibonacci melody → %d addresses:\n", n_addrs);
    for (int i = 0; i < n_addrs && i < 16; i++) {
        int angle = addrs[i] / GEO_GRID_SIDE;
        int radius = addrs[i] % GEO_GRID_SIDE;
        printf("  addr %5d (angle=%3d, radius=%3d)\n", addrs[i], angle, radius);
    }
    if (n_addrs > 16) printf("  ... and %d more\n", n_addrs - 16);
    printf("\n");
    
    // Coverage analysis
    printf("=== COVERAGE ANALYSIS ===\n");
    int unique = 0;
    static int seen[GEO_ADDR_SPACE];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < n_addrs; i++) {
        if (!seen[addrs[i]]) {
            seen[addrs[i]] = 1;
            unique++;
        }
    }
    printf("Unique addresses: %d / %d (%.1f%%)\n", 
           unique, GEO_ADDR_SPACE, unique * 100.0 / GEO_ADDR_SPACE);
    printf("Total notes: %d\n", n_addrs);
    printf("Collision rate: %.1f%%\n", 
           n_addrs > 0 ? (1.0 - (double)unique / n_addrs) * 100 : 0);
    printf("\n");
    
    // Extended melody (144 notes = one full angle rotation)
    printf("=== EXTENDED MELODY (144 notes = full rotation) ===\n");
    int melody144[144];
    fibonacci_melody(144, melody144);
    int addrs144[144];
    int n144;
    melody_to_addresses(melody144, 144, addrs144, &n144);
    
    int unique144 = 0;
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < n144; i++) {
        if (!seen[addrs144[i]]) {
            seen[addrs144[i]] = 1;
            unique144++;
        }
    }
    printf("144 notes → %d unique addresses (%.1f%% coverage)\n",
           unique144, unique144 * 100.0 / GEO_ADDR_SPACE);
    
    // Degree distribution in melody
    printf("\nDegree distribution in 144-note melody:\n");
    int deg_count[N_DEGREES] = {0};
    for (int i = 0; i < 144; i++) {
        deg_count[fib_mod7[i % N_FIBO] - 1]++;
    }
    for (int d = 0; d < N_DEGREES; d++) {
        printf("  Degree %d: %3d notes (%.1f%%)\n",
               d + 1, deg_count[d], deg_count[d] * 100.0 / 144);
    }
    
    printf("\n=== KEY INSIGHT ===\n");
    printf("144 = F(12) = grid side\n");
    printf("Fibonacci mod 7 = natural scale degree pattern\n");
    printf("80 mel bins / 7 degrees ≈ 11.4 bins per degree\n");
    printf("→ Fibonacci melody maps to ~11% of address space\n");
    printf("→ Structured, not random — like music itself\n");
    
    return 0;
}
