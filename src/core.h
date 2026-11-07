#ifndef OVERTONE_CORE_H
#define OVERTONE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "preferences.h"
#include "resources.h"
#include "thinking.h"
#include "connection.h"

/* One serial executor owns the app. Callbacks may read configuration, but must
 * not reenter mutating operations. Strings are
 * borrowed only for the duration of a call; returned JSON belongs to the caller. */
typedef struct ot_app ot_app;
typedef struct {
    unsigned sort, minimum, maximum;
    int64_t from, until;
    bool code, errors, compacted, maximum_set;
    unsigned model_count, runtime_count;
    char models[16][257], runtimes[8][32];
} ot_browse_options;
typedef struct {
    char endpoint[2049], model[257];
    bool ollama, remote;
} ot_connection;
typedef struct {
    void *context;
    void (*request)(void *, uint64_t, const char *body);
    void (*cancel)(void *, uint64_t);
    void (*language)(void *, const char *prompt, char tag[16]); /* Optional OS hint; no network. */
} ot_io;
ot_app *ot_open(const char *database, ot_io io);
void ot_close(ot_app *app);
bool ot_submit(ot_app *app, const char *prompt, int64_t replace); /* NULL prompt retries replace. */
bool ot_configure(ot_app *app, const ot_connection *connection, int64_t model_id);
const ot_connection *ot_configuration(const ot_app *app);
int64_t ot_local_model(const ot_app *app); /* Library identity, independent of engine residency. */
void ot_connection_ready(ot_app *app, bool ready);
void ot_connection_failed(ot_app *app, const char *code); /* Connection status, not model-authored text. */
bool ot_ready(const ot_app *app);
void ot_connection_observe(ot_app *app, const ot_server_info *info);
const ot_server_info *ot_connection_model(const ot_app *app);
void ot_chat_visible(ot_app *app, bool visible); /* Presentation visibility, no generation changes. */
void ot_start_without_model(ot_app *app); /* Session only; retain the last model for later starts. */
void ot_thinking_capability(ot_app *app, const ot_thinking *thinking);
bool ot_chat_thinking(ot_app *app, const char *mode);
bool ot_set_preferences(ot_app *app, const char *json);
bool ot_reset_preferences(ot_app *app);
bool ot_reset_advice(ot_app *app); /* Erases only bounded local personalization samples. */
/* Library record only, after release. Hidden models retain parameters; erased models do not. */
bool ot_remove_model(ot_app *app, int64_t model_id, bool erase);
const ot_preferences *ot_get_preferences(const ot_app *app);
bool ot_is_idle(const ot_app *app); /* Observe readiness without preempting a generation. */
void ot_resources_update(ot_app *app, const ot_resources *resources);
const ot_resources *ot_resources_get(const ot_app *app);
char *ot_resources_view(const ot_app *app);
bool ot_draft(ot_app *app, int64_t chat, const char *text);
bool ot_classify(ot_app *app, int classification); /* 0 active, 1 archived, 2 trash */
bool ot_empty_trash(ot_app *app);
bool ot_restore_archive(ot_app *app);
void ot_filter(ot_app *app, int classification);
bool ot_maintenance(ot_app *app, bool busy, const char *error);
bool ot_choose(ot_app *app, int64_t turn);
bool ot_rename(ot_app *app, const char *title);
bool ot_search(ot_app *app, const char *query, int64_t before);
bool ot_search_filters(ot_app *app, const char *json);
bool ot_page(ot_app *app, int64_t conversation,
             int64_t anchor);                      /* Read-only; stale chat requests are ignored. */
int64_t ot_selected_chat(const ot_app *app);
bool ot_select(ot_app *app, int64_t conversation); /* zero starts an empty draft */
void ot_background(ot_app *app); /* Opportunistic, preempted by every interactive command. */
bool ot_improve(ot_app *app, const char *prompt);
void ot_stop(ot_app *app);
void ot_bytes(ot_app *app, uint64_t operation, const void *bytes, size_t count);
void ot_end(ot_app *app, uint64_t operation, const char *error_code);
char *ot_view(ot_app *app);
char *ot_progress(ot_app *app); /* Active answer only; no database read. */
char *ot_health(ot_app *app);
uint64_t ot_revision(const ot_app *app);
uint64_t ot_stream_revision(const ot_app *app);
#endif
