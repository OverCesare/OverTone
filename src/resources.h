#ifndef OVERTONE_RESOURCES_H
#define OVERTONE_RESOURCES_H
#include <stdbool.h>
#include <stdint.h>
#include "preferences.h"
#include "gguf.h"
typedef struct {
    uint64_t memory_total, memory_available, memory_used, gpu_recommended, disk_available;
    unsigned cores, performance_cores, interactive_cores, pressure;
    bool unified, gpu;
    char cpu_name[96], system_build[64];
} ot_resources;
/* macOS sampler; unknown values remain zero. No RAM/VRAM addition on unified hardware. */
ot_resources ot_resources_sample(const char *profile);
uint64_t ot_memory_budget(const ot_resources *r, const ot_preferences *p);
unsigned ot_thread_budget(const ot_resources *r, const ot_preferences *p);
enum { OT_PRESET_RECOMMENDED = 4, OT_PRESET_COUNT = 5 };
extern const char *const ot_model_profile_names[OT_PRESET_COUNT];
/* One policy for initial values and the parameter dialog. Custom values stay untouched. */
ot_preferences ot_model_preset(const ot_preferences *base, const ot_model_info *model,
                               const ot_resources *resources, unsigned profile);
#endif
