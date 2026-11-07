#ifndef OVERTONE_THINKING_H
#define OVERTONE_THINKING_H
#include <stdbool.h>
typedef struct yyjson_val yyjson_val;
typedef struct yyjson_mut_val yyjson_mut_val;
typedef struct yyjson_mut_doc yyjson_mut_doc;
enum { OT_THINKING_VALUES = 8, OT_THINKING_NAME = 32 };
typedef enum { OT_THINKING_FALSE, OT_THINKING_TRUE, OT_THINKING_LEVEL } ot_thinking_type;
typedef struct {
    char value[OT_THINKING_NAME];
    ot_thinking_type type;
} ot_thinking_option;
typedef struct {
    unsigned count;
    bool known;
    ot_thinking_option options[OT_THINKING_VALUES];
} ot_thinking;
/* Exact provider values; absent or ambiguous metadata never invents selectable levels. */
ot_thinking ot_thinking_detect(yyjson_val *metadata, bool ollama);
bool ot_thinking_name(const char *value);
const ot_thinking_option *ot_thinking_find(const ot_thinking *thinking, const char *value);
yyjson_mut_val *ot_thinking_json(yyjson_mut_doc *doc, const ot_thinking *thinking);
#endif
