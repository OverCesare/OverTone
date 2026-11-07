#include "preferences.h"
#include "yyjson.h"
#include <math.h>
#include <string.h>
ot_preferences ot_preferences_default(void) {
    return (ot_preferences){
        .context = 8192,
        .setup_engines = 3,
        .network = OT_NETWORK_ALL,
        .output = 2048,
        .threads = 0,
        .gpu_layers = -1,
        .memory_percent = 75,
        .temperature = .7,
        .top_p = .95,
        .titles = true,
        .summaries = true,
        .thinking = true,
        .tooltips = true,
        .advice_notices = true,
        .advice_learning = true,
        .spellcheck = true,
        .improve = true,
        .improve_context = 4,
        .input_language = "auto",
        .thinking_mode = "auto",
        .keep_alive = "5m",
        .profile = "recommended",
        .log_level = "info",
        .theme = "system",
        .accent = "teal",
        .language = "it",
        .region = "auto",
        .date_format = "auto",
        .hour_format = "auto",
        .shortcut = "enter",
        .system = "Answer directly; add only useful details. Be concise, clear and technically precise. "
                  "Avoid repetition, unsolicited digressions, emojis and decorative symbols. "
                  "Use readable Markdown and copyable code when useful. State uncertainty honestly."};
}
bool ot_network_allowed(const ot_preferences *p, ot_network_scope scope) {
    unsigned required = (unsigned)scope;
    return !required || (p && p->internet && !(required & ~(unsigned)OT_NETWORK_ALL) &&
                         (p->network & required) == required);
}
void ot_network_enable(ot_preferences *p, unsigned scopes) {
    scopes &= OT_NETWORK_ALL;
    if (!scopes) return;
    p->network = p->internet ? p->network | scopes : scopes;
    p->internet = true;
}
/* Only these values belong to a model. Appearance, writing and startup stay global. */
void ot_preferences_model_values(ot_preferences *target, const ot_preferences *model) {
    target->context = model->context;
    target->input = model->input;
    target->output = model->output;
    target->threads = model->threads;
    target->gpu_layers = model->gpu_layers;
    target->memory_percent = model->memory_percent;
    target->temperature = model->temperature;
    target->top_p = model->top_p;
    target->require_gpu = model->require_gpu;
    memcpy(target->keep_alive, model->keep_alive, sizeof target->keep_alive);
    memcpy(target->profile, model->profile, sizeof target->profile);
}
bool ot_preferences_execution_changed(const ot_preferences *a, const ot_preferences *b) {
    return a->context != b->context || a->threads != b->threads || a->gpu_layers != b->gpu_layers ||
           a->require_gpu != b->require_gpu || a->memory_percent != b->memory_percent ||
           strcmp(a->keep_alive, b->keep_alive) || strcmp(a->profile, b->profile);
}
static bool choice(char *target, size_t capacity, yyjson_val *root, const char *name, const char *choices) {
    yyjson_val *v = yyjson_obj_get(root, name);
    if (!v)
        return true;
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    if (!s || !n || n >= capacity || memchr(s, 0, n))
        return false;
    const char *p = choices;
    while (*p) {
        const char *end = strchr(p, '|');
        size_t size = end ? (size_t)(end - p) : strlen(p);
        if (size == n && !memcmp(p, s, n)) {
            memcpy(target, s, n + 1);
            return true;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}
/* Bounded ASCII identities used by setup and recommendation read state. */
static bool identifier(char *target, size_t capacity, yyjson_val *root, const char *key) {
    yyjson_val *value = yyjson_obj_get(root, key);
    if (!value) return true;
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (!text || length >= capacity || memchr(text, 0, length)) return false;
    for (size_t i = 0; i < length; i++)
        if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
              (text[i] >= '0' && text[i] <= '9') || strchr(".:-_/@", text[i]))) return false;
    memcpy(target, text, length + 1);
    return true;
}
static bool integer(unsigned *target, yyjson_val *root, const char *name, unsigned min, unsigned max) {
    yyjson_val *v = yyjson_obj_get(root, name);
    if (!v)
        return true;
    if (!yyjson_is_uint(v) || yyjson_get_uint(v) < min || yyjson_get_uint(v) > max)
        return false;
    *target = (unsigned)yyjson_get_uint(v);
    return true;
}
static bool boolean(bool *target, yyjson_val *root, const char *name) {
    yyjson_val *v = yyjson_obj_get(root, name);
    if (!v)
        return true;
    if (!yyjson_is_bool(v))
        return false;
    *target = yyjson_get_bool(v);
    return true;
}
static bool residency(char target[8], yyjson_val *root) {
    yyjson_val *v = yyjson_obj_get(root, "keepAlive");
    if (!v)
        return true;
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    if (!s || !n || n >= 8 || memchr(s, 0, n))
        return false;
    if (strcmp(s, "0") && strcmp(s, "-1")) {
        unsigned minutes = 0;
        if (n < 2 || s[n - 1] != 'm')
            return false;
        for (size_t i = 0; i < n - 1; i++) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            minutes = minutes * 10 + (unsigned)(s[i] - '0');
        }
        if (!minutes || minutes > 240)
            return false;
    }
    memcpy(target, s, n + 1);
    return true;
}
static bool thinking_mode(char target[OT_THINKING_NAME], yyjson_val *root) {
    yyjson_val *value = yyjson_obj_get(root, "thinkingMode");
    if (!value)
        return true;
    const char *name = yyjson_get_str(value);
    if (!name || yyjson_get_len(value) != strlen(name) || !ot_thinking_name(name))
        return false;
    strcpy(target, name);
    return true;
}
bool ot_preferences_read(ot_preferences *out, yyjson_val *json) {
    if (!yyjson_is_obj(json))
        return false;
    yyjson_val *root = json;
    ot_preferences p = ot_preferences_default();
    bool ok =
        integer(&p.setup, root, "setup", 0, 3) && integer(&p.purpose, root, "purpose", 0, 5) &&
        integer(&p.setup_engines, root, "setupEngines", 1, 3) &&
        integer(&p.setup_target_engines, root, "setupTargetEngines", 0, 3) &&
        boolean(&p.setup_paused, root, "setupPaused") &&
        integer(&p.context, root, "context", 1024, 262144) && integer(&p.input, root, "input", 0, 262144) &&
        integer(&p.output, root, "output", 64, 32768) && integer(&p.threads, root, "threads", 0, 1024) &&
        integer(&p.memory_percent, root, "memoryPercent", 10, 100) && boolean(&p.titles, root, "titles") &&
        boolean(&p.summaries, root, "summaries") && boolean(&p.thinking, root, "thinking") &&
        boolean(&p.spellcheck, root, "spellcheck") && boolean(&p.code_wrap, root, "codeWrap") &&
        boolean(&p.internet, root, "internet") && integer(&p.network, root, "network", 0, OT_NETWORK_ALL) && boolean(&p.improve, root, "improve") &&
        boolean(&p.developer, root, "developer") && boolean(&p.require_gpu, root, "requireGpu") &&
        boolean(&p.tooltips, root, "tooltips") && boolean(&p.advice_notices, root, "adviceNotices") &&
        boolean(&p.advice_learning, root, "adviceLearning") &&
        integer(&p.guide_seen, root, "guideSeen", 0, 7) &&
        integer(&p.advice_goal, root, "adviceGoal", 0, 3) &&
        integer(&p.advice_size_gb, root, "adviceSizeGB", 0, 512) &&
        integer(&p.improve_context, root, "improveContext", 0, 8) &&
        choice(p.input_language, sizeof p.input_language, root, "inputLanguage",
               "auto|it|en|fr|es|de|pt|ru|zh|ja|ko") &&
        thinking_mode(p.thinking_mode, root) && residency(p.keep_alive, root) &&
        choice(p.profile, sizeof p.profile, root, "profile", "light|balanced|long|max|recommended|custom") &&
        choice(p.log_level, sizeof p.log_level, root, "logLevel", "error|warning|info|debug") &&
        choice(p.theme, sizeof p.theme, root, "theme", "system|dark|light") &&
        choice(p.accent, sizeof p.accent, root, "accent", "blue|purple|teal|green|amber|rose") &&
        choice(p.language, sizeof p.language, root, "language", "it|en|es|fr|de|pt|ru|zh|ja|ko") &&
        choice(p.region, sizeof p.region, root, "region",
               "auto|it-IT|en-US|en-GB|fr-FR|de-DE|es-ES|pt-PT|pt-BR|nl-NL|pl-PL|ru-RU|ja-JP|ko-KR|zh-CN|ar-"
               "SA|hi-IN") &&
        choice(p.date_format, sizeof p.date_format, root, "dateFormat", "auto|dmy|mdy|ymd") &&
        choice(p.hour_format, sizeof p.hour_format, root, "hourFormat", "auto|12|24") &&
        choice(p.shortcut, sizeof p.shortcut, root, "shortcut", "enter|ctrl_enter|shift_enter");
    if (!yyjson_obj_get(root, "thinkingMode") && !p.thinking)
        strcpy(p.thinking_mode, "off");
    p.thinking = strcmp(p.thinking_mode, "off") != 0;
    if (!yyjson_obj_get(root, "profile") &&
        (yyjson_obj_get(root, "context") || yyjson_obj_get(root, "gpuLayers") ||
         yyjson_obj_get(root, "output") || yyjson_obj_get(root, "keepAlive")))
        strcpy(p.profile, "custom");
    yyjson_val *startup = yyjson_obj_get(root, "startupModel");
    ok = ok && identifier(p.setup_target, sizeof p.setup_target, root, "setupTarget") &&
         identifier(p.advice_seen, sizeof p.advice_seen, root, "adviceSeen");
    if (startup) {
        ok = ok && yyjson_is_int(startup) &&
             (!yyjson_is_uint(startup) || yyjson_get_uint(startup) <= INT64_MAX) &&
             yyjson_get_sint(startup) >= -1;
        p.startup_model = yyjson_get_sint(startup);
    }
    yyjson_val *since = yyjson_obj_get(root, "adviceSince");
    if (since) {
        ok = ok && yyjson_is_int(since) && yyjson_get_sint(since) >= 0 &&
             (!yyjson_is_uint(since) || yyjson_get_uint(since) <= INT64_MAX);
        p.advice_since = yyjson_get_sint(since);
    }
    yyjson_val *v = yyjson_obj_get(root, "gpuLayers");
    if (v) {
        int64_t n = yyjson_get_sint(v);
        ok = ok && yyjson_is_int(v) && (!yyjson_is_uint(v) || yyjson_get_uint(v) <= 10000) && n >= -2 &&
             n <= 10000;
        p.gpu_layers = (int)n;
    }
    v = yyjson_obj_get(root, "temperature");
    if (v) {
        p.temperature = yyjson_get_num(v);
        ok = ok && yyjson_is_num(v) && isfinite(p.temperature) && p.temperature >= 0 && p.temperature <= 2;
    }
    v = yyjson_obj_get(root, "topP");
    if (v) {
        p.top_p = yyjson_get_num(v);
        ok = ok && yyjson_is_num(v) && isfinite(p.top_p) && p.top_p > 0 && p.top_p <= 1;
    }
    v = yyjson_obj_get(root, "system");
    if (v && !yyjson_is_null(v)) {
        size_t n = yyjson_get_len(v);
        const char *s = yyjson_get_str(v);
        if (!s || n >= sizeof p.system || memchr(s, 0, n))
            return false;
        if (n)
            memcpy(p.system, s, n + 1);
    }
    if (!ok || p.output + 512 >= p.context || (p.input && p.input > p.context - p.output - 256))
        return false;
    *out = p;
    return true;
}
yyjson_mut_val *ot_preferences_model_json(yyjson_mut_doc *d, const ot_preferences *p) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    if (!v)
        return NULL;
    bool ok = yyjson_mut_obj_add_uint(d, v, "context", p->context) &&
              yyjson_mut_obj_add_uint(d, v, "input", p->input) &&
              yyjson_mut_obj_add_uint(d, v, "output", p->output) &&
              yyjson_mut_obj_add_uint(d, v, "threads", p->threads) &&
              yyjson_mut_obj_add_int(d, v, "gpuLayers", p->gpu_layers) &&
              yyjson_mut_obj_add_uint(d, v, "memoryPercent", p->memory_percent) &&
              yyjson_mut_obj_add_real(d, v, "temperature", p->temperature) &&
              yyjson_mut_obj_add_real(d, v, "topP", p->top_p) &&
              yyjson_mut_obj_add_bool(d, v, "requireGpu", p->require_gpu) &&
              yyjson_mut_obj_add_strcpy(d, v, "keepAlive", p->keep_alive) &&
              yyjson_mut_obj_add_strcpy(d, v, "profile", p->profile);
    return ok ? v : NULL;
}

