/* RSS touch test: mmap field, touch every page, measure peak RSS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "build/fieldA.bin";
    int touch = (argc > 2) ? atoi(argv[2]) : 1;
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) { printf("open fail\n"); return 1; }
    LARGE_INTEGER sz; GetFileSizeEx(hf, &sz);
    HANDLE hm = CreateFileMapping(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    uint8_t *base = (uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!base) { printf("map fail\n"); return 1; }
    printf("mapped %llu bytes\n", (unsigned long long)sz.QuadPart);
    fflush(stdout);
    if (touch) {
        volatile uint8_t acc = 0;
        for (uint64_t i = 0; i < (uint64_t)sz.QuadPart; i += 4096) {
            acc ^= base[i];
        }
        printf("touched all pages (acc=%u)\n", acc);
    } else {
        printf("no touch\n");
    }
    fflush(stdout);
    Sleep(200); /* allow measurement */
    UnmapViewOfFile(base);
    CloseHandle(hm); CloseHandle(hf);
    return 0;
}