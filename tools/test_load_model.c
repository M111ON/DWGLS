
#include <stdio.h>
#include "llama.h"

int main() {
    printf("Starting test...\n");
    llama_backend_init();
    printf("Backend init done\n");
    
    const char *model_path = "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    printf("Loading model: %s\n", model_path);
    
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) {
        printf("FAILED to load model\n");
        return 1;
    }
    printf("Model loaded successfully\n");
    
    llama_model_free(model);
    llama_backend_free();
    printf("Test passed\n");
    return 0;
}
