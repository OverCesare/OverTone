#include "thinking.h"
#include "yyjson.h"
#include <string.h>
bool ot_thinking_name(const char *value) {
    if (!value || !*value || strlen(value) >= OT_THINKING_NAME)
        return false;
    for (const char *p = value; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-'))
            return false;
    return true;
}
const ot_thinking_option *ot_thinking_find(const ot_thinking *thinking, const char *value) {
    for (unsigned i = 0; i < thinking->count && i < OT_THINKING_VALUES; i++)
        if (!strcmp(thinking->options[i].value, value))
            return &thinking->options[i];
    return NULL;
}
static bool add(ot_thinking *thinking, const char *value, ot_thinking_type type) {
    if (!ot_thinking_name(value) || !strcmp(value, "auto") || thinking->count == OT_THINKING_VALUES ||
        ot_thinking_find(thinking, value))
        return false;
    ot_thinking_option *option = &thinking->options[thinking->count++];
    strcpy(option->value, value);
    option->type = type;
    return true;
}
static const char *string(yyjson_val *root, const char *key) {
    yyjson_val *field = yyjson_obj_get(root, key);
    const char *value = yyjson_get_str(field);
    return value && yyjson_get_len(field) == strlen(value) ? value : "";
}
ot_thinking ot_thinking_detect(yyjson_val *metadata, bool ollama) {
    ot_thinking result = {0};
    yyjson_val *thinking = yyjson_obj_get(metadata, "thinking");
    if (ollama && thinking) {
        yyjson_val *values = yyjson_obj_get(thinking, "values"), *value;
        if (!yyjson_is_arr(values) || !yyjson_arr_size(values) ||
            yyjson_arr_size(values) > OT_THINKING_VALUES)
            return result;
        result.known = true;
        size_t i, n;
        yyjson_arr_foreach(values, i, n, value) {
            const char *name =
                yyjson_is_bool(value) ? (yyjson_get_bool(value) ? "on" : "off") : yyjson_get_str(value);
            if (!name || (yyjson_is_str(value) && yyjson_get_len(value) != strlen(name)) ||
                !add(&result, name,
                     yyjson_is_bool(value) ? (yyjson_get_bool(value) ? OT_THINKING_TRUE : OT_THINKING_FALSE)
                                           : OT_THINKING_LEVEL)) {
                result = (ot_thinking){0};
                return result;
            }
        }
        /* Ollama explicitly defines [false] as a model without thinking. */
        if (result.count == 1 && result.options[0].type == OT_THINKING_FALSE)
            result.count = 0;
        return result;
    }
    const char *templ = string(metadata, ollama ? "template" : "chat_template");
    bool toggle = strstr(templ, "enable_thinking") != NULL;
    if (ollama) {
        yyjson_val *caps = yyjson_obj_get(metadata, "capabilities"), *cap;
        if (!yyjson_is_arr(caps) || yyjson_arr_size(caps) > 64)
            return result;
        bool supported = false;
        size_t i, n;
        yyjson_arr_foreach(caps, i, n, cap) {
            const char *value = yyjson_get_str(cap);
            if (!value || !*value || yyjson_get_len(cap) != strlen(value))
                return result;
            supported |= !strcmp(value, "thinking");
        }
        result.known = true;
        if (!supported)
            return result;
        const char *family = string(yyjson_obj_get(metadata, "details"), "family"),
                   *architecture = string(yyjson_obj_get(metadata, "model_info"), "general.architecture");
        toggle |= !strcmp(family, "qwen3") || !strcmp(architecture, "qwen3");
        if (!strcmp(family, "gptoss") || !strcmp(architecture, "gptoss")) {
            add(&result, "low", OT_THINKING_LEVEL);
            add(&result, "medium", OT_THINKING_LEVEL);
            add(&result, "high", OT_THINKING_LEVEL);
            return result;
        }
        add(&result, "on", OT_THINKING_TRUE);
        if (toggle)
            add(&result, "off", OT_THINKING_FALSE);
        return result;
    }
    if (toggle) {
        result.known = true;
        add(&result, "off", OT_THINKING_FALSE);
        add(&result, "on", OT_THINKING_TRUE);
    } else if (strstr(templ, "reasoning_effort")) {
        /* Only recognised template literals, not every level understood by the transport. */
        const char *levels[] = {"low", "medium", "high"};
        const char *single[] = {"'low'", "'medium'", "'high'"};
        const char *double_quote[] = {"\"low\"", "\"medium\"", "\"high\""};
        for (unsigned i = 0; i < 3; i++)
            if (strstr(templ, single[i]) || strstr(templ, double_quote[i]))
                add(&result, levels[i], OT_THINKING_LEVEL);
        result.known = result.count > 0;
    }
    return result;
}
yyjson_mut_val *ot_thinking_json(yyjson_mut_doc *doc, const ot_thinking *thinking) {
    yyjson_mut_val *values = yyjson_mut_arr(doc);
    for (unsigned i = 0; values && i < thinking->count; i++)
        if (!yyjson_mut_arr_add_strcpy(doc, values, thinking->options[i].value))
            return NULL;
    return values;
}
