/* tools/tesspack_server.c — .tesspack HTTP inference server (OpenAI-compatible)
 * ═══════════════════════════════════════════════════════════════════════════
 * Minimal HTTP server that loads a model from .tesspack via mmap-only mode,
 * serves /v1/chat/completions and /v1/completions using the llama.cpp API.
 *
 * No source GGUF required. The .tesspack must contain an __gguf_header__
 * entry (created by tess_gguf_pack).
 *
 * BUILD: make tess-server
 * RUN:   ./build/tesspack_server <tesspack> [port] [dll_dir]
 *        env TESS_PORT  = port (default 8080)
 *        env TESS_NGPU  = GPU layers (default 0)
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <psapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close(s) close(s)
#endif
#include "llama.h"
#include "ggml-backend.h"
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

/* ── per-tensor hook (shared with bridge) ── */
typedef struct {
    TESS_PackIndex *pi;
    uint32_t n_pack, n_zero, errors;
    uint64_t b_pack;
    double   ms_pack;
} TensorHook;

static uint32_t cell_size_of(enum ggml_type type) {
    static const uint32_t C[16] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return C[idx];
}

static double now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static double rss_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    long pages = 0;
    if (fscanf(f, "%ld", &pages) != 1) { fclose(f); return 0.0; }
    fclose(f);
    return (double)pages * 4096.0 / (1024.0 * 1024.0);
#endif
}

/* ── scatter-decode from pack (mmap-only) ── */
static int load_pack_tensor(TensorHook *h, const char *name, uint32_t cell_size,
                            uint64_t total_cells, uint8_t *dst) {
    TESS_PackIndex *pi = h->pi;
    uint32_t capo_count = 0;
    {
        const uint8_t *cur = pi->base + pi->index_offset;
        const uint8_t *end = pi->base + pi->file_sz;
        uint32_t nlen = (uint32_t)strlen(name);
        for (uint32_t i = 0; i < pi->n_capos; i++) {
            if (cur + 1 > end) break;
            uint8_t nl = *cur++;
            if (cur + nl + 16 > end) break;
            const uint8_t *np = cur;
            cur += nl;
            uint32_t cid = *(const uint32_t *)cur;
            cur += 16;
            if (nl == (uint8_t)nlen && memcmp(np, name, nlen) == 0) {
                if (cid + 1 > capo_count) capo_count = cid + 1;
            }
        }
    }
    if (capo_count == 0) return -1;
    uint64_t cells_left = total_cells;
    for (uint32_t c = 0; c < capo_count && cells_left > 0; c++) {
        TESS_CapoReader cr;
        if (tess_pack_get_capo_mmap(pi, &cr, name, c) != 0) return -2;
        uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                       ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
        uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * cell_size;
        uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
        if (got != cells * cell_size) return -3;
        cells_left -= cells;
    }
    return (cells_left == 0) ? (int)capo_count : -4;
}

static void provide_tensor(struct ggml_tensor *t, void *ud) {
    TensorHook *h = (TensorHook *)ud;
    const char *name = ggml_get_name(t);
    size_t need = (size_t)ggml_nbytes(t);
    uint8_t *dst = (uint8_t *)t->data;

    if (!name || name[0] == '\0' || need == 0) {
        if (dst && need) memset(dst, 0, need);
        h->n_zero++;
        return;
    }

    uint32_t csz = cell_size_of(t->type);
    uint64_t total_cells = (csz == 0) ? 0
        : (uint64_t)ggml_nelements(t) / ggml_blck_size(t->type);
    if (csz != 0 && total_cells * csz == need) {
        double t0 = now_ms();
        int rc = load_pack_tensor(h, name, csz, total_cells, dst);
        h->ms_pack += now_ms() - t0;
        if (rc > 0) { h->n_pack++; h->b_pack += need; return; }
        if (rc != -1)
            fprintf(stderr, "  [server] PACK-FAIL %s rc=%d\n", name, rc);
    }

    memset(dst, 0, need);

    size_t nm_len = strlen(name);
    if ((nm_len >= 6 && strcmp(name + nm_len - 6, ".scale") == 0) ||
        (nm_len >= 13 && strcmp(name + nm_len - 13, ".input_scale") == 0)) {
        for (size_t k = 0; k + 4 <= need; k += 4)
            *(float *)(dst + k) = 1.0f;
    }

    h->n_zero++;
}

