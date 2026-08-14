/* GGUFFieldStr for string handling (like engine demo) */
typedef struct {
    uint64_t len;
    char    *data;
} GGUFFieldStr;

#define GGUF_MAGIC  0x46554747u  /* 'GGUF' */

/* GGML tensor types (from llama.cpp) */
typedef enum {
    GGML_TYPE_F32     = 0,
    GGML_TYPE_F16     = 1,
    GGML_TYPE_Q4_0    = 2,
    GGML_TYPE_Q4_1    = 3,
    GGML_TYPE_Q5_0    = 6,
    GGML_TYPE_Q5_1    = 7,
    GGML_TYPE_Q8_0    = 8,
    GGML_TYPE_Q8_1    = 9,
} GGMLType;

/* GGML type block size info */
static int ggml_type_block_size(uint32_t type, uint64_t *block_sz, uint64_t *weights_per_block)
{
    switch (type) {
        case GGML_TYPE_F32:  *block_sz=4;  *weights_per_block=1;  return 0;
        case GGML_TYPE_F16:  *block_sz=2;  *weights_per_block=1;  return 0;
        case GGML_TYPE_Q8_0: *block_sz=34; *weights_per_block=32; return 0;
        case GGML_TYPE_Q4_0: *block_sz=18; *weights_per_block=32; return 0;
        case GGML_TYPE_Q8_1: *block_sz=34; *weights_per_block=32; return 0;
        default: return -1;
    }
}

/* Tensor info — uses fixed dims[4] like engine demo */
typedef struct {
    char      name[256];
    uint32_t  n_dims;
    uint64_t  dims[4];
    uint32_t  type;
    uint64_t  offset;
    uint64_t  size_bytes;
    uint64_t  n_weights;
} GGUF_Tensor;

/* GGUF file handle — uses uint64_t for tensor_data_start (not long!) */
typedef struct {
    FILE          *fp;
    uint32_t       version;
    uint64_t       tensor_count;
    uint64_t       kv_count;
    GGUF_Tensor   *tensors;
    uint64_t       tensor_data_start;
} GGUF_File;

/* ── Read a GGUF string (malloc-based, like engine demo) ── */
static int read_gguf_str_fp(FILE *fp, GGUFFieldStr *s)
{
    if (fread(&s->len, sizeof(s->len), 1, fp) != 1) return -1;
    s->data = (char*)malloc((size_t)(s->len + 1));
    if (!s->data) return -1;
    if (s->len > 0) {
        if (fread(s->data, 1, (size_t)s->len, fp) != (size_t)s->len) {
            free(s->data); s->data = NULL; return -1;
        }
    }
    s->data[s->len] = '\0';
    return 0;
}

/* ── Skip a GGUF value (recursive) ── */
static int skip_gguf_value(FILE *fp, uint32_t type)
{
    switch (type) {
        case 0: { uint8_t v; return fread(&v,1,1,fp) ? 0 : -1; }
        case 1: { int8_t v;  return fread(&v,1,1,fp) ? 0 : -1; }
        case 2: { uint16_t v; return fread(&v,2,1,fp) ? 0 : -1; }
        case 3: { int16_t v; return fread(&v,2,1,fp) ? 0 : -1; }
        case 4: { uint32_t v; return fread(&v,4,1,fp) ? 0 : -1; }
        case 5: { int32_t v; return fread(&v,4,1,fp) ? 0 : -1; }
        case 6: { float v; return fread(&v,4,1,fp) ? 0 : -1; }
        case 7: { uint8_t v; return fread(&v,1,1,fp) ? 0 : -1; }
        case 8: { GGUFFieldStr s; int r=read_gguf_str_fp(fp,&s); free(s.data); return r; }
        case 9: {
            uint32_t arr_type;
            uint64_t arr_len;
            if (fread(&arr_type,4,1,fp)!=1) return -1;
            if (fread(&arr_len,8,1,fp)!=1) return -1;
            for (uint64_t i=0;i<arr_len;i++)
                if (skip_gguf_value(fp,arr_type)) return -1;
            return 0;
        }
        case 10: { uint64_t v; return fread(&v,8,1,fp) ? 0 : -1; }
        case 11: { int64_t v;  return fread(&v,8,1,fp) ? 0 : -1; }
        case 12: { double v;   return fread(&v,8,1,fp) ? 0 : -1; }
        default: return -1;
    }
}

