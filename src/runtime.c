#include "runtime.h"
#include "yyjson.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static bool number(const char **text, unsigned *value) {
    const char *s = *text;
    if (*s < '0' || *s > '9')
        return false;
    unsigned n = 0;
    while (*s >= '0' && *s <= '9') {
        unsigned digit = (unsigned)(*s++ - '0');
        if (n > (UINT32_MAX - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *text = s;
    *value = n;
    return true;
}
static const char *message(const char *line) {
    const char *s = line;
    unsigned value;
    for (unsigned i = 0; i < 4; i++) {
        if (!number(&s, &value))
            return line;
        if (i && *s == ' ' && s[1] && strchr("IDWET", s[1]) && s[2] == ' ')
            return s + 3;
        if (*s++ != '.')
            return line;
    }
    return line;
}
bool ot_runtime_line(ot_runtime_info *info, const char *line) {
    line = message(line);
    const char *s = strstr(line, "load_tensors: offloaded ");
    if (s) {
        s += strlen("load_tensors: offloaded ");
        unsigned gpu, total;
        if (!number(&s, &gpu) || *s++ != '/' || !number(&s, &total) || strcmp(s, " layers to GPU") ||
            !total || total > 100000 || gpu > total)
            return false;
        info->gpu_layers = gpu;
        info->total_layers = total;
        info->kind = !gpu ? OT_RUNTIME_CPU : gpu == total ? OT_RUNTIME_GPU : OT_RUNTIME_HYBRID;
        return true;
    }
    s = strstr(line, "llama_context: ");
    if (!s)
        return false;
    s += strlen("llama_context: ");
    const char *keys[] = {"n_ctx", "n_batch", "n_ubatch"};
    unsigned *outputs[] = {&info->context, &info->batch, &info->micro_batch};
    for (unsigned i = 0; i < 3; i++) {
        size_t length = strlen(keys[i]);
        if (strncmp(s, keys[i], length) || s[length] != ' ')
            continue;
        s += length;
        while (*s == ' ')
            s++;
        if (*s++ != '=')
            return false;
        while (*s == ' ')
            s++;
        unsigned value;
        if (!number(&s, &value) || *s || !value)
            return false;
        *outputs[i] = value;
        return true;
    }
    return false;
}
bool ot_ollama_model_equal(const char *value, const char *model) {
    if (!value || !*value || !model || !*model)
        return false;
    size_t a = strlen(value), b = strlen(model);
    if (a > 7 && !strcasecmp(value + a - 7, ":latest"))
        a -= 7;
    if (b > 7 && !strcasecmp(model + b - 7, ":latest"))
        b -= 7;
    return a == b && !strncasecmp(value, model, a);
}
static bool matches(yyjson_val *row, const char *key, const char *model) {
    yyjson_val *field = yyjson_obj_get(row, key);
    const char *value = yyjson_get_str(field);
    return value && yyjson_get_len(field) == strlen(value) && ot_ollama_model_equal(value, model);
}
bool ot_runtime_buffer_line(ot_runtime_buffers *b, ot_runtime_info *info, const char *line) {
    line = message(line);
    if (strstr(line, "load_tensors: loading model tensors")) {
        *b = (ot_runtime_buffers){0};
        *info = (ot_runtime_info){0};
        return true;
    }
    if (strstr(line, "llama_kv_cache_iswa: creating")) {
        b->window = strstr(line, "non-SWA") == NULL;
        if (b->window) {
            const char *size = strstr(line, "size = ");
            unsigned cells = 0;
            if (size)
                size += strlen("size = ");
            if (!size || !number(&size, &cells) || strcmp(size, " cells"))
                info->window_cells = UINT32_MAX;
            else if (cells > info->window_cells)
                info->window_cells = cells;
        }
        if (b->bank < 16)
            b->bank++;
        else
            b->limited = true;
        return false;
    }
    char component[64], backend[64], kind[16];
    double mib;
    int end = 0;
    if (sscanf(line, "%63[^:]: %63s %15s buffer size = %lf MiB%n", component, backend, kind, &mib, &end) !=
            4 ||
        !end || line[end] || !isfinite(mib) || mib < 0 || mib >= 0x1p44)
        return false;
    for (const char *p = component; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return false;
    if (strcmp(kind, "model") && strcmp(kind, "KV") && strcmp(kind, "compute") && strcmp(kind, "output") &&
        strcmp(kind, "RS"))
        return false;
    unsigned bank = !strcmp(kind, "KV") ? b->bank : 0, index = 0;
    while (index < b->count &&
           (strcmp(b->rows[index].component, component) || strcmp(b->rows[index].backend, backend) ||
            strcmp(b->rows[index].kind, kind) || b->rows[index].bank != bank))
        index++;
    if (index == 32)
        b->limited = true;
    if (b->limited)
        return false;
    ot_runtime_buffers *destination = b, next = *b;
    b = &next;
    if (index == b->count)
        b->count++;
    strcpy(b->rows[index].component, component);
    strcpy(b->rows[index].backend, backend);
    strcpy(b->rows[index].kind, kind);
    b->rows[index].bank = bank;
    b->rows[index].window = bank && b->window;
    uint64_t allocation_bytes = (uint64_t)(mib * 1048576);
    /* A mapped backend may split weights across several allocations. These are
     * additive within one load; the loading-model event resets the whole set.
     * KV/compute reports instead replace a resized reservation. */
    if (!strcmp(kind, "model")) {
        if (allocation_bytes > UINT64_MAX - b->rows[index].bytes)
            return false;
        allocation_bytes += b->rows[index].bytes;
    }
    b->rows[index].bytes = allocation_bytes;
    ot_runtime_info result = *info;
    result.allocation = result.gpu_allocation = result.weights = result.cache = result.compute = 0;
    result.file_weights = result.cpu_mapped = 0;
    result.linear_cache = 0;
    for (unsigned i = 0; i < b->count; i++) {
        const char *name = b->rows[i].backend, *type = b->rows[i].kind;
        uint64_t bytes = b->rows[i].bytes;
        if (bytes > UINT64_MAX - result.allocation)
            return false;
        bool gpu =
            strcmp(name, "CPU") && strcmp(name, "BLAS") && strncmp(name, "CPU_", 4) && !strstr(name, "_Host");
        result.allocation += bytes;
        if (gpu)
            result.gpu_allocation += bytes;
        if (!strcmp(type, "model")) {
            result.weights += bytes;
            bool mapped = strstr(name, "_Mapped") != NULL;
            if (gpu || mapped)
                result.file_weights += bytes;
            if (!gpu && mapped)
                result.cpu_mapped += bytes;
        } else if (!strcmp(type, "KV") || !strcmp(type, "RS")) {
            result.cache += bytes;
            if (!strcmp(type, "KV") && !b->rows[i].window)
                result.linear_cache += bytes;
        } else
            result.compute += bytes;
    }
    *destination = next;
    *info = result;
    return true;
}
void ot_runtime_reconcile(ot_runtime_info *info, uint64_t model_bytes) {
    if (model_bytes && info->file_weights > model_bytes) {
        uint64_t overlap = info->file_weights - model_bytes;
        if (overlap > info->cpu_mapped)
            overlap = info->cpu_mapped;
        info->allocation -= overlap;
        info->weights -= overlap;
        info->file_weights -= overlap;
        info->cpu_mapped -= overlap;
    }
}
bool ot_runtime_ollama(ot_runtime_info *info, const char *json, size_t size, const char *model) {
    if (!json || !model || !*model || size > 1048576)
        return false;
    yyjson_doc *d = yyjson_read(json, size, 0);
    yyjson_val *models = yyjson_obj_get(yyjson_doc_get_root(d), "models");
    bool ok = yyjson_is_arr(models) && yyjson_arr_size(models) <= 64, found = false;
    ot_runtime_info result = {.kind = OT_RUNTIME_ASLEEP};
    size_t i, n;
    yyjson_val *row;
    yyjson_arr_foreach(models, i, n, row) {
        if (!ok)
            break;
        yyjson_val *name = yyjson_obj_get(row, "name"), *alias = yyjson_obj_get(row, "model");
        const char *identity = yyjson_get_str(name), *alternative = yyjson_get_str(alias);
        if (!yyjson_is_obj(row) || (!identity && !alternative) || (name && !identity) ||
            (alias && !alternative) ||
            (identity && (!*identity || yyjson_get_len(name) != strlen(identity))) ||
            (alternative && (!*alternative || yyjson_get_len(alias) != strlen(alternative)))) {
            ok = false;
            break;
        }
        if (!matches(row, "name", model) && !matches(row, "model", model))
            continue;
        if (found) {
            ok = false;
            break;
        }
        found = true;
        yyjson_val *bytes = yyjson_obj_get(row, "size"), *gpu = yyjson_obj_get(row, "size_vram"),
                   *context = yyjson_obj_get(row, "context_length");
        ok = yyjson_is_uint(bytes) && yyjson_get_uint(bytes) && yyjson_is_uint(gpu) &&
             yyjson_get_uint(gpu) <= yyjson_get_uint(bytes) &&
             (!context || (yyjson_is_uint(context) && yyjson_get_uint(context) <= UINT32_MAX));
        if (ok) {
            result.allocation = yyjson_get_uint(bytes);
            result.gpu_allocation = yyjson_get_uint(gpu);
            result.context = (unsigned)yyjson_get_uint(context);
            yyjson_val *digest = yyjson_obj_get(row, "digest");
            const char *identity_digest = yyjson_get_str(digest);
            if (identity_digest && yyjson_get_len(digest) < sizeof result.model_digest &&
                strlen(identity_digest) == yyjson_get_len(digest))
                memcpy(result.model_digest, identity_digest, yyjson_get_len(digest) + 1);
            /* Allocation on GPU proves GPU use, not the fraction of computation or layer placement. */
            result.kind = result.gpu_allocation ? OT_RUNTIME_GPU : OT_RUNTIME_CPU;
        }
    }
    yyjson_doc_free(d);
    if (ok) {
        result.other_models = (unsigned)n - (found ? 1u : 0u);
        *info = result;
    }
    return ok;
}
const char *ot_runtime_name(ot_runtime_kind kind) {
    static const char *names[] = {"unknown", "cpu", "gpu", "hybrid", "asleep"};
    return (unsigned)kind < sizeof names / sizeof *names ? names[kind] : names[0];
}
