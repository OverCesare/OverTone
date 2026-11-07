#include "resources.h"
#include "memory.h"
#include <stdio.h>
const char *const ot_model_profile_names[OT_PRESET_COUNT] = {"light", "balanced", "long", "max",
                                                             "recommended"};
uint64_t ot_memory_budget(const ot_resources *r, const ot_preferences *p) {
    uint64_t policy = r->memory_total / 100 * p->memory_percent;
    uint64_t available = r->memory_available / 10 * 9;
    if (r->pressure >= 2)
        return 0;
    if (r->pressure == 1)
        available /= 2;
    return available < policy ? available : policy;
}
unsigned ot_thread_budget(const ot_resources *r, const ot_preferences *p) {
    unsigned n = p->threads ? p->threads : (r->performance_cores ? r->performance_cores : r->cores);
    if (n > r->cores)
        n = r->cores;
    return n ? n : 1;
}
ot_preferences ot_model_preset(const ot_preferences *base, const ot_model_info *model, const ot_resources *r,
                               unsigned profile) {
    ot_preferences p = *base;
    if (profile >= OT_PRESET_COUNT)
        return p;
    snprintf(p.profile, sizeof p.profile, "%s", ot_model_profile_names[profile]);
    unsigned maximum = model->context ? model->context : 32768;
    if (maximum > 262144)
        maximum = 262144;
    if (maximum < 1024)
        maximum = 1024;
    unsigned balanced = maximum < 8192 ? maximum : 8192;
    unsigned fraction = ((maximum * (profile == 2 ? 3 : 1) / 4 + 256) / 512) * 512;
    p.context = profile == 0   ? (fraction < balanced ? fraction : balanced)
                : profile == 2 ? (fraction > balanced ? fraction : balanced)
                : profile == 3 ? maximum
                               : balanced;
    if (p.context < 1024)
        p.context = 1024;
    if (p.context > maximum)
        p.context = maximum;
    if (profile == OT_PRESET_RECOMMENDED) {
        uint64_t budget = r ? ot_memory_budget(r, &p) : 0;
        if (r && r->gpu && r->gpu_recommended && r->gpu_recommended < budget)
            budget = r->gpu_recommended;
        uint64_t weights = model->tensor_bytes ? model->tensor_bytes : model->bytes;
        uint64_t room = budget > weights ? budget - weights : 0;
        const uint64_t gib = UINT64_C(1) << 30;
        unsigned target = !r                          ? 32768
                          : !r->gpu || room < 2 * gib ? 8192
                          : room < 8 * gib            ? 16384
                          : room < 16 * gib           ? 32768
                          : room < 48 * gib           ? 65536
                                                      : 131072;
        /* Interactive writing/general chat should not reserve an agent-sized KV
         * cache merely because the machine has ample RAM. Longer source material
         * is useful for code, study and document-oriented work. */
        if ((p.purpose == 0 || p.purpose == 3) && target > 65536)
            target = 65536;
        p.context = target < maximum ? target : maximum;
        /* Total tensor bytes include all experts, never just the active experts.
         * Metadata KV is a conservative fit check, not a measured graph allocation.
         * Hybrid geometry remains uncertain; the engine's allocation checks still apply. */
        while (budget > weights && p.context > 4096 && ot_memory_approximate(model, p.context).total > budget)
            p.context /= 2;
        if (p.context < 1024)
            p.context = 1024;
        /* This is an upper output budget, not a requested response length. Leave
         * useful input/history space while allowing reasoning and substantial code. */
        p.output = p.context / 2;
        if (p.output > 32768)
            p.output = 32768;
        if (p.output + 512 >= p.context)
            p.output = p.context - 513;
        p.input = p.context - p.output - 256;
        p.gpu_layers = -1;
        p.require_gpu = r && r->gpu;
        p.threads = 0;
        return p;
    }
    unsigned divisor = profile == 0 ? 16 : profile == 2 ? 40 : profile == 3 ? 4 : 8;
    p.output = ((p.context / divisor + 64) / 128) * 128;
    if (p.output < 128)
        p.output = 128;
    if (p.output > 32768)
        p.output = 32768;
    if (p.output + 512 >= p.context)
        p.output = p.context - 513;
    p.input = p.context - p.output - 256;
    /* The engine fits tensor placement, including experts. Context presets must not
     * silently override the user's independent residency policy or force all layers. */
    return p;
}
