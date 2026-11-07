#ifndef OVERTONE_CONNECTION_H
#define OVERTONE_CONNECTION_H
#include "gguf.h"
#include "thinking.h"
typedef struct {
    ot_model_info model;
    ot_thinking thinking;
    bool verified, resident_known, resident, has_key;
    unsigned loaded_context;
} ot_server_info;
typedef enum { OT_SERVER_INVALID, OT_SERVER_OK, OT_SERVER_NOT_CHAT } ot_server_result;
/* Bounded, untrusted protocol metadata. No URLs, credentials, network or process ownership. */
char *ot_server_models(const char *json, bool ollama);
ot_server_result ot_server_metadata(const char *json, bool ollama, const char *model, ot_server_info *info);
void ot_server_residency(const char *json, const char *model, ot_server_info *info);
#endif
