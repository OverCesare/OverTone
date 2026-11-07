#include "connection.h"
#include "runtime.h"
#include "utf8.h"
#include "yyjson.h"
#include <stdio.h>
#include <string.h>

static const char *text(yyjson_val *v, const char *key) {
    yyjson_val *field = yyjson_obj_get(v, key);
    const char *s = yyjson_get_str(field);
    return s && strlen(s) == yyjson_get_len(field) ? s : "";
}
static bool identifier(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (!n || n > 256 || !ot_utf8_valid(s, n)) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return false;
    return true;
}
static unsigned positive(yyjson_val *v) {
    uint64_t n = yyjson_get_uint(v);
    return n && n <= 16777216 ? (unsigned)n : 0;
}
char *ot_server_models(const char *json, bool ollama) {
    if (!json || strlen(json) > 1048576) return NULL;
    yyjson_doc *source = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_obj_get(yyjson_doc_get_root(source), ollama ? "models" : "data");
    if (!yyjson_is_arr(rows)) { yyjson_doc_free(source); return NULL; }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) { yyjson_doc_free(source); return NULL; }
    yyjson_mut_val *out = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, out);
    size_t i, count;
    yyjson_val *row;
    yyjson_arr_foreach(rows, i, count, row) {
        const char *id = text(row, ollama ? "name" : "id");
        if (yyjson_mut_arr_size(out) >= 256) break;
        if (ollama && !*id) id = text(row, "model");
        if (!identifier(id)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < yyjson_mut_arr_size(out); j++)
            if (!strcmp(id, yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(out, j), "id"))))
                duplicate = true;
        if (duplicate) continue;
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, item, "id", id);
        char title[257], quant[33];
        ot_model_label(id, "", title, quant);
        char display[294];
        snprintf(display, sizeof display, "%s%s%s", title, *quant ? " · " : "", quant);
        yyjson_mut_obj_add_strcpy(d, item, "name", display);
        yyjson_mut_obj_add_strcpy(d, item, "displayName", title);
        yyjson_mut_obj_add_strcpy(d, item, "quantization", quant);
        yyjson_mut_obj_add_uint(d, item, "bytes", yyjson_get_uint(yyjson_obj_get(row, "size")));
        unsigned context = positive(yyjson_obj_get(row, "context_length"));
        if (!context) context = positive(yyjson_obj_get(row, "context_window"));
        if (!context) context = positive(yyjson_obj_get(row, "max_model_len"));
        if (!context) context = positive(yyjson_obj_get(yyjson_obj_get(row, "meta"), "n_ctx_train"));
        yyjson_mut_obj_add_uint(d, item, "context", context);
        yyjson_mut_arr_append(out, item);
    }
    char *result = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d); yyjson_doc_free(source);
    return result;
}
ot_server_result ot_server_metadata(const char *json, bool ollama, const char *id, ot_server_info *result) {
    if (!json || !result || strlen(json) > 1048576 || !identifier(id)) return OT_SERVER_INVALID;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *v = yyjson_doc_get_root(d);
    if (!yyjson_is_obj(v)) { yyjson_doc_free(d); return OT_SERVER_INVALID; }
    ot_server_info info = {.verified = true};
    snprintf(info.model.name, sizeof info.model.name, "%s", id);
    info.thinking = ot_thinking_detect(v, ollama);
    if (ollama) {
        yyjson_val *caps = yyjson_obj_get(v, "capabilities"), *cap;
        size_t i, n;
        bool chat = !yyjson_is_arr(caps);
        yyjson_arr_foreach(caps, i, n, cap)
            if (yyjson_is_str(cap) && !strcmp(yyjson_get_str(cap), "completion")) chat = true;
        if (!chat) { yyjson_doc_free(d); return OT_SERVER_NOT_CHAT; }
        yyjson_val *model = yyjson_obj_get(v, "model_info");
        const char *architecture = text(model, "general.architecture");
        if (strlen(architecture) <= 64)
            snprintf(info.model.architecture, sizeof info.model.architecture, "%s", architecture);
        const char *keys[] = {"context_length", "embedding_length", "block_count", "attention.head_count",
                              "attention.head_count_kv", "attention.key_length", "attention.value_length",
                              "expert_count", "expert_used_count"};
        unsigned *fields[] = {&info.model.context, &info.model.embedding, &info.model.layers, &info.model.heads,
                               &info.model.kv_heads, &info.model.key_length, &info.model.value_length,
                               &info.model.experts, &info.model.experts_used};
        for (unsigned j = 0; j < sizeof keys / sizeof *keys; j++) {
            char key[128]; snprintf(key, sizeof key, "%.64s.%s", architecture, keys[j]);
            *fields[j] = positive(yyjson_obj_get(model, key));
        }
        const char *quant = text(yyjson_obj_get(v, "details"), "quantization_level");
        if (strlen(quant) < sizeof info.model.quantization)
            snprintf(info.model.quantization, sizeof info.model.quantization, "%s", quant);
    } else {
        const char *keys[] = {"context_length", "max_model_len", "max_context_length", "context_window"};
        for (unsigned i = 0; i < sizeof keys / sizeof *keys && !info.model.context; i++)
            info.model.context = positive(yyjson_obj_get(v, keys[i]));
        yyjson_val *defaults = yyjson_obj_get(v, "default_generation_settings");
        unsigned loaded = positive(yyjson_obj_get(defaults, "n_ctx"));
        if (loaded) info.loaded_context = info.model.context = loaded;
        yyjson_val *sleeping = yyjson_obj_get(v, "is_sleeping");
        if (yyjson_is_bool(sleeping)) {
            info.resident_known = true; info.resident = !yyjson_get_bool(sleeping);
        }
    }
    *result = info;
    yyjson_doc_free(d);
    return OT_SERVER_OK;
}
void ot_server_residency(const char *json, const char *model, ot_server_info *info) {
    if (!json || !identifier(model) || !info || strlen(json) > 1048576) return;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_obj_get(yyjson_doc_get_root(d), "models"), *row;
    bool valid = yyjson_is_arr(rows), resident = false;
    unsigned context = 0;
    size_t i, n;
    yyjson_arr_foreach(rows, i, n, row) {
        const char *name = text(row, "name"), *alias = text(row, "model");
        if ((!identifier(name) && !identifier(alias)) ||
            (yyjson_obj_get(row, "name") && !identifier(name)) ||
            (yyjson_obj_get(row, "model") && !identifier(alias))) { valid = false; break; }
        if (!ot_ollama_model_equal(name, model) && !ot_ollama_model_equal(alias, model)) continue;
        if (resident) { valid = false; break; }
        resident = true;
        context = positive(yyjson_obj_get(row, "context_length"));
    }
    if (valid) {
        info->resident_known = true; info->resident = resident; info->loaded_context = context;
    }
    yyjson_doc_free(d);
}
