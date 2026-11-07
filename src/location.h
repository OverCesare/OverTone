#ifndef OVERTONE_LOCATION_H
#define OVERTONE_LOCATION_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef struct {
    char base[4096], active[4096], pending[4096], marker[33], error[64];
    uint64_t bytes, files, device, inode;
} ot_location;
typedef void (*ot_location_progress)(void *context, const char *stage, uint64_t bytes, uint64_t total);
/* Called under the application's location lock. Only current-format data is moved.
 * A planned change is applied at startup, before either database or engines are opened.
 * The source remains authoritative until the last durable bootstrap commit. */
bool ot_location_open(ot_location *location, const char *default_root);
bool ot_location_plan(ot_location *location, const char *target);
bool ot_location_cancel(ot_location *location);
bool ot_location_apply(ot_location *location, const atomic_bool *cancel, ot_location_progress progress,
                       void *context);
#endif