yyjson_mut_val *ot_preferences_json(yyjson_mut_doc *d, const ot_preferences *p) {
    yyjson_mut_val *v = ot_preferences_model_json(d, p);
    if (!v)
        return NULL;
    bool ok = yyjson_mut_obj_add_uint(d, v, "setup", p->setup) &&
              yyjson_mut_obj_add_uint(d, v, "setupEngines", p->setup_engines) &&
              yyjson_mut_obj_add_uint(d, v, "setupTargetEngines", p->setup_target_engines) &&
              yyjson_mut_obj_add_bool(d, v, "setupPaused", p->setup_paused) &&
              yyjson_mut_obj_add_uint(d, v, "purpose", p->purpose) &&
              yyjson_mut_obj_add_strcpy(d, v, "setupTarget", p->setup_target) &&
              yyjson_mut_obj_add_strcpy(d, v, "adviceSeen", p->advice_seen) &&
              yyjson_mut_obj_add_sint(d, v, "startupModel", p->startup_model) &&
              yyjson_mut_obj_add_bool(d, v, "titles", p->titles) &&
              yyjson_mut_obj_add_bool(d, v, "summaries", p->summaries) &&
              yyjson_mut_obj_add_bool(d, v, "thinking", p->thinking) &&
              yyjson_mut_obj_add_bool(d, v, "spellcheck", p->spellcheck) &&
              yyjson_mut_obj_add_bool(d, v, "codeWrap", p->code_wrap) &&
              yyjson_mut_obj_add_bool(d, v, "internet", p->internet) &&
              yyjson_mut_obj_add_uint(d, v, "network", p->network) &&
              yyjson_mut_obj_add_bool(d, v, "developer", p->developer) &&
              yyjson_mut_obj_add_bool(d, v, "tooltips", p->tooltips) &&
              yyjson_mut_obj_add_bool(d, v, "adviceNotices", p->advice_notices) &&
              yyjson_mut_obj_add_bool(d, v, "adviceLearning", p->advice_learning) &&
              yyjson_mut_obj_add_uint(d, v, "guideSeen", p->guide_seen) &&
              yyjson_mut_obj_add_uint(d, v, "adviceGoal", p->advice_goal) &&
              yyjson_mut_obj_add_uint(d, v, "adviceSizeGB", p->advice_size_gb) &&
              yyjson_mut_obj_add_sint(d, v, "adviceSince", p->advice_since) &&
              yyjson_mut_obj_add_strcpy(d, v, "logLevel", p->log_level) &&
              yyjson_mut_obj_add_bool(d, v, "improve", p->improve) &&
              yyjson_mut_obj_add_uint(d, v, "improveContext", p->improve_context) &&
              yyjson_mut_obj_add_strcpy(d, v, "inputLanguage", p->input_language) &&
              yyjson_mut_obj_add_strcpy(d, v, "thinkingMode", p->thinking_mode) &&
              yyjson_mut_obj_add_strcpy(d, v, "theme", p->theme) &&
              yyjson_mut_obj_add_strcpy(d, v, "accent", p->accent) &&
              yyjson_mut_obj_add_strcpy(d, v, "language", p->language) &&
              yyjson_mut_obj_add_strcpy(d, v, "region", p->region) &&
              yyjson_mut_obj_add_strcpy(d, v, "dateFormat", p->date_format) &&
              yyjson_mut_obj_add_strcpy(d, v, "hourFormat", p->hour_format) &&
              yyjson_mut_obj_add_strcpy(d, v, "shortcut", p->shortcut) &&
              yyjson_mut_obj_add_strcpy(d, v, "system", p->system);
    return ok ? v : NULL;
}
