#ifndef OVERTONE_ENGINE_H
#define OVERTONE_ENGINE_H
#include <stdbool.h>
#include <sys/types.h>
#include "preferences.h"
#include "resources.h"
#include "memory.h"
#include "runtime.h"
typedef struct {
    pid_t pid, child;
    unsigned char handshake[sizeof(pid_t)];
    size_t handshake_size;
    int pipe, control, exit_code;
    double deadline;
    bool stopping, killed;
    unsigned port;
    char tail[4097];
    size_t tail_size;
    ot_runtime_info runtime;
    ot_runtime_buffers buffers;
    uint64_t runtime_revision;
    char runtime_line[1025];
    size_t runtime_line_size;
    bool runtime_line_overflow, ollama;
} ot_engine;
int ot_engine_supervise(char *const arguments[]); /* Internal child mode of this same executable. */
void ot_engine_init(ot_engine *engine);
bool ot_engine_find(const char *name, char path[4096]);
/* Bounded capability discovery; never starts an inference server. */
bool ot_engine_sleep_supported(const char *executable);
bool ot_engine_validate(const char *executable, bool ollama);
bool ot_engine_memory_plan(const char *executable, const char *model, const ot_preferences *preferences,
                           const ot_resources *resources, const atomic_bool *cancel, ot_memory_plan *plan);
/* Own process tree only. Physical footprint already includes IOKit mappings on macOS. */
bool ot_engine_memory(const ot_engine *engine, uint64_t *bytes);
bool ot_engine_start(ot_engine *engine, const char *executable, const char *profile, const char *model,
                     bool ollama, bool idle_sleep, const char *store, const ot_preferences *preferences,
                     const ot_resources *resources);
bool ot_engine_listening(const ot_engine *engine);
void ot_engine_poll(ot_engine *engine);
void ot_engine_stop(ot_engine *engine);
void ot_engine_close(ot_engine *engine);
#endif
