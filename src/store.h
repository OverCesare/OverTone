#ifndef OVERTONE_STORE_H
#define OVERTONE_STORE_H
#include "core.h"
#include "preferences.h"
#include "recommendations.h"
#include "yyjson.h"
/* Private SQLite owner, used only on the application's serial executor.
 * JSON projections belong to the caller. No statement or borrowed row escapes. */
typedef struct ot_store ot_store;
typedef struct sqlite3 sqlite3;
enum { OT_DATABASE_ID = 1330927443, OT_DATABASE_VERSION = 13 };
/* Borrowed only on the same executor. Feature queries never retain live statements. */
sqlite3 *ot_store_connection(ot_store *store);
ot_store *ot_app_store(ot_app *app);
ot_store *ot_store_open(const char *path);
void ot_store_close(ot_store *s);
bool ot_store_settings(ot_store *s, ot_connection *value, int64_t *selected, int64_t *model);
bool ot_store_configure(ot_store *s, const ot_connection *value, int64_t model_id,
                        ot_preferences *preferences);
bool ot_store_select(ot_store *s, int64_t chat);
bool ot_store_head(ot_store *s, int64_t chat, int64_t *head);
int ot_store_classification(ot_store *s, int64_t chat); /* -1 missing/error; 0 active, 1 archive, 2 trash. */
int64_t ot_store_chat_position(ot_store *s, int64_t chat, int classification);
bool ot_store_edit(ot_store *s, int64_t chat, int64_t turn, int64_t *parent, char **prompt);
bool ot_store_accept(ot_store *s, int64_t *chat, int64_t parent, const char *prompt, bool replacement,
                     int64_t regenerate, int64_t *turn);
bool ot_store_finish(ot_store *s, int64_t chat, int64_t turn, const char *answer, const char *reasoning,
                     const char *state, const char *error, const char *completion, bool publish);
bool ot_store_choose(ot_store *s, int64_t chat, int64_t turn);
bool ot_store_rename(ot_store *s, int64_t chat, const char *title);
/* 1: complete, 0: context exceeds budget, -1: storage/allocation error. */
int ot_store_context(ot_store *s, int64_t chat, int64_t parent, yyjson_mut_doc *doc, yyjson_mut_val *messages,
                     size_t budget, unsigned recent, bool *trimmed);
/* Title input: only the first user question on the selected branch, within a byte budget. */
bool ot_store_title_context(ot_store *s, int64_t chat, int64_t head, yyjson_mut_doc *doc,
                            yyjson_mut_val *messages, size_t budget);
yyjson_mut_doc *ot_store_view(ot_store *s, int64_t chat, int64_t active_turn, int64_t page, const char *query,
                              int64_t before, int filter, const ot_browse_options *options);
int ot_store_auxiliary(ot_store *s, int64_t chat, int64_t *turn, bool titles, bool summaries);
bool ot_store_annotation(ot_store *s, int64_t chat, int64_t turn, int kind, const char *text, bool limited);
bool ot_store_preferences(ot_store *s, ot_preferences *value);
/* -1 storage/format error, 0 defaults, 1 saved local-model or API-connection values. */
int ot_store_generation(ot_store *s, int64_t model_id, ot_preferences *value);
/* Seed resolved initial local values after a successful connection, preserving saved choices. */
bool ot_store_initial_generation(ot_store *s, int64_t model_id, const ot_preferences *value);
bool ot_store_set_preferences(ot_store *s, const ot_preferences *value);
bool ot_store_remove_model(ot_store *s, int64_t model_id, bool erase, ot_preferences *preferences);
bool ot_store_chat_thinking(ot_store *s, int64_t chat, char mode[OT_THINKING_NAME]);
bool ot_store_set_chat_thinking(ot_store *s, int64_t chat, const char *mode);
bool ot_store_advice_record(ot_store *s, int64_t turn, const ot_advice_profile *sample);
bool ot_store_advice_reset(ot_store *s);
bool ot_store_advice_profile(ot_store *s, ot_advice_profile *out, int64_t since);
void ot_store_advice_identity(ot_store *s, ot_advice_profile *sample);
/* Up to32 identities; fills measured count/latency/speed, preserving matching fields. */
bool ot_store_advice_performance(ot_store *s, ot_advice_profile *targets, size_t count);
bool ot_store_draft(ot_store *s, int64_t chat, const char *text);
bool ot_store_classify(ot_store *s, int64_t chat, int classification);
bool ot_store_empty_trash(ot_store *s);
bool ot_store_restore_archive(ot_store *s);
/* Independent file operations: separate connections, safe on a file worker. Never overwrite. */
bool ot_store_backup(const char *source, const char *destination);
bool ot_store_export(const char *source, const char *destination);
bool ot_store_apply_restore(const char *profile); /* With exclusive profile lock, before opening the owner. */
bool ot_store_validate(const char *path);
bool ot_store_schema_valid(sqlite3 *database); /* Current schema only; never executes imported DDL. */
#endif
