#ifndef OVERTONE_GGUF_H
#define OVERTONE_GGUF_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint64_t bytes, tensor_bytes;
    unsigned context, embedding, layers, heads, kv_heads, key_length, value_length;
    unsigned experts, experts_used;
    bool complex_cache;
    char name[257], architecture[65], quantization[33];
} ot_model_info;
/* Bounded metadata inspection; inference compatibility remains the engine's responsibility. */
bool ot_gguf(const char *path, ot_model_info *result);
const char *ot_model_precision(const char *quantization);
/* Presentation only: identities and file names remain unchanged. */
void ot_model_label(const char *name, const char *quantization, char title[257], char quant[33]);
#endif