/* ── llama CPU backend loader ── */
static void register_cpu_backend(const char *dir) {
    static const char *cpu_plugins[] = {
        "ggml-cpu-zen4.dll", "ggml-cpu-alderlake.dll",
        "ggml-cpu-icelake.dll", "ggml-cpu-sapphirerapids.dll",
        "ggml-cpu-haswell.dll", "ggml-cpu-cannonlake.dll",
        "ggml-cpu-cooperlake.dll", "ggml-cpu-skylakex.dll",
        "ggml-cpu-cascadelake.dll", "ggml-cpu-piledriver.dll",
        "ggml-cpu-ivybridge.dll", "ggml-cpu-sandybridge.dll",
        "ggml-cpu-sse42.dll", NULL
    };
    char dll_path[MAX_PATH];
    int loaded = 0;
    for (int i = 0; cpu_plugins[i]; i++) {
        snprintf(dll_path, sizeof(dll_path), "%s\\%s", dir, cpu_plugins[i]);
        if (ggml_backend_load(dll_path)) { loaded = 1; fprintf(stderr, "  loaded %s\n", cpu_plugins[i]); break; }
    }
    if (!loaded) {
        fprintf(stderr, "  (CPU-only load failed, falling back to load-all)\n");
        ggml_backend_load_all_from_path(dir);
    }
}

/* ── simple JSON string escaping for HTTP responses ── */
static void json_escape(char *dst, size_t cap, const char *src) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 6 < cap; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[di++] = '\\'; dst[di++] = c; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else { dst[di++] = c; }
    }
    dst[di] = '\0';
}

/* ── inference ── */
typedef struct {
    char *text;
    int   n_tokens;
    double prompt_ms, gen_ms;
} InferenceResult;

static int run_inference(struct llama_model *model, struct llama_context *ctx,
                         const char *prompt, int n_gen, InferenceResult *res) {
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(42));

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    llama_token tokens[4096];
    int n_tokens = 0;

    n_tokens = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                              tokens, sizeof(tokens)/sizeof(tokens[0]), true, true);
    if (n_tokens <= 0) return -1;

    float t0 = (float)now_ms();
    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    for (int i = 0; i < n_tokens; i++) {
        batch.token[batch.n_tokens] = tokens[i];
        batch.pos[batch.n_tokens] = i;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id[batch.n_tokens][0] = 0;
        batch.logits[batch.n_tokens] = (i == n_tokens - 1) ? 1 : 0;
        batch.n_tokens++;
    }
    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch); return -2;
    }
    float prompt_end = (float)now_ms();
    res->prompt_ms = prompt_end - t0;

    size_t buf_cap = 8192;
    char *buf = (char *)malloc(buf_cap);
    size_t buf_len = 0;
    buf[0] = '\0';

    llama_token cur = llama_sampler_sample(smpl, ctx, -1);
    for (int g = 0; g < n_gen; g++) {
        if (cur == llama_vocab_eos(vocab)) break;
        char piece[256];
        int n = llama_token_to_piece(vocab, cur, piece, sizeof(piece) - 1, 0, true);
        if (n > 0) {
            piece[n] = '\0';
            if (buf_len + n + 1 > buf_cap) {
                buf_cap *= 2;
                buf = (char *)realloc(buf, buf_cap);
            }
            memcpy(buf + buf_len, piece, n);
            buf_len += n;
            buf[buf_len] = '\0';
        }
        llama_batch batch2 = llama_batch_init(1, 0, 1);
        batch2.token[0] = cur;
        batch2.pos[0] = n_tokens + g;
        batch2.n_seq_id[0] = 1;
        batch2.seq_id[0][0] = 0;
        batch2.logits[0] = 1;
        batch2.n_tokens = 1;
        llama_decode(ctx, batch2);
        llama_batch_free(batch2);
        cur = llama_sampler_sample(smpl, ctx, -1);
    }
    res->gen_ms = (float)now_ms() - prompt_end;
    res->text = buf;
    res->n_tokens = n_gen;

    llama_sampler_free(smpl);
    llama_batch_free(batch);
    return 0;
}

/* ── minimal HTTP server ── */
static int http_recv_full(sock_t fd, char *buf, int cap) {
    int total = 0;
    while (total < cap - 1) {
        int n = recv(fd, buf + total, cap - total - 1, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

static void http_send(sock_t fd, const char *status, const char *ctype,
                      const char *body, int blen) {
    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", status, ctype, blen);
    send(fd, hdr, hlen, 0);
    if (blen > 0 && body) send(fd, body, blen, 0);
}

static char *json_get_string(const char *json, const char *key, char *val, int cap) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < cap - 1) { val[i++] = *p++; }
        val[i] = '\0';
    } else {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && i < cap - 1) { val[i++] = *p++; }
        val[i] = '\0';
    }
    return val;
}

static int json_get_int(const char *json, const char *key, int def) {
    char val[32];
    if (!json_get_string(json, key, val, sizeof(val))) return def;
    return atoi(val);
}

