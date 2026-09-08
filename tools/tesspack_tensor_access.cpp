// tesspack_tensor_access.cpp — minimal helper to access model->tensors_by_name
// Compiled separately to avoid OOM in main translation unit
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "llama.h"
#include "llama-model.h"

struct tensor_info {
    const char *name;
    void *data;
    size_t nbytes;
    int type;
};

extern "C" int enumerate_model_tensors(struct llama_model *model, struct tensor_info *out, int max_out) {
    int count = 0;
    for (auto &p : model->tensors_by_name) {
        if (count >= max_out) break;
        out[count].name = p.first.c_str();
        out[count].data = p.second ? p.second->data : NULL;
        out[count].nbytes = p.second ? ggml_nbytes(p.second) : 0;
        out[count].type = p.second ? (int)p.second->type : -1;
        count++;
    }
    return count;
}

extern "C" int find_tensor_data(struct llama_model *model, const char *name, void **data, size_t *nbytes) {
    for (auto &p : model->tensors_by_name) {
        if (strcmp(p.first.c_str(), name) == 0) {
            *data = p.second ? p.second->data : NULL;
            *nbytes = p.second ? ggml_nbytes(p.second) : 0;
            return 0;
        }
    }
    return -1;
}

extern "C" void set_tensor_data(struct llama_model *model, const char *name, void *new_data) {
    for (auto &p : model->tensors_by_name) {
        if (strcmp(p.first.c_str(), name) == 0) {
            if (p.second) p.second->data = new_data;
            return;
        }
    }
}
