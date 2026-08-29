#include <stdio.h>
#include "llama.h"

int main() {
    fprintf(stderr, "MINIMAL TEST START\n");
    fflush(stderr);
    
    fprintf(stderr, "Calling llama_backend_init...\n");
    fflush(stderr);
    llama_backend_init();
    
    fprintf(stderr, "Backend initialized\n");
    fflush(stderr);
    
    llama_backend_free();
    fprintf(stderr, "Backend freed\n");
    fflush(stderr);
    
    return 0;
}