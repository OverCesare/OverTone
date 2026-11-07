#ifndef OVERTONE_PULL_H
#define OVERTONE_PULL_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
/* Private, per-acquisition Ollama stores. Shared installed weights are read-only
 * cache inputs; only a verified manifest publishes a model to the regular store. */
bool ot_pull_path(const char *profile, int64_t job, char out[4096]);
bool ot_pull_prepare(const char *profile, int64_t job, const atomic_bool *cancel, char out[4096]);
bool ot_pull_publish(const char *profile, int64_t job, const atomic_bool *cancel);
bool ot_pull_discard(const char *profile, int64_t job);
#endif
