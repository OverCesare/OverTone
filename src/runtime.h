#ifndef OVERTONE_RUNTIME_H
#define OVERTONE_RUNTIME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum {
    OT_RUNTIME_UNKNOWN,
    OT_RUNTIME_CPU,
    OT_RUNTIME_GPU,
    OT_RUNTIME_HYBRID,
    OT_RUNTIME_ASLEEP
} ot_runtime_kind;
typedef struct {
    ot_runtime_kind kind;
    unsigned context, batch, micro_batch, gpu_layers, total_layers, other_models, window_cells;
    uint64_t allocation, gpu_allocation;
    uint64_t weights, cache, linear_cache, compute, file_weights, cpu_mapped;
    char model_digest[96]; /* Optional service identity; never inferred from a model name. */
} ot_runtime_info;
typedef struct {
    struct {
        char component[64], backend[64], kind[16];
        unsigned bank;
        uint64_t bytes;
        bool window;
    } rows[32];
    unsigned count, bank;
    bool limited, window;
} ot_runtime_buffers;
/* Engine observations, never inferred from requested preferences. Malformed input changes nothing. */
bool ot_runtime_line(ot_runtime_info *info, const char *line);
/* Owned runner allocation logs: weight chunks and independent KV banks add;
 * resized KV/compute reservations replace their previous report within each bank.
 * No prompt/log text is retained. File-backed spans are reconciled against model size later. */
bool ot_runtime_buffer_line(ot_runtime_buffers *buffers, ot_runtime_info *info, const char *line);
void ot_runtime_reconcile(ot_runtime_info *info, uint64_t model_bytes);
/* Ollama names are case-insensitive; an omitted tag means :latest. */
bool ot_ollama_model_equal(const char *value, const char *model);
bool ot_runtime_ollama(ot_runtime_info *info, const char *json, size_t size, const char *model);
const char *ot_runtime_name(ot_runtime_kind kind);
#endif
