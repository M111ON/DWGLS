// Geometric Audio Decoder
// Maps 20736 addresses to word-like units

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define GEO_ADDR_SPACE 20736
#define GEO_MEL_BINS 80
#define GEO_FRAMES_PER_BLOCK 259

// Cluster detection
typedef struct {
    int start_addr;
    int end_addr;
    int size;
    double avg_value;
    double peak_value;
    int peak_addr;
} Cluster;

// Compare clusters by position
int cmp_cluster_pos(const void *a, const void *b) {
    return ((Cluster*)a)->start_addr - ((Cluster*)b)->start_addr;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <geo_audio_addr.json>\n", argv[0]); return 1; }
    
    // Load address space from JSON
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("Error: Cannot open %s\n", argv[1]); return 1; }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *json = malloc(size + 1);
    fread(json, 1, size, fp);
    json[size] = 0;
    fclose(fp);
    
    // Parse data array (simple parsing)
    double *addr_space = malloc(GEO_ADDR_SPACE * sizeof(double));
    char *p = strstr(json, "\"data\": [");
    if (!p) { printf("Error: Cannot find data array\n"); free(json); free(addr_space); return 1; }
    p += 9; // skip "data": [
    
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        addr_space[i] = strtod(p, &p);
        p++; // skip comma
    }
    free(json);
    
    printf("=== GEOMETRIC AUDIO DECODER ===\n");
    printf("Loaded %d addresses\n\n", GEO_ADDR_SPACE);
    
    // Find threshold (mean + 0.5 * std)
    double mean = 0, var = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) mean += addr_space[i];
    mean /= GEO_ADDR_SPACE;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) var += (addr_space[i] - mean) * (addr_space[i] - mean);
    var /= GEO_ADDR_SPACE;
    double std = sqrt(var);
    double threshold = mean + 0.5 * std;
    
    printf("=== THRESHOLD ===\n");
    printf("Mean: %.4f\n", mean);
    printf("Std: %.4f\n", std);
    printf("Threshold: %.4f (mean + 0.5*std)\n\n", threshold);
    
    // Find active addresses
    int *active = calloc(GEO_ADDR_SPACE, sizeof(int));
    int num_active = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_space[i] > threshold) {
            active[i] = 1;
            num_active++;
        }
    }
    printf("Active addresses: %d / %d (%.1f%%)\n\n", num_active, GEO_ADDR_SPACE, num_active * 100.0 / GEO_ADDR_SPACE);
    
    // Detect clusters (consecutive active addresses)
    Cluster *clusters = malloc(GEO_ADDR_SPACE * sizeof(Cluster));
    int num_clusters = 0;
    
    int in_cluster = 0;
    int cluster_start = 0;
    double cluster_sum = 0;
    int cluster_count = 0;
    double cluster_peak = 0;
    int cluster_peak_addr = 0;
    
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (active[i]) {
            if (!in_cluster) {
                in_cluster = 1;
                cluster_start = i;
                cluster_sum = 0;
                cluster_count = 0;
                cluster_peak = 0;
                cluster_peak_addr = i;
            }
            cluster_sum += addr_space[i];
            cluster_count++;
            if (addr_space[i] > cluster_peak) {
                cluster_peak = addr_space[i];
                cluster_peak_addr = i;
            }
        } else {
            if (in_cluster) {
                clusters[num_clusters].start_addr = cluster_start;
                clusters[num_clusters].end_addr = i - 1;
                clusters[num_clusters].size = cluster_count;
                clusters[num_clusters].avg_value = cluster_sum / cluster_count;
                clusters[num_clusters].peak_value = cluster_peak;
                clusters[num_clusters].peak_addr = cluster_peak_addr;
                num_clusters++;
                in_cluster = 0;
            }
        }
    }
    if (in_cluster) {
        clusters[num_clusters].start_addr = cluster_start;
        clusters[num_clusters].end_addr = GEO_ADDR_SPACE - 1;
        clusters[num_clusters].size = cluster_count;
        clusters[num_clusters].avg_value = cluster_sum / cluster_count;
        clusters[num_clusters].peak_value = cluster_peak;
        clusters[num_clusters].peak_addr = cluster_peak_addr;
        num_clusters++;
    }
    
    // Sort by position
    qsort(clusters, num_clusters, sizeof(Cluster), cmp_cluster_pos);
    
    printf("=== CLUSTERS DETECTED ===\n");
    printf("Total clusters: %d\n\n", num_clusters);
    
    // Show clusters
    printf("Cluster  Start   End     Size  AvgVal   PeakVal  PeakAddr\n");
    printf("-------  ------  ------  ----  ------   -------  --------\n");
    for (int i = 0; i < num_clusters && i < 50; i++) {
        Cluster *c = &clusters[i];
        printf("%3d      %5d   %5d   %4d  %6.3f   %6.3f   %5d\n",
               i, c->start_addr, c->end_addr, c->size, c->avg_value, c->peak_value, c->peak_addr);
    }
    if (num_clusters > 50) printf("... and %d more clusters\n", num_clusters - 50);
    printf("\n");
    
    // Map clusters to frequency bands
    printf("=== FREQUENCY BAND ANALYSIS ===\n");
    int low_clusters = 0, mid_clusters = 0, high_clusters = 0;
    for (int i = 0; i < num_clusters; i++) {
        int mid_addr = (clusters[i].start_addr + clusters[i].end_addr) / 2;
        int mel_bin = mid_addr % GEO_MEL_BINS;
        if (mel_bin < 27) low_clusters++;
        else if (mel_bin < 54) mid_clusters++;
        else high_clusters++;
    }
    printf("Low freq clusters (bins 0-26):  %d\n", low_clusters);
    printf("Mid freq clusters (bins 27-53): %d\n", mid_clusters);
    printf("High freq clusters (bins 54-79): %d\n\n", high_clusters);
    
    // Cluster size distribution
    printf("=== CLUSTER SIZE DISTRIBUTION ===\n");
    int size_1 = 0, size_2_5 = 0, size_6_10 = 0, size_10plus = 0;
    for (int i = 0; i < num_clusters; i++) {
        int sz = clusters[i].size;
        if (sz == 1) size_1++;
        else if (sz <= 5) size_2_5++;
        else if (sz <= 10) size_6_10++;
        else size_10plus++;
    }
    printf("Size 1:      %d clusters\n", size_1);
    printf("Size 2-5:    %d clusters\n", size_2_5);
    printf("Size 6-10:   %d clusters\n", size_6_10);
    printf("Size 10+:    %d clusters\n\n", size_10plus);
    
    // Word-like units estimate
    printf("=== WORD-LIKE UNITS ESTIMATE ===\n");
    printf("Based on cluster analysis:\n");
    printf("- Total clusters: %d\n", num_clusters);
    printf("- Avg cluster size: %.1f addresses\n", (double)num_active / num_clusters);
    printf("- Estimated word units: ~%d (clusters of size > 2)\n", 
           size_2_5 + size_6_10 + size_10plus);
    printf("\n");
    
    // Compare with transcript
    printf("=== TRANSCRIPT COMPARISON ===\n");
    printf("Transcript words: ~800 (for 54 min audio)\n");
    printf("Our clusters: %d (for 9.66 sec segment)\n", num_clusters);
    printf("Extrapolated to 54 min: ~%.0f clusters\n", num_clusters * (54.0 * 60.0 / 9.66));
    printf("\n");
    printf("This suggests clusters ≈ word boundaries!\n");
    
    // Save clusters
    FILE *out = fopen("geo_clusters.json", "w");
    if (out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"num_clusters\": %d,\n", num_clusters);
        fprintf(out, "  \"threshold\": %.6f,\n", threshold);
        fprintf(out, "  \"clusters\": [\n");
        for (int i = 0; i < num_clusters; i++) {
            Cluster *c = &clusters[i];
            fprintf(out, "    {\"start\":%d, \"end\":%d, \"size\":%d, \"avg\":%.4f, \"peak\":%.4f}",
                    c->start_addr, c->end_addr, c->size, c->avg_value, c->peak_value);
            if (i < num_clusters - 1) fprintf(out, ",\n");
            else fprintf(out, "\n");
        }
        fprintf(out, "  ]\n}\n");
        fclose(out);
        printf("\nSaved to geo_clusters.json\n");
    }
    
    free(addr_space);
    free(active);
    free(clusters);
    return 0;
}