static char *extract_messages_content(const char *json, char *buf, int cap) {
    /* Try to find "content" field in the messages array */
    const char *p = strstr(json, "\"content\"");
    if (!p) {
        /* fallback: try "prompt" field */
        p = strstr(json, "\"prompt\"");
        if (!p) return NULL;
        p = strchr(p + 8, ':');
        if (!p) return NULL;
    } else {
        p = strchr(p + 9, ':');
        if (!p) return NULL;
    }
    p++;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < cap - 1) {
            if (*p == '\\' && p[1]) { p++; }
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        return buf;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *pack_path = (argc > 1) ? argv[1] : NULL;
    int port = (argc > 2) ? atoi(argv[2]) : 0;
    const char *dll_dir   = (argc > 3) ? argv[3] : "I:\\llama\\llama-v040-bin-win-vulkan-x64";
    if (!port) { const char *ep = getenv("TESS_PORT"); port = ep ? atoi(ep) : 8080; }
    if (port < 1 || port > 65535) port = 8080;
    int n_gpu = 0;
    { const char *eg = getenv("TESS_NGPU"); if (eg) n_gpu = atoi(eg); }
    if (!pack_path) { fprintf(stderr, "Usage: tesspack_server <tesspack> [port] [dll_dir]\n"); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    SetDllDirectoryA(dll_dir);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    llama_backend_init();
    register_cpu_backend(dll_dir);

    /* ── open tesspack ── */
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, pack_path) != 0) {
        fprintf(stderr, "FAIL: tesspack open %s\n", pack_path); return 1;
    }
    fprintf(stderr, "Pack: %s (%u capos, %.1f MB)\n",
            pack_path, pi.n_capos, (double)pi.file_sz / 1e6);

    /* ── extract embedded GGUF header → sparse temp file ── */
    uint64_t hdr_sz = 0;
    const uint8_t *hdr_bytes = tess_pack_get_gguf_header(&pi, &hdr_sz);
    if (!hdr_bytes || hdr_sz == 0) {
        fprintf(stderr, "FAIL: no embedded GGUF header in pack\n");
        tess_pack_close(&pi); return 1;
    }
    fprintf(stderr, "Embedded header: %llu bytes\n", (unsigned long long)hdr_sz);

    char tmp_gguf[MAX_PATH];
    snprintf(tmp_gguf, sizeof(tmp_gguf), "%s\\tesspack_server_hdr.tmp",
             getenv("TEMP") ? getenv("TEMP") : ".");
    FILE *tf = fopen(tmp_gguf, "wb");
    if (!tf) { fprintf(stderr, "FAIL: tmp create\n"); tess_pack_close(&pi); return 1; }
    fwrite(hdr_bytes, 1, (size_t)hdr_sz, tf);
    uint64_t pad_to = hdr_sz + (1024ULL * 1024 * 1024);
    _fseeki64(tf, (long)(pad_to - 1), SEEK_SET);
    fputc(0, tf);
    fclose(tf);

    struct gguf_init_params gip = { /*.no_alloc =*/ true, /*.ctx =*/ NULL };
    struct gguf_context *meta = gguf_init_from_file(tmp_gguf, gip);
    if (!meta) {
        fprintf(stderr, "FAIL: gguf_init_from_file on sparse tmp\n");
        remove(tmp_gguf); tess_pack_close(&pi); return 1;
    }

    /* ── load model via callback ── */
    TensorHook hook;
    memset(&hook, 0, sizeof(hook));
    hook.pi = &pi;

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = n_gpu;
    static struct llama_model_tensor_buft_override ovr[2];
    ovr[0].pattern = ".*";
    ovr[0].buft = ggml_backend_cpu_buffer_type();
    ovr[1].pattern = NULL;
    if (ovr[0].buft) mp.tensor_buft_overrides = ovr;

    double t0 = now_ms();
    struct llama_model *model = llama_model_init_from_user(meta, provide_tensor,
                                                          &hook, mp);
    double load_ms = now_ms() - t0;
    if (!model) {
        fprintf(stderr, "FAIL: model init (pack=%u zero=%u errors=%u)\n",
                hook.n_pack, hook.n_zero, hook.errors);
        gguf_free(meta); remove(tmp_gguf); tess_pack_close(&pi); return 1;
    }
    fprintf(stderr, "Model loaded in %.0f ms (pack=%u tensors, %.1f MB, %.1f ms)\n",
            load_ms, hook.n_pack, hook.b_pack / 1e6, hook.ms_pack);
    fprintf(stderr, "RSS: %.1f MB\n", rss_mb());

    /* ── create context for inference ── */
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_batch = 128;
    cparams.n_ctx   = 256;
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "FAIL: llama_init_from_model\n");
        llama_model_free(model); gguf_free(meta);
        remove(tmp_gguf); tess_pack_close(&pi); return 1;
    }

    /* ── start HTTP server ── */
    sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == SOCK_INVALID) { fprintf(stderr, "FAIL: socket\n"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "FAIL: bind port %d\n", port); return 1;
    }
    listen(server_fd, 8);
    fprintf(stderr, "tesspack_server listening on http://localhost:%d\n", port);
    fprintf(stderr, "  POST /v1/chat/completions  (OpenAI-compatible)\n");
    fprintf(stderr, "  POST /v1/completions       (text completion)\n");
    fprintf(stderr, "  GET  /health               (health check)\n");

    /* ── serve forever ── */
    while (1) {
        sock_t client = accept(server_fd, NULL, NULL);
        if (client == SOCK_INVALID) continue;

        char req_buf[65536];
        int nread = http_recv_full(client, req_buf, sizeof(req_buf));
        if (nread <= 0) { sock_close(client); continue; }
        req_buf[nread] = '\0';

        /* parse method + path */
        char method[16] = "", path[256] = "";
        sscanf(req_buf, "%15s %255s", method, path);

        /* health check */
        if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
            http_send(client, "200 OK", "application/json",
                      "{\"status\":\"ok\"}", 14);
            sock_close(client); continue;
        }

        /* CORS preflight */
        if (strcmp(method, "OPTIONS") == 0) {
            http_send(client, "204 No Content",
                      "text/plain", "", 0);
            sock_close(client); continue;
        }

        /* completion endpoints */
        int is_chat = (strstr(path, "chat/completions") != NULL);
        int is_comp = (strstr(path, "/completions") != NULL && !is_chat);
        if (strcmp(method, "POST") != 0 || (!is_chat && !is_comp)) {
            http_send(client, "404 Not Found", "application/json",
                      "{\"error\":\"not found\"}", 22);
            sock_close(client); continue;
        }

        /* find body (after \r\n\r\n) */
        char *body = strstr(req_buf, "\r\n\r\n");
        if (!body) { sock_close(client); continue; }
        body += 4;

        /* extract prompt */
        char prompt[4096] = "";
        if (!extract_messages_content(body, prompt, sizeof(prompt))) {
            http_send(client, "400 Bad Request", "application/json",
                      "{\"error\":\"no prompt/content\"}", 28);
            sock_close(client); continue;
        }
        int n_gen = json_get_int(body, "max_tokens", 128);
        if (n_gen < 1) n_gen = 1;
        if (n_gen > 2048) n_gen = 2048;

        fprintf(stderr, "REQUEST: prompt=%d chars, max_tokens=%d\n",
                (int)strlen(prompt), n_gen);

        /* run inference */
        llama_memory_clear(llama_get_memory(ctx), false);
        InferenceResult ires;
        memset(&ires, 0, sizeof(ires));
        int rc = run_inference(model, ctx, prompt, n_gen, &ires);
        if (rc != 0) {
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"inference failed rc=%d\"}", rc);
            http_send(client, "500 Internal Server Error", "application/json",
                      err, (int)strlen(err));
            sock_close(client); continue;
        }
        fprintf(stderr, "  prompt eval %.0f ms | gen %d tok in %.0f ms\n",
                ires.prompt_ms, ires.n_tokens, ires.gen_ms);

        /* build JSON response */
        char escaped[8192];
        json_escape(escaped, sizeof(escaped), ires.text ? ires.text : "");

        char resp[16384];
        int rlen;
        if (is_chat) {
            rlen = snprintf(resp, sizeof(resp),
                "{\"id\":\"tess-%llu\",\"object\":\"chat.completion\","
                "\"created\":%lld,\"model\":\"tesspack\","
                "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                "\"content\":\"%s\"},\"finish_reason\":\"stop\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                (unsigned long long)time(NULL),
                (long long)time(NULL), escaped,
                (int)strlen(prompt) / 4, ires.n_tokens,
                (int)strlen(prompt) / 4 + ires.n_tokens);
        } else {
            rlen = snprintf(resp, sizeof(resp),
                "{\"id\":\"tess-%llu\",\"object\":\"text_completion\","
                "\"created\":%lld,\"model\":\"tesspack\","
                "\"choices\":[{\"text\":\"%s\",\"index\":0,"
                "\"finish_reason\":\"stop\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
                "\"total_tokens\":%d}}",
                (unsigned long long)time(NULL),
                (long long)time(NULL), escaped,
                (int)strlen(prompt) / 4, ires.n_tokens,
                (int)strlen(prompt) / 4 + ires.n_tokens);
        }
        http_send(client, "200 OK", "application/json", resp, rlen);

        free(ires.text);
        sock_close(client);
    }

    llama_free(ctx);
    llama_model_free(model);
    gguf_free(meta);
    remove(tmp_gguf);
    tess_pack_close(&pi);
    return 0;
}
