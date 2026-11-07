#ifndef OVERTONE_MEMORY_H
#define OVERTONE_MEMORY_H
#include "resources.h"
#include "runtime.h"
#include <stdatomic.h>
#include <stddef.h>
typedef struct {
    uint64_t weights, cache, compute, total;
    bool engine_plan, known_cache, observed;
} ot_memory_plan;
typedef struct {
    int64_t model;
    unsigned context;
    int gpu_layers;
    bool pending;
    ot_memory_plan plan;
    unsigned memory_percent, threads;
} ot_memory_view;
typedef bool (*ot_memory_probe)(const char *engine, const char *model, const ot_preferences *preferences,
                                const ot_resources *resources, const atomic_bool *cancel,
                                ot_memory_plan *result);
typedef struct ot_memory ot_memory;
/* One latest request, one worker, sixteen identity-checked results. No database or UI access. */
ot_memory *ot_memory_open(ot_memory_probe probe);
void ot_memory_close(ot_memory *memory);
void ot_memory_request(ot_memory *memory, int64_t model, const char *file, const char *engine,
                       const ot_model_info *info, const ot_preferences *preferences,
                       const ot_resources *resources, const ot_memory_plan *observed);
void ot_memory_cancel(ot_memory *memory);
bool ot_memory_poll(ot_memory *memory, ot_memory_view *view);
ot_memory_plan ot_memory_approximate(const ot_model_info *model, unsigned context);
/* Project an observed allocation only where the cache geometry has a known context slope.
 * Compute/weight overhead stays anchored to the same engine, model and GPU placement. */
bool ot_memory_project(unsigned context, const ot_runtime_info *observed, const ot_runtime_info *previous,
                       uint64_t *bytes);
/* Reads only the planner's structured allocation event, never its free-memory or self totals. */
bool ot_memory_parse(const char *json, size_t length, ot_memory_plan *result);
/* Two bounded observations, keyed by the caller's model/engine/hardware identity.
 * No hidden loads: exact reuse, or interpolation between compatible measured contexts.
 * observe returns NULL when unchanged/invalid; the caller keeps the previous record. */
char *ot_memory_observe(const char *saved, const char *key, const ot_runtime_info *observed);
bool ot_memory_recall(const char *saved, const char *key, unsigned context, ot_memory_plan *result);
#endif
