/* tools/zone_lookup.c — zone-map v1 lookup: prompt → seedfile.
 * v1 classifier: case-insensitive substring match over keyword table.
 * Placeholder for anchor/keyword index (same lookup shape, better rank).
 * Map format (zones.map): <keyword> => <seedfile>  (# comments, blank skip)
 * Exit 0 + prints path on hit; exit 1 on miss (caller falls back to full-E).
 * BUILD: gcc -O2 -w -o build/zone_lookup tools/zone_lookup.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void lower(char *d, const char *s, size_t n) {
    size_t i;
    for (i = 0; i + 1 < n && s[i]; i++) d[i] = (char)tolower((unsigned char)s[i]);
    d[i] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: zone_lookup <zones.map> <prompt...>\n");
        return 2;
    }
    char prompt[4096] = {0}, pl[4096];
    for (int i = 2; i < argc && strlen(prompt) + strlen(argv[i]) + 2 < sizeof(prompt); i++) {
        if (i > 2) strcat(prompt, " ");
        strcat(prompt, argv[i]);
    }
    lower(pl, prompt, sizeof(pl));
    FILE *f = fopen(argv[1], "r");
    if (!f) { printf("MISS: no map\n"); return 1; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *sep = strstr(line, "=>");
        if (!sep) continue;
        *sep = '\0';
        char *kw = line, *path = sep + 2;
        while (*kw == ' ' || *kw == '\t') kw++;
        char *ke = kw + strlen(kw);
        while (ke > kw && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r' || ke[-1] == '\n')) *--ke = '\0';
        while (*path == ' ' || *path == '\t') path++;
        char *pe = path + strlen(path);
        while (pe > path && (pe[-1] == ' ' || pe[-1] == '\t' || pe[-1] == '\r' || pe[-1] == '\n')) *--pe = '\0';
        if (!*kw || !*path) continue;
        char kl[256];
        lower(kl, kw, sizeof(kl));
        if (strstr(pl, kl)) { printf("%s\n", path); fclose(f); return 0; }
    }
    fclose(f);
    printf("MISS\n");
    return 1;
}
