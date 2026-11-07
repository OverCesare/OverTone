#ifndef OVERTONE_PREFERENCES_H
#define OVERTONE_PREFERENCES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "thinking.h"
typedef struct yyjson_val yyjson_val;
typedef struct yyjson_mut_val yyjson_mut_val;
typedef struct yyjson_mut_doc yyjson_mut_doc;
/* Capabilities, not hosts: a loopback pull can itself download remote data.
 * LOCAL is reserved for inference/discovery that never leaves this computer. */
typedef enum { OT_NETWORK_LOCAL = 0, OT_NETWORK_CATALOGUES = 1, OT_NETWORK_MODELS = 2,
               OT_NETWORK_ENGINES = 4, OT_NETWORK_SERVERS = 8, OT_NETWORK_ALL = 15 } ot_network_scope;
/* One validated value, persisted only after the transaction succeeds. */
typedef struct {
    unsigned context, input, output, threads, memory_percent, improve_context;
    int gpu_layers;
    int64_t startup_model;
    unsigned setup, purpose, setup_engines, setup_target_engines;
    char setup_target[257], advice_seen[280];
    double temperature, top_p;
    bool titles, summaries, thinking, spellcheck, code_wrap, improve, internet, developer, require_gpu,
        tooltips, advice_notices, advice_learning, setup_paused;
    unsigned guide_seen, advice_goal, advice_size_gb, network;
    int64_t advice_since;
    char theme[8], accent[8], language[3], region[8], date_format[8], hour_format[8],
        input_language[5], thinking_mode[OT_THINKING_NAME], keep_alive[8],
        shortcut[12], profile[12], log_level[8], system[4097];
} ot_preferences;
ot_preferences ot_preferences_default(void);
bool ot_network_allowed(const ot_preferences *preferences, ot_network_scope scope);
void ot_network_enable(ot_preferences *preferences, unsigned scopes);
void ot_preferences_model_values(ot_preferences *target, const ot_preferences *model);
yyjson_mut_val *ot_preferences_model_json(yyjson_mut_doc *doc, const ot_preferences *value);
bool ot_preferences_execution_changed(const ot_preferences *a, const ot_preferences *b);
bool ot_preferences_read(ot_preferences *out, yyjson_val *json);
yyjson_mut_val *ot_preferences_json(yyjson_mut_doc *doc, const ot_preferences *value);
#endif
