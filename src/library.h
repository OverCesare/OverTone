#ifndef OVERTONE_LIBRARY_H
#define OVERTONE_LIBRARY_H
#include "core.h"
typedef struct ot_library ot_library;
typedef struct {
    void *context;
    /* etag is NULL for metadata; non-NULL (possibly empty) for resumable files.
     * The transport preserves their wire representation and may compress metadata. */
    void (*request)(void *, uint64_t id, const char *method, const char *url, const char *body,
                    const char *etag, uint64_t offset, ot_network_scope scope);
    void (*cancel)(void *, uint64_t id);
    void (*connected)(void *); /* Clears session-only credentials when selecting a local engine. */
    const char *components; /* Stable private profile anchor; copied at open. NULL uses profile. */
} ot_library_io;
ot_library *ot_library_open(ot_app *app, const char *profile, ot_library_io io);
void ot_library_manual(ot_library *library);
void ot_library_server_changed(ot_library *library); /* Refresh profiles after API metadata changes. */
bool ot_library_manages_model(const ot_library *library); /* Library route, even when held; not process ownership. */
bool ot_library_preferences_allowed(ot_library *library, const ot_preferences *preferences);
void ot_library_preferences_changed(ot_library *library, const ot_preferences *previous);
void ot_library_close(ot_library *library);
void ot_library_command(ot_library *library, const char *json);
void ot_library_tick(ot_library *library);
/* Whether timed preparation/progress needs frequent owner ticks. No allocation or I/O. */
bool ot_library_polling(const ot_library *library);
bool ot_library_headers(ot_library *library, uint64_t id, int status, const char *etag, const char *range,
                        int64_t length);
void ot_library_bytes(ot_library *library, uint64_t id, const void *data, size_t size);
void ot_library_end(ot_library *library, uint64_t id, const char *error);
char *ot_library_view(ot_library *library);
char *ot_library_health(const ot_library *library);
uint64_t ot_library_revision(const ot_library *library);
char *ot_library_memory(const ot_library *library);
uint64_t ot_library_memory_revision(const ot_library *library);
#endif