/* ── Open GGUF file (EXACT copy of engine demo's proven implementation) ── */
static GGUF_File *gguf_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    GGUF_File *gf = (GGUF_File*)calloc(1, sizeof(GGUF_File));
    if (!gf) { fclose(fp); return NULL; }
    gf->fp = fp;

    uint32_t magic;
    if (fread(&magic,4,1,fp)!=1 || magic!=GGUF_MAGIC) {
        fclose(fp); free(gf); return NULL;
    }
    if (fread(&gf->version,4,1,fp)!=1) goto fail;
    if (fread(&gf->tensor_count,8,1,fp)!=1) goto fail;
    if (fread(&gf->kv_count,8,1,fp)!=1) goto fail;

    /* Skip metadata KV pairs */
    for (uint64_t i=0;i<gf->kv_count;i++) {
        GGUFFieldStr key;
        if (read_gguf_str_fp(fp,&key)!=0) goto fail;
        uint32_t val_type;
        if (fread(&val_type,4,1,fp)!=1) { free(key.data); goto fail; }
        if (skip_gguf_value(fp,val_type)!=0) { free(key.data); goto fail; }
        free(key.data);
    }

    /* Read tensor info */
    gf->tensors = (GGUF_Tensor*)calloc((size_t)gf->tensor_count, sizeof(GGUF_Tensor));
    if (!gf->tensors) goto fail;

    for (uint64_t i=0;i<gf->tensor_count;i++) {
        GGUFFieldStr name;
        if (read_gguf_str_fp(fp,&name)!=0) goto fail;
        strncpy(gf->tensors[i].name, name.data, 255);
        free(name.data);

        if (fread(&gf->tensors[i].n_dims,4,1,fp)!=1) goto fail;
        if (gf->tensors[i].n_dims>4) gf->tensors[i].n_dims=4;
        for (uint32_t d=0;d<gf->tensors[i].n_dims;d++)
            if (fread(&gf->tensors[i].dims[d],8,1,fp)!=1) goto fail;
        if (fread(&gf->tensors[i].type,4,1,fp)!=1) goto fail;
        if (fread(&gf->tensors[i].offset,8,1,fp)!=1) goto fail;

        /* Compute size */
        uint64_t n_weights = 1;
        for (uint32_t d=0;d<gf->tensors[i].n_dims;d++)
            n_weights *= gf->tensors[i].dims[d];
        gf->tensors[i].n_weights = n_weights;

        uint64_t block_sz=0, wpb=1;
        if (ggml_type_block_size(gf->tensors[i].type, &block_sz, &wpb)!=0)
            gf->tensors[i].size_bytes = n_weights;
        else {
            uint64_t n_blocks = (n_weights + wpb - 1) / wpb;
            gf->tensors[i].size_bytes = n_blocks * block_sz;
        }
    }

    /* Tensor data starts after all tensor info */
    gf->tensor_data_start = ftell(fp);

    return gf;

fail:
    fclose(fp); free(gf->tensors); free(gf);
    return NULL;
}

static void gguf_close(GGUF_File *gf)
{
    if (!gf) return;
    if (gf->fp) fclose(gf->fp);
    free(gf->tensors);
    free(gf);
}

/* Find tensor by name substring */
static int gguf_find_tensor(GGUF_File *gf, const char *substr)
{
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (strstr(gf->tensors[i].name, substr))
            return (int)i;
    return -1;
}
