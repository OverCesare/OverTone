#include "library.h"
#include "store.h"
#include "engine.h"
#include "gguf.h"
#include "inventory.h"
#include "package.h"
#include "pull.h"
#include "recommendations.h"
#include <CommonCrypto/CommonDigest.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { HEALTH_SERVER, HEALTH_MODEL, HEALTH_WARM, HEALTH_PROPS, HEALTH_CPU };
typedef struct {
    int64_t id;
    char *path;
    dev_t device;
    ino_t inode;
    struct stat observed;
    int64_t modified, modified_nsec;
    uint64_t bytes;
    char checksum[65];
    size_t root_length;
    bool available, replaced, checked, manifest, verified, unverified;
} registered_file;
typedef struct {
    char *url, *data;
    size_t bytes;
    double expires, used;
    bool has_more;
} catalog_entry;
// One ordered consumer plus bounded look-ahead: at most eight metadata requests.
enum { ADVICE_PREFETCH = 7 };
typedef struct {
    uint64_t id;
    char url[1024], error[64];
    char *body;
    size_t size, capacity;
    double started;
    bool complete;
} advice_page;
struct ot_library {
    ot_app *app;
    sqlite3 *db;
    ot_library_io io;
    ot_engine engine, download_engine;
    struct stat llama_identity;
    const ot_package *package;
    uint64_t setup_revision, engine_scan;
    char profile[4096], components[4096], directory[4096], error[64], engine_state[24], query_mode[16],
        repo[257], ollama[256], download_uri[256], engine_error[64];
    struct {
        uint64_t max_parameters;
        char task[32], license[32];
    } search_filter;
    bool activity_error, cpu_probing, queue_pending, cleanup_pending, pull_published;
    int64_t pull_job, cleanup_job;
    double cpu_started;
    uint64_t sequence, revision, query_id, transfer_id, health_id, prepare_id, remove_id;
    int64_t selected, pending, requested, job, removing, refresh_model;
    int64_t initial_model;
    ot_model_info initial_info;
    ot_preferences initial_values;
    ot_model_candidate advice[OT_ADVICE_MODELS], advice_families[OT_ADVICE_FAMILIES];
    size_t advice_count, advice_family_count, advice_family, advice_verification;
    uint64_t advice_id, advice_request_epoch;
    advice_page advice_pages[ADVICE_PREFETCH];
    char advice_url[1024], advice_buffer_error[64];
    unsigned advice_sources, advice_requests, advice_hits, advice_peak;
    bool advice_buffered;
    double advice_finished;
    char *advice_body;
    size_t advice_size, advice_capacity;
    double advice_cached, advice_started;
    double advice_cycle_started;
    int64_t advice_updated, advice_checked;
    char *advice_snapshot;
    char advice_evidence_ids[OT_ADVICE_FAMILIES * 2][257];
    unsigned advice_evidence_count;
    size_t advice_evidence_index;
    bool advice_ready, advice_popular, advice_visible, advice_hf, advice_verifying, advice_evidence, advice_recent;
    char advice_error[64];
    char remove_name[257];
    ot_memory *memory;
    ot_memory_view memory_view;
    ot_runtime_info memory_previous;
    ot_runtime_info memory_recorded;
    char memory_load_key[1025], memory_engine_identity[96];
    int64_t memory_previous_model;
    uint64_t memory_revision, process_memory;
    unsigned engine_memory_percent;
    pid_t process_memory_child;
    bool process_memory_known;
    double next_memory;
    ot_runtime_info runtime;
    uint64_t runtime_id, runtime_engine_revision;
    int64_t runtime_model;
    pid_t runtime_child;
    double runtime_next, runtime_started;
    char *runtime_body;
    size_t runtime_size, runtime_capacity;
    bool runtime_ready;
    double started, next_health, download_started, download_next, download_idle;
    double checkpoint_time, rate_time, bytes_per_second;
    uint64_t rate_bytes;
    bool running_ollama, engine_ollama, health_external, pull_done, pull_aggregate, pull_resuming,
        download_preparing, download_external;
    unsigned health_stage;
    ot_thinking thinking;
    char *query, *health, *catalog, *prepare;
    char *query_url;
    catalog_entry catalog_cache[8];
    size_t catalog_cache_bytes;
    uint64_t catalog_hits, catalog_misses, query_limit;
    bool catalog_has_more;
    char detected_engines[2][4096];
    bool custom_engines[2];
    unsigned validation[2], validation_kind;
    bool llama_sleep;
    size_t query_size, health_size, prepare_size;
    size_t query_capacity, health_capacity, prepare_capacity;
    char pull_line[65537];
    size_t pull_size;
    struct {
        char digest[65];
        uint64_t received, total;
        bool resumed;
    } pull_layers[128];
    unsigned pull_layer_count;
    int file;
    unsigned char transfer_buffer[262144];
    size_t buffered;
    uint64_t received, total, checkpoint, pull_expected;
    char etag[513], checksum[65], job_name[257], url[8193];
    CC_SHA256_CTX digest;
    pthread_t worker;
    bool working;
    int worker_kind;
    ot_inventory inventory;
    registered_file *registered;
    size_t registered_count;
    bool registered_limited;
    atomic_bool worker_done, worker_cancel;
    char worker_path[4096];
    char import_path[4096], preview_url[8193], preview_name[257], preview_checksum[65];
    ot_model_info import_info;
    struct stat import_identity, partial_identity;
    bool partial_hashed;
    uint64_t import_token, preview_token, preview_bytes;
    bool import_ready, preview_ready;
    uint64_t worker_bytes;
    CC_SHA256_CTX worker_digest;
    bool worker_ok;
};
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void clear_registered(ot_library *l) {
    for (size_t i = 0; i < l->registered_count; i++)
        free(l->registered[i].path);
    free(l->registered);
    l->registered = NULL;
    l->registered_count = 0;
    l->registered_limited = false;
}
static void changed(ot_library *l, const char *error) {
    snprintf(l->error, sizeof l->error, "%s", error ? error : "");
    l->activity_error = false;
    l->revision++;
}
static sqlite3_stmt *query(ot_library *l, const char *sql) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(l->db, sql, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}
static bool done(sqlite3_stmt *q) {
    bool ok = q && sqlite3_step(q) == SQLITE_DONE;
    sqlite3_finalize(q);
    return ok;
}
static const char *col(sqlite3_stmt *q, int n) {
    const char *s = (const char *)sqlite3_column_text(q, n);
    return s ? s : "";
}
static const char *str(yyjson_val *v, const char *key) {
    yyjson_val *field = yyjson_obj_get(v, key);
    const char *s = yyjson_get_str(field);
    return s && strlen(s) == yyjson_get_len(field) ? s : "";
}
static bool copy(char *out, size_t capacity, const char *value) {
    size_t n = strlen(value);
    if (n >= capacity)
        return false;
    memcpy(out, value, n + 1);
    return true;
}
static bool same_file(const struct stat *a, const struct stat *b) {
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) && a->st_dev == b->st_dev &&
           a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}
static bool path_for(ot_library *l, int64_t job, const char *suffix, char path[4096]) {
    return job > 0 && snprintf(path, 4096, "%s/%lld%s", l->directory, (long long)job, suffix) < 4096;
}
static bool sha(const char *s) {
    if (strlen(s) != 64)
        return false;
    for (unsigned i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}
static char *encoded(const char *s, bool slash) {
    size_t n = strlen(s);
    if (n > 2048)
        return NULL;
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    size_t k = 0;
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || strchr("-_.~", c) || (slash && c == '/'))
            out[k++] = (char)c;
        else {
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    out[k] = 0;
    return out;
}
static uint64_t request(ot_library *l, const char *method, const char *url, const char *body,
                        const char *etag, uint64_t offset, ot_network_scope scope) {
    if (!ot_network_allowed(ot_get_preferences(l->app), scope)) {
        changed(l, "internet_disabled");
        return 0;
    }
    uint64_t id = ++l->sequence;
    if (l->io.request)
        l->io.request(l->io.context, id, method, url, body, etag, offset, scope);
    return id;
}
static void cancel(ot_library *l, uint64_t *id) {
    uint64_t old = *id;
    *id = 0;
    if (old && l->io.cancel)
        l->io.cancel(l->io.context, old);
}
static bool runtime_current(const ot_library *l) {
    return l->selected > 0 && l->selected == l->runtime_model && l->runtime_child == l->engine.child &&
           !l->engine.stopping && !strcmp(l->engine_state, "ready");
}
static bool runtime_buffers(ot_library *l, ot_runtime_info *result) {
    const ot_runtime_info *buffers = &l->engine.runtime;
    if (l->engine.buffers.limited || !buffers->weights || !buffers->compute || !buffers->context ||
        (result->context && buffers->context != result->context) || result->kind == OT_RUNTIME_ASLEEP)
        return false;
    unsigned other_models = result->other_models;
    ot_runtime_kind kind = result->kind;
    char digest[96];
    memcpy(digest, result->model_digest, sizeof digest);
    *result = *buffers;
    memcpy(result->model_digest, digest, sizeof digest);
    if (result->kind == OT_RUNTIME_UNKNOWN)
        result->kind = kind;
    result->other_models = other_models;
    sqlite3_stmt *q = query(l, "SELECT bytes FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, l->selected);
    if (q && sqlite3_step(q) == SQLITE_ROW)
        ot_runtime_reconcile(result, (uint64_t)sqlite3_column_int64(q, 0));
    sqlite3_finalize(q);
    return true;
}
static void memory_record(ot_library *l);
static void runtime_poll(ot_library *l) {
    bool ready = l->selected > 0 && !l->engine.stopping && !strcmp(l->engine_state, "ready");
    if (l->runtime_model != l->selected || l->runtime_child != l->engine.child || l->runtime_ready != ready) {
        cancel(l, &l->runtime_id);
        free(l->runtime_body);
        l->runtime_body = NULL;
        l->runtime_size = l->runtime_capacity = 0;
        l->runtime = (ot_runtime_info){0};
        l->memory_recorded = (ot_runtime_info){0};
        l->runtime_model = l->selected;
        l->runtime_child = l->engine.child;
        l->runtime_ready = ready;
        l->runtime_engine_revision = 0;
        l->runtime_next = 0;
        l->memory_revision++;
    }
    if (!ready)
        return;
    if (!l->running_ollama && l->runtime_engine_revision != l->engine.runtime_revision) {
        l->runtime_engine_revision = l->engine.runtime_revision;
        l->runtime = l->engine.runtime;
        runtime_buffers(l, &l->runtime);
        memory_record(l);
        l->memory_revision++;
    }
    if (l->runtime_id) {
        if (now() - l->runtime_started >= 2)
            ot_library_end(l, l->runtime_id, "engine_timeout");
        return;
    }
    if (now() < l->runtime_next)
        return;
    free(l->runtime_body);
    l->runtime_body = NULL;
    l->runtime_size = l->runtime_capacity = 0;
    const ot_connection *connection = ot_configuration(l->app);
    char url[512];
    int n = snprintf(url, sizeof url, "%s/%s", connection->endpoint, l->running_ollama ? "api/ps" : "props");
    if (n < 0 || (size_t)n >= sizeof url)
        return;
    l->runtime_started = now();
    l->runtime_next = now() + 5;
    l->runtime_id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_LOCAL);
}
static bool save_model(ot_library *l, const char *name, int kind, const char *reference,
                       const ot_model_info *m, int64_t managed, bool restore) {
    struct stat st = {0};
    if ((managed || kind == 1) && lstat(reference, &st))
        return false;
    yyjson_mut_doc *metadata_doc = yyjson_mut_doc_new(NULL);
    if (!metadata_doc)
        return false;
    yyjson_mut_val *metadata = yyjson_mut_obj(metadata_doc);
    bool metadata_ok = metadata != NULL;
    if (managed) {
        sqlite3_stmt *job = query(l, "SELECT metadata FROM downloads WHERE id=?");
        if (job)
            sqlite3_bind_int64(job, 1, managed);
        if (job && sqlite3_step(job) == SQLITE_ROW) {
            const char *json = col(job, 0);
            yyjson_doc *stored = yyjson_read(json, strlen(json), 0);
            if (stored && yyjson_is_obj(yyjson_doc_get_root(stored)))
                metadata = yyjson_val_mut_copy(metadata_doc, yyjson_doc_get_root(stored));
            metadata_ok = metadata_ok && metadata != NULL;
            yyjson_doc_free(stored);
        }
        sqlite3_finalize(job);
        const char *transient[] = {"replaceId",    "replacePath", "replaceDevice",
                                   "replaceInode", "replaceJob",  "pullProgress", "setup", "setupBorrowed", "pullStore", "pullCleanup", "discard"};
        for (unsigned i = 0; metadata && i < sizeof transient / sizeof *transient; i++)
            yyjson_mut_obj_remove_key(metadata, transient[i]);
    }
    /* Tags/manifests report presence and bytes, not model geometry. */
    if (kind == 1 || *m->architecture) {
        yyjson_mut_obj_remove_key(metadata, "family");
        metadata_ok =
            metadata_ok && yyjson_mut_obj_add_strcpy(metadata_doc, metadata, "family", m->architecture);
        const char *memory_keys[] = {"tensorBytes", "keyLength", "valueLength", "expertCount", "expertsUsed"};
        uint64_t memory_values[] = {m->tensor_bytes, m->key_length, m->value_length, m->experts,
                                    m->experts_used};
        for (unsigned i = 0; i < 5; i++) {
            yyjson_mut_obj_remove_key(metadata, memory_keys[i]);
            metadata_ok = metadata_ok &&
                          yyjson_mut_obj_add_uint(metadata_doc, metadata, memory_keys[i], memory_values[i]);
        }
        yyjson_mut_obj_remove_key(metadata, "complexCache");
        metadata_ok =
            metadata_ok && yyjson_mut_obj_add_bool(metadata_doc, metadata, "complexCache", m->complex_cache);
    }
    if (restore)
        metadata_ok = metadata_ok && yyjson_mut_obj_add_bool(metadata_doc, metadata, "fileChanged", false);
    if (kind == 1) {
        yyjson_mut_obj_remove_key(metadata, "modelName");
        yyjson_mut_obj_remove_key(metadata, "quantization");
        metadata_ok = metadata_ok &&
                      yyjson_mut_obj_add_strcpy(metadata_doc, metadata, "modelName", m->name) &&
                      yyjson_mut_obj_add_strcpy(metadata_doc, metadata, "quantization", m->quantization);
        metadata_ok =
            metadata_ok &&
            yyjson_mut_obj_add_sint(metadata_doc, metadata, "fileModified", st.st_mtimespec.tv_sec) &&
            yyjson_mut_obj_add_sint(metadata_doc, metadata, "fileModifiedNsec", st.st_mtimespec.tv_nsec);
    }
    yyjson_mut_doc_set_root(metadata_doc, metadata);
    char *metadata_json = metadata_ok ? yyjson_mut_write(metadata_doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(metadata_doc);
    if (!metadata_json)
        return false;
    sqlite3_stmt *q = query(
        l, "INSERT INTO "
           "models(name,kind,reference,bytes,context,embedding,layers,heads,kv_heads,managed,device,inode,"
           "metadata,origin) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?15) ON CONFLICT(kind,reference) DO UPDATE SET "
           "name=excluded.name,bytes=CASE WHEN excluded.bytes>0 THEN excluded.bytes ELSE models.bytes END,"
           "context=CASE WHEN excluded.kind=1 OR excluded.context>0 THEN excluded.context ELSE models.context END,"
           "embedding=CASE WHEN excluded.kind=1 OR excluded.embedding>0 THEN excluded.embedding ELSE models.embedding END,"
           "layers=CASE WHEN excluded.kind=1 OR excluded.layers>0 THEN excluded.layers ELSE models.layers END,"
           "heads=CASE WHEN excluded.kind=1 OR excluded.heads>0 THEN excluded.heads ELSE models.heads END,"
           "kv_heads=CASE WHEN excluded.kind=1 OR excluded.kv_heads>0 THEN excluded.kv_heads ELSE models.kv_heads END,"
           "managed=CASE WHEN excluded.managed>0 THEN excluded.managed ELSE models.managed END,"
           "device=CASE WHEN excluded.managed>0 OR ?14 THEN excluded.device ELSE models.device END,"
           "inode=CASE WHEN excluded.managed>0 OR ?14 THEN excluded.inode ELSE models.inode END,"
           "metadata=CASE WHEN ?14 AND excluded.managed=0 AND "
           "(models.device<>excluded.device OR models.inode<>excluded.inode OR models.bytes<>excluded.bytes "
           "OR coalesce(json_extract(models.metadata,'$.fileModified'),0)<>"
           "coalesce(json_extract(excluded.metadata,'$.fileModified'),0) OR "
           "coalesce(json_extract(models.metadata,'$.fileModifiedNsec'),0)<>"
           "coalesce(json_extract(excluded.metadata,'$.fileModifiedNsec'),0)) THEN "
           "json_patch(json_remove(models.metadata,'$.checksum','$.sourceURL','$.revision','$.license',"
           "'$.quantization','$.parameterSize','$.cpuQualification','$.memoryObservation',"
           "'$.thinking','$.thinkingKnown','$.chatSupported'),excluded.metadata) "
           "WHEN excluded.managed=0 AND NOT ?14 THEN json_patch(models.metadata,"
           "json_remove(excluded.metadata,'$.fileModified','$.fileModifiedNsec')) ELSE "
           "json_patch(models.metadata,excluded.metadata) END,available=1,"
           "origin=CASE WHEN excluded.managed>0 OR (?14 AND models.managed=0) THEN excluded.origin ELSE "
           "models.origin END,"
           "hidden=CASE WHEN ?14 THEN 0 ELSE models.hidden END");
    if (!q) {
        free(metadata_json);
        return false;
    }
    sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(q, 2, kind);
    sqlite3_bind_text(q, 3, reference, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 4, (sqlite3_int64)m->bytes);
    sqlite3_bind_int64(q, 5, m->context);
    sqlite3_bind_int64(q, 6, m->embedding);
    sqlite3_bind_int64(q, 7, m->layers);
    sqlite3_bind_int64(q, 8, m->heads);
    sqlite3_bind_int64(q, 9, m->kv_heads);
    sqlite3_bind_int64(q, 10, managed);
    sqlite3_bind_int64(q, 11, (sqlite3_int64)st.st_dev);
    sqlite3_bind_int64(q, 12, (sqlite3_int64)st.st_ino);
    sqlite3_bind_text(q, 13, metadata_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(q, 14, restore);
    sqlite3_bind_text(q, 15,
                      kind == 2 ? "ollama"
                      : managed ? "overtone"
                      : restore ? "manual"
                                : "cache",
                      -1, SQLITE_STATIC);
    free(metadata_json);
    return done(q);
}
static bool import_file(ot_library *l, const char *path, int64_t managed, const char *name) {
    char canonical[4096];
    ot_model_info info;
    if (!realpath(path, canonical) || !ot_gguf(canonical, &info)) {
        changed(l, "invalid_gguf");
        return false;
    }
    const char *filename = strrchr(canonical, '/');
    filename = filename ? filename + 1 : canonical;
    bool ok = save_model(l,
                         *name        ? name
                         : *info.name ? info.name
                                      : filename,
                         1, canonical, &info, managed, true);
    changed(l, ok ? NULL : "storage_write");
    return ok;
}
static bool save_job(ot_library *l, const char *state, const char *error) {
    bool finished = !strcmp(state, "complete") || !strcmp(state, "cancelled");
    yyjson_mut_doc *progress = NULL;
    char *json = NULL;
    if (!finished && !strncmp(l->url, "ollama:", 7) && l->pull_layer_count) {
        progress = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *rows = progress ? yyjson_mut_arr(progress) : NULL;
        if (progress)
            yyjson_mut_doc_set_root(progress, rows);
        for (unsigned i = 0; rows && i < l->pull_layer_count; i++) {
            yyjson_mut_val *row = yyjson_mut_obj(progress);
            yyjson_mut_obj_add_strcpy(progress, row, "digest", l->pull_layers[i].digest);
            yyjson_mut_obj_add_uint(progress, row, "received", l->pull_layers[i].received);
            yyjson_mut_obj_add_uint(progress, row, "total", l->pull_layers[i].total);
            yyjson_mut_arr_append(rows, row);
        }
        json = progress ? yyjson_mut_write(progress, 0, NULL) : NULL;
        if (!json) {
            yyjson_mut_doc_free(progress);
            changed(l, "memory_limit");
            return false;
        }
    }
    sqlite3_stmt *q =
        query(l, "UPDATE downloads SET received=?,total=?,etag=?,state=?,error=?,updated=unixepoch(),"
                 "metadata=CASE WHEN ?7 THEN json_remove(metadata,'$.pullProgress') WHEN ?8 IS NOT NULL THEN "
                 "json_set(metadata,'$.pullProgress',json(?8)) ELSE metadata END WHERE id=?6");
    if (q) {
        sqlite3_bind_int64(q, 1, (sqlite3_int64)l->received);
        sqlite3_bind_int64(q, 2, (sqlite3_int64)l->total);
        sqlite3_bind_text(q, 3, l->etag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 4, state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 5, error ? error : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 6, l->job);
        sqlite3_bind_int(q, 7, finished);
        if (json)
            sqlite3_bind_text(q, 8, json, -1, SQLITE_TRANSIENT);
    }
    bool ok = done(q);
    free(json);
    yyjson_mut_doc_free(progress);
    l->checkpoint = l->received;
    l->checkpoint_time = now();
    changed(l, ok ? error : "storage_write");
    l->activity_error = ok && error && *error;
    return ok;
}
static void transfer_progress(ot_library *l) {
    double time = now();
    if (l->received < l->rate_bytes) {
        l->rate_time = time;
        l->rate_bytes = l->received;
        l->bytes_per_second = 0;
    }
    double elapsed = time - l->rate_time;
    if (elapsed >= .2 && l->received >= l->rate_bytes) {
        double sample = (double)(l->received - l->rate_bytes) / elapsed;
        l->bytes_per_second = l->bytes_per_second > 0 ? .7 * l->bytes_per_second + .3 * sample : sample;
        l->rate_time = time;
        l->rate_bytes = l->received;
    }
    if (time - l->checkpoint_time >= 1)
        save_job(l, "downloading", NULL);
    else
        l->revision++;
}
static bool flush_transfer(ot_library *l) {
    size_t offset = 0;
    while (offset < l->buffered) {
        ssize_t n = write(l->file, l->transfer_buffer + offset, l->buffered - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            l->buffered = 0;
            return false;
        }
        CC_SHA256_Update(&l->digest, l->transfer_buffer + offset, (CC_LONG)n);
        offset += (size_t)n;
    }
    l->buffered = 0;
    return true;
}
static void stop_transfer(ot_library *l) {
    bool active = l->transfer_id || l->download_preparing || l->file >= 0 ||
                  (l->working && (l->worker_kind == 1 || l->worker_kind == 5 || l->worker_kind == 6 || l->worker_kind == 7));
    cancel(l, &l->prepare_id);
    l->download_preparing = false;
    if (l->working && (l->worker_kind == 1 || l->worker_kind == 5 || l->worker_kind == 6 || l->worker_kind == 7))
        atomic_store(&l->worker_cancel, true);
    cancel(l, &l->transfer_id);
    if (l->pull_job && l->pull_job==l->job) {
        ot_engine_stop(&l->download_engine);l->download_uri[0]=0;
    }
    bool flushed = true;
    if (l->file >= 0) {
        flushed = flush_transfer(l) && fsync(l->file) == 0;
        close(l->file);
        l->file = -1;
    }
    if (l->job && active)
        save_job(l, "paused", flushed ? NULL : "file_operation");
}
static bool remove_partial(ot_library *l, int64_t id) {
    char part[4096], final[4096];
    struct stat source, target;
    if (!path_for(l, id, ".part", part) || !path_for(l, id, ".gguf", final))
        return false;
    if (lstat(part, &source))
        return errno == ENOENT;
    /* A failed registration can leave a hard link to the verified partial.
     * Remove it only if it is the same file and no installed model references it. */
    if (!lstat(final, &target) && S_ISREG(source.st_mode) && source.st_dev == target.st_dev &&
        source.st_ino == target.st_ino) {
        sqlite3_stmt *q = query(l, "SELECT EXISTS(SELECT 1 FROM models WHERE reference=?)");
        if (q)
            sqlite3_bind_text(q, 1, final, -1, SQLITE_TRANSIENT);
        bool known = q && sqlite3_step(q) == SQLITE_ROW;
        bool referenced = known && sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
        if (!known || (!referenced && unlink(final)))
            return false;
    }
    if (unlink(part))
        return false;
    int directory = open(l->directory, O_RDONLY);
    bool ok = directory >= 0 && fsync(directory) == 0;
    if (directory >= 0)
        close(directory);
    return ok;
}
static void *hash_partial(void *arg);
static bool cleanup_job(ot_library *l, int64_t id) {
    sqlite3_stmt *q=query(l,"SELECT coalesce(json_extract(metadata,'$.discard'),0),state FROM downloads WHERE id=?");
    if (q) sqlite3_bind_int64(q,1,id);
    bool found=q && sqlite3_step(q)==SQLITE_ROW, discard=found && sqlite3_column_int(q,0)!=0;
    bool complete=found && !strcmp(col(q,1),"complete");
    sqlite3_finalize(q);
    if (!found || (!complete && !remove_partial(l,id))) return false;
    q=query(l,discard ? "DELETE FROM downloads WHERE id=?" :
        "UPDATE downloads SET metadata=json_remove(metadata,'$.pullCleanup') WHERE id=?");
    if (q) sqlite3_bind_int64(q,1,id);
    if (!done(q)) return false;
    q=query(l,"SELECT EXISTS(SELECT 1 FROM downloads WHERE json_extract(metadata,'$.pullCleanup')=1)");
    bool ok=q && sqlite3_step(q)==SQLITE_ROW;
    if (ok) l->cleanup_pending=sqlite3_column_int(q,0)!=0;
    sqlite3_finalize(q);return ok;
}
static bool request_cleanup(ot_library *l,int64_t id,bool discard) {
    if (id==l->job) stop_transfer(l);
    sqlite3_stmt *q=query(l,"UPDATE downloads SET metadata=json_set(metadata,'$.pullCleanup',1,'$.discard',?2) WHERE id=?1");
    if (q) {sqlite3_bind_int64(q,1,id);sqlite3_bind_int(q,2,discard);}
    bool ok=done(q);l->cleanup_pending|=ok;return ok;
}
static void clean_downloads(ot_library *l) {
    if (!l->cleanup_pending || l->working || l->transfer_id || l->download_preparing) return;
    sqlite3_stmt *q=query(l,"SELECT id FROM downloads WHERE json_extract(metadata,'$.pullCleanup')=1 ORDER BY id LIMIT 1");
    bool found=q && sqlite3_step(q)==SQLITE_ROW;
    int64_t id=found ? sqlite3_column_int64(q,0) : 0;sqlite3_finalize(q);
    if (!found) {l->cleanup_pending=false;return;}
    if (l->pull_job==id && l->download_engine.pid>0) {ot_engine_stop(&l->download_engine);return;}
    char stage[4096];struct stat st;
    if (ot_pull_path(l->profile,id,stage) && !lstat(stage,&st)) {
        l->cleanup_job=id;l->worker_kind=8;
        atomic_store(&l->worker_done,false);atomic_store(&l->worker_cancel,false);
        if (!pthread_create(&l->worker,NULL,hash_partial,l)) {l->working=true;return;}
        changed(l,"worker_failed");l->cleanup_pending=false;return;
    }
    if (cleanup_job(l,id)) {if (l->job==id) l->job=0;l->revision++;}
    else {changed(l,"file_operation");l->cleanup_pending=false;}
}
/* Only changed files with a recorded digest are re-read. Mount device numbers are
 * transient; neither a new device number nor an identical size proves corruption.
 * Verify the descriptor and its identity again after hashing before adopting it. */
static bool verify_registered(ot_library *l, registered_file *file) {
    if (!sha(file->checksum) || (uint64_t)file->observed.st_size != file->bytes)
        return false;
    int fd = open(file->path, O_RDONLY | O_NOFOLLOW);
    unsigned char *buffer = malloc(1024 * 1024);
    struct stat before, after, named;
    bool ok = fd >= 0 && buffer && !fstat(fd, &before) && S_ISREG(before.st_mode) &&
              before.st_dev == file->observed.st_dev && before.st_ino == file->observed.st_ino;
    CC_SHA256_CTX digest;
    CC_SHA256_Init(&digest);
    uint64_t bytes = 0;
    while (ok && !atomic_load(&l->worker_cancel)) {
        ssize_t n = read(fd, buffer, 1024 * 1024);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        CC_SHA256_Update(&digest, buffer, (CC_LONG)n);
        bytes += (uint64_t)n;
        if (bytes > file->bytes) {
            ok = false;
            break;
        }
    }
    ok = ok && !atomic_load(&l->worker_cancel) && bytes == file->bytes && !fstat(fd, &after) &&
         before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
         before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
         before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
    ok = ok && !lstat(file->path, &named) && S_ISREG(named.st_mode) && named.st_dev == after.st_dev &&
         named.st_ino == after.st_ino && named.st_size == after.st_size &&
         named.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
         named.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
    unsigned char value[32];
    char hash[65];
    CC_SHA256_Final(value, &digest);
    for (unsigned i = 0; i < 32; i++)
        snprintf(hash + i * 2, 3, "%02x", value[i]);
    if (fd >= 0)
        close(fd);
    free(buffer);
    return ok && !strcasecmp(hash, file->checksum);
}
/* A replacement commits the reference before retiring the old managed file.
 * Persist its identity in the job so a crash can only leave a reclaimable copy,
 * never an installed entry pointing to a half-downloaded or missing model. */
static bool cleanup_replacement(ot_library *l, int64_t id) {
    sqlite3_stmt *q = query(l, "SELECT metadata FROM downloads WHERE id=? AND state='complete'");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    bool found = q && sqlite3_step(q) == SQLITE_ROW;
    const char *text = found ? col(q, 0) : "{}";
    yyjson_doc *d = yyjson_read(text, strlen(text), 0);
    sqlite3_finalize(q);
    yyjson_val *v = yyjson_doc_get_root(d);
    bool ok = d != NULL;
    int64_t old_job = yyjson_get_sint(yyjson_obj_get(v, "replaceJob"));
    const char *old_path = str(v, "replacePath");
    if (ok && old_job > 0 && *old_path) {
        char owned[4096];
        struct stat st;
        q = query(l, "SELECT EXISTS(SELECT 1 FROM models WHERE reference=?)");
        if (q)
            sqlite3_bind_text(q, 1, old_path, -1, SQLITE_TRANSIENT);
        ok = q && sqlite3_step(q) == SQLITE_ROW && !sqlite3_column_int(q, 0) &&
             path_for(l, old_job, ".gguf", owned) && !strcmp(owned, old_path);
        sqlite3_finalize(q);
        if (ok && lstat(old_path, &st))
            ok = errno == ENOENT;
        else if (ok) {
            ok = S_ISREG(st.st_mode) &&
                 st.st_dev == (dev_t)yyjson_get_sint(yyjson_obj_get(v, "replaceDevice")) &&
                 st.st_ino == (ino_t)yyjson_get_sint(yyjson_obj_get(v, "replaceInode")) && !unlink(old_path);
            int dir = ok ? open(l->directory, O_RDONLY) : -1;
            ok = ok && dir >= 0 && !fsync(dir);
            if (dir >= 0)
                close(dir);
        }
    }
    q = query(l, ok ? "UPDATE downloads SET metadata=json_remove(metadata,'$.replaceId','$.replacePath',"
                      "'$.replaceDevice','$.replaceInode','$.replaceJob'),error='' WHERE id=?"
                    : "UPDATE downloads SET error='replacement_cleanup' WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    bool saved = done(q);
    yyjson_doc_free(d);
    return ok && saved;
}
static void *hash_partial(void *arg) {
    ot_library *l = arg;
    if (l->worker_kind == 6 || l->worker_kind == 7 || l->worker_kind == 8) {
        l->worker_ok=l->worker_kind==6 ? ot_pull_prepare(l->profile,l->job,&l->worker_cancel,l->worker_path) :
            l->worker_kind==7 ? ot_pull_publish(l->profile,l->job,&l->worker_cancel) :
                               ot_pull_discard(l->profile,l->cleanup_job);
        atomic_store_explicit(&l->worker_done,true,memory_order_release);return NULL;
    }
    if (l->worker_kind == 5) {
        l->worker_ok =
            ot_package_install(l->package, l->components, l->worker_path, &l->worker_cancel, l->worker_path);
        atomic_store_explicit(&l->worker_done, true, memory_order_release);
        return NULL;
    }
    if (l->worker_kind == 4) {
        l->worker_ok = ot_engine_validate(l->worker_path, l->validation_kind == 2);
        atomic_store_explicit(&l->worker_done, true, memory_order_release);
        return NULL;
    }
    if (l->worker_kind == 3) {
        struct stat after;
        l->worker_ok = !lstat(l->import_path, &l->import_identity) && S_ISREG(l->import_identity.st_mode) &&
                       ot_gguf(l->import_path, &l->import_info) && !lstat(l->import_path, &after) &&
                       same_file(&l->import_identity, &after);
        atomic_store_explicit(&l->worker_done, true, memory_order_release);
        return NULL;
    }
    if (l->worker_kind == 2) {
        l->inventory = ot_inventory_read(&l->worker_cancel);
        l->inventory.limited |= l->registered_limited;
        double deadline = now() + 5;
        for (size_t i = 0; i < l->registered_count && !atomic_load(&l->worker_cancel); i++) {
            if (now() >= deadline) {
                l->inventory.limited = true;
                break;
            }
            registered_file *file = &l->registered[i];
            file->available = file->manifest
                                  ? ot_inventory_manifest(file->path, file->root_length)
                                  : !lstat(file->path, &file->observed) && S_ISREG(file->observed.st_mode);
            file->replaced =
                !file->manifest && file->available && file->inode > 0 &&
                (file->unverified || file->device != file->observed.st_dev ||
                 file->inode != file->observed.st_ino ||
                 (file->modified && (file->modified != file->observed.st_mtimespec.tv_sec ||
                                     file->modified_nsec != file->observed.st_mtimespec.tv_nsec)) ||
                 file->bytes != (uint64_t)file->observed.st_size);
            file->verified = file->replaced && verify_registered(l, file);
            if (file->verified)
                file->replaced = false;
            file->checked = true;
        }
        atomic_store_explicit(&l->worker_done, true, memory_order_release);
        return NULL;
    }
    CC_SHA256_Init(&l->worker_digest);
    l->worker_bytes = 0;
    l->worker_ok = false;
    int fd = open(l->worker_path, O_RDONLY | O_NOFOLLOW);
    unsigned char *buffer = malloc(1024 * 1024);
    if (fd >= 0 && buffer) {
        struct stat st;
        l->worker_ok = !fstat(fd, &l->partial_identity) && S_ISREG(l->partial_identity.st_mode) &&
                       l->partial_identity.st_size >= 0;
        while (l->worker_ok && !atomic_load(&l->worker_cancel)) {
            ssize_t n = read(fd, buffer, 1024 * 1024);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                l->worker_ok = false;
                break;
            }
            if (!n)
                break;
            CC_SHA256_Update(&l->worker_digest, buffer, (CC_LONG)n);
            l->worker_bytes += (uint64_t)n;
        }
        l->worker_ok = l->worker_ok && !fstat(fd, &st) && same_file(&l->partial_identity, &st) &&
                       l->worker_bytes == (uint64_t)st.st_size;
    }
    if (fd >= 0)
        close(fd);
    free(buffer);
    atomic_store_explicit(&l->worker_done, true, memory_order_release);
    return NULL;
}
static bool engine_path(ot_library *l, int kind, char path[4096]);
static void prepare_pending(ot_library *l);
static void engine_failure(ot_library *l, const char *code);
static void download_probe(ot_library *l) {
    free(l->prepare);
    l->prepare = NULL;
    l->prepare_size = l->prepare_capacity = 0;
    char url[512];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/api/tags",
             l->download_external ? 11434 : l->download_engine.port);
    l->prepare_id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_LOCAL);
    l->download_next = now() + 1;
}
static void download_preparation_failed(ot_library *l, const char *code) {
    cancel(l, &l->prepare_id);
    l->download_preparing = false;
    ot_engine_stop(&l->download_engine);
    if (l->removing) {
        l->removing = 0;
        changed(l, code);
    } else if (l->job)
        save_job(l, "paused", code);
    else
        changed(l, code);
}
static void finish_download(ot_library *l);
static void finish_pull(ot_library *l);
static void transfer(ot_library *l) {
    if (l->removing && *l->download_uri) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
        yyjson_mut_doc_set_root(d, v);
        bool ok = v && yyjson_mut_obj_add_strcpy(d, v, "model", l->remove_name);
        char *body = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
        yyjson_mut_doc_free(d);
        if (!body) {
            download_preparation_failed(l, "memory_limit");
            return;
        }
        char url[512];
        snprintf(url, sizeof url, "%s/api/delete", l->download_uri);
        l->remove_id = request(l, "DELETE", url, body, NULL, 0, OT_NETWORK_LOCAL);
        free(body);
        changed(l, NULL);
        return;
    }
    if (!ot_network_allowed(ot_get_preferences(l->app), OT_NETWORK_MODELS) && !strncmp(l->url, "ollama:", 7)) {
        l->download_preparing = false;
        save_job(l, "paused", "internet_disabled");
        return;
    }
    if (!strncmp(l->url, "ollama:", 7)) {
        if (!*l->download_uri) {
            if (!l->download_preparing) {
                l->download_preparing = true;
                l->download_external = true;
                l->download_started = now();
                save_job(l, "preparing", NULL);
                download_probe(l);
            }
            return;
        }
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (!d)
            return;
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, v);
        yyjson_mut_obj_add_strcpy(d, v, "model", l->url + 7);
        yyjson_mut_obj_add_bool(d, v, "stream", true);
        char *body = yyjson_mut_write(d, 0, NULL);
        yyjson_mut_doc_free(d);
        char url[512];
        snprintf(url, sizeof url, "%s/api/pull", l->download_uri);
        l->pull_size = 0;
        l->pull_done = false;
        l->transfer_id = request(l, "POST", url, body, NULL, 0, OT_NETWORK_MODELS);
        free(body);
        save_job(l, "downloading", NULL);
        return;
    }
    char path[4096];
    if (!path_for(l, l->job, ".part", path)) {
        save_job(l, "paused", "invalid_path");
        return;
    }
    l->file = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    l->buffered = 0;
    if (l->file < 0) {
        save_job(l, "paused", "file_operation");
        return;
    }
    struct stat st;
    if (fstat(l->file, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != l->received ||
        (l->partial_hashed && !same_file(&l->partial_identity, &st)) ||
        lseek(l->file, (off_t)l->received, SEEK_SET) < 0) {
        close(l->file);
        l->file = -1;
        save_job(l, "paused", "partial_changed");
        return;
    }
    l->partial_hashed = false;
    if (l->total && l->received == l->total) {
        finish_download(l);
        return;
    }
    if (!ot_network_allowed(ot_get_preferences(l->app), l->package ? OT_NETWORK_ENGINES : OT_NETWORK_MODELS)) {
        close(l->file);
        l->file = -1;
        save_job(l, "paused", "internet_disabled");
        return;
    }
    l->transfer_id = request(l, "GET", l->url, NULL, l->etag, l->received,
        ot_package_for_download(l->url, l->checksum) ? OT_NETWORK_ENGINES : OT_NETWORK_MODELS);
    save_job(l, "downloading", NULL);
}
static void restore_pull_progress(ot_library *l, const char *json) {
    l->pull_layer_count = 0;
    l->pull_aggregate = l->pull_resuming = false;
    if (strlen(json) > 32768)
        return;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_doc_get_root(doc);
    bool ok = yyjson_is_arr(rows) && yyjson_arr_size(rows) <= 128;
    for (size_t i = 0; ok && i < yyjson_arr_size(rows); i++) {
        yyjson_val *row = yyjson_arr_get(rows, i), *received = yyjson_obj_get(row, "received"),
                   *total = yyjson_obj_get(row, "total");
        const char *digest = str(row, "digest");
        bool aggregate = !strcmp(digest, "model");
        ok = (sha(digest) || (aggregate && yyjson_arr_size(rows) == 1)) && yyjson_is_uint(received) &&
             yyjson_is_uint(total) && yyjson_get_uint(received) <= yyjson_get_uint(total) &&
             yyjson_get_uint(total) <= INT64_MAX;
        for (size_t j = 0; ok && j < i; j++)
            ok = strcmp(l->pull_layers[j].digest, digest) != 0;
        if (ok) {
            copy(l->pull_layers[i].digest, sizeof l->pull_layers[i].digest, digest);
            l->pull_layers[i].received = yyjson_get_uint(received);
            l->pull_layers[i].total = yyjson_get_uint(total);
            l->pull_layers[i].resumed = true;
            l->pull_aggregate = aggregate;
            l->pull_layer_count++;
        }
    }
    if (!ok)
        l->pull_layer_count = 0;
    l->pull_resuming = l->pull_layer_count != 0;
    yyjson_doc_free(doc);
}
static void resume(ot_library *l, int64_t id) {
    if (l->cleanup_pending) {l->queue_pending=true;changed(l,NULL);return;}
    if (l->transfer_id || l->working || l->download_preparing || l->removing || l->download_engine.stopping) {
        changed(l, "busy");
        return;
    }
    sqlite3_stmt *q = query(l, "SELECT url,name,checksum,total,etag,state,error,received,"
                               "json_extract(metadata,'$.pullProgress'),json_extract(metadata,'$.expectedBytes'),"
                               "coalesce(json_extract(metadata,'$.pullStore'),0),coalesce(json_extract(metadata,'$.pullCleanup'),0) FROM downloads WHERE id=?");
    if (!q)
        return;
    sqlite3_bind_int64(q, 1, id);
    bool ok = sqlite3_step(q) == SQLITE_ROW && strcmp(col(q, 5), "complete") &&
              copy(l->url, sizeof l->url, col(q, 0)) && copy(l->job_name, sizeof l->job_name, col(q, 1)) &&
              copy(l->checksum, sizeof l->checksum, col(q, 2)) && copy(l->etag, sizeof l->etag, col(q, 4));
    l->total = ok ? (uint64_t)sqlite3_column_int64(q, 3) : 0;
    l->received = ok && !strncmp(l->url, "ollama:", 7) ? (uint64_t)sqlite3_column_int64(q, 7) : 0;
    l->pull_expected = ok ? (uint64_t)sqlite3_column_int64(q, 9) : 0;
    restore_pull_progress(l, ok ? col(q, 8) : "");
    bool register_only = ok && !strncmp(l->url, "ollama:", 7) && !strcmp(col(q, 6), "storage_write") &&
                         l->total > 0 && l->total == (uint64_t)sqlite3_column_int64(q, 7);
    bool restart_invalid = ok && strncmp(l->url, "ollama:", 7) &&
                           (!strcmp(col(q, 6), "checksum_mismatch") || !strcmp(col(q, 6), "invalid_gguf"));
    bool staged=ok && sqlite3_column_int(q,10)!=0, cleanup=ok && sqlite3_column_int(q,11)!=0;
    sqlite3_finalize(q);
    if (!ok) {
        changed(l, "invalid_job");
        return;
    }
    if (cleanup || (l->download_engine.pid>0 && l->pull_job!=(staged ? id : 0))) {
        if (cleanup) l->cleanup_pending=true;
        else {ot_engine_stop(&l->download_engine);l->download_uri[0]=0;}
        q=query(l,"UPDATE downloads SET state='queued' WHERE id=?");
        if (q) sqlite3_bind_int64(q,1,id);
        l->queue_pending=true;changed(l,done(q) ? NULL : "storage_write");return;
    }
    l->job = id;l->pull_job=staged ? id : 0;l->pull_published=false;
    l->package = ot_package_for_download(l->url, l->checksum);
    if (register_only) {
        l->received = l->total;
        finish_pull(l);
        return;
    }
    if (staged && l->download_engine.pid<=0) {
        l->worker_kind=6;atomic_store(&l->worker_done,false);atomic_store(&l->worker_cancel,false);
        if (!pthread_create(&l->worker,NULL,hash_partial,l)) {l->working=true;save_job(l,"preparing",NULL);}
        else save_job(l,"paused","worker_failed");
        return;
    }
    l->partial_hashed = false;
    l->rate_time = now();
    l->rate_bytes = l->received;
    l->bytes_per_second = 0;
    CC_SHA256_Init(&l->digest);
    char path[4096];
    if (!path_for(l, id, ".part", path))
        return;
    if (restart_invalid) {
        if (!remove_partial(l, id)) {
            save_job(l, "paused", "file_operation");
            return;
        }
        l->etag[0] = 0;
    }
    struct stat st;
    bool present = !lstat(path, &st);
    if (!ot_network_allowed(ot_get_preferences(l->app), l->package ? OT_NETWORK_ENGINES : OT_NETWORK_MODELS) &&
        (!strncmp(l->url, "ollama:", 7) || !present || !S_ISREG(st.st_mode) || st.st_size < 0 || !l->total ||
         (uint64_t)st.st_size != l->total)) {
        save_job(l, "paused", "internet_disabled");
        return;
    }
    if (present && S_ISREG(st.st_mode) && st.st_size > 0 &&
        (*l->etag || (l->total && (uint64_t)st.st_size == l->total))) {
        /* Re-hashing prepares a resume; it does not undo bytes already on disk
         * and is not the final verification step after the transfer. */
        l->received = (uint64_t)st.st_size;
        l->worker_kind = 1;
        copy(l->worker_path, sizeof l->worker_path, path);
        atomic_store(&l->worker_done, false);
        atomic_store(&l->worker_cancel, false);
        if (!pthread_create(&l->worker, NULL, hash_partial, l)) {
            l->working = true;
            save_job(l, "preparing", NULL);
            return;
        }
        save_job(l, "paused", "worker_failed");
        return;
    }
    if (lstat(path, &st) == 0 && unlink(path)) {
        save_job(l, "paused", "file_operation");
        return;
    }
    l->etag[0] = 0;
    transfer(l);
}
static void fail_transfer(ot_library *l, const char *error) {
    cancel(l, &l->transfer_id);
    if (l->file >= 0) {
        if (!flush_transfer(l) || fsync(l->file))
            error = "file_operation";
        close(l->file);
        l->file = -1;
    }
    save_job(l, "paused", error);
}
static bool append(char **buffer, size_t *length, size_t *capacity,
                   const void *bytes, size_t count, size_t limit) {
    if (*length > limit || count > limit - *length)
        return false;
    size_t needed = *length + count + 1;
    if (!*buffer || needed > *capacity) {
        size_t next = *buffer && *capacity ? *capacity : 256;
        if (next > limit + 1) next = limit + 1;
        while (next < needed)
            next = next > (limit + 1) / 2 ? limit + 1 : next * 2;
        char *p = realloc(*buffer, next);
        if (!p) return false;
        *buffer = p;
        *capacity = next;
    }
    memcpy(*buffer + *length, bytes, count);
    *length += count;
    (*buffer)[*length] = 0;
    return true;
}

static bool public_catalog(const char *mode) {
    return !strcmp(mode, "search") || !strcmp(mode, "variants") || !strcmp(mode, "ollamaSearch") ||
           !strcmp(mode, "ollamaVariants");
}
static void evict_catalog(ot_library *l, catalog_entry *entry) {
    l->catalog_cache_bytes -= entry->bytes;
    free(entry->url);
    free(entry->data);
    *entry = (catalog_entry){0};
}
static char *cached_catalog(ot_library *l, const char *url, bool refresh) {
    double time = now();
    for (unsigned i = 0; i < 8; i++) {
        catalog_entry *entry = &l->catalog_cache[i];
        if (entry->url && (entry->expires <= time || (refresh && !strcmp(entry->url, url))))
            evict_catalog(l, entry);
        if (entry->url && !strcmp(entry->url, url)) {
            char *result = strdup(entry->data);
            if (result) {
                entry->used = time;
                l->catalog_hits++;
                l->catalog_has_more = entry->has_more;
                return result;
            }
        }
    }
    l->catalog_misses++;
    return NULL;
}
static void cache_catalog(ot_library *l) {
    if (!l->query_url || !l->catalog || *l->error || !public_catalog(l->query_mode))
        return;
    size_t bytes = strlen(l->catalog) + strlen(l->query_url) + 2;
    if (bytes > 1024 * 1024)
        return;
    char *url = strdup(l->query_url), *data = strdup(l->catalog);
    if (!url || !data) {
        free(url);
        free(data);
        return;
    }
    catalog_entry *slot;
    for (;;) {
        slot = NULL;
        catalog_entry *oldest = NULL;
        for (unsigned i = 0; i < 8; i++) {
            catalog_entry *entry = &l->catalog_cache[i];
            if (entry->url && !strcmp(entry->url, url))
                evict_catalog(l, entry);
            if (!entry->url)
                slot = entry;
            else if (!oldest || entry->used < oldest->used)
                oldest = entry;
        }
        if (slot && l->catalog_cache_bytes + bytes <= 1024 * 1024)
            break;
        if (!oldest) {
            free(url);
            free(data);
            return;
        }
        evict_catalog(l, oldest);
    }
    *slot = (catalog_entry){url, data, bytes, now() + 300, now(), l->catalog_has_more};
    l->catalog_cache_bytes += bytes;
}
static void begin_query(ot_library *l, const char *mode, const char *url, bool refresh) {
    cancel(l, &l->query_id);
    free(l->query);
    l->query = NULL;
    l->query_size = l->query_capacity = 0;
    free(l->catalog);
    free(l->query_url);
    l->catalog_has_more = false;
    l->query_url = public_catalog(mode) ? strdup(url) : NULL;
    l->catalog = public_catalog(mode) ? cached_catalog(l, url, refresh) : NULL;
    copy(l->query_mode, sizeof l->query_mode, mode);
    if (!l->catalog)
        l->query_id = request(l, "GET", url, NULL, NULL, 0, public_catalog(mode) ? OT_NETWORK_CATALOGUES : OT_NETWORK_LOCAL);
    changed(l, NULL);
}
static void tags(ot_library *l, yyjson_val *root) {
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(root, "models"), i, n, v) {
        if (i == 1000)
            break;
        const char *name = str(v, "name");
        if (!ot_model_name(name))
            continue;
        ot_model_info m = {.bytes = yyjson_get_uint(yyjson_obj_get(v, "size"))};
        save_model(l, name, 2, name, &m, 0, false);
        sqlite3_stmt *route = query(
            l,
            "UPDATE models SET metadata=json_set(metadata,'$.cloud',json(?)) WHERE kind=2 AND reference=?");
        if (route) {
            sqlite3_bind_text(route, 1, *str(v, "remote_host") || *str(v, "remote_model") ? "true" : "false",
                              -1, SQLITE_STATIC);
            sqlite3_bind_text(route, 2, name, -1, SQLITE_TRANSIENT);
        }
        done(route);
        const char *digest = str(v, "digest");
        if (*digest && strlen(digest) <= 80) {
            sqlite3_stmt *q = query(
                l, "UPDATE models SET metadata=json_set(metadata,'$.digest',?) WHERE kind=2 AND reference=?");
            if (q) {
                sqlite3_bind_text(q, 1, digest, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(q, 2, name, -1, SQLITE_TRANSIENT);
            }
            done(q);
        }
    }
}
static void model_label(yyjson_mut_doc *d, yyjson_mut_val *item, const char *name, yyjson_val *metadata) {
    char title[257], quant[33], unused[257];
    ot_model_label(name, str(metadata, "quantization"), unused, quant);
    if (!*quant)
        ot_model_label(str(metadata, "fileName"), "", unused, quant);
    const char *actual = str(metadata, "modelName");
    ot_model_label(*actual ? actual : name, quant, title, quant);
    yyjson_mut_obj_add_strcpy(d, item, "displayName", title);
    yyjson_mut_obj_remove_key(item, "quantization");
    if (*quant)
        yyjson_mut_obj_add_strcpy(d, item, "quantization", quant);
    yyjson_mut_obj_add_str(d, item, "precision", ot_model_precision(quant));
}
/* Discovery and setup share metadata/usage interpretation. No model is loaded to filter it. */
static void catalog_facts(yyjson_mut_doc *d, yyjson_mut_val *item, yyjson_val *source, bool hf) {
    const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(item, "name"));
    const char *description = yyjson_mut_get_str(yyjson_mut_obj_get(item, "description"));
    ot_model_candidate model = {.kind = hf ? 1 : 2};
    copy(model.name, sizeof model.name, name ? name : "");
    copy(model.description, sizeof model.description, description ? description : model.name);
    model.context = (unsigned)yyjson_mut_get_uint(yyjson_mut_obj_get(item, "context"));
    model.bytes = yyjson_mut_get_uint(yyjson_mut_obj_get(item, "bytes"));
    model.reasoning = yyjson_mut_is_true(yyjson_mut_obj_get(item, "reasoning"));
    if (hf)
        ot_model_hf_facts(&model, source);
    ot_model_variant_facts(&model, model.name);
    if (model.context) {
        yyjson_mut_obj_remove_key(item, "context");
        yyjson_mut_obj_add_uint(d, item, "context", model.context);
    }
    if (*model.quantization) {
        yyjson_mut_obj_remove_key(item, "quantization");
        yyjson_mut_obj_add_strcpy(d, item, "quantization", model.quantization);
    }
    if (*model.quantization) {
        char title[257], quant[33];
        ot_model_label(model.name, model.quantization, title, quant);
        yyjson_mut_obj_remove_key(item, "displayName");
        yyjson_mut_obj_add_strcpy(d, item, "displayName", title);
    }
    if (model.parameters > 0)
        yyjson_mut_obj_add_real(d, item, "parameters", model.parameters);
    yyjson_mut_obj_remove_key(item, "precision");
    yyjson_mut_obj_add_str(d, item, "precision", ot_model_precision(model.quantization));
    yyjson_mut_obj_remove_key(item, "reasoning");
    yyjson_mut_obj_add_bool(d, item, "reasoning", model.reasoning);
    if (hf) {
        bool vision = !strcmp(str(source, "pipeline_tag"), "image-text-to-text"), tools = false,
             embedding = !strcmp(str(source, "pipeline_tag"), "feature-extraction");
        yyjson_val *tags = yyjson_obj_get(source, "tags");
        for (size_t i = 0; i < yyjson_arr_size(tags) && i < 256; i++) {
            const char *tag = yyjson_get_str(yyjson_arr_get(tags, i));
            if (!tag)
                continue;
            vision |= !strcmp(tag, "vision") || !strcmp(tag, "image-text-to-text");
            tools |=
                !strcmp(tag, "tool-calling") || !strcmp(tag, "tool-use") || !strcmp(tag, "function-calling");
            embedding |= !strcmp(tag, "sentence-transformers") || !strcmp(tag, "feature-extraction");
        }
        yyjson_mut_obj_add_bool(d, item, "vision", vision);
        yyjson_mut_obj_add_bool(d, item, "tools", tools);
        yyjson_mut_obj_add_bool(d, item, "embedding", embedding);
    }
    yyjson_mut_obj_add_uint(d, item, "purposes", ot_model_purposes(&model));
    const char *digest = yyjson_mut_get_str(yyjson_mut_obj_get(item, hf ? "checksum" : "catalogueDigest"));
    yyjson_mut_obj_add_bool(
        d, item, "verifiedChat",
        digest && (hf ? ot_model_file_verified(digest) : ot_model_verified(model.name, digest)));
}
static void catalog_resources(ot_library *l, yyjson_mut_doc *d, yyjson_mut_val *items) {
    const ot_resources *resources = ot_resources_get(l->app);
    ot_preferences preferences = *ot_get_preferences(l->app);
    preferences.setup_engines = 3;
    size_t i, n;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(items, i, n, item) {
        ot_model_candidate model = {.kind = !strcmp(l->query_mode, "variants") ? 1 : 2,
                                    .installed = 1,
                                    .bytes = yyjson_mut_get_uint(yyjson_mut_obj_get(item, "bytes"))};
        if (model.bytes && resources->memory_total) {
            yyjson_mut_obj_add_bool(d, item, "fitsMemory",
                                    ot_model_candidate_fits(&model, resources, &preferences));
            if (resources->disk_available)
                yyjson_mut_obj_add_bool(d, item, "fitsDisk",
                                        model.bytes < resources->disk_available &&
                                            (UINT64_C(2) << 30) < resources->disk_available - model.bytes);
        }
    }
}
static void catalog(ot_library *l, yyjson_val *root) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return;
    yyjson_mut_val *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, array);
    size_t i, n;
    yyjson_val *v;
    bool variants = !strcmp(l->query_mode, "variants");
    yyjson_val *items = variants ? yyjson_obj_get(root, "siblings") : root;
    l->catalog_has_more = !variants && l->query_limit < 100 && yyjson_arr_size(items) >= l->query_limit;
    const char *commit = str(root, "sha");
    bool revision = strlen(commit) == 40;
    for (size_t j = 0; revision && j < 40; j++)
        revision = isxdigit((unsigned char)commit[j]);
    yyjson_arr_foreach(items, i, n, v) {
        if (i >= 1000 || yyjson_mut_arr_size(array) >= 100)
            break;
        const char *name = str(v, variants ? "rfilename" : "id");
        size_t len = strlen(name);
        if (!len || len > 1024)
            continue;
        if (variants && (len < 5 || strcmp(name + len - 5, ".gguf") || !ot_model_primary_file(name)))
            continue;
        if (!variants) {
            uint64_t parameters = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(v, "gguf"), "total"));
            if (!parameters)
                parameters = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(v, "safetensors"), "total"));
            if (l->search_filter.max_parameters &&
                (!parameters || parameters > l->search_filter.max_parameters * UINT64_C(1000000000)))
                continue;
            if (*l->search_filter.task && strcmp(str(v, "pipeline_tag"), l->search_filter.task))
                continue;
        }
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_arr_append(array, item);
        yyjson_mut_obj_add_strcpy(d, item, "name", name);
        yyjson_val *source = variants ? root : v;
        const char *license = str(yyjson_obj_get(source, "cardData"), "license");
        if (*license && strlen(license) <= 128)
            yyjson_mut_obj_add_strcpy(d, item, "license", license);
        const char *modified = str(source, "lastModified");
        if (strlen(modified) <= 64)
            yyjson_mut_obj_add_strcpy(d, item, "modified", modified);
        for (const char **field = (const char *[]){"downloads", "likes", NULL}; *field; field++) {
            yyjson_val *number = yyjson_obj_get(source, *field);
            if (yyjson_is_uint(number))
                yyjson_mut_obj_add_uint(d, item, *field, yyjson_get_uint(number));
        }
        yyjson_val *tags = yyjson_obj_get(source, "tags");
        yyjson_mut_val *tag_list = yyjson_mut_arr(d);
        size_t ti, count;
        yyjson_val *tag;
        yyjson_arr_foreach(tags, ti, count, tag) {
            const char *text = yyjson_get_str(tag);
            if (text && strlen(text) <= 128 && yyjson_mut_arr_size(tag_list) < 32)
                yyjson_mut_arr_add_strcpy(d, tag_list, text);
            if (!*license && text && !strncmp(text, "license:", 8) && strlen(text) <= 136) {
                yyjson_mut_obj_add_strcpy(d, item, "license", text + 8);
                license = text + 8;
            }
        }
        if (!variants && *l->search_filter.license && strcmp(license, l->search_filter.license)) {
            yyjson_mut_arr_remove(array, yyjson_mut_arr_size(array) - 1);
            continue;
        }
        yyjson_mut_obj_add_val(d, item, "tags", tag_list);
        char source_url[2304];
        char *source_repo = encoded(variants ? l->repo : name, true);
        if (source_repo) {
            snprintf(source_url, sizeof source_url, "https://huggingface.co/%s", source_repo);
            yyjson_mut_obj_add_strcpy(d, item, "sourceURL", source_url);
            free(source_repo);
        }
        if (variants) {
            model_label(d, item, name, NULL);
            yyjson_mut_obj_add_strcpy(d, item, "repository", l->repo);
            yyjson_val *lfs = yyjson_obj_get(v, "lfs");
            const char *hash = str(lfs, "sha256");
            if (!*hash)
                hash = str(lfs, "oid");
            char *repo = encoded(l->repo, true), *file = encoded(name, true), url[8193];
            yyjson_mut_obj_add_strcpy(d, item, "revision", commit);
            if (repo && file && revision && sha(hash)) {
                snprintf(url, sizeof url, "https://huggingface.co/%s/resolve/%s/%s", repo, commit, file);
                yyjson_mut_obj_add_strcpy(d, item, "url", url);
                yyjson_mut_obj_add_strcpy(d, item, "checksum", hash);
            }
            free(repo);
            free(file);
            yyjson_mut_obj_add_uint(d, item, "bytes", yyjson_get_uint(yyjson_obj_get(lfs, "size")));
        }
        catalog_facts(d, item, source, true);
    }
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    if (json) {
        free(l->catalog);
        l->catalog = json;
    }
    changed(l, json ? NULL : "request_failed");
    if (json)
        cache_catalog(l);
}
static void ollama_catalog(ot_library *l) {
    char *json = ot_ollama_catalog(l->query, !strcmp(l->query_mode, "ollamaVariants") ? l->repo : NULL);
    if (json) {
        yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
        yyjson_mut_doc *mutable = parsed ? yyjson_doc_mut_copy(parsed, NULL) : NULL;
        yyjson_mut_val *items = yyjson_mut_doc_get_root(mutable);
        size_t i, n;
        yyjson_mut_val *item;
        yyjson_mut_arr_foreach(items, i, n, item) catalog_facts(mutable, item, NULL, false);
        char *enriched = mutable ? yyjson_mut_write(mutable, 0, NULL) : NULL;
        yyjson_mut_doc_free(mutable);
        yyjson_doc_free(parsed);
        free(json);
        json = enriched;
        if (json) {
            free(l->catalog);
            l->catalog = json;
        }
    }
    changed(l, json ? NULL : "request_failed");
    if (json)
        cache_catalog(l);
}
static bool engine_path(ot_library *l, int kind, char path[4096]) {
    sqlite3_stmt *q = query(l, "SELECT path FROM engines WHERE kind=?");
    if (!q)
        return false;
    sqlite3_bind_int(q, 1, kind);
    bool custom = sqlite3_step(q) == SQLITE_ROW;
    bool ok = custom && ot_engine_find(col(q, 0), path);
    sqlite3_finalize(q);
    if (custom)
        return ok;
    return ot_engine_find(kind == 1 ? "llama-server" : "ollama", path) ||
           ot_package_find(ot_package_for_kind(kind), l->components, path);
}
static void detect_engines(ot_library *l) {
    l->engine_scan++;
    l->memory_previous = (ot_runtime_info){0};
    l->memory_previous_model = 0;
    char previous[4096];
    copy(previous, sizeof previous, l->detected_engines[0]);
    for (int kind = 1; kind <= 2; kind++) {
        l->validation[kind - 1] = 0;
        sqlite3_stmt *q = query(l, "SELECT path FROM engines WHERE kind=?");
        if (!q)
            continue;
        sqlite3_bind_int(q, 1, kind);
        l->custom_engines[kind - 1] = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
        if (!engine_path(l, kind, l->detected_engines[kind - 1]))
            l->detected_engines[kind - 1][0] = 0;
    }
    struct stat identity = {0};
    if (*l->detected_engines[0]) (void)stat(l->detected_engines[0],&identity);
    const struct stat *old = &l->llama_identity;
    bool capabilities_changed = strcmp(previous,l->detected_engines[0]) ||
        identity.st_dev != old->st_dev || identity.st_ino != old->st_ino || identity.st_size != old->st_size ||
        identity.st_mtimespec.tv_sec != old->st_mtimespec.tv_sec || identity.st_mtimespec.tv_nsec != old->st_mtimespec.tv_nsec ||
        identity.st_ctimespec.tv_sec != old->st_ctimespec.tv_sec || identity.st_ctimespec.tv_nsec != old->st_ctimespec.tv_nsec;
    if (capabilities_changed)
        l->llama_sleep = *l->detected_engines[0] && ot_engine_sleep_supported(l->detected_engines[0]);
    l->llama_identity = identity;
    changed(l, NULL);
}
static void engine_failure(ot_library *l, const char *code) {
    l->cpu_probing = false;
    copy(l->engine_error, sizeof l->engine_error, code);
    cancel(l, &l->health_id);
    ot_engine_stop(&l->engine);
    l->pending = 0;
    ot_connection_ready(l->app, false);
    copy(l->engine_state, sizeof l->engine_state, "failed");
    ot_maintenance(l->app, false, code);
    changed(l, code);
}
static void health_request(ot_library *l) {
    l->health_stage = HEALTH_SERVER;
    free(l->health);
    l->health = NULL;
    l->health_size = l->health_capacity = 0;
    char url[512];
    if (l->health_external)
        snprintf(url, sizeof url, "%s/api/tags", *l->ollama ? l->ollama : "http://127.0.0.1:11434");
    else
        snprintf(url, sizeof url, "http://127.0.0.1:%u/%s", l->engine.port,
                 l->running_ollama ? "api/tags" : "health");
    l->health_id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_LOCAL);
    l->next_health = now() + 1;
}
/* CPU qualification runs only on an explicit CPU choice, with bounded working memory. */
static unsigned cpu_threads(const ot_resources *r, const ot_preferences *p) {
    return p->threads             ? ot_thread_budget(r, p)
           : r->interactive_cores ? r->interactive_cores
                                  : ot_thread_budget(r, p);
}
static unsigned cpu_capacity(ot_library *l, int64_t id) {
    const ot_resources *r = ot_resources_get(l->app);
    if (!r->cores || r->pressure)
        return 0;
    uint64_t budget = r->memory_total / 32;
    if (budget > r->memory_available / 8)
        budget = r->memory_available / 8;
    if (budget > UINT64_C(2147483648))
        budget = UINT64_C(2147483648);
    sqlite3_stmt *q = query(l, "SELECT bytes,context,embedding,layers,heads,kv_heads,metadata FROM models "
                               "WHERE id=? AND hidden=0 AND available=1");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    unsigned result = 0;
    if (q && sqlite3_step(q) == SQLITE_ROW) {
        ot_model_info info = {.bytes = (uint64_t)sqlite3_column_int64(q, 0),
                              .embedding = (unsigned)sqlite3_column_int(q, 2),
                              .layers = (unsigned)sqlite3_column_int(q, 3),
                              .heads = (unsigned)sqlite3_column_int(q, 4),
                              .kv_heads = (unsigned)sqlite3_column_int(q, 5)};
        yyjson_doc *d = yyjson_read(col(q, 6), strlen(col(q, 6)), 0);
        yyjson_val *meta = yyjson_doc_get_root(d);
        info.complex_cache = yyjson_get_bool(yyjson_obj_get(meta, "complexCache"));
        uint64_t key = yyjson_get_uint(yyjson_obj_get(meta, "keyLength")),
                 value = yyjson_get_uint(yyjson_obj_get(meta, "valueLength"));
        info.key_length = key <= UINT32_MAX ? (unsigned)key : 0;
        info.value_length = value <= UINT32_MAX ? (unsigned)value : 0;
        unsigned low = 1024, high = (unsigned)sqlite3_column_int(q, 1);
        if (high > 262144)
            high = 262144;
        while (info.bytes && low <= high) {
            unsigned context = low + (high - low) / 2;
            ot_memory_plan plan = ot_memory_approximate(&info, context);
            if (plan.known_cache && plan.total <= budget) {
                result = context;
                low = context + 1;
            } else
                high = context - 1;
        }
        yyjson_doc_free(d);
    }
    sqlite3_finalize(q);
    return result;
}
static bool model_identity(ot_library *l, int64_t id, char identity[160], char engine[96],
                           bool local_manifest) {
    sqlite3_stmt *q = query(l, "SELECT kind,reference,metadata FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    bool ok = q && sqlite3_step(q) == SQLITE_ROW;
    char executable[4096];
    struct stat st;
    if (ok && engine_path(l, sqlite3_column_int(q, 0), executable) && !stat(executable, &st)) {
        snprintf(engine, 96, "%llu:%llu:%lld:%ld:%lld", (unsigned long long)st.st_dev,
                 (unsigned long long)st.st_ino, (long long)st.st_mtime, st.st_mtimespec.tv_nsec,
                 (long long)st.st_size);
        if (sqlite3_column_int(q, 0) == 2) {
            yyjson_doc *d = yyjson_read(col(q, 2), strlen(col(q, 2)), 0);
            const char *digest = str(yyjson_doc_get_root(d), "digest");
            /* The digest identifies content; API and local accounting can include
             * different auxiliary blobs. Bind offline memory observations to the
             * local manifest too, so an external replacement invalidates them
             * before the next service query, without hashing model gigabytes. */
            ok = *digest && copy(identity, 160, digest);
            if (ok && local_manifest) {
                const char *manifest = str(yyjson_doc_get_root(d), "localManifest");
                ok = *manifest && !lstat(manifest, &st) && S_ISREG(st.st_mode) &&
                     snprintf(identity, 160, "%s@%llu:%llu:%lld:%ld:%lld", digest,
                              (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
                              (long long)st.st_mtime, st.st_mtimespec.tv_nsec, (long long)st.st_size) < 160;
            }
            yyjson_doc_free(d);
        } else {
            ok = !stat(col(q, 1), &st);
            if (ok)
                snprintf(identity, 160, "%llu:%llu:%lld:%ld:%lld", (unsigned long long)st.st_dev,
                         (unsigned long long)st.st_ino, (long long)st.st_mtime, st.st_mtimespec.tv_nsec,
                         (long long)st.st_size);
        }
    } else
        ok = false;
    sqlite3_finalize(q);
    return ok;
}
static bool memory_key(ot_library *l, int64_t id, const ot_preferences *p, char key[1025]) {
    char identity[160], engine[96];
    const ot_resources *r = ot_resources_get(l->app);
    if (!*r->cpu_name || !*r->system_build || !model_identity(l, id, identity, engine, true))
        return false;
    int n =
        snprintf(key, 1025, "%s/%s/%s/%s/%llu/%llu/%u/%u/%u/%d/%u/%u", identity, engine, r->cpu_name,
                 r->system_build, (unsigned long long)r->memory_total, (unsigned long long)r->gpu_recommended,
                 r->cores, ot_thread_budget(r, p), p->memory_percent, p->gpu_layers, r->gpu, r->unified);
    return n > 0 && n < 1025;
}
static char *memory_saved(ot_library *l, int64_t id) {
    sqlite3_stmt *q = query(l, "SELECT json_extract(metadata,'$.memoryObservation') FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    char *saved = q && sqlite3_step(q) == SQLITE_ROW ? strdup(col(q, 0)) : NULL;
    sqlite3_finalize(q);
    return saved;
}
static void memory_record(ot_library *l) {
    const ot_runtime_info *r = &l->runtime, *before = &l->memory_recorded;
    const ot_preferences *p = ot_get_preferences(l->app);
    /* Partial automatic offload can change with free memory. Keep those observations
     * live only. Never learn from an external process or from an incomplete load. */
    if (l->engine.pid <= 0 || !runtime_current(l) || r->context != p->context ||
        (r->kind != OT_RUNTIME_CPU && r->kind != OT_RUNTIME_GPU) || !r->weights || !r->cache || !r->compute ||
        !r->total_layers || (r->kind == OT_RUNTIME_GPU && r->gpu_layers != r->total_layers) ||
        (before->context == r->context && before->allocation == r->allocation &&
         before->weights == r->weights && before->cache == r->cache && before->compute == r->compute))
        return;
    char key[1025];
    if (!*l->memory_load_key || !memory_key(l, l->selected, p, key) || strcmp(key, l->memory_load_key))
        return;
    if (l->running_ollama) {
        sqlite3_stmt *identity = query(l, "SELECT json_extract(metadata,'$.digest') FROM models WHERE id=?");
        if (identity)
            sqlite3_bind_int64(identity, 1, l->selected);
        bool matches = *r->model_digest && identity && sqlite3_step(identity) == SQLITE_ROW &&
                       !strcmp(col(identity, 0), r->model_digest);
        sqlite3_finalize(identity);
        if (!matches)
            return;
    }
    char *saved = memory_saved(l, l->selected), *json = ot_memory_observe(saved, key, r);
    free(saved);
    if (json) {
        sqlite3_stmt *q = query(
            l, "UPDATE models SET metadata=json_set(metadata,'$.memoryObservation',json(?)) WHERE id=?");
        if (q) {
            sqlite3_bind_text(q, 1, json, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 2, l->selected);
        }
        if (!done(q)) {
            free(json);
            return;
        }
        free(json);
    }
    l->memory_recorded = *r;
}
static bool memory_recall(ot_library *l, int64_t id, const ot_preferences *p, ot_memory_plan *plan) {
    char key[1025];
    if (!memory_key(l, id, p, key))
        return false;
    char *saved = memory_saved(l, id);
    bool ok = ot_memory_recall(saved, key, p->context, plan) &&
              plan->total <= ot_memory_budget(ot_resources_get(l->app), p);
    free(saved);
    return ok;
}
static bool cpu_cached(ot_library *l, int64_t id, const ot_preferences *p, double *rate) {
    char identity[160], engine[96];
    if (!model_identity(l, id, identity, engine, false))
        return false;
    sqlite3_stmt *q = query(l, "SELECT json_extract(metadata,'$.cpuQualification') FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    yyjson_doc *d = q && sqlite3_step(q) == SQLITE_ROW ? yyjson_read(col(q, 0), strlen(col(q, 0)), 0) : NULL;
    yyjson_val *v = yyjson_doc_get_root(d);
    const ot_resources *r = ot_resources_get(l->app);
    bool ok = yyjson_get_uint(yyjson_obj_get(v, "version")) == 1 && !strcmp(str(v, "identity"), identity) &&
              !strcmp(str(v, "engine"), engine) && *r->cpu_name && !strcmp(str(v, "cpu"), r->cpu_name) &&
              yyjson_get_uint(yyjson_obj_get(v, "memory")) == r->memory_total &&
              yyjson_get_uint(yyjson_obj_get(v, "cores")) == r->cores &&
              yyjson_get_uint(yyjson_obj_get(v, "context")) >= p->context &&
              yyjson_get_uint(yyjson_obj_get(v, "threads")) == cpu_threads(r, p);
    if (ok)
        *rate = yyjson_get_num(yyjson_obj_get(v, "tokensPerSecond"));
    yyjson_doc_free(d);
    sqlite3_finalize(q);
    return ok;
}
static bool cpu_allowed(ot_library *l, int64_t id, const ot_preferences *p) {
    double rate = 0;
    if (cpu_cached(l, id, p, &rate))
        return rate >= 8;
    return p->context <= cpu_capacity(l, id);
}
bool ot_library_preferences_allowed(ot_library *l, const ot_preferences *p) {
    const ot_preferences *current = ot_get_preferences(l->app);
    if (p->gpu_layers != 0 || !l->selected ||
        (current->gpu_layers == 0 && p->context == current->context && p->threads == current->threads) ||
        cpu_allowed(l, l->selected, p))
        return true;
    changed(l, "cpu_not_qualified");
    return false;
}
static bool cpu_record(ot_library *l, int64_t id, const ot_preferences *p, double rate) {
    char identity[160], engine[96];
    if (!model_identity(l, id, identity, engine, false))
        return false;
    const ot_resources *r = ot_resources_get(l->app);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
    bool ok = v && yyjson_mut_obj_add_uint(d, v, "version", 1) &&
              yyjson_mut_obj_add_strcpy(d, v, "identity", identity) &&
              yyjson_mut_obj_add_strcpy(d, v, "engine", engine) &&
              yyjson_mut_obj_add_strcpy(d, v, "cpu", r->cpu_name) &&
              yyjson_mut_obj_add_uint(d, v, "memory", r->memory_total) &&
              yyjson_mut_obj_add_uint(d, v, "cores", r->cores) &&
              yyjson_mut_obj_add_uint(d, v, "context", p->context) &&
              yyjson_mut_obj_add_uint(d, v, "threads", cpu_threads(r, p)) &&
              yyjson_mut_obj_add_real(d, v, "tokensPerSecond", rate);
    if (d)
        yyjson_mut_doc_set_root(d, v);
    char *json = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    sqlite3_stmt *q =
        json ? query(l,
                     "UPDATE models SET metadata=json_set(metadata,'$.cpuQualification',json(?)) WHERE id=?")
             : NULL;
    if (q) {
        sqlite3_bind_text(q, 1, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 2, id);
    }
    ok = done(q);
    free(json);
    yyjson_mut_doc_free(d);
    return ok;
}
static bool model_info(ot_library *l, int64_t id, ot_model_info *model) {
    *model = (ot_model_info){0};
    sqlite3_stmt *q =
        query(l, "SELECT bytes,context,embedding,layers,heads,kv_heads,metadata,name FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    bool found = q && sqlite3_step(q) == SQLITE_ROW;
    if (found) {
        model->bytes = (uint64_t)sqlite3_column_int64(q, 0);
        model->context = (unsigned)sqlite3_column_int(q, 1);
        model->embedding = (unsigned)sqlite3_column_int(q, 2);
        model->layers = (unsigned)sqlite3_column_int(q, 3);
        model->heads = (unsigned)sqlite3_column_int(q, 4);
        model->kv_heads = (unsigned)sqlite3_column_int(q, 5);
        copy(model->name, sizeof model->name, col(q, 7));
        yyjson_doc *doc = yyjson_read(col(q, 6), strlen(col(q, 6)), 0);
        yyjson_val *meta = yyjson_doc_get_root(doc);
        model->tensor_bytes = yyjson_get_uint(yyjson_obj_get(meta, "tensorBytes"));
        if (model->tensor_bytes > model->bytes)
            model->tensor_bytes = 0;
        uint64_t key = yyjson_get_uint(yyjson_obj_get(meta, "keyLength")),
                 value = yyjson_get_uint(yyjson_obj_get(meta, "valueLength"));
        model->key_length = key <= UINT32_MAX ? (unsigned)key : 0;
        model->value_length = value <= UINT32_MAX ? (unsigned)value : 0;
        model->complex_cache = yyjson_get_bool(yyjson_obj_get(meta, "complexCache"));
        model->experts = (unsigned)yyjson_get_uint(yyjson_obj_get(meta, "expertCount"));
        model->experts_used = (unsigned)yyjson_get_uint(yyjson_obj_get(meta, "expertsUsed"));
        copy(model->architecture, sizeof model->architecture, str(meta, "family"));
        yyjson_doc_free(doc);
    }
    sqlite3_finalize(q);
    return found;
}
static ot_resources model_resources(const ot_library *l) {
    ot_resources r = *ot_resources_get(l->app);
    if (r.memory_available > r.memory_total)
        r.memory_available = r.memory_total;
    /* Credit only measured physical memory of our own live process. Allocation
     * reservations and stale reports must never be added as available RAM. */
    if (l->process_memory_known && l->engine.pid > 0 && !l->engine.stopping &&
        l->process_memory_child == l->engine.child) {
        uint64_t credit = l->process_memory < r.memory_used ? l->process_memory : r.memory_used;
        r.memory_available =
            credit < r.memory_total - r.memory_available ? r.memory_available + credit : r.memory_total;
    }
    return r;
}
static bool model_preferences(ot_library *l, unsigned context, ot_preferences *p) {
    *p = *ot_get_preferences(l->app);
    ot_preferences model_values = ot_preferences_default();
    int stored =
        ot_store_generation(ot_app_store(l->app), l->pending ? l->pending : l->selected, &model_values);
    if (stored < 0)
        return false;
    ot_preferences_model_values(p, &model_values);
    int64_t id = l->pending ? l->pending : l->selected;
    ot_model_info model;
    model_info(l, id, &model);
    if (context)
        model.context = context;
    ot_resources resources = model_resources(l);
    for (unsigned i = 0; i < OT_PRESET_COUNT; i++)
        if (!strcmp(p->profile, ot_model_profile_names[i])) {
            if (i != OT_PRESET_RECOMMENDED)
                *p = ot_model_preset(p, &model, &resources, i);
            else if (!stored) {
                /* Resolve once per preparation geometry, then persist on connect.
                 * Loading the model must not shrink its own initial configuration. */
                const ot_model_info *before = &l->initial_info;
                if (l->initial_model != id || before->context != model.context ||
                    before->bytes != model.bytes || before->tensor_bytes != model.tensor_bytes ||
                    before->layers != model.layers || before->embedding != model.embedding ||
                    before->heads != model.heads || before->kv_heads != model.kv_heads ||
                    before->key_length != model.key_length || before->value_length != model.value_length ||
                    before->complex_cache != model.complex_cache) {
                    l->initial_values = ot_model_preset(p, &model, &resources, i);
                    l->initial_model = id;
                    l->initial_info = model;
                }
                ot_preferences_model_values(p, &l->initial_values);
            }
            break;
        }
    if (context && p->context > context)
        p->context = context;
    if (p->context < 1024)
        return false;
    if (p->gpu_layers != 0 || !cpu_allowed(l, l->pending ? l->pending : l->selected, p))
        p->gpu_layers = -1;
    if (p->gpu_layers == 0 && !p->threads)
        p->threads = cpu_threads(ot_resources_get(l->app), p);
    p->require_gpu = ot_resources_get(l->app)->gpu && p->gpu_layers != 0;
    if (p->output + 512 >= p->context)
        p->output = p->context - 513;
    if (p->input > p->context - p->output - 256)
        p->input = p->context - p->output - 256;
    return true;
}
static void launch_pending(ot_library *l) {
    char path[4096], model[4096] = "";
    ot_preferences p = *ot_get_preferences(l->app);
    ot_model_info m = {0};
    if (l->pending > 0) {
        sqlite3_stmt *q = query(l, "SELECT kind,reference FROM models WHERE id=?");
        if (!q) {
            engine_failure(l, "storage_read");
            return;
        }
        sqlite3_bind_int64(q, 1, l->pending);
        bool ok = sqlite3_step(q) == SQLITE_ROW;
        l->running_ollama = ok && sqlite3_column_int(q, 0) == 2;
        ok = ok && copy(model, sizeof model, col(q, 1));
        sqlite3_finalize(q);
        if (!ok) {
            engine_failure(l, "invalid_model");
            return;
        }
    } else
        l->running_ollama = true;
    if (!l->running_ollama) {
        if (!ot_gguf(model, &m)) {
            engine_failure(l, "invalid_gguf");
            return;
        }
        if (!model_preferences(l, m.context, &p)) {
            engine_failure(l, "context_limit");
            return;
        }
        uint64_t budget = ot_memory_budget(ot_resources_get(l->app), &p);
        ot_memory_plan estimate = ot_memory_approximate(&m, p.context);
        /* A uniform KV guess is not a minimum for hybrid/recurrent models.
         * Let the runner plan that unknown geometry; keep the known lower bound
         * and the post-load allocation/acceleration checks. */
        if (!budget || (estimate.known_cache ? estimate.total : estimate.weights) > budget) {
            engine_failure(l, "memory_budget");
            return;
        }
    }
    if (l->running_ollama && !model_preferences(l, 0, &p)) {
        engine_failure(l, "context_limit");
        return;
    }
    l->engine_memory_percent = p.memory_percent;
    if (!engine_path(l, l->running_ollama ? 2 : 1, path)) {
        engine_failure(l, "engine_missing");
        return;
    }
    char identity[160];
    l->memory_engine_identity[0] = l->memory_load_key[0] = 0;
    if (l->pending > 0 && model_identity(l, l->pending, identity, l->memory_engine_identity, false) &&
        !l->running_ollama)
        memory_key(l, l->pending, &p, l->memory_load_key);
    if (!ot_engine_start(&l->engine, path, l->profile, model, l->running_ollama, l->llama_sleep, NULL, &p,
                         ot_resources_get(l->app))) {
        engine_failure(l, "engine_start");
        return;
    }
    l->engine_ollama = l->running_ollama;
    l->health_external = false;
    l->started = now();
    l->next_health = now();
    copy(l->engine_state, sizeof l->engine_state, "loading");
    changed(l, NULL);
}
static void connect_pending(ot_library *l, yyjson_val *root);
static bool cpu_verified(ot_library *l, yyjson_val *value) {
    ot_preferences p;
    l->cpu_probing = false;
    if (!model_preferences(l, 0, &p))
        return false;
    yyjson_val *timings = yyjson_obj_get(value, "timings");
    double tokens = l->running_ollama ? yyjson_get_num(yyjson_obj_get(value, "eval_count"))
                                      : yyjson_get_num(yyjson_obj_get(timings, "predicted_n"));
    double seconds = l->running_ollama ? yyjson_get_num(yyjson_obj_get(value, "eval_duration")) / 1e9
                                       : yyjson_get_num(yyjson_obj_get(timings, "predicted_ms")) / 1000;
    double rate = tokens >= 4 && seconds > 0 ? tokens / seconds : 0;
    if (!cpu_record(l, l->pending, &p, rate)) {
        engine_failure(l, "storage_write");
        return false;
    }
    if (rate < 8 || seconds > 3) {
        engine_failure(l, "cpu_not_suitable");
        return false;
    }
    return true;
}
static void model_request(ot_library *l, unsigned stage) {
    l->health_stage = stage;
    free(l->health);
    l->health = NULL;
    l->health_size = l->health_capacity = 0;
    if (stage == HEALTH_CPU) {
        ot_preferences p;
        double rate = 0;
        if (!model_preferences(l, 0, &p)) {
            engine_failure(l, "invalid_model");
            return;
        }
        if (cpu_cached(l, l->pending, &p, &rate)) {
            connect_pending(l, NULL);
            return;
        }
        l->cpu_probing = true;
        l->cpu_started = now();
        char url[512];
        snprintf(url, sizeof url, "http://127.0.0.1:%u/completion", l->engine.port);
        l->health_id =
            request(l, "POST", url,
                    "{\"prompt\":\"Continue this C code: int add(int a, int b) "
                    "{\",\"n_predict\":24,\"temperature\":0,\"stream\":false,\"cache_prompt\":false}",
                    NULL, 0, OT_NETWORK_LOCAL);
        return;
    }
    if (stage == HEALTH_PROPS) {
        char url[512];
        snprintf(url, sizeof url, "http://127.0.0.1:%u/props", l->engine.port);
        l->health_id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_LOCAL);
        return;
    }
    sqlite3_stmt *q = query(l, "SELECT reference,context FROM models WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, l->pending);
    bool found = q && sqlite3_step(q) == SQLITE_ROW;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
    bool ok = found && v && yyjson_mut_obj_add_strcpy(d, v, "model", col(q, 0));
    unsigned context = found ? (unsigned)sqlite3_column_int64(q, 1) : 0;
    sqlite3_finalize(q);
    if (stage == HEALTH_WARM) {
        ot_preferences p = {0};
        yyjson_mut_val *options = d ? yyjson_mut_obj(d) : NULL;
        ok = ok && model_preferences(l, context, &p) && options &&
             yyjson_mut_obj_add_uint(d, options, "num_ctx", p.context) &&
             yyjson_mut_obj_add_uint(d, options, "num_thread",
                                     ot_thread_budget(ot_resources_get(l->app), &p)) &&
             yyjson_mut_obj_add_val(d, v, "options", options);
        char identity[160], engine[96];
        l->memory_load_key[0] = 0;
        if (ok && model_identity(l, l->pending, identity, engine, false) &&
            !strcmp(engine, l->memory_engine_identity))
            memory_key(l, l->pending, &p, l->memory_load_key);
        if (ok && l->engine.pid > 0 && !ot_memory_budget(ot_resources_get(l->app), &p)) {
            yyjson_mut_doc_free(d);
            engine_failure(l, "memory_budget");
            return;
        }
        if (ok && p.gpu_layers != -1)
            ok = yyjson_mut_obj_add_int(d, options, "num_gpu", p.gpu_layers == -2 ? 999 : p.gpu_layers);
        double rate = 0;
        l->cpu_probing = ok && p.gpu_layers == 0 && !cpu_cached(l, l->pending, &p, &rate);
        if (l->cpu_probing) {
            l->cpu_started = now();
            yyjson_mut_obj_add_uint(d, options, "num_predict", 24);
            yyjson_mut_obj_add_real(d, options, "temperature", 0);
            if (ot_thinking_find(&l->thinking, "off"))
                yyjson_mut_obj_add_bool(d, v, "think", false);
        }
        const char *keep = p.keep_alive;
        /* Empty prompt + zero is Ollama's unload command, not a warmup. Keep
         * preparation resident briefly so GPU verification can complete. Real
         * answers still use the user's zero and release immediately afterwards. */
        if (l->cpu_probing || !strcmp(keep, "0"))
            keep = "30s";
        ok = ok && yyjson_mut_obj_add_bool(d, v, "stream", false) &&
             yyjson_mut_obj_add_strcpy(d, v, "prompt",
                                       l->cpu_probing ? "Continue this C code: int add(int a, int b) {" : "");
        ok = ok && (!strcmp(keep, "-1") ? yyjson_mut_obj_add_int(d, v, "keep_alive", -1)
                                        : yyjson_mut_obj_add_strcpy(d, v, "keep_alive", keep));
    }
    yyjson_mut_doc_set_root(d, v);
    char *body = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    if (!body) {
        engine_failure(l, "invalid_model");
        return;
    }
    char url[512];
    snprintf(url, sizeof url, "%s/api/%s", l->ollama, stage == HEALTH_MODEL ? "show" : "generate");
    l->health_id = request(l, "POST", url, body, NULL, 0, OT_NETWORK_LOCAL);
    free(body);
    copy(l->engine_state, sizeof l->engine_state, "loading");
    changed(l, NULL);
}
static void model_capabilities(ot_library *l, yyjson_val *v) {
    l->thinking = ot_thinking_detect(v, l->running_ollama);
    if (l->running_ollama) {
        yyjson_val *info = yyjson_obj_get(v, "model_info"), *details = yyjson_obj_get(v, "details");
        const char *architecture = str(info, "general.architecture");
        uint64_t dimensions[7] = {0};
        const char *suffixes[] = {"context_length",        "embedding_length",        "block_count",
                                  "attention.head_count",  "attention.head_count_kv", "attention.key_length",
                                  "attention.value_length"};
        for (unsigned j = 0; j < 7; j++) {
            char key_name[128];
            snprintf(key_name, sizeof key_name, "%.64s.%s", architecture, suffixes[j]);
            dimensions[j] = yyjson_get_uint(yyjson_obj_get(info, key_name));
        }
        yyjson_mut_doc *meta = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *fields = meta ? yyjson_mut_obj(meta) : NULL;
        const char *labels[] = {"family", "parameterSize", "quantization"};
        const char *descriptions[] = {*architecture ? architecture : str(details, "family"),
                                      str(details, "parameter_size"), str(details, "quantization_level")};
        for (unsigned j = 0; fields && j < 3; j++)
            if (strlen(descriptions[j]) <= 256)
                yyjson_mut_obj_add_strcpy(meta, fields, labels[j], descriptions[j]);
        if (fields) {
            yyjson_mut_obj_add_val(meta, fields, "thinking", ot_thinking_json(meta, &l->thinking));
            yyjson_mut_obj_add_bool(meta, fields, "thinkingKnown", l->thinking.known);
            if (dimensions[5] && dimensions[5] <= UINT32_MAX)
                yyjson_mut_obj_add_uint(meta, fields, "keyLength", dimensions[5]);
            if (dimensions[6] && dimensions[6] <= UINT32_MAX)
                yyjson_mut_obj_add_uint(meta, fields, "valueLength", dimensions[6]);
            const char *extra_keys[] = {"expert_count", "expert_used_count", "ssm.state_size",
                                        "attention.sliding_window", "attention.kv_lora_rank"};
            uint64_t extra[5] = {0};
            for (unsigned j = 0; j < 5; j++) {
                char key_name[128];
                snprintf(key_name, sizeof key_name, "%.64s.%s", architecture, extra_keys[j]);
                extra[j] = yyjson_get_uint(yyjson_obj_get(info, key_name));
            }
            yyjson_mut_obj_add_uint(meta, fields, "expertCount", extra[0]);
            yyjson_mut_obj_add_uint(meta, fields, "expertsUsed", extra[1]);
            yyjson_mut_obj_add_bool(meta, fields, "complexCache", extra[2] || extra[3] || extra[4]);
            const char *license_text = str(v, "license");
            if (*license_text && strlen(license_text) <= 128 && !strchr(license_text, '\n'))
                yyjson_mut_obj_add_strcpy(meta, fields, "license", license_text);
            yyjson_mut_doc_set_root(meta, fields);
        }
        char *metadata = fields ? yyjson_mut_write(meta, 0, NULL) : NULL;
        sqlite3_stmt *q =
            query(l, "UPDATE models SET context=coalesce(nullif(?,0),context),"
                     "embedding=coalesce(nullif(?,0),embedding),layers=coalesce(nullif(?,0),layers),"
                     "heads=coalesce(nullif(?,0),heads),kv_heads=coalesce(nullif(?,0),kv_heads),metadata="
                     "json_patch(metadata,?) "
                     "WHERE id=?");
        if (q) {
            for (unsigned j = 0; j < 5; j++)
                sqlite3_bind_int64(q, (int)j + 1,
                                   dimensions[j] <= INT32_MAX ? (sqlite3_int64)dimensions[j] : 0);
            sqlite3_bind_text(q, 6, metadata ? metadata : "{}", -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 7, l->pending);
        }
        free(metadata);
        yyjson_mut_doc_free(meta);
        if (!done(q))
            changed(l, "storage_write");
    }
}
static void prepare_pending(ot_library *l) {
    if (l->running_ollama) {
        l->health_external = true;
        l->started = now();
        copy(l->engine_state, sizeof l->engine_state, "probing");
        health_request(l);
    } else
        launch_pending(l);
}
static void connect_pending(ot_library *l, yyjson_val *root) {
    if (l->running_ollama) {
        if (!l->health_external)
            snprintf(l->ollama, sizeof l->ollama, "http://127.0.0.1:%u", l->engine.port);
        else if (!*l->ollama)
            copy(l->ollama, sizeof l->ollama, "http://127.0.0.1:11434");
        if (yyjson_is_arr(yyjson_obj_get(root, "models")))
            tags(l, root);
    }
    ot_maintenance(l->app, false, NULL);
    if (l->pending > 0) {
        ot_connection c = {.ollama = l->running_ollama};
        if (l->running_ollama)
            copy(c.endpoint, sizeof c.endpoint, l->ollama);
        else
            snprintf(c.endpoint, sizeof c.endpoint, "http://127.0.0.1:%u", l->engine.port);
        sqlite3_stmt *q = query(l, "SELECT name,reference,context FROM models WHERE id=?");
        if (!q) {
            engine_failure(l, "storage_read");
            return;
        }
        sqlite3_bind_int64(q, 1, l->pending);
        bool ok = sqlite3_step(q) == SQLITE_ROW;
        ok = ok && copy(c.model, sizeof c.model, col(q, l->running_ollama ? 1 : 0));
        unsigned context = (unsigned)sqlite3_column_int(q, 2);
        sqlite3_finalize(q);
        if (!ok || !ot_configure(l->app, &c, l->pending)) {
            engine_failure(l, "connection_failed");
            return;
        }
        {
            ot_preferences p;
            if (!model_preferences(l, context, &p)) {
                engine_failure(l, "context_limit");
                return;
            }
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            if (!d) {
                engine_failure(l, "request_failed");
                return;
            }
            yyjson_mut_val *v = ot_preferences_json(d, &p);
            yyjson_mut_doc_set_root(d, v);
            char *s = yyjson_mut_write(d, 0, NULL);
            yyjson_mut_doc_free(d);
            bool saved = s && ot_set_preferences(l->app, s) &&
                         ot_store_initial_generation(ot_app_store(l->app), l->pending, &p);
            free(s);
            if (!saved) {
                engine_failure(l, "storage_write");
                return;
            }
        }
        l->selected = l->pending;
        if (ot_get_preferences(l->app)->require_gpu)
            ot_connection_ready(l->app, false);
        ot_thinking_capability(l->app, &l->thinking);
        if (l->io.connected)
            l->io.connected(l->io.context);
    }
    l->pending = 0;
    copy(l->engine_state, sizeof l->engine_state, "ready");
    l->engine_error[0] = 0;
    changed(l, NULL);
}
static void select_model(ot_library *l, int64_t id) {
    l->cpu_probing = false;
    if (id == l->selected && !strcmp(l->engine_state, "ready"))
        return;
    if (l->pending == id)
        return;
    int kind = 2;
    sqlite3_stmt *q = NULL;
    if (id >= 0) {
        q = query(l, "SELECT kind,available,coalesce(json_extract(metadata,'$.fileChanged'),0) "
                     "FROM models WHERE id=? AND hidden=0");
        if (!q) {
            changed(l, "storage_read");
            return;
        }
        sqlite3_bind_int64(q, 1, id);
        bool found = sqlite3_step(q) == SQLITE_ROW;
        kind = found ? sqlite3_column_int(q, 0) : 0;
        bool available = found && sqlite3_column_int(q, 1), modified = found && sqlite3_column_int(q, 2);
        sqlite3_finalize(q);
        if (!kind || (kind == 1 && (!available || modified))) {
            changed(l, !kind ? "invalid_model" : !available ? "not_found" : "file_changed");
            return; // Refusing a changed file must not stop the current usable engine.
        }
    }
    if (l->pending) {
        cancel(l, &l->health_id);
        l->pending = 0;
        ot_maintenance(l->app, false, NULL);
    }
    if (!ot_maintenance(l->app, true, NULL)) {
        changed(l, "busy");
        return;
    }
    if (id > 0) {
        q = query(l, "UPDATE settings SET model_id=? WHERE id=1");
        if (q)
            sqlite3_bind_int64(q, 1, id);
        if (!done(q)) {
            engine_failure(l, "storage_write");
            return;
        }
    }
    ot_memory_cancel(l->memory);
    l->memory_load_key[0] = 0;
    l->memory_recorded = (ot_runtime_info){0};
    l->requested = id;
    l->initial_model = 0;
    l->engine_error[0] = 0;
    ot_connection_ready(l->app, false);
    l->pending = id;
    l->selected = 0;
    l->running_ollama = kind == 2;
    l->thinking = (ot_thinking){0};
    ot_thinking_capability(l->app, &l->thinking);
    ot_preferences requested_preferences;
    bool budget_changed = model_preferences(l, 0, &requested_preferences) &&
                          l->engine_memory_percent != requested_preferences.memory_percent;
    if (l->engine.pid > 0 && (l->engine.stopping || kind != 2 || !l->engine_ollama || budget_changed)) {
        ot_engine_stop(&l->engine);
        copy(l->engine_state, sizeof l->engine_state, "stopping");
    } else if (kind == 2 && l->engine.pid > 0 && l->engine_ollama) {
        l->health_external = false;
        l->started = now();
        copy(l->engine_state, sizeof l->engine_state, "probing");
        health_request(l);
    } else
        prepare_pending(l);
    changed(l, NULL);
}
static bool setup_owns_job(ot_library *l,int64_t id) {
    sqlite3_stmt *q=query(l,"UPDATE downloads SET metadata=json_set(metadata,'$.setupBorrowed',"
        "coalesce(json_extract(metadata,'$.setupBorrowed'),NOT coalesce(json_extract(metadata,'$.setup'),0)),"
        "'$.setup',json('true')) WHERE id=?");
    if (q) sqlite3_bind_int64(q,1,id);
    return done(q);
}
static void enqueue(ot_library *l, const char *url, const char *name, const char *checksum, uint64_t bytes,
                    yyjson_val *source) {
    bool pull = !strncmp(url, "ollama:", 7);
    int64_t replace = yyjson_get_sint(yyjson_obj_get(source, "replaceId"));
    if ((!pull && strncmp(url, "https://", 8)) || strlen(url) >= sizeof l->url ||
        strlen(name) >= sizeof l->job_name || (!pull && !sha(checksum)) ||
        (pull && !ot_model_name(url + 7)) || bytes > INT64_MAX) {
        changed(l, "invalid_input");
        return;
    }
    sqlite3_stmt *q = NULL;
    char replaced_path[4096] = "";
    int64_t replaced_device = 0, replaced_inode = 0, replaced_job = 0;
    if (replace) {
        q = query(l, "SELECT reference,device,inode,managed FROM models WHERE id=? AND "
                     "((kind=1 AND json_extract(metadata,'$.checksum')=?) OR (kind=2 AND name=?))");
        if (q) {
            sqlite3_bind_int64(q, 1, replace);
            sqlite3_bind_text(q, 2, checksum, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, pull ? url + 7 : "", -1, SQLITE_TRANSIENT);
        }
        bool found =
            q && sqlite3_step(q) == SQLITE_ROW && copy(replaced_path, sizeof replaced_path, col(q, 0));
        if (found) {
            replaced_device = sqlite3_column_int64(q, 1);
            replaced_inode = sqlite3_column_int64(q, 2);
            replaced_job = sqlite3_column_int64(q, 3);
        }
        sqlite3_finalize(q);
        if (!found) {
            changed(l, "replacement_changed");
            return;
        }
    }
    if (!pull && !replace) {
        q = query(l, "SELECT reference,device,inode FROM models WHERE kind=1 AND "
                     "json_extract(metadata,'$.checksum')=? AND "
                     "coalesce(json_extract(metadata,'$.fileChanged'),0)=0");
        if (!q) {
            changed(l, "storage_read");
            return;
        }
        sqlite3_bind_text(q, 1, checksum, -1, SQLITE_TRANSIENT);
        bool exists = false;
        int step;
        while ((step = sqlite3_step(q)) == SQLITE_ROW) {
            struct stat st;
            if (!lstat(col(q, 0), &st) && S_ISREG(st.st_mode) &&
                st.st_dev == (dev_t)sqlite3_column_int64(q, 1) &&
                st.st_ino == (ino_t)sqlite3_column_int64(q, 2)) {
                exists = true;
                break;
            }
        }
        sqlite3_finalize(q);
        if (exists || step != SQLITE_DONE) {
            changed(l, exists ? "already_installed" : "storage_read");
            return;
        }
    }
    q = query(l, "SELECT d.id,d.state,m.id FROM downloads d LEFT JOIN models m ON "
                 "m.managed=d.id WHERE d.url=? AND d.checksum=? AND coalesce(json_extract(d.metadata,'$.discard'),0)=0 ORDER BY d.id DESC LIMIT 1");
    if (!q) {
        changed(l, "storage_read");
        return;
    }
    sqlite3_bind_text(q, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, checksum, -1, SQLITE_TRANSIENT);
    int64_t previous = 0;
    bool installed = false;
    if (sqlite3_step(q) == SQLITE_ROW) {
        if (strcmp(col(q, 1), "complete"))
            previous = sqlite3_column_int64(q, 0);
        else
            installed = sqlite3_column_int64(q, 2) > 0;
    }
    sqlite3_finalize(q);
    if (installed && !replace) {
        changed(l, "already_installed");
        return;
    }
    q = query(l, "SELECT count(*) FROM downloads WHERE state='queued' AND id<>?");
    if (q)
        sqlite3_bind_int64(q, 1, previous);
    bool room = q && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) < 64;
    sqlite3_finalize(q);
    if (!room) {
        changed(l, "queue_full");
        return;
    }
    if (previous) {
        if (yyjson_is_true(yyjson_obj_get(source,"setup")) && !setup_owns_job(l,previous)) {
            changed(l,"storage_write");return;
        }
        if (previous == l->job &&
            (l->transfer_id || l->download_preparing || (l->working && (l->worker_kind == 1 || l->worker_kind == 6 || l->worker_kind == 7) && !atomic_load(&l->worker_cancel)))) {
            changed(l, NULL); // A repeated setup intent still needs an immediate acknowledgement.
            return;
        }
        q = query(l, "UPDATE downloads SET state='queued',updated=unixepoch(),metadata=CASE WHEN ?2>0 "
                     "THEN json_set(metadata,'$.replaceId',?2,'$.replacePath',?3,'$.replaceDevice',?4,"
                     "'$.replaceInode',?5,'$.replaceJob',?6) ELSE json_remove(metadata,'$.replaceId',"
                     "'$.replacePath','$.replaceDevice','$.replaceInode','$.replaceJob') END WHERE id=?1");
        if (q) {
            sqlite3_bind_int64(q, 1, previous);
            sqlite3_bind_int64(q, 2, pull ? 0 : replace);
            sqlite3_bind_text(q, 3, replaced_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 4, replaced_device);
            sqlite3_bind_int64(q, 5, replaced_inode);
            sqlite3_bind_int64(q, 6, replaced_job);
        }
        if (!done(q)) {
            changed(l, "storage_write");
            return;
        }
        l->queue_pending = true;
        changed(l, NULL);
        if (!l->transfer_id && !l->working && !l->download_preparing && !l->removing &&
            !l->download_engine.stopping)
            resume(l, previous);
        return;
    }
    q = query(l, "INSERT INTO downloads(url,name,checksum,total,state,metadata) VALUES(?,?,?,?,'queued',?)");
    if (!q)
        return;
    sqlite3_bind_text(q, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 3, checksum, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 4, (sqlite3_int64)bytes);
    yyjson_mut_doc *meta = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *fields = meta ? yyjson_mut_obj(meta) : NULL;
    const char *keys[] = {"license", "sourceURL", "revision", "family", "quantization", "parameterSize"};
    for (unsigned i = 0; fields && i < sizeof keys / sizeof *keys; i++) {
        const char *text = str(source, keys[i]);
        if (*text && strlen(text) <= (i == 1 ? 2048 : 256))
            yyjson_mut_obj_add_strcpy(meta, fields, keys[i], text);
    }
    if (fields) {
        if (yyjson_is_true(yyjson_obj_get(source, "setup"))) {
            yyjson_mut_obj_add_bool(meta, fields, "setup", true);
            if (pull) yyjson_mut_obj_add_bool(meta, fields,"pullStore",true);
        }
        yyjson_mut_obj_add_strcpy(meta, fields, "fileName", name);
        if (bytes) yyjson_mut_obj_add_uint(meta, fields, "expectedBytes", bytes);
        if (!pull)
            yyjson_mut_obj_add_strcpy(meta, fields, "checksum", checksum);
        if (replace && !pull) {
            yyjson_mut_obj_add_sint(meta, fields, "replaceId", replace);
            yyjson_mut_obj_add_strcpy(meta, fields, "replacePath", replaced_path);
            yyjson_mut_obj_add_sint(meta, fields, "replaceDevice", replaced_device);
            yyjson_mut_obj_add_sint(meta, fields, "replaceInode", replaced_inode);
            yyjson_mut_obj_add_sint(meta, fields, "replaceJob", replaced_job);
        }
    }
    if (meta)
        yyjson_mut_doc_set_root(meta, fields);
    char *metadata = fields ? yyjson_mut_write(meta, 0, NULL) : NULL;
    sqlite3_bind_text(q, 5, metadata ? metadata : "{}", -1, SQLITE_TRANSIENT);
    free(metadata);
    yyjson_mut_doc_free(meta);
    if (!done(q)) {
        changed(l, "storage_write");
        return;
    }
    l->queue_pending = true;
    changed(l, NULL);
    if (!l->transfer_id && !l->working && !l->download_preparing && !l->removing &&
        !l->download_engine.stopping)
        resume(l, sqlite3_last_insert_rowid(l->db));
}
static bool save_preferences(ot_library *l, const ot_preferences *p) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_doc_set_root(d, ot_preferences_json(d, p));
    char *json = yyjson_mut_write(d, 0, NULL);
    bool ok = json && ot_set_preferences(l->app, json);
    free(json);
    yyjson_mut_doc_free(d);
    return ok;
}
static advice_page *advice_page_for_id(ot_library *l, uint64_t id) {
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++)
        if (id && l->advice_pages[i].id == id && !l->advice_pages[i].complete) return &l->advice_pages[i];
    return NULL;
}
static void advice_clear_pages(ot_library *l) {
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++) {
        advice_page *p = &l->advice_pages[i];
        cancel(l, &p->id);
        free(p->body);
        *p = (advice_page){0};
    }
}
static void advice_count_request(ot_library *l) {
    unsigned active = l->advice_id ? 1 : 0;
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++)
        active += l->advice_pages[i].id && !l->advice_pages[i].complete;
    if (active > l->advice_peak) l->advice_peak = active;
    l->advice_requests++;
}
// Read ahead only independent metadata. Consume in the original semantic order,
// so response timing cannot alter candidate quotas, evidence or the winner.
static void advice_prefetch(ot_library *l, const char *url) {
    if (!*url || (l->advice_id && !strcmp(l->advice_url, url))) return;
    advice_page *free_page = NULL;
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++) {
        advice_page *p = &l->advice_pages[i];
        if (p->id && !strcmp(p->url, url)) return;
        if (!p->id && !free_page) free_page = p;
    }
    if (!free_page) return;
    copy(free_page->url, sizeof free_page->url, url);
    free_page->started = now();
    free_page->id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_CATALOGUES);
    advice_count_request(l);
}
static unsigned advice_sources(const ot_library *l) {
    unsigned mask = 0;
    for (size_t i = 0; i < l->advice_count; i++)
        if (l->advice[i].kind == 1 || l->advice[i].kind == 2) mask |= (unsigned)l->advice[i].kind;
    return mask;
}
static void advice_restore(ot_library *l) {
    l->advice_count = l->advice_snapshot ? ot_model_catalogue_read(l->advice_snapshot, l->advice, OT_ADVICE_MODELS) : 0;
    l->advice_ready = l->advice_count > 0;
    l->advice_sources = advice_sources(l);
}
static void advice_cancel(ot_library *l) {
    cancel(l, &l->advice_id);
    advice_clear_pages(l);
    free(l->advice_body);
    l->advice_body = NULL;
    l->advice_size = l->advice_capacity = 0;
    l->advice_buffered = false;
    l->advice_url[0] = 0;
}
static void advice_save(ot_library *l, bool publish) {
    char *catalogue = publish && l->advice_count ? ot_model_catalogue_write(l->advice, l->advice_count) : NULL;
    if (catalogue) {
        free(l->advice_snapshot);
        l->advice_snapshot = catalogue;
        l->advice_updated = (int64_t)time(NULL);
    }
    sqlite3_stmt *q = query(l, "UPDATE advice_state SET catalogue=?1,updated=?2,checked=?3 WHERE id=1");
    if (q) {
        sqlite3_bind_text(q, 1, l->advice_snapshot ? l->advice_snapshot : "[]", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 2, l->advice_updated);
        sqlite3_bind_int64(q, 3, l->advice_checked);
        (void)done(q);
    }
}
static void advice_load(ot_library *l) {
    sqlite3_stmt *q = query(l, "SELECT catalogue,updated,checked FROM advice_state WHERE id=1");
    if (q && sqlite3_step(q) == SQLITE_ROW) {
        l->advice_count = ot_model_catalogue_read(col(q, 0), l->advice, OT_ADVICE_MODELS);
        if (l->advice_count) {
            l->advice_snapshot = ot_model_catalogue_write(l->advice, l->advice_count);
            l->advice_ready = l->advice_snapshot != NULL;
            l->advice_updated = sqlite3_column_int64(q, 1);
        }
        l->advice_checked = sqlite3_column_int64(q, 2);
    }
    sqlite3_finalize(q);
    int64_t date = (int64_t)time(NULL);
    if (l->advice_checked < 0 || l->advice_checked > date + 86400) l->advice_checked = 0;
    l->advice_sources = advice_sources(l);
    // Cache freshness survives a restart; wall time is only used at this boundary.
    if (l->advice_ready && l->advice_updated > 0 && l->advice_updated <= date && date - l->advice_updated < 300)
        l->advice_cached = now() - (double)(date - l->advice_updated);
}
static void advice_complete(ot_library *l) {
    advice_cancel(l);
    l->advice_ready = true;
    l->advice_cached = now();
    l->advice_checked = (int64_t)time(NULL);
    if (!l->advice_count && !l->advice_snapshot) copy(l->advice_error, sizeof l->advice_error, "suggestions_unavailable");
    advice_save(l, l->advice_count > 0);
    advice_restore(l);
    l->advice_finished = now();
    l->revision++;
}
static void advice_request(ot_library *l, const char *url) {
    free(l->advice_body);
    l->advice_body = NULL;
    l->advice_size = l->advice_capacity = 0;
    copy(l->advice_url, sizeof l->advice_url, url);
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++) {
        advice_page *p = &l->advice_pages[i];
        if (!p->id || strcmp(p->url, url)) continue;
        l->advice_id = p->id;
        l->advice_started = p->started;
        l->advice_body = p->body;
        l->advice_size = p->size;
        l->advice_capacity = p->capacity;
        l->advice_buffered = p->complete;
        copy(l->advice_buffer_error, sizeof l->advice_buffer_error, p->error);
        *p = (advice_page){0};
        l->advice_hits++;
        l->revision++;
        return;
    }
    l->advice_id = request(l, "GET", url, NULL, NULL, 0, OT_NETWORK_CATALOGUES);
    l->advice_started = now();
    advice_count_request(l);
    l->revision++;
}
static bool advice_family_url(const ot_library *l, size_t index, char out[1024]) {
    const ot_model_candidate *m = &l->advice_families[index];
    char *repo = l->advice_hf ? encoded(m->repository, true) : NULL;
    if (l->advice_hf && !repo) return false;
    char name[257]; copy(name, sizeof name, m->name);
    char *colon = strchr(name, ':'); if (colon) *colon = 0;
    int n = l->advice_hf ? snprintf(out, 1024, "https://huggingface.co/api/models/%s?blobs=true", repo)
                       : snprintf(out, 1024, "https://ollama.com/library/%s/tags", name);
    free(repo);
    return n > 0 && n < 1024;
}
static bool advice_evidence_url(const char *id, char out[1024]) {
    char *value = encoded(id, true);
    if (!value) return false;
    int n = snprintf(out, 1024, "https://huggingface.co/api/models/%s?expand[]=evalResults&expand[]=cardData&expand[]=gguf", value);
    free(value);
    return n > 0 && n < 1024;
}
static bool advice_manifest_url(const ot_model_candidate *m, char out[1024]) {
    if (m->kind != 2 || !ot_model_verification_candidate(m->name)) return false;
    char family[257]; copy(family, sizeof family, m->name);
    char *tag = strchr(family, ':'); if (!tag) return false;
    *tag++ = 0;
    int n = snprintf(out, 1024, "https://registry.ollama.ai/v2/library/%s/manifests/%s", family, tag);
    return n > 0 && n < 1024;
}
static const char advice_hf_recent[] = "https://huggingface.co/api/models?filter=gguf&sort=lastModified&direction=-1&"
    "limit=24&expand[]=gguf&expand[]=library_name&expand[]=tags&expand[]=gated&expand[]=pipeline_tag&expand[]=cardData";
static void advice_hf_begin(ot_library *l) {
    advice_clear_pages(l);
    advice_prefetch(l, advice_hf_recent);
    l->advice_hf = true;
    l->advice_recent = false;
    l->advice_family = l->advice_family_count = 0;
    advice_request(l, "https://huggingface.co/api/models?filter=gguf&"
                      "sort=downloads&direction=-1&limit=24&expand[]=gguf&expand[]=library_name&expand[]=tags&"
                      "expand[]=gated&expand[]=pipeline_tag&expand[]=cardData");
}
static void advice_next(ot_library *l) {
    if (l->advice_evidence) {
        for (size_t i = l->advice_evidence_index; i < l->advice_count * 2; i++) {
            const ot_model_candidate *m = &l->advice[i / 2];
            const char *id = i % 2 ? m->base : m->repository;
            bool seen = !*id;
            for (unsigned j = 0; j < l->advice_evidence_count; j++)
                if (!strcmp(id, l->advice_evidence_ids[j])) seen = true;
            char url[1024];
            if (!seen && advice_evidence_url(id, url)) advice_prefetch(l, url);
        }
        while (l->advice_evidence_index < l->advice_count * 2) {
            const ot_model_candidate *m = &l->advice[l->advice_evidence_index / 2];
            const char *id = l->advice_evidence_index++ % 2 ? m->base : m->repository;
            if (!*id) continue;
            bool seen = false;
            for (unsigned i = 0; i < l->advice_evidence_count; i++)
                if (!strcmp(l->advice_evidence_ids[i], id)) seen = true;
            if (seen || l->advice_evidence_count == OT_ADVICE_FAMILIES * 2) continue;
            copy(l->advice_evidence_ids[l->advice_evidence_count++], sizeof l->advice_evidence_ids[0], id);
            char url[1024];
            if (!advice_evidence_url(id, url)) continue;
            advice_request(l, url);
            return;
        }
        advice_complete(l);
    } else if (l->advice_family < l->advice_family_count) {
        char url[1024];
        if (!advice_family_url(l, l->advice_family, url)) {
            advice_cancel(l); changed(l, "request_failed"); return;
        }
        for (size_t i = l->advice_family + 1; i < l->advice_family_count; i++) {
            char next[1024];
            if (advice_family_url(l, i, next)) advice_prefetch(l, next);
        }
        advice_request(l, url);
    } else if (!l->advice_hf && (ot_get_preferences(l->app)->setup_engines & 1)) {
        advice_hf_begin(l);
    } else {
        l->advice_verifying = true;
        for (size_t i = l->advice_verification; i < l->advice_count; i++) {
            char url[1024];
            if (advice_manifest_url(&l->advice[i], url)) advice_prefetch(l, url);
        }
        while (l->advice_verification < l->advice_count) {
            ot_model_candidate *m = &l->advice[l->advice_verification++];
            if (m->kind == 1) {
                m->verified = ot_model_file_verified(m->checksum);
                continue;
            }
            char url[1024];
            if (!advice_manifest_url(m, url)) continue;
            advice_request(l, url);
            return;
        }
        l->advice_evidence = true;
        advice_next(l);
    }
}
static void advice_begin(ot_library *l, bool refresh) {
    if (!ot_network_allowed(ot_get_preferences(l->app), OT_NETWORK_CATALOGUES) || l->advice_id ||
        (!refresh && l->advice_ready && !*l->advice_error && l->advice_cached > 0 && now() - l->advice_cached < 300 &&
         (l->advice_sources & ot_get_preferences(l->app)->setup_engines) == ot_get_preferences(l->app)->setup_engines))
        return;
    l->advice_request_epoch++;
    l->advice_requests = l->advice_hits = l->advice_peak = 0;
    l->advice_finished = 0;
    l->advice_ready = false;
    l->advice_popular = false;
    l->advice_verifying = false;
    l->advice_verification = 0;
    l->advice_evidence = false;
    l->advice_evidence_count = 0;
    l->advice_evidence_index = 0;
    l->advice_recent = false;
    l->advice_cycle_started = now();
    l->advice_hf = false;
    l->advice_family = l->advice_family_count = l->advice_count = 0;
    l->advice_error[0] = 0;
    if (ot_get_preferences(l->app)->setup_engines & 2)
        advice_request(l, "https://ollama.com/api/experimental/model-recommendations");
    else
        advice_next(l);
}
static void advice_variants(ot_library *l, const char *json, const ot_model_candidate *family, bool hf) {
    if (!json || !l->advice_family_count) return;
    ot_model_candidate *variants = calloc(100, sizeof *variants);
    if (!variants) return;
    size_t count = hf ? ot_model_hf_variants(json, family, variants, 100)
                      : ot_model_variants(json, family, variants, 100);
    ot_resources resources = model_resources(l);
    const ot_preferences *p = ot_get_preferences(l->app);
    double scores[100];
    size_t order[100];
    for (size_t i = 0; i < count; i++) scores[i] = ot_model_candidate_score(&variants[i], &resources, p);
    size_t ranked = ot_model_candidate_order(variants, scores, count, order);
    /* A provider/family cannot consume the whole catalogue. Inspect the bounded
     * file list first, then retain variants suited to this machine, not file order. */
    size_t budget = p->setup_engines == 3 ? OT_ADVICE_MODELS / 2 : OT_ADVICE_MODELS;
    size_t quota = budget / l->advice_family_count;
    bool retained[100] = {0};
    double distance[100];
    for (size_t i = 0; i < count; i++) distance[i] = HUGE_VAL;
    size_t previous = SIZE_MAX;
    for (size_t slot = 0; slot < quota && slot < ranked && l->advice_count < OT_ADVICE_MODELS; slot++) {
        size_t best = SIZE_MAX;
        double spread = -HUGE_VAL;
        for (size_t rank = 0; rank < ranked; rank++) {
            size_t i = order[rank];
            if (retained[i] || (scores[order[0]] >= 0 && scores[i] < 0)) continue;
            if (previous != SIZE_MAX && variants[i].bytes && variants[previous].bytes)
                distance[i] = fmin(distance[i], fabs(log((double)variants[i].bytes / (double)variants[previous].bytes)));
            /* Keep size coverage, not twelve near-identical variants around the
             * old filter. Later local reranking can then honour new size/purpose choices. */
            double value = previous == SIZE_MAX ? scores[i] : distance[i] + .1 * fmax(0,scores[i]);
            if (best == SIZE_MAX || value > spread) {best = i;spread = value;}
        }
        if (best == SIZE_MAX) break;
        retained[best] = true; previous = best;
        l->advice[l->advice_count++] = variants[best];
    }
    free(variants);
}
static void advice_response(ot_library *l, const char *error) {
    cancel(l, &l->advice_id);
    if (l->advice_evidence) {
        if (!error && l->advice_body && l->advice_evidence_count) {
            const char *id = l->advice_evidence_ids[l->advice_evidence_count - 1];
            for (size_t i = 0; i < l->advice_count; i++) {
                ot_model_candidate *m = &l->advice[i];
                if (!strcmp(id, m->repository) || !strcmp(id, m->base))
                    ot_model_evidence(m, l->advice_body, strcmp(id, m->repository) != 0);
            }
        }
        advice_next(l);
        return;
    }
    if (l->advice_verifying) {
        if (!error && l->advice_body && l->advice_verification) {
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            char hash[65];
            CC_SHA256(l->advice_body, (CC_LONG)l->advice_size, digest);
            for (unsigned i = 0; i < sizeof digest; i++)
                snprintf(hash + i * 2, 3, "%02x", digest[i]);
            ot_model_candidate *m = &l->advice[l->advice_verification - 1];
            m->verified = ot_model_verified(m->name, hash);
        }
        advice_next(l);
        return;
    }
    if (l->advice_hf) {
        if (!l->advice_family_count || l->advice_recent) {
            ot_model_candidate families[32];
            size_t count = !error ? ot_model_hf_feed(l->advice_body, families, 32) : 0;
            ot_resources resources = model_resources(l);
            const ot_preferences *p = ot_get_preferences(l->app);
            size_t limit = l->advice_recent ? OT_ADVICE_FAMILIES : OT_ADVICE_FAMILIES / 2;
            for (size_t rank = 0; rank < count && l->advice_family_count < limit; rank++) {
                size_t best = rank;
                for (size_t i = rank + 1; i < count; i++)
                    if (ot_model_candidate_score(&families[i], &resources, p) >
                        ot_model_candidate_score(&families[best], &resources, p))
                        best = i;
                ot_model_candidate swap = families[rank];
                families[rank] = families[best];
                families[best] = swap;
                bool duplicate = false;
                for (size_t j = 0; j < l->advice_family_count; j++)
                    if (!strcmp(families[rank].repository, l->advice_families[j].repository) ||
                        (*families[rank].base && !strcmp(families[rank].base, l->advice_families[j].base))) duplicate = true;
                if (!duplicate) l->advice_families[l->advice_family_count++] = families[rank];
            }
            if (!l->advice_recent) {
                l->advice_recent = true;
                advice_request(l, advice_hf_recent);
                return;
            }
            l->advice_recent = false;
        } else {
            if (!error && l->advice_family < l->advice_family_count)
                advice_variants(l, l->advice_body, &l->advice_families[l->advice_family], true);
            l->advice_family++;
        }
        advice_next(l);
        return;
    }
    if (!l->advice_family_count) {
        if (!l->advice_popular && !error)
            l->advice_family_count =
                ot_model_feed(l->advice_body, l->advice_size, l->advice_families, OT_ADVICE_FAMILIES);
        if (!l->advice_family_count && !l->advice_popular) {
            l->advice_popular = true;
            advice_request(l, "https://ollama.com/search?o=popular");
            return;
        }
        if (!l->advice_family_count && !error) {
            char *catalogue = ot_ollama_catalog(l->advice_body, NULL);
            yyjson_doc *doc = catalogue ? yyjson_read(catalogue, strlen(catalogue), 0) : NULL;
            yyjson_val *rows = yyjson_doc_get_root(doc);
            for (size_t i = 0; i < yyjson_arr_size(rows) && l->advice_family_count < OT_ADVICE_FAMILIES;
                 i++) {
                const char *name = str(yyjson_arr_get(rows, i), "name");
                if (ot_model_name(name) && !strchr(name, ':') && !strchr(name, '/') &&
                    !yyjson_is_false(yyjson_obj_get(yyjson_arr_get(rows, i), "chatSupported"))) {
                    ot_model_candidate *m = &l->advice_families[l->advice_family_count++];
                    *m = (ot_model_candidate){.kind = 2};
                    copy(m->name, sizeof m->name, name);
                }
            }
            free(catalogue);
            yyjson_doc_free(doc);
        }
        advice_next(l);
        return;
    }
    if (!error && l->advice_family < l->advice_family_count) {
        ot_model_candidate *family = &l->advice_families[l->advice_family];
        char name[257];
        copy(name, sizeof name, family->name);
        char *colon = strchr(name, ':');
        if (colon)
            *colon = 0;
        char *catalogue = ot_ollama_catalog(l->advice_body, name);
        if (catalogue) advice_variants(l, catalogue, family, false);
        free(catalogue);
    }
    l->advice_family++;
    advice_next(l);
}
static void advice_deliver(ot_library *l, const char *error) {
    advice_response(l, error);
    while (l->advice_id && l->advice_buffered) {
        char next[64] = {0}; copy(next, sizeof next, l->advice_buffer_error);
        l->advice_buffered = false;
        advice_response(l, *next ? next : NULL);
    }
}
static int64_t named_model(ot_library *l, const char *name) {
    bool local = !strncmp(name, "local:", 6), gguf = !strncmp(name, "gguf:", 5);
    sqlite3_stmt *q =
        query(l, local  ? "SELECT id FROM models WHERE id=? AND hidden=0 AND available=1"
                 : gguf ? "SELECT id FROM models WHERE kind=1 AND json_extract(metadata,'$.checksum')=? "
                          "AND hidden=0 AND available=1"
                        : "SELECT id FROM models WHERE kind=2 AND reference=? AND hidden=0 AND available=1");
    if (q) {
        if (local)
            sqlite3_bind_text(q, 1, name + 6, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_text(q, 1, name + (gguf ? 5 : 0), -1, SQLITE_TRANSIENT);
    }
    int64_t id = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    return id;
}
static bool setup_engines_ready(ot_library *l, const ot_preferences *p) {
    char path[4096];
    unsigned engines = p->setup_target_engines ? p->setup_target_engines : p->setup_engines;
    for (int kind = 1; kind <= 2; kind++)
        if ((engines & (unsigned)kind) && !engine_path(l, kind, path))
            return false;
    return true;
}
static void setup_tick(ot_library *l) {
    const ot_preferences *p = ot_get_preferences(l->app);
    if (p->setup_paused || !*p->setup_target || l->setup_revision == l->revision)
        return;
    l->setup_revision = l->revision;
    int64_t id = named_model(l, p->setup_target);
    if (!id || l->working || l->transfer_id || l->pending || !ot_is_idle(l->app))
        return;
    if (!setup_engines_ready(l, p))
        return;
    if (l->selected == id && !strcmp(l->engine_state, "ready") && ot_ready(l->app))
        return; /* Only the explicit Finish action completes onboarding. */
    if (l->requested != id)
        select_model(l, id);
}
static void setup_initial_preferences(ot_library *l, ot_preferences *p) {
    if (p->setup != 2 && !*p->setup_target && !ot_ready(l->app) && !strcmp(p->thinking_mode, "auto")) {
        p->thinking = false;
        copy(p->thinking_mode, sizeof p->thinking_mode, "off");
    }
}
/* Back holds the accepted plan. A new confirmation retains only resources still
 * requested; completed installations and borrowed user jobs are never erased. */
static bool setup_jobs(ot_library *l, const char *target, unsigned engines, bool hold) {
    int64_t cursor = 0;
    for (;;) {
        int64_t ids[64];
        bool keep[64], borrowed[64];
        size_t count = 0;
        sqlite3_stmt *q = query(l, "SELECT id,url,checksum,coalesce(json_extract(metadata,'$.setupBorrowed'),0) "
            "FROM downloads WHERE id>? AND json_extract(metadata,'$.setup')=1 AND state<>'complete' AND coalesce(json_extract(metadata,'$.discard'),0)=0 ORDER BY id LIMIT 64");
        if (!q) return false;
        sqlite3_bind_int64(q, 1, cursor);
        int step;
        while ((step = sqlite3_step(q)) == SQLITE_ROW) {
            const char *url = col(q, 1), *hash = col(q, 2);
            const ot_package *package = ot_package_for_download(url, hash);
            char path[4096];
            ids[count] = sqlite3_column_int64(q, 0);
            borrowed[count] = sqlite3_column_int(q, 3) != 0;
            keep[count++] = package ? (engines & (unsigned)package->kind) && !engine_path(l,package->kind,path) :
                !strncmp(target, "gguf:", 5) ? !strcmp(hash, target + 5) :
                !strncmp(url, "ollama:", 7) && !strcmp(url + 7, target);
        }
        sqlite3_finalize(q);
        if (step != SQLITE_DONE) return false;
        if (!count) return true;
        for (size_t i = 0; i < count; i++) {
            int64_t id = ids[i];
            if ((hold || !keep[i]) && id == l->job) stop_transfer(l);
            if (!hold && !keep[i] && !borrowed[i]) {
                if (!request_cleanup(l,id,true)) return false;
                q=query(l,"UPDATE downloads SET state='cancelled' WHERE id=?");
            } else if (!hold && !keep[i]) {
                q = query(l, "UPDATE downloads SET state=CASE WHEN state='cancelled' THEN state ELSE 'paused' END,"
                    "metadata=json_remove(metadata,'$.setup','$.setupBorrowed'),updated=unixepoch() WHERE id=?");
            } else if (hold) {
                q = query(l, "UPDATE downloads SET state=CASE WHEN state='cancelled' THEN state ELSE 'paused' END,"
                    "updated=unixepoch() WHERE id=?");
            } else continue;
            if (q) sqlite3_bind_int64(q, 1, id);
            if (!done(q)) return false;
        }
        cursor = ids[count - 1];
    }
}
static bool hold_setup(ot_library *l, ot_preferences *p) {
    p->setup_paused = *p->setup_target || p->setup_target_engines;
    bool stopped = setup_jobs(l, "", 0, true);
    if (l->pending && named_model(l,p->setup_target)==l->pending) {
        cancel(l,&l->health_id);l->pending=0;l->requested=0;
        ot_maintenance(l->app,false,NULL);ot_engine_stop(&l->engine);
        copy(l->engine_state,sizeof l->engine_state,"stopped");
    }
    bool saved = save_preferences(l, p);
    return stopped && saved;
}
static bool local_download(ot_library *l,int64_t id,const char *url,uint64_t total) {
    char part[4096];struct stat st;
    return strncmp(url,"ollama:",7) && total>0 && path_for(l,id,".part",part) && !lstat(part,&st) &&
        S_ISREG(st.st_mode) && st.st_size>=0 && (uint64_t)st.st_size==total;
}
/* Required scopes are computed from the durable plan, not the visible catalogue.
 * A complete local partial can finish verification/install with Internet disabled. */
static bool setup_access(ot_library *l, const ot_preferences *p, unsigned *required) {
    *required=0;
    char executable[4096];
    unsigned missing = 0;
    for (int kind=1;kind<=2;kind++)
        if ((p->setup_target_engines & (unsigned)kind) && !engine_path(l,kind,executable))
            missing |= (unsigned)kind;
    sqlite3_stmt *q=query(l,"SELECT id,url,checksum,total FROM downloads WHERE json_extract(metadata,'$.setup')=1 "
        "AND state<>'complete' AND coalesce(json_extract(metadata,'$.discard'),0)=0");
    if (!q) return false;
    int step;
    while ((step=sqlite3_step(q))==SQLITE_ROW) {
        bool local=local_download(l,sqlite3_column_int64(q,0),col(q,1),(uint64_t)sqlite3_column_int64(q,3));
        const ot_package *package = ot_package_for_download(col(q,1),col(q,2));
        if (local && package) missing &= ~(unsigned)package->kind;
        if (!local) *required |= package ? OT_NETWORK_ENGINES : OT_NETWORK_MODELS;
    }
    if (missing) *required |= OT_NETWORK_ENGINES;
    sqlite3_finalize(q);return step==SQLITE_DONE;
}
static bool setup_engines(ot_library *l, const ot_preferences *p, bool allow);
static bool attach_setup(ot_library *l,const ot_preferences *p) {
    for (int kind=0;kind<=2;kind++) {
        char url[512];const char *hash="";
        bool gguf=kind==0 && !strncmp(p->setup_target,"gguf:",5);
        if (kind) {
            char executable[4096];
            if (!(p->setup_target_engines&(unsigned)kind) || engine_path(l,kind,executable)) continue;
            const ot_package *package=ot_package_for_kind(kind);hash=package->checksum;
            copy(url,sizeof url,package->url);
        } else {
            if (!*p->setup_target || !strncmp(p->setup_target,"local:",6)) continue;
            if (gguf) hash=p->setup_target+5;
            snprintf(url,sizeof url,"ollama:%s",p->setup_target);
        }
        sqlite3_stmt *q=query(l,gguf ?
            "SELECT id FROM downloads WHERE checksum=? AND state<>'complete' AND coalesce(json_extract(metadata,'$.discard'),0)=0 ORDER BY id DESC LIMIT 1" :
            "SELECT id FROM downloads WHERE url=? AND checksum=? AND state<>'complete' AND coalesce(json_extract(metadata,'$.discard'),0)=0 ORDER BY id DESC LIMIT 1");
        if (q) {sqlite3_bind_text(q,1,gguf ? hash : url,-1,SQLITE_TRANSIENT);if (!gguf) sqlite3_bind_text(q,2,hash,-1,SQLITE_TRANSIENT);}
        int result=q ? sqlite3_step(q) : SQLITE_ERROR;
        int64_t id=result==SQLITE_ROW ? sqlite3_column_int64(q,0) : 0;sqlite3_finalize(q);
        if ((result!=SQLITE_ROW && result!=SQLITE_DONE) || (id && !setup_owns_job(l,id))) return false;
    }
    return true;
}
static bool resume_setup(ot_library *l, ot_preferences *p) {
    if (!attach_setup(l,p)) return false;
    if (!setup_jobs(l,p->setup_target,p->setup_target_engines,false)) return false;
    unsigned required;
    if (!setup_access(l,p,&required)) {changed(l,"storage_read");return false;}
    if (!ot_network_allowed(p,(ot_network_scope)required)) {changed(l,"internet_disabled");return false;}
    p->setup_paused = false;p->setup = 3;
    if (!save_preferences(l, p)) return false;
    bool ok = done(query(l, "UPDATE downloads SET state='queued',error='',updated=unixepoch() "
        "WHERE json_extract(metadata,'$.setup')=1 AND state IN('paused','cancelled') AND coalesce(json_extract(metadata,'$.discard'),0)=0"));
    l->queue_pending = true;l->setup_revision = 0;
    /* A previously completed dependency may have been removed outside OverTone. */
    ot_preferences missing=*p;missing.setup_engines=0;
    for (int kind=1;ok && kind<=2;kind++) {
        char executable[4096];
        if (!(p->setup_target_engines&(unsigned)kind) || engine_path(l,kind,executable)) continue;
        const ot_package *package=ot_package_for_kind(kind);
        sqlite3_stmt *job=query(l,"SELECT EXISTS(SELECT 1 FROM downloads WHERE url=? AND checksum=? AND state<>'complete' "
            "AND coalesce(json_extract(metadata,'$.discard'),0)=0)");
        if (job) {sqlite3_bind_text(job,1,package->url,-1,SQLITE_STATIC);sqlite3_bind_text(job,2,package->checksum,-1,SQLITE_STATIC);}
        ok=job && sqlite3_step(job)==SQLITE_ROW;
        if (ok && !sqlite3_column_int(job,0)) missing.setup_engines|=(unsigned)kind;
        sqlite3_finalize(job);
    }
    return ok && setup_engines(l,&missing,true);
}
static bool setup_engines(ot_library *l, const ot_preferences *p, bool allow) {
    char path[4096];
    for (int kind = 1; kind <= 2; kind++) {
        if (!(p->setup_engines & (unsigned)kind) || engine_path(l, kind, path))
            continue;
        if (!allow || !ot_network_allowed(p, OT_NETWORK_ENGINES)) {
            changed(l, "internet_disabled");
            return false;
        }
        const ot_package *package = ot_package_for_kind(kind);
        if (!package) {
            changed(l, "engine_not_found");
            return false;
        }
        const char metadata[] = "{\"setup\":true}";
        yyjson_doc *doc = yyjson_read(metadata, sizeof metadata - 1, 0);
        if (!doc) {changed(l, "request_failed"); return false;}
        enqueue(l, package->url, package->name, package->checksum, package->bytes, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
        if (*l->error)
            return false;
    }
    return true;
}
static void enqueue_suggestion(ot_library *l, const ot_model_candidate *m, int64_t replace, bool setup) {
    char url[2048];
    yyjson_mut_doc *meta = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *fields = meta ? yyjson_mut_obj(meta) : NULL;
    if (!meta || !fields) {
        yyjson_mut_doc_free(meta);
        changed(l, "request_failed");
        return;
    }
    if (m->kind == 1) {
        char *repo = encoded(m->repository, true), *file = encoded(m->file, true);
        if (!repo || !file || !meta || !fields) {
            free(repo);
            free(file);
            yyjson_mut_doc_free(meta);
            changed(l, "request_failed");
            return;
        }
        snprintf(url, sizeof url, "https://huggingface.co/%s/resolve/%s/%s", repo, m->revision, file);
        char source[1024];
        snprintf(source, sizeof source, "https://huggingface.co/%s", repo);
        yyjson_mut_obj_add_strcpy(meta, fields, "sourceURL", source);
        yyjson_mut_obj_add_strcpy(meta, fields, "license", m->license);
        yyjson_mut_obj_add_strcpy(meta, fields, "revision", m->revision);
        free(repo);
        free(file);
    } else
        snprintf(url, sizeof url, "ollama:%s", m->name);
    if (setup) yyjson_mut_obj_add_bool(meta, fields, "setup", true);
    if (replace) yyjson_mut_obj_add_sint(meta, fields, "replaceId", replace);
    yyjson_mut_doc_set_root(meta, fields);
    char *json = yyjson_mut_write(meta, 0, NULL);
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    enqueue(l, url, m->name, m->kind == 1 ? m->checksum : "", m->bytes, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    free(json);
    yyjson_mut_doc_free(meta);
}
static void setup_command(ot_library *l, yyjson_val *v) {
    const char *mode = str(v, "mode");
    ot_preferences p = *ot_get_preferences(l->app);
    if (!strcmp(mode, "open")) {
        l->advice_visible = true;
        /* Opening setup is a durable in-progress state, distinct from completion.
         * A restart can then resume even after a server/model has become ready. */
        if (p.setup < 2) {
            p.setup = 3;
            changed(l, save_preferences(l, &p) ? NULL : "storage_write");
        } else changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "follow")) {
        /* Returning to committed work is local and independent of a changing catalogue. */
        const char *target = str(v, "target");
        unsigned engines = (unsigned)yyjson_get_uint(yyjson_obj_get(v, "engines"));
        if ((!*p.setup_target && !p.setup_target_engines) || strcmp(target, p.setup_target) ||
            engines != p.setup_target_engines) {
            changed(l, "suggestion_changed");
            return;
        }
        advice_cancel(l);
        advice_restore(l);
        if (!resume_setup(l, &p)) {if (!*l->error) changed(l,"storage_write");return;}
        changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "back") || !strcmp(mode, "choose")) {
        if (!hold_setup(l, &p)) {changed(l,"storage_write");return;}
        l->advice_visible = true;
        advice_cancel(l);advice_restore(l);
        if (!strcmp(mode,"choose")) detect_engines(l);
        changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "enginesOnly")) {
        bool missing = !setup_engines_ready(l, &(ot_preferences){.setup_engines=p.setup_engines});
        if (yyjson_is_true(yyjson_obj_get(v, "allowDownloads")))
            ot_network_enable(&p, OT_NETWORK_ENGINES);
        bool allowed = ot_network_allowed(&p, OT_NETWORK_ENGINES);
        if (missing && !allowed) { changed(l, "internet_disabled"); return; }
        p.setup = 3;p.setup_paused = false;
        p.setup_target[0] = 0;
        p.setup_target_engines = p.setup_engines;
        if (!save_preferences(l, &p)) { changed(l, "storage_write"); return; }
        if (!setup_jobs(l, "", p.setup_engines, false)) {changed(l,"storage_write");return;}
        advice_cancel(l);
        advice_restore(l);
        if (setup_engines(l, &p, allowed)) changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "engines")) {
        yyjson_val *mask = yyjson_obj_get(v, "engines");
        if (!yyjson_is_uint(mask) || yyjson_get_uint(mask) < 1 || yyjson_get_uint(mask) > 3) {
            changed(l, "invalid_input");
            return;
        }
        unsigned engines = (unsigned)yyjson_get_uint(mask);
        if (p.setup_engines != engines) {
            p.setup_engines = engines;
            if (!save_preferences(l, &p)) {
                changed(l, "storage_write");
                return;
            }
            advice_cancel(l);
            advice_restore(l);
            advice_begin(l, false);
        }
        changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "use")) {
        int64_t id = yyjson_get_sint(yyjson_obj_get(v, "id"));
        sqlite3_stmt *q =
            query(l, "SELECT kind,reference FROM models WHERE id=? AND hidden=0 AND available=1");
        if (q)
            sqlite3_bind_int64(q, 1, id);
        bool found = q && sqlite3_step(q) == SQLITE_ROW;
        int kind = found ? sqlite3_column_int(q, 0) : 0;
        char target[257];
        bool copied =
            found && snprintf(target, sizeof target, "local:%lld", (long long)id) < (int)sizeof target;
        sqlite3_finalize(q);
        if (!copied) {
            changed(l, "invalid_model");
            return;
        }
        yyjson_val *engines = yyjson_obj_get(v, "engines");
        if (engines) {
            uint64_t value = yyjson_get_uint(engines);
            if (!yyjson_is_uint(engines) || value < 1 || value > 3 || !(value & (unsigned)kind)) {
                changed(l, "invalid_input");
                return;
            }
            p.setup_engines = (unsigned)value;
        }
        setup_initial_preferences(l, &p);
        p.setup_engines |= (unsigned)kind;
        bool consent = yyjson_is_true(yyjson_obj_get(v, "allowDownloads"));
        if (consent) { ot_network_enable(&p, OT_NETWORK_CATALOGUES | OT_NETWORK_MODELS | OT_NETWORK_ENGINES); }
        bool allowed = ot_network_allowed(&p, OT_NETWORK_ENGINES);
        char path[4096];
        bool missing = false;
        for (int k = 1; k <= 2; k++)
            missing |= (p.setup_engines & (unsigned)k) && !engine_path(l, k, path);
        if (missing && !allowed) {
            changed(l, "internet_disabled");
            return;
        }
        if (missing)
            p.internet = true;
        p.setup = 3;p.setup_paused = false;
        copy(p.setup_target, sizeof p.setup_target, target);
        p.setup_target_engines = p.setup_engines;
        if (!save_preferences(l, &p)) {
            changed(l, "storage_write");
            return;
        }
        if (!setup_jobs(l, target, p.setup_engines, false)) {changed(l,"storage_write");return;}
        advice_cancel(l);
        if (l->requested == id && !strcmp(l->engine_state, "failed"))
            l->requested = 0;
        if (setup_engines(l, &p, allowed)) {
            l->setup_revision = 0;
            changed(l, NULL);
        }
        return;
    }
    if (!strcmp(mode, "size")) {
        yyjson_val *value = yyjson_obj_get(v, "gb");
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) > 512) { changed(l,"invalid_input"); return; }
        p.advice_size_gb = (unsigned)yyjson_get_uint(value);
        changed(l, save_preferences(l,&p) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "purpose")) {
        yyjson_val *value = yyjson_obj_get(v, "purpose");
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) > 5) {
            changed(l, "invalid_input");
            return;
        }
        p.purpose = (unsigned)yyjson_get_uint(value);
        changed(l, save_preferences(l, &p) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "skip") || !strcmp(mode, "finish")) {
        bool finish = !strcmp(mode, "finish");
        bool operation = *p.setup_target || p.setup_target_engines;
        if (finish && (p.setup_paused || (operation && !setup_engines_ready(l, &p)) ||
            (*p.setup_target ? !ot_ready(l->app) || named_model(l, p.setup_target) != l->selected :
                              !p.setup_target_engines && !ot_ready(l->app)))) {
            changed(l, "engine_not_ready");
            return;
        }
        if (!finish && !hold_setup(l, &p)) {changed(l,"storage_write");return;}
        if (finish && !done(query(l,"UPDATE downloads SET metadata=json_remove(metadata,'$.setup','$.setupBorrowed') WHERE json_extract(metadata,'$.setup')=1"))) {changed(l,"storage_write");return;}
        advice_cancel(l);
        l->advice_visible = false;
        advice_restore(l);
        if (finish) {p.setup_target[0] = 0;p.setup_target_engines=0;p.setup_paused=false;}
        p.setup = finish || p.setup == 2 ? 2 : 1;
        if (!save_preferences(l, &p))
            changed(l, "storage_write");
        else
            changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "reset")) {
        if (!setup_jobs(l,"",0,false)) {changed(l,"storage_write");return;}
        p.setup_target[0] = 0;p.setup_target_engines=0;p.setup_paused=false;
        changed(l, save_preferences(l, &p) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "suggest")) {
        l->advice_visible = true;
        if (yyjson_is_true(yyjson_obj_get(v, "allowDownloads"))) {
            ot_network_enable(&p, OT_NETWORK_CATALOGUES | OT_NETWORK_MODELS | OT_NETWORK_ENGINES);
            if (!save_preferences(l, &p)) {
                changed(l, "storage_write");
                return;
            }
        }
        if (!ot_network_allowed(&p, OT_NETWORK_CATALOGUES)) {
            changed(l, "internet_disabled");
            return;
        }
        advice_begin(l, yyjson_is_true(yyjson_obj_get(v, "refresh")));
        changed(l, NULL);
        return;
    }
    if (!strcmp(mode, "cancelSuggestions")) {
        advice_cancel(l);advice_restore(l);
        changed(l, NULL);
        return;
    }
    if (strcmp(mode, "prepare")) {
        changed(l, "invalid_input");
        return;
    }
    if (yyjson_is_true(yyjson_obj_get(v, "allowDownloads"))) {
        ot_network_enable(&p, OT_NETWORK_CATALOGUES | OT_NETWORK_MODELS | OT_NETWORK_ENGINES);
    }
    bool allowed = ot_network_allowed(&p, OT_NETWORK_MODELS);
    if (!allowed) {
        changed(l, "internet_disabled");
        return;
    }
    const char *name = str(v, "name");
    yyjson_val *engine_kind = yyjson_obj_get(v, "kind");
    if (engine_kind && (!yyjson_is_uint(engine_kind) || yyjson_get_uint(engine_kind) < 1 ||
                        yyjson_get_uint(engine_kind) > 2)) {
        changed(l, "invalid_input");
        return;
    }
    int kind = engine_kind ? (int)yyjson_get_uint(engine_kind) : 2;
    const ot_model_candidate *m = NULL;
    for (size_t i = 0; i < l->advice_count; i++)
        if (!strcmp(name, l->advice[i].name) && l->advice[i].kind == kind &&
            (p.setup_engines & (unsigned)kind)) {
            m = &l->advice[i];
            break;
        }
    if (!m) {
        changed(l, "suggestion_changed");
        return;
    }
    char target[257];
    if (m->kind == 1)
        snprintf(target, sizeof target, "gguf:%s", m->checksum);
    else
        copy(target, sizeof target, m->name);
    int64_t existing = named_model(l, target);
    ot_resources resources = model_resources(l);
    if (!existing && !ot_model_candidate_fits(m, &resources, &p)) {
        changed(l, resources.disk_available && (m->bytes >= resources.disk_available ||
                                                (UINT64_C(2) << 30) >= resources.disk_available - m->bytes)
                       ? "disk_budget"
                       : "memory_limit");
        return;
    }
    setup_initial_preferences(l, &p);
    p.setup = 3;p.setup_paused = false;
    p.internet = allowed;
    copy(p.setup_target, sizeof p.setup_target, target);
    p.setup_target_engines = p.setup_engines;
    if (!save_preferences(l, &p)) {
        changed(l, "storage_write");
        return;
    }
    if (!setup_jobs(l, target, p.setup_engines, false)) {changed(l,"storage_write");return;}
    advice_cancel(l);
    if (!setup_engines(l, &p, allowed))
        return;
    if (existing) {
        if (l->requested == existing && !strcmp(l->engine_state, "failed"))
            l->requested = 0;
        l->setup_revision = 0;
        changed(l, NULL);
        return;
    }
    enqueue_suggestion(l, m, 0, true);
}
static void remove_model(ot_library *l, int64_t id, bool uninstall) {
    if (l->removing || l->download_preparing || l->transfer_id) {
        changed(l, "busy");
        return;
    }
    sqlite3_stmt *q = query(l, "SELECT reference,managed,device,inode,kind,origin FROM models WHERE id=?");
    if (!q)
        return;
    sqlite3_bind_int64(q, 1, id);
    bool ok = sqlite3_step(q) == SQLITE_ROW;
    char path[4096];
    int64_t managed = ok ? sqlite3_column_int64(q, 1) : 0;
    bool service = ok && uninstall && sqlite3_column_int(q, 4) == 2 && ot_model_name(col(q, 0)) &&
                   copy(l->remove_name, sizeof l->remove_name, col(q, 0));
    if (ok && uninstall && !service) {
        struct stat st;
        bool owned = managed && path_for(l, managed, ".gguf", path) && !strcmp(path, col(q, 0));
        bool manual = !managed && sqlite3_column_int(q, 4) == 1 && !strcmp(col(q, 5), "manual") &&
                      copy(path, sizeof path, col(q, 0));
        ok = (owned || manual) && !lstat(path, &st) && S_ISREG(st.st_mode) &&
             st.st_dev == (dev_t)sqlite3_column_int64(q, 2) && st.st_ino == (ino_t)sqlite3_column_int64(q, 3);
    }
    sqlite3_finalize(q);
    if (!ok) {
        changed(l, "file_not_owned");
        return;
    }
    ot_memory_cancel(l->memory);
    if (l->selected == id || l->pending == id || l->requested == id || ot_local_model(l->app)==id) {
        if (l->pending)
            ot_maintenance(l->app, false, NULL);
        ot_connection connection = *ot_configuration(l->app);
        connection.model[0] = 0;
        if (!ot_configure(l->app, &connection, 0)) {
            changed(l, "busy");
            return;
        }
        cancel(l, &l->health_id);
        l->pending = l->selected = l->requested = 0;
        l->engine_error[0] = 0;
        ot_engine_stop(&l->engine);
        copy(l->engine_state, sizeof l->engine_state, l->engine.pid > 0 ? "stopping" : "stopped");
    }
    if (service) {
        l->removing = id;
        if (*l->download_uri)
            transfer(l);
        else {
            l->download_preparing = true;
            l->download_external = true;
            l->download_started = now();
            download_probe(l);
            changed(l, NULL);
        }
        return;
    }
    if (uninstall && unlink(path)) {
        changed(l, "file_operation");
        return;
    }
    changed(l, ot_remove_model(l->app, id, uninstall) ? NULL : "storage_write");
}
static void discard_job(ot_library *l, int64_t id) {
    sqlite3_stmt *q = query(l, "SELECT state FROM downloads WHERE id=?");
    if (!q)
        return;
    sqlite3_bind_int64(q, 1, id);
    bool ok =
        sqlite3_step(q) == SQLITE_ROW && (!strcmp(col(q, 0), "complete") || !strcmp(col(q, 0), "cancelled"));
    bool completed = ok && !strcmp(col(q, 0), "complete");
    sqlite3_finalize(q);
    if (!ok) {
        changed(l, "invalid_job");
        return;
    }
    if (completed && !cleanup_replacement(l, id)) {
        changed(l, "replacement_cleanup");
        return;
    }
    if (!completed) {
        changed(l,request_cleanup(l,id,true) ? NULL : "storage_write");return;
    }
    q = query(l, "DELETE FROM downloads WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, id);
    changed(l, done(q) ? NULL : "storage_write");
}
static void discover(ot_library *l) {
    if (l->working || l->transfer_id) {
        changed(l, "busy");
        return;
    }
    l->worker_kind = 2;
    clear_registered(l);
    l->registered = calloc(1000, sizeof *l->registered);
    sqlite3_stmt *q = l->registered
                          ? query(l, "SELECT id,CASE WHEN kind=1 THEN reference ELSE "
                                     "json_extract(metadata,'$.localManifest') END,device,inode,kind,"
                                     "coalesce(json_extract(metadata,'$.localManifestRootLength'),0) "
                                     ",bytes,coalesce(json_extract(metadata,'$.checksum'),''),"
                                     "coalesce(json_extract(metadata,'$.fileModified'),0),"
                                     "coalesce(json_extract(metadata,'$.fileModifiedNsec'),0) "
                                     ",coalesce(json_extract(metadata,'$.fileChanged'),0) "
                                     "FROM models WHERE kind=1 OR "
                                     "(kind=2 AND json_type(metadata,'$.localManifest')='text' AND "
                                     "json_type(metadata,'$.localManifestRootLength')='integer') "
                                     "ORDER BY (id=?1) DESC,id LIMIT 1001")
                          : NULL;
    if (!q) {
        clear_registered(l);
        changed(l, "storage_read");
        return;
    }
    sqlite3_bind_int64(q, 1, ot_local_model(l->app));
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (l->registered_count == 1000) { l->registered_limited = true; break; }
        registered_file *file = &l->registered[l->registered_count];
        file->path = strdup(col(q, 1));
        if (!file->path) {
            sqlite3_finalize(q);
            clear_registered(l);
            changed(l, "storage_read");
            return;
        }
        file->id = sqlite3_column_int64(q, 0);
        file->device = (dev_t)sqlite3_column_int64(q, 2);
        file->inode = (ino_t)sqlite3_column_int64(q, 3);
        file->manifest = sqlite3_column_int(q, 4) == 2;
        sqlite3_int64 root_length = sqlite3_column_int64(q, 5);
        file->root_length = root_length > 0 && root_length < 4096 ? (size_t)root_length : 0;
        file->bytes = (uint64_t)sqlite3_column_int64(q, 6);
        copy(file->checksum, sizeof file->checksum, col(q, 7));
        file->modified = sqlite3_column_int64(q, 8);
        file->modified_nsec = sqlite3_column_int64(q, 9);
        file->unverified = sqlite3_column_int(q, 10) != 0;
        l->registered_count++;
    }
    sqlite3_finalize(q);
    atomic_store(&l->worker_done, false);
    atomic_store(&l->worker_cancel, false);
    if (pthread_create(&l->worker, NULL, hash_partial, l)) {
        clear_registered(l);
        changed(l, "worker_failed");
        return;
    }
    l->working = true;
    changed(l, NULL);
}
ot_library *ot_library_open(ot_app *app, const char *profile, ot_library_io io) {
    ot_library *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->app = app;
    l->db = ot_store_connection(ot_app_store(app));
    l->io = io;
    l->file = -1;
    ot_engine_init(&l->engine);
    ot_engine_init(&l->download_engine);
    atomic_init(&l->worker_done, false);
    atomic_init(&l->worker_cancel, false);
    l->memory = ot_memory_open(ot_engine_memory_plan);
    l->memory_revision = 1;
    if (!copy(l->profile, sizeof l->profile, profile) ||
        !copy(l->components, sizeof l->components, io.components ? io.components : profile) ||
        snprintf(l->directory, sizeof l->directory, "%s/models", profile) >= (int)sizeof l->directory)
        goto fail;
    if (mkdir(l->directory, 0700) && errno != EEXIST)
        goto fail;
    struct stat st;
    if (lstat(l->components, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077))
        goto fail;
    if (lstat(l->directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077))
        goto fail;
    if (sqlite3_exec(l->db,
                     "UPDATE downloads SET state='paused',error='interrupted' WHERE state "
                     "IN('preparing','downloading','verifying','registering')",
                     NULL, NULL, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_stmt *queued = query(l, "SELECT EXISTS(SELECT 1 FROM downloads WHERE state='queued')");
    l->queue_pending = queued && sqlite3_step(queued) == SQLITE_ROW && sqlite3_column_int(queued, 0);
    sqlite3_finalize(queued);
    sqlite3_stmt *completed = query(l, "SELECT id,json_type(metadata,'$.replaceId')='integer' FROM downloads "
                                       "WHERE state='complete' AND url NOT LIKE 'ollama:%'");
    if (!completed)
        goto fail;
    while (sqlite3_step(completed) == SQLITE_ROW) {
        char part[4096], final[4096];
        struct stat a, b;
        int64_t id = sqlite3_column_int64(completed, 0);
        if (path_for(l, id, ".part", part) && path_for(l, id, ".gguf", final) && !lstat(part, &a) &&
            !lstat(final, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            unlink(part);
        if (sqlite3_column_int(completed, 1))
            cleanup_replacement(l, id);
    }
    sqlite3_finalize(completed);
    ot_preferences boot = *ot_get_preferences(l->app);
    if ((*boot.setup_target || boot.setup_target_engines) && !hold_setup(l, &boot)) goto fail;
    sqlite3_stmt *cleanup=query(l,"SELECT EXISTS(SELECT 1 FROM downloads WHERE json_extract(metadata,'$.pullCleanup')=1)");
    l->cleanup_pending=cleanup && sqlite3_step(cleanup)==SQLITE_ROW && sqlite3_column_int(cleanup,0);
    sqlite3_finalize(cleanup);
    l->catalog = strdup("[]");
    if (!l->catalog)
        goto fail;
    copy(l->engine_state, sizeof l->engine_state, "stopped");
    l->revision = 1;
    advice_load(l);
    detect_engines(l);
    sqlite3_stmt *selection = query(l, "SELECT model_id FROM settings WHERE id=1");
    int64_t saved =
        selection && sqlite3_step(selection) == SQLITE_ROW ? sqlite3_column_int64(selection, 0) : 0;
    sqlite3_finalize(selection);
    if (!ot_get_preferences(app)->setup && !*ot_get_preferences(app)->setup_target &&
        (saved > 0 || (*ot_configuration(app)->model && *ot_configuration(app)->endpoint))) {
        ot_preferences configured = *ot_get_preferences(app);
        configured.setup = 2;
        (void)save_preferences(l, &configured);
    }
    if (ot_get_preferences(app)->startup_model > 0)
        saved = ot_get_preferences(app)->startup_model;
    if (ot_get_preferences(app)->startup_model == -1) {
        saved = 0;
        ot_start_without_model(app);
    }
    bool held_selection=boot.setup_paused && saved>0 && named_model(l,boot.setup_target)==saved;
    if (saved > 0 && !held_selection) {
        select_model(l, saved);
        if (!strcmp(l->error, "file_changed") || !strcmp(l->error, "not_found"))
            l->requested = l->refresh_model = saved;
    }
    discover(l);
    return l;
fail:
    ot_library_close(l);
    return NULL;
}
void ot_library_manual(ot_library *l) {
    ot_preferences p = *ot_get_preferences(l->app);
    if (*p.setup_target || p.setup_target_engines) {
        if (!setup_jobs(l,"",0,false)) {changed(l,"file_operation");return;}
        p.setup_target[0] = 0;p.setup_target_engines=0;p.setup_paused=false;
        if (!save_preferences(l, &p)) {
            changed(l, "storage_write");
            return;
        }
    }
    l->engine_error[0] = 0;
    ot_memory_cancel(l->memory);
    cancel(l, &l->health_id);
    l->pending = l->selected = l->requested = 0;
    if (l->engine.pid > 0) {
        ot_engine_stop(&l->engine);
        l->ollama[0] = 0;
        copy(l->engine_state, sizeof l->engine_state, "stopping");
    }
    changed(l, NULL);
}
void ot_library_preferences_changed(ot_library *l, const ot_preferences *previous) {
    const ot_preferences *p = ot_get_preferences(l->app);
    bool catalogue_revoked = ot_network_allowed(previous, OT_NETWORK_CATALOGUES) &&
                             !ot_network_allowed(p, OT_NETWORK_CATALOGUES);
    if (catalogue_revoked) {
        advice_cancel(l);
        l->advice_count = 0; l->advice_ready = false;
        cancel(l, &l->query_id);
        free(l->catalog); l->catalog = NULL;
        for (unsigned i = 0; i < 8; i++) evict_catalog(l, &l->catalog_cache[i]);
    }
    ot_network_scope download = l->package ? OT_NETWORK_ENGINES : OT_NETWORK_MODELS;
    if (!ot_network_allowed(p, download) && !l->removing) {
        bool preparing = l->download_preparing;
        cancel(l, &l->prepare_id);
        l->download_preparing = false;
        ot_engine_stop(&l->download_engine);
        if (l->transfer_id) fail_transfer(l, "internet_disabled");
        else if (preparing && l->job) save_job(l, "paused", "internet_disabled");
    }
    if ((p->internet && !previous->internet) || (p->network & ~previous->network))
        l->queue_pending = true;
    bool resources_changed = ot_preferences_execution_changed(p, previous);
    if (resources_changed) {
        bool comparable = p->context != previous->context && p->gpu_layers == previous->gpu_layers &&
                          p->threads == previous->threads && p->memory_percent == previous->memory_percent;
        l->memory_previous = comparable && runtime_current(l) ? l->runtime : (ot_runtime_info){0};
        l->memory_previous_model = comparable ? l->selected : 0;
    }
    if (resources_changed && ((l->selected > 0 && !strcmp(l->engine_state, "ready")) ||
                              (l->requested > 0 && !strcmp(l->engine_state, "failed")))) {
        int64_t id = l->selected > 0 ? l->selected : l->requested;
        /* Some Ollama backends retain the original context/placement in their
         * runner. Recreate only our own server when its execution shape changes. */
        if (l->engine.pid > 0 && l->engine_ollama &&
            (p->context != previous->context || p->gpu_layers != previous->gpu_layers ||
             p->threads != previous->threads))
            ot_engine_stop(&l->engine);
        l->selected = 0;
        select_model(l, id);
    }
    /* Advice and library controls also depend on global preferences. Refresh
     * their projection even when no engine reload or network event is needed. */
    l->revision++;
}
void ot_library_server_changed(ot_library *l) {
    l->revision++;
}
bool ot_library_manages_model(const ot_library *l) {
    return l->selected || l->requested || l->pending || ot_local_model(l->app)>0;
}
static void advice_key(const ot_model_candidate *m, char out[280]) {
    if (m->kind == 1 && *m->checksum) snprintf(out, 280, "gguf:%s", m->checksum);
    else if (strlen(m->catalogue_digest) >= 12) snprintf(out, 280, "ollama:%.12s", m->catalogue_digest);
    else snprintf(out, 280, "name:%s", m->name);
}
static bool advice_ignored(ot_library *l, const char *key) {
    sqlite3_stmt *q = query(l, "SELECT 1 FROM advice_state,json_each(dismissed) WHERE value=? LIMIT 1");
    if (q) sqlite3_bind_text(q, 1, key, -1, SQLITE_TRANSIENT);
    bool ignored = q && sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    return ignored;
}
static bool advice_tag_changed(const ot_model_candidate *m, const char *digest) {
    if (!strncmp(digest, "sha256:", 7)) digest += 7;
    size_t length = strlen(m->catalogue_digest);
    return (length == 12 || length == 64) && sha(digest) &&
           strncasecmp(m->catalogue_digest, digest, length) != 0;
}
static void advice_command(ot_library *l, yyjson_val *v) {
    const char *mode = str(v, "mode");
    if (!strcmp(mode, "reset")) { changed(l, ot_reset_advice(l->app) ? NULL : "storage_write"); return; }
    if (!strcmp(mode, "close")) { l->advice_visible = false; changed(l, NULL); return; }
    if (!strcmp(mode, "seen")) {
        const char *key = str(v, "key");
        bool valid = (!strncmp(key, "gguf:", 5) && sha(key + 5)) ||
                     (!strncmp(key, "ollama:", 7) && strlen(key) == 19 &&
                      strspn(key + 7, "0123456789abcdefABCDEF") == 12) ||
                     (!strncmp(key, "name:", 5) && ot_model_name(key + 5));
        if (!valid) { changed(l, "invalid_input"); return; }
        ot_preferences p = *ot_get_preferences(l->app);
        copy(p.advice_seen, sizeof p.advice_seen, key);
        changed(l, save_preferences(l, &p) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "ignore")) {
        const char *key = str(v, "key");
        if (!*key || strlen(key) >= 280 || advice_ignored(l, key)) { changed(l, NULL); return; }
        sqlite3_stmt *q = query(l, "UPDATE advice_state SET dismissed=json_insert(CASE WHEN "
            "json_array_length(dismissed)>=128 THEN json_remove(dismissed,'$[0]') ELSE dismissed END,'$[#]',?1) WHERE id=1");
        if (q) sqlite3_bind_text(q, 1, key, -1, SQLITE_TRANSIENT);
        changed(l, done(q) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "restoreNotices")) {
        changed(l, done(query(l, "UPDATE advice_state SET dismissed='[]' WHERE id=1")) ? NULL : "storage_write");
        return;
    }
    if (!strcmp(mode, "open") || !strcmp(mode, "refresh")) {
        l->advice_visible = true;
        if (!strcmp(mode, "open") && l->advice_id && l->advice_snapshot) {
            advice_cancel(l);
            l->advice_count = ot_model_catalogue_read(l->advice_snapshot, l->advice, OT_ADVICE_MODELS);
            l->advice_ready = true;
        }
        /* Opening is read-only. Only an explicit refresh or the idle update
         * schedule contacts catalogues; a cached recommendation is already ready. */
        if (!strcmp(mode, "refresh")) {
            if (ot_network_allowed(ot_get_preferences(l->app), OT_NETWORK_CATALOGUES)) advice_begin(l, true);
        }
        changed(l, NULL);
        return;
    }
    if (strcmp(mode, "acquire")) { changed(l, "invalid_input"); return; }
    if (l->advice_id && l->advice_snapshot) {
        advice_cancel(l);
        l->advice_count = ot_model_catalogue_read(l->advice_snapshot, l->advice, OT_ADVICE_MODELS);
        l->advice_ready = true;
    }
    const char *name = str(v, "name");
    int kind = yyjson_get_int(yyjson_obj_get(v, "kind"));
    for (size_t i = 0; i < l->advice_count; i++) {
        const ot_model_candidate *m = &l->advice[i];
        if (kind != m->kind || strcmp(name, m->name)) continue;
        char target[280];
        if (kind == 1) snprintf(target, sizeof target, "gguf:%s", m->checksum);
        else copy(target, sizeof target, m->name);
        int64_t existing = named_model(l, target);
        bool update = false;
        if (existing && kind == 2) {
            sqlite3_stmt *q = query(l, "SELECT coalesce(json_extract(metadata,'$.digest'),'') FROM models WHERE id=?");
            if (q) sqlite3_bind_int64(q, 1, existing);
            update = q && sqlite3_step(q) == SQLITE_ROW && advice_tag_changed(m, col(q, 0));
            sqlite3_finalize(q);
        }
        if (existing && !update) { select_model(l, existing); return; }
        if (update && yyjson_get_sint(yyjson_obj_get(v, "replaceId")) != existing) {
            changed(l, "replacement_changed"); return;
        }
        if (!ot_network_allowed(ot_get_preferences(l->app), OT_NETWORK_MODELS)) { changed(l, "internet_disabled"); return; }
        ot_resources resources = model_resources(l);
        if (!ot_model_candidate_fits(m, &resources, ot_get_preferences(l->app))) {
            changed(l, "memory_limit"); return;
        }
        enqueue_suggestion(l, m, update ? existing : 0, false);
        return;
    }
    changed(l, "suggestion_changed");
}
void ot_library_close(ot_library *l) {
    if (!l)
        return;
    ot_preferences closing = *ot_get_preferences(l->app);
    if (*closing.setup_target || closing.setup_target_engines) (void)hold_setup(l, &closing);
    atomic_store(&l->worker_cancel, true);
    stop_transfer(l);
    cancel(l, &l->query_id);
    advice_cancel(l);
    free(l->advice_snapshot);
    cancel(l, &l->health_id);
    cancel(l, &l->runtime_id);
    cancel(l, &l->remove_id);
    if (l->working)
        pthread_join(l->worker, NULL);
    ot_memory_close(l->memory);
    ot_engine_close(&l->engine);
    ot_engine_close(&l->download_engine);
    free(l->prepare);
    free(l->query);
    free(l->health);
    free(l->runtime_body);
    free(l->catalog);
    free(l->query_url);
    for (unsigned i = 0; i < 8; i++)
        evict_catalog(l, &l->catalog_cache[i]);
    free(l->inventory.items);
    clear_registered(l);
    free(l);
}
void ot_library_command(ot_library *l, const char *json) {
    if (!json || strlen(json) > 16384) {
        changed(l, "invalid_input");
        return;
    }
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *v = yyjson_doc_get_root(d);
    const char *action = str(v, "action");
    const char *numbers[] = {"id", "kind", "gpuLayers"};
    bool valid = yyjson_is_obj(v);
    for (unsigned i = 0; valid && i < 3; i++) {
        yyjson_val *number = yyjson_obj_get(v, numbers[i]);
        valid = !number ||
                (yyjson_is_int(number) && (!yyjson_is_uint(number) || yyjson_get_uint(number) <= INT64_MAX));
        if (valid && number && i == 1)
            valid = yyjson_get_sint(number) >= 0 && yyjson_get_sint(number) <= 2;
    }
    if (!valid) {
        changed(l, "invalid_input");
        yyjson_doc_free(d);
        return;
    }
    int64_t id = yyjson_get_sint(yyjson_obj_get(v, "id"));
    if (l->removing) {
        changed(l, "busy");
        yyjson_doc_free(d);
        return;
    }
    ot_network_scope scope =
        !strcmp(action, "search") || !strcmp(action, "variants") || !strcmp(action, "ollamaSearch") ||
        !strcmp(action, "ollamaVariants") ? OT_NETWORK_CATALOGUES :
        !strcmp(action, "download") || !strcmp(action, "pull") || !strcmp(action, "previewDownload") ||
        !strcmp(action, "confirmDownload") ? OT_NETWORK_MODELS : OT_NETWORK_LOCAL;
    if (!ot_network_allowed(ot_get_preferences(l->app), scope)) {
        changed(l, "internet_disabled");
        yyjson_doc_free(d);
        return;
    }
    if (!strcmp(action, "advice"))
        advice_command(l, v);
    else if (!strcmp(action, "setup"))
        setup_command(l, v);
    else if (!strcmp(action, "installEngine")) {
        const ot_package *package = ot_package_for_kind((int)yyjson_get_int(yyjson_obj_get(v, "kind")));
        char path[4096];
        if (!package || engine_path(l, package->kind, path)) {
            changed(l, package ? NULL : "invalid_engine");
            yyjson_doc_free(d);
            return;
        }
        ot_preferences p = *ot_get_preferences(l->app);
        if (yyjson_is_true(yyjson_obj_get(v, "allowDownloads"))) {
            ot_network_enable(&p, OT_NETWORK_ENGINES);
            if (!save_preferences(l, &p)) {
                changed(l, "storage_write");
                yyjson_doc_free(d);
                return;
            }
        }
        if (!ot_network_allowed(&p, OT_NETWORK_ENGINES))
            changed(l, "internet_disabled");
        else
            enqueue(l, package->url, package->name, package->checksum, package->bytes, v);
    } else if (!strcmp(action, "previewImport")) {
        if (l->working)
            changed(l, "busy");
        else if (!realpath(str(v, "path"), l->import_path))
            changed(l, "invalid_path");
        else {
            l->import_ready = false;
            l->worker_kind = 3;
            l->import_token = ++l->sequence;
            atomic_store(&l->worker_done, false);
            atomic_store(&l->worker_cancel, false);
            if (pthread_create(&l->worker, NULL, hash_partial, l))
                changed(l, "worker_failed");
            else {
                l->working = true;
                changed(l, NULL);
            }
        }
    } else if (!strcmp(action, "confirmImport")) {
        struct stat st;
        if (!l->import_ready || yyjson_get_uint(yyjson_obj_get(v, "token")) != l->import_token ||
            lstat(l->import_path, &st) || !same_file(&l->import_identity, &st))
            changed(l, "import_changed");
        else if (import_file(l, l->import_path, 0, ""))
            l->import_ready = false;
    } else if (!strcmp(action, "cancelPreview")) {
        l->import_ready = l->preview_ready = false;
        if (l->working && l->worker_kind == 3)
            atomic_store(&l->worker_cancel, true);
        if (!strcmp(l->query_mode, "urlMetadata"))
            cancel(l, &l->query_id);
        changed(l, NULL);
    } else if (!strcmp(action, "previewDownload")) {
        const char *url = str(v, "url"), *checksum = str(v, "checksum");
        if (strncmp(url, "https://", 8) || !sha(checksum) ||
            !copy(l->preview_url, sizeof l->preview_url, url) ||
            !copy(l->preview_checksum, sizeof l->preview_checksum, checksum) ||
            !copy(l->preview_name, sizeof l->preview_name, str(v, "name")))
            changed(l, "invalid_input");
        else {
            cancel(l, &l->query_id);
            l->preview_ready = false;
            l->preview_bytes = 0;
            copy(l->query_mode, sizeof l->query_mode, "urlMetadata");
            l->query_id = request(l, "HEAD", url, NULL, NULL, 0, OT_NETWORK_MODELS);
            l->preview_token = l->query_id;
            changed(l, NULL);
        }
    } else if (!strcmp(action, "confirmDownload")) {
        if (l->preview_ready && yyjson_get_uint(yyjson_obj_get(v, "token")) == l->preview_token) {
            enqueue(l, l->preview_url, l->preview_name, l->preview_checksum, l->preview_bytes, v);
            l->preview_ready = false;
        } else
            changed(l, "invalid_input");
    } else if (!strcmp(action, "import"))
        import_file(l, str(v, "path"), 0, "");
    else if (!strcmp(action, "select")) {
        ot_preferences p = *ot_get_preferences(l->app);
        if (*p.setup_target || p.setup_target_engines) {
            if (!setup_jobs(l,"",0,false)) {changed(l,"storage_write");yyjson_doc_free(d);return;}
            p.setup_target[0] = 0;p.setup_target_engines=0;p.setup_paused=false;
            p.setup = 1;
            if (!save_preferences(l, &p)) {
                changed(l, "storage_write");
                yyjson_doc_free(d);
                return;
            }
        }
        select_model(l, id);
    } else if (!strcmp(action, "startOllama"))
        select_model(l, -1);
    else if (!strcmp(action, "stop")) {
        if (l->pending)
            ot_maintenance(l->app, false, NULL);
        if (ot_maintenance(l->app, true, NULL)) {
            if (sqlite3_exec(l->db, "UPDATE settings SET model_id=NULL,model='' WHERE id=1", NULL, NULL, NULL) !=
                SQLITE_OK) {
                ot_maintenance(l->app, false, NULL);
                changed(l, "storage_write");
                yyjson_doc_free(d);
                return;
            }
            cancel(l, &l->health_id);
            ot_memory_cancel(l->memory);
            l->pending = 0;
            l->selected = 0;
            l->requested = 0;
            l->engine_error[0] = 0;
            ot_start_without_model(l->app);
            ot_engine_stop(&l->engine);
            copy(l->engine_state, sizeof l->engine_state, l->engine.pid > 0 ? "stopping" : "stopped");
            ot_maintenance(l->app, false, NULL);
            changed(l, NULL);
        } else
            changed(l, "busy");
    } else if (!strcmp(action, "engine")) {
        char path[4096];
        int kind = (int)yyjson_get_int(yyjson_obj_get(v, "kind"));
        if ((kind == 1 || kind == 2) && ot_engine_find(str(v, "path"), path)) {
            sqlite3_stmt *q = query(l, "INSERT INTO engines(kind,path) VALUES(?,?) ON CONFLICT(kind) DO "
                                       "UPDATE SET path=excluded.path");
            if (q) {
                sqlite3_bind_int(q, 1, kind);
                sqlite3_bind_text(q, 2, path, -1, SQLITE_TRANSIENT);
            }
            bool saved = done(q);
            changed(l, saved ? NULL : "storage_write");
            if (saved)
                detect_engines(l);
            if (saved && l->requested > 0 && !strcmp(l->engine_state, "failed"))
                select_model(l, l->requested);
        } else
            changed(l, "invalid_engine");
    } else if (!strcmp(action, "validateEngine")) {
        int kind = (int)yyjson_get_int(yyjson_obj_get(v, "kind"));
        if (l->working)
            changed(l, "busy");
        else if ((kind != 1 && kind != 2) || !engine_path(l, kind, l->worker_path))
            changed(l, "invalid_engine");
        else {
            l->worker_kind = 4;
            l->validation_kind = (unsigned)kind;
            atomic_store(&l->worker_done, false);
            atomic_store(&l->worker_cancel, false);
            if (pthread_create(&l->worker, NULL, hash_partial, l))
                changed(l, "worker_failed");
            else {
                l->working = true;
                changed(l, NULL);
            }
        }
    } else if (!strcmp(action, "detectEngines"))
        detect_engines(l);
    else if (!strcmp(action, "cancelEstimate")) {
        ot_memory_cancel(l->memory);
    } else if (!strcmp(action, "estimate")) {
        uint64_t context = yyjson_get_uint(yyjson_obj_get(v, "context"));
        int64_t layers = yyjson_get_sint(yyjson_obj_get(v, "gpuLayers"));
        ot_preferences p = *ot_get_preferences(l->app);
        yyjson_val *budget = yyjson_obj_get(v, "memoryPercent"), *threads = yyjson_obj_get(v, "threads");
        if ((budget &&
             (!yyjson_is_uint(budget) || yyjson_get_uint(budget) < 10 || yyjson_get_uint(budget) > 100)) ||
            (threads && (!yyjson_is_uint(threads) || yyjson_get_uint(threads) > 1024))) {
            changed(l, "invalid_input");
            yyjson_doc_free(d);
            return;
        }
        if (budget)
            p.memory_percent = (unsigned)yyjson_get_uint(budget);
        if (threads)
            p.threads = (unsigned)yyjson_get_uint(threads);
        sqlite3_stmt *q =
            context >= 1024 && context <= 262144 && layers >= -2 && layers <= 10000
                ? query(l, "SELECT bytes,embedding,layers,heads,kv_heads,kind,reference,metadata FROM models "
                           "WHERE id=?")
                : NULL;
        if (q)
            sqlite3_bind_int64(q, 1, id);
        if (q && sqlite3_step(q) == SQLITE_ROW) {
            ot_model_info info = {.bytes = (uint64_t)sqlite3_column_int64(q, 0),
                                  .embedding = (unsigned)sqlite3_column_int64(q, 1),
                                  .layers = (unsigned)sqlite3_column_int64(q, 2),
                                  .heads = (unsigned)sqlite3_column_int64(q, 3),
                                  .kv_heads = (unsigned)sqlite3_column_int64(q, 4)};
            const char *json_metadata = col(q, 7);
            yyjson_doc *meta = yyjson_read(json_metadata, strlen(json_metadata), 0);
            yyjson_val *fields = yyjson_doc_get_root(meta);
            info.tensor_bytes = yyjson_get_uint(yyjson_obj_get(fields, "tensorBytes"));
            if (info.tensor_bytes > info.bytes)
                info.tensor_bytes = 0;
            uint64_t key = yyjson_get_uint(yyjson_obj_get(fields, "keyLength")),
                     value = yyjson_get_uint(yyjson_obj_get(fields, "valueLength"));
            info.key_length = key <= UINT32_MAX ? (unsigned)key : 0;
            info.value_length = value <= UINT32_MAX ? (unsigned)value : 0;
            info.complex_cache = yyjson_get_bool(yyjson_obj_get(fields, "complexCache"));
            copy(info.architecture, sizeof info.architecture, str(fields, "family"));
            yyjson_doc_free(meta);
            p.context = (unsigned)context;
            p.gpu_layers = (int)layers;
            char engine[4096] = "";
            bool local_file = sqlite3_column_int(q, 5) == 1;
            if (local_file)
                engine_path(l, 1, engine);
            ot_memory_plan observed;
            const ot_memory_plan *calibrated = memory_recall(l, id, &p, &observed) ? &observed : NULL;
            if (l->memory)
                ot_memory_request(l->memory, id, local_file ? col(q, 6) : "", engine, &info, &p,
                                  ot_resources_get(l->app), calibrated);
            else {
                l->memory_view = (ot_memory_view){
                    .model = id,
                    .context = p.context,
                    .gpu_layers = p.gpu_layers,
                    .plan = calibrated ? *calibrated : ot_memory_approximate(&info, p.context),
                    .memory_percent = p.memory_percent,
                    .threads = p.threads};
                l->memory_revision++;
            }
        }
        sqlite3_finalize(q);
    } else if (!strcmp(action, "resetEngine")) {
        int kind = (int)yyjson_get_int(yyjson_obj_get(v, "kind"));
        if (kind != 1 && kind != 2) {
            changed(l, "invalid_engine");
        } else {
            sqlite3_stmt *q = query(l, "DELETE FROM engines WHERE kind=?");
            if (q)
                sqlite3_bind_int(q, 1, kind);
            if (done(q))
                detect_engines(l);
            else
                changed(l, "storage_write");
        }
    } else if (!strcmp(action, "clearSearch")) {
        cancel(l, &l->query_id);
        free(l->query);
        l->query = NULL;
        l->query_size = l->query_capacity = 0;
        free(l->catalog);
        l->catalog = strdup("[]");
        copy(l->query_mode, sizeof l->query_mode,
             !strcmp(str(v, "source"), "ollama") ? "ollamaSearch" : "search");
        changed(l, NULL);
    } else if (!strcmp(action, "discover"))
        discover(l);
    else if (!strcmp(action, "refresh")) {
        char url[512];
        snprintf(url, sizeof url, "%s/api/tags", *l->ollama ? l->ollama : "http://127.0.0.1:11434");
        begin_query(l, "tags", url, false);
    } else if (!strcmp(action, "search") || !strcmp(action, "ollamaSearch") ||
               !strcmp(action, "ollamaVariants")) {
        bool hf = !strcmp(action, "search"), variants = !strcmp(action, "ollamaVariants");
        const char *name = str(v, variants ? "repo" : "query"), *task = str(v, "task"),
                   *license = str(v, "license"), *order = str(v, "order");
        const char *allowed_hf[] = {"", "text-generation", "image-text-to-text", "feature-extraction"},
                   *allowed_ollama[] = {"", "thinking", "vision", "tools", "embedding", "cloud"},
                   *allowed_license[] = {"", "apache-2.0", "mit", "bsd-3-clause", "cc-by-4.0"},
                   *allowed_order[] = {"", "downloads", "recent", "trending"};
        bool task_ok = false, license_ok = false, order_ok = false;
        for (unsigned i = 0; i < (hf ? 4u : 6u); i++)
            task_ok |= !strcmp(task, hf ? allowed_hf[i] : allowed_ollama[i]);
        for (unsigned i = 0; i < 5; i++)
            license_ok |= !strcmp(license, allowed_license[i]);
        for (unsigned i = 0; i < 4; i++)
            order_ok |= !strcmp(order, allowed_order[i]);
        uint64_t max_parameters = yyjson_get_uint(yyjson_obj_get(v, "maxParameters"));
        if ((yyjson_obj_get(v, "maxParameters") && !yyjson_is_uint(yyjson_obj_get(v, "maxParameters"))) ||
            strlen(name) > 256 || !task_ok || !license_ok || !order_ok || max_parameters > 2048 ||
            (!hf && (*license || max_parameters || !strcmp(order, "trending"))) ||
            (variants &&
             (!ot_model_name(name) || strchr(name, ':') || !copy(l->repo, sizeof l->repo, name)))) {
            changed(l, "invalid_input");
        } else {
            char *part = encoded(name, variants), url[8193];
            uint64_t limit = yyjson_get_uint(yyjson_obj_get(v, "limit"));
            if (limit < 20 || limit > 100)
                limit = 20;
            if (!part)
                changed(l, "invalid_input");
            else if (hf) {
                l->query_limit = limit;
                l->search_filter.max_parameters = max_parameters;
                copy(l->search_filter.task, sizeof l->search_filter.task, task);
                copy(l->search_filter.license, sizeof l->search_filter.license, license);
                const char *sort = !strcmp(order, "downloads")  ? "downloads"
                                   : !strcmp(order, "recent")   ? "lastModified"
                                   : !strcmp(order, "trending") ? "trendingScore"
                                                                : "";
                char parameters[96] = "", licensing[160] = "";
                if (*license)
                    snprintf(licensing, sizeof licensing, "&filter=license%%3A%s", license);
                if (max_parameters)
                    snprintf(parameters, sizeof parameters, "&num_parameters=max%%3A%lluB",
                             (unsigned long long)max_parameters);
                snprintf(
                    url, sizeof url,
                    "https://huggingface.co/api/models?filter=gguf&limit=%llu&search=%s&"
                    "expand[]=gguf&expand[]=safetensors&expand[]=cardData&expand[]=tags&expand[]=downloads&"
                    "expand[]=likes&expand[]=pipeline_tag&expand[]=lastModified&expand[]=gated&"
                    "pipeline_tag=%s%s%s%s%s%s",
                    (unsigned long long)limit, part, task, licensing, parameters, *sort ? "&sort=" : "", sort,
                    *sort ? "&direction=-1" : "");
                begin_query(l, action, url, yyjson_get_bool(yyjson_obj_get(v, "refresh")));
            } else {
                const char *sort = !strcmp(order, "recent") ? "newest" : "popular";
                if (variants)
                    snprintf(url, sizeof url, "https://ollama.com/%s%s/tags",
                             strchr(name, '/') ? "" : "library/", part);
                else
                    snprintf(url, sizeof url, "https://ollama.com/search?q=%s&c=%s&o=%s", part, task, sort);
                begin_query(l, action, url, yyjson_get_bool(yyjson_obj_get(v, "refresh")));
            }
            free(part);
        }
    } else if (!strcmp(action, "variants")) {
        const char *repo = str(v, "repo");
        char *part = encoded(repo, true), url[8193];
        if (part && ot_model_name(repo) && copy(l->repo, sizeof l->repo, repo)) {
            snprintf(url, sizeof url, "https://huggingface.co/api/models/%s?blobs=true", part);
            begin_query(l, "variants", url, yyjson_get_bool(yyjson_obj_get(v, "refresh")));
        } else
            changed(l, "invalid_input");
        free(part);
    } else if (!strcmp(action, "download"))
        enqueue(l, str(v, "url"), str(v, "name"), str(v, "checksum"),
                yyjson_get_uint(yyjson_obj_get(v, "bytes")), v);
    else if (!strcmp(action, "pull")) {
        char url[512];
        snprintf(url, sizeof url, "ollama:%s", str(v, "name"));
        enqueue(l, url, str(v, "name"), "", yyjson_get_uint(yyjson_obj_get(v, "bytes")), v);
    } else if (!strcmp(action, "resume") || !strcmp(action, "restart")) {
        bool restart = !strcmp(action, "restart");
        if (ot_get_preferences(l->app)->setup_paused) {
            sqlite3_stmt *detached=query(l,"UPDATE downloads SET metadata=json_remove(metadata,'$.setup','$.setupBorrowed') WHERE id=?");
            if (detached) sqlite3_bind_int64(detached,1,id);
            if (!done(detached)) {changed(l,"storage_write");yyjson_doc_free(d);return;}
        }
        if (restart) {
            sqlite3_stmt *q = query(l, "SELECT state FROM downloads WHERE id=?");
            if (q)
                sqlite3_bind_int64(q, 1, id);
            bool cancelled = q && sqlite3_step(q) == SQLITE_ROW && !strcmp(col(q, 0), "cancelled");
            sqlite3_finalize(q);
            if (!cancelled || !remove_partial(l, id)) {
                changed(l, cancelled ? "file_operation" : "invalid_job");
                yyjson_doc_free(d);
                return;
            }
        }
        if (l->transfer_id || l->working || l->download_preparing || l->removing || l->cleanup_pending ||
            l->download_engine.stopping) {
            sqlite3_stmt *q = query(l, "UPDATE downloads SET state='queued',updated=unixepoch() "
                                       "WHERE id=? AND (state='paused' OR (? AND state='cancelled')) AND "
                                       "(SELECT count(*) FROM downloads WHERE state='queued')<64");
            if (q) {
                sqlite3_bind_int64(q, 1, id);
                sqlite3_bind_int(q, 2, restart);
            }
            bool ok = done(q) && sqlite3_changes(l->db) == 1;
            l->queue_pending = true;
            changed(l, ok ? NULL : "invalid_job");
        } else
            resume(l, id);
    } else if (!strcmp(action, "pause")) {
        if (!id || id == l->job)
            stop_transfer(l);
        else {
            sqlite3_stmt *q = query(l, "UPDATE downloads SET state='paused',updated=unixepoch() "
                                       "WHERE id=? AND state='queued'");
            if (q)
                sqlite3_bind_int64(q, 1, id);
            changed(l, done(q) ? NULL : "storage_write");
        }
    } else if (!strcmp(action, "cancelJob")) {
        sqlite3_stmt *q = query(l, "SELECT state FROM downloads WHERE id=?");
        if (!q) {
            yyjson_doc_free(d);
            return;
        }
        sqlite3_bind_int64(q, 1, id);
        bool cancellable = sqlite3_step(q) == SQLITE_ROW && strcmp(col(q, 0), "complete");
        sqlite3_finalize(q);
        if (!cancellable) {
            changed(l, "invalid_job");
            yyjson_doc_free(d);
            return;
        }
        if (!request_cleanup(l,id,false)) {
            changed(l,"storage_write");yyjson_doc_free(d);return;
        }
        q = query(l, "UPDATE downloads SET state='cancelled',received=0,etag='',error='',"
                     "metadata=json_remove(metadata,'$.pullProgress'),"
                     "updated=unixepoch() WHERE id=? AND state<>'complete'");
        if (q)
            sqlite3_bind_int64(q, 1, id);
        changed(l, done(q) ? NULL : "storage_write");
    } else if (!strcmp(action, "cleanupReplacement"))
        changed(l, cleanup_replacement(l, id) ? NULL : "replacement_cleanup");
    else if (!strcmp(action, "discard"))
        discard_job(l, id);
    else if (!strcmp(action, "remove"))
        remove_model(l, id, yyjson_get_bool(yyjson_obj_get(v, "uninstall")));
    else if (!strcmp(action, "restoreModel")) {
        sqlite3_stmt *q = query(l, "UPDATE models SET hidden=0 WHERE id=?");
        if (q)
            sqlite3_bind_int64(q, 1, id);
        changed(l, done(q) ? NULL : "storage_write");
    } else
        changed(l, "invalid_input");
    clean_downloads(l);
    yyjson_doc_free(d);
}
bool ot_library_polling(const ot_library *l) {
    return l->working || l->pending || l->removing || l->query_id || l->health_id || l->prepare_id ||
           l->transfer_id || l->runtime_id || l->advice_id || l->download_preparing || l->engine.stopping ||
           l->download_engine.stopping || l->memory_view.pending || l->refresh_model ||
           l->cleanup_pending || l->queue_pending;
}
static const char *advice_local_reference(ot_library *l) {
    const ot_connection *c = ot_configuration(l->app);
    const char *server = *l->ollama ? l->ollama : "http://127.0.0.1:11434";
    return c->ollama && !c->remote && !strcmp(c->endpoint, server) ? c->model : "";
}
void ot_library_tick(ot_library *l) {
    const ot_preferences *preferences = ot_get_preferences(l->app);
    for (unsigned i = 0; i < ADVICE_PREFETCH; i++) {
        advice_page *p = &l->advice_pages[i];
        if (!p->id || p->complete || now() - p->started < 8) continue;
        if (l->io.cancel) l->io.cancel(l->io.context, p->id);
        p->complete = true;
        copy(p->error, sizeof p->error, "connection_failed");
    }
    double collection_time = now() - l->advice_cycle_started;
    if (l->advice_id && collection_time >= 45)
        advice_complete(l);
    else if (l->advice_id && !l->advice_verifying && !l->advice_evidence &&
             collection_time >= (!l->advice_hf && preferences->setup_engines == 3 ? 18 : 38)) {
        /* Reserve time for every selected provider, then optional evidence.
         * Slow Ollama tags cannot consume the GGUF discovery budget. */
        advice_cancel(l);
        if (!l->advice_hf && (preferences->setup_engines & 1)) advice_hf_begin(l);
        else { l->advice_family = l->advice_family_count; advice_next(l); }
    } else if (l->advice_id && now() - l->advice_started >= 8)
        advice_deliver(l, "connection_failed");
    int64_t date = (int64_t)time(NULL);
    if (!l->advice_id && !l->advice_visible && preferences->advice_notices && ot_network_allowed(preferences, OT_NETWORK_CATALOGUES) && preferences->setup == 2 &&
        (l->selected > 0 || *advice_local_reference(l)) && now() - l->started > 10 && ot_is_idle(l->app) &&
        !l->transfer_id && !l->query_id && !l->working &&
        (l->advice_checked <= 0 || date - l->advice_checked >= (l->advice_snapshot ? 7 * 86400 : 3600)))
        advice_begin(l, false);
    if (ot_memory_poll(l->memory, &l->memory_view))
        l->memory_revision++;
    if (now() >= l->next_memory) {
        l->next_memory = now() + 2;
        uint64_t bytes = 0;
        bool known = ot_engine_memory(&l->engine, &bytes);
        if (known != l->process_memory_known || bytes != l->process_memory ||
            l->process_memory_child != l->engine.child) {
            l->process_memory_known = known;
            l->process_memory = bytes;
            l->process_memory_child = l->engine.child;
            l->memory_revision++;
        }
    }
    if (l->working && atomic_load_explicit(&l->worker_done, memory_order_acquire)) {
        pthread_join(l->worker, NULL);
        l->working = false;
        if (l->worker_kind==8) {
            if (l->worker_ok && cleanup_job(l,l->cleanup_job)) {if (l->job==l->cleanup_job) l->job=0;l->revision++;}
            else {changed(l,"file_operation");l->cleanup_pending=false;}
        } else if (l->worker_kind==7 && l->worker_ok) {
            l->pull_published=true;finish_pull(l);
        } else if (!atomic_load(&l->worker_cancel)) {
            if (l->worker_kind==6) {
                char executable[4096];
                if (l->worker_ok && engine_path(l,2,executable) &&
                    ot_engine_start(&l->download_engine,executable,l->profile,"",true,false,l->worker_path,
                                    ot_get_preferences(l->app),ot_resources_get(l->app))) {
                    l->download_preparing=true;l->download_external=false;l->download_started=now();
                } else save_job(l,"paused","engine_start");
            } else if (l->worker_kind==7) save_job(l,"paused","checksum_mismatch");
            else if (l->worker_kind == 2) {
                bool ok = sqlite3_exec(l->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
                for (size_t i = 0; i < l->inventory.count; i++) {
                    ot_inventory_item *m = &l->inventory.items[i];
                    /* The recovery hard link belongs to the uncommitted download,
                     * not to discovery. Registering it here would create a second
                     * model and prevent the intended atomic reference switch. */
                    if (ok && m->kind == 1 && !strncmp(m->reference, l->directory, strlen(l->directory))) {
                        sqlite3_stmt *q = query(
                            l, "SELECT EXISTS(SELECT 1 FROM downloads WHERE "
                               "state<>'complete' AND url NOT LIKE 'ollama:%' AND ?1=?2||'/'||id||'.gguf')");
                        if (q) {
                            sqlite3_bind_text(q, 1, m->reference, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(q, 2, l->directory, -1, SQLITE_TRANSIENT);
                        }
                        ok = q && sqlite3_step(q) == SQLITE_ROW;
                        bool staged = ok && sqlite3_column_int(q, 0);
                        sqlite3_finalize(q);
                        if (staged)
                            continue;
                    }
                    ok = ok && save_model(l, m->name, m->kind, m->reference, &m->info, 0, false);
                    if (ok && *m->manifest) {
                        sqlite3_stmt *q =
                            query(l, "UPDATE models SET metadata=json_set(metadata,"
                                     "'$.localManifest',?1,'$.localManifestRootLength',?2,'$.digest',?4) "
                                     "WHERE kind=2 AND reference=?3");
                        if (q) {
                            sqlite3_bind_text(q, 1, m->manifest, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(q, 2, (sqlite3_int64)m->manifest_root_length);
                            sqlite3_bind_text(q, 3, m->reference, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(q, 4, m->manifest_digest, -1, SQLITE_TRANSIENT);
                        }
                        ok = done(q);
                    }
                }
                for (size_t i = 0; ok && i < l->registered_count; i++) {
                    registered_file *file = &l->registered[i];
                    if (!file->checked)
                        continue;
                    sqlite3_stmt *q =
                        query(l, "UPDATE models SET "
                                 "available=?1,device=CASE WHEN ?4 THEN ?5 ELSE device END,"
                                 "inode=CASE WHEN ?4 THEN ?6 ELSE inode END,"
                                 "metadata=CASE WHEN ?7 THEN json_set(metadata,'$.fileChanged',json(?2),"
                                 "'$.fileModified',?8,'$.fileModifiedNsec',?9) ELSE "
                                 "json_set(metadata,'$.fileChanged',json(?2)) END WHERE id=?3");
                    if (q) {
                        sqlite3_bind_int(q, 1, file->available);
                        sqlite3_bind_text(q, 2, file->replaced ? "true" : "false", -1, SQLITE_STATIC);
                        sqlite3_bind_int64(q, 3, file->id);
                        sqlite3_bind_int(q, 4, file->verified);
                        sqlite3_bind_int64(q, 5, file->observed.st_dev);
                        sqlite3_bind_int64(q, 6, (sqlite3_int64)file->observed.st_ino);
                        sqlite3_bind_int(q, 7, !file->manifest && file->available && !file->replaced);
                        sqlite3_bind_int64(q, 8, file->observed.st_mtimespec.tv_sec);
                        sqlite3_bind_int64(q, 9, file->observed.st_mtimespec.tv_nsec);
                    }
                    ok = done(q);
                }
                if (ok)
                    ok = sqlite3_exec(l->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
                if (!ok)
                    sqlite3_exec(l->db, "ROLLBACK", NULL, NULL, NULL);
                changed(l, !ok ? "storage_write" : l->inventory.limited ? "inventory_limited" : NULL);
            } else if (l->worker_kind == 3) {
                l->import_ready = l->worker_ok;
                changed(l, l->worker_ok ? NULL : "invalid_gguf");
            } else if (l->worker_kind == 5) {
                if (l->worker_ok) {
                    sqlite3_stmt *q = query(l, "INSERT INTO engines(kind,path) VALUES(?,?) "
                                               "ON CONFLICT(kind) DO UPDATE SET path=excluded.path");
                    if (q) {
                        sqlite3_bind_int(q, 1, l->package->kind);
                        sqlite3_bind_text(q, 2, l->worker_path, -1, SQLITE_TRANSIENT);
                    }
                    if (done(q) && save_job(l, "complete", NULL)) {
                        char part[4096];
                        if (path_for(l, l->job, ".part", part))
                            unlink(part);
                        l->job = 0;
                        l->queue_pending = true;
                        detect_engines(l);
                    } else
                        save_job(l, "paused", "storage_write");
                } else
                    save_job(l, "paused", "package_install");
            } else if (l->worker_kind == 4) {
                l->validation[l->validation_kind - 1] = l->worker_ok ? 1 : 2;
                changed(l, l->worker_ok ? NULL : "invalid_engine");
            } else if (l->worker_ok) {
                l->received = l->worker_bytes;
                l->rate_bytes = l->received;
                l->rate_time = now();
                l->digest = l->worker_digest;
                l->partial_hashed = true;
                transfer(l);
            } else
                save_job(l, "paused", "partial_changed");
        }
        if (l->worker_kind == 2) {
            free(l->inventory.items);
            l->inventory = (ot_inventory){0};
        }
        clear_registered(l);
    }
    bool downloader_running = l->download_engine.pid > 0;
    ot_engine_poll(&l->download_engine);
    if (downloader_running && l->download_engine.pid <= 0) {
        char uri[256];
        snprintf(uri, sizeof uri, "http://127.0.0.1:%u", l->download_engine.port);
        if (!strcmp(l->download_uri, uri))
            l->download_uri[0] = 0;
        if (l->download_preparing)
            download_preparation_failed(l, "engine_exited");
        else if (l->remove_id)
            ot_library_end(l, l->remove_id, "engine_exited");
        else if (l->transfer_id && !strncmp(l->url, "ollama:", 7))
            fail_transfer(l, "engine_exited");
    }
    if (l->download_preparing) {
        if (now() - l->download_started > 120)
            download_preparation_failed(l, "engine_timeout");
        else if (!l->download_external && !l->prepare_id && now() >= l->download_next &&
                 ot_engine_listening(&l->download_engine))
            download_probe(l);
    }
    if (l->download_engine.pid > 0 && !l->download_engine.stopping) {
        if (l->download_preparing || l->remove_id || (l->transfer_id && !strncmp(l->url, "ollama:", 7)))
            l->download_idle = 0;
        else if (l->download_idle <= 0)
            l->download_idle = now();
        else if (now() - l->download_idle > 30)
            ot_engine_stop(&l->download_engine);
    }
    clean_downloads(l);
    if (l->queue_pending && !l->cleanup_pending && !l->transfer_id && !l->working && !l->download_preparing && !l->removing &&
        !l->download_engine.stopping) {
        const ot_package *package = ot_package_for_kind(2);
        sqlite3_stmt *q =
            query(l, "SELECT EXISTS(SELECT 1 FROM downloads WHERE url=? AND state<>'complete')");
        if (q)
            sqlite3_bind_text(q, 1, package->url, -1, SQLITE_TRANSIENT);
        bool dependency = q && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
        char path[4096];
        bool waiting_for_engine = dependency && !engine_path(l, 2, path);
        q = query(
            l,
            "SELECT id,url,checksum,total FROM downloads WHERE state='queued' AND "
            "(?=0 OR coalesce(json_extract(metadata,'$.setup'),0)=0) AND coalesce(json_extract(metadata,'$.pullCleanup'),0)=0 ORDER BY updated,id LIMIT 64");
        if (q) sqlite3_bind_int(q,1,preferences->setup_paused);
        int64_t next = 0;
        while (q && sqlite3_step(q) == SQLITE_ROW)
            if ((!waiting_for_engine || strncmp(col(q, 1), "ollama:", 7)) &&
                (ot_network_allowed(preferences, ot_package_for_download(col(q, 1), col(q, 2)) ?
                                   OT_NETWORK_ENGINES : OT_NETWORK_MODELS) ||
                 local_download(l,sqlite3_column_int64(q,0),col(q,1),(uint64_t)sqlite3_column_int64(q,3)))) {
                next = sqlite3_column_int64(q, 0);
                break;
            }
        sqlite3_finalize(q);
        if (next)
            resume(l, next);
        else
            l->queue_pending = false;
    }
    if (l->refresh_model && l->requested != l->refresh_model)
        l->refresh_model = 0;
    if (l->refresh_model && !l->pending && !l->working && ot_is_idle(l->app)) {
        int64_t id = l->refresh_model;
        l->refresh_model = 0;
        l->selected = 0; // The engine still owns the previous inode until the live turn finishes.
        select_model(l, id);
    }
    bool running = l->engine.pid > 0;
    ot_engine_poll(&l->engine);
    if (running && l->engine.pid <= 0) {
        char uri[256];
        snprintf(uri, sizeof uri, "http://127.0.0.1:%u", l->engine.port);
        if (l->engine_ollama && !strcmp(l->ollama, uri))
            l->ollama[0] = 0;
        if (l->pending && !strcmp(l->engine_state, "stopping")) {
            prepare_pending(l);
        } else if (strcmp(l->engine_state, "failed")) {
            if (l->pending) {
                engine_failure(l, "engine_exited");
                return;
            }
            l->selected = 0;
            if (l->requested)
                ot_connection_ready(l->app, false);
            bool expected = !strcmp(l->engine_state, "stopping");
            copy(l->engine_state, sizeof l->engine_state, expected ? "stopped" : "failed");
            copy(l->engine_error, sizeof l->engine_error, expected ? "" : "engine_exited");
            changed(l, expected ? NULL : "engine_exited");
        }
    }
    if (l->cpu_probing && now() - l->cpu_started > 8) {
        ot_preferences p;
        if (model_preferences(l, 0, &p))
            cpu_record(l, l->pending, &p, 0);
        cancel(l, &l->health_id);
        l->cpu_probing = false;
        engine_failure(l, "cpu_not_suitable");
        return;
    }
    if (l->pending && strcmp(l->engine_state, "stopping")) {
        if (now() - l->started > 120) {
            engine_failure(l, "engine_timeout");
            return;
        }
        if (!l->health_external && !l->health_id && now() >= l->next_health &&
            ot_engine_listening(&l->engine))
            health_request(l);
    }
    runtime_poll(l);
    setup_tick(l);
}
bool ot_library_headers(ot_library *l, uint64_t id, int status, const char *etag, const char *range,
                        int64_t length) {
    if (!id)
        return false;
    if (id != l->query_id && id != l->health_id && id != l->prepare_id && id != l->transfer_id &&
        id != l->runtime_id && id != l->remove_id && id != l->advice_id && !advice_page_for_id(l, id))
        return false;
    if (id == l->remove_id && status == 404) {
        ot_library_end(l, id, NULL); /* Already absent is a successful uninstall. */
        return false;
    }
    if (id == l->transfer_id && status == 416 && l->received) {
        fail_transfer(l, "resume_changed");
        l->etag[0] = 0;
        save_job(l, "paused", "resume_changed");
        return false;
    }
    if (status < 200 || status >= 300) {
        ot_library_end(l, id, status == 404 ? "not_found" : "http_error");
        return false;
    }
    if ((id == l->runtime_id || id == l->advice_id || advice_page_for_id(l, id)) && length > 1048576) {
        ot_library_end(l, id, "response_limit");
        return false;
    }
    if (id != l->transfer_id || !strncmp(l->url, "ollama:", 7)) {
        if (id == l->query_id && !strcmp(l->query_mode, "urlMetadata"))
            l->preview_bytes = length > 0 ? (uint64_t)length : 0;
        return true;
    }
    unsigned long long start = 0, end = 0, total = 0;
    char trailing = 0;
    if (l->received) {
        bool valid = status == 206 && range &&
                     sscanf(range, "bytes %llu-%llu/%llu%c", &start, &end, &total, &trailing) == 3 &&
                     start == l->received && end >= start && end < total && etag && *etag &&
                     !strcmp(etag, l->etag);
        if (!valid) {
            fail_transfer(l, "resume_changed");
            l->etag[0] = 0;
            save_job(l, "paused", "resume_changed");
            return false;
        }
    } else if (status != 200) {
        fail_transfer(l, "http_range");
        return false;
    }
    if (length >= 0) {
        uint64_t expected = l->received + (uint64_t)length;
        if ((l->total && l->total != expected) || (total && total != expected) || expected > INT64_MAX) {
            fail_transfer(l, "download_size");
            return false;
        }
        l->total = expected;
    }
    const ot_resources *r = ot_resources_get(l->app);
    if (l->total && r->disk_available &&
        l->total - l->received > r->disk_available - r->disk_available / 20) {
        fail_transfer(l, "disk_budget");
        return false;
    }
    if (!l->received) {
        l->etag[0] = 0;
        if (etag && etag[0] == '"' && strlen(etag) < sizeof l->etag)
            copy(l->etag, sizeof l->etag, etag);
    }
    save_job(l, "downloading", NULL);
    return true;
}
static void pull_frame(ot_library *l) {
    if (!l->pull_size)
        return;
    if (l->pull_done) {
        l->pull_size = 0;
        return;
    }
    yyjson_doc *d = yyjson_read(l->pull_line, l->pull_size, 0);
    yyjson_val *v = yyjson_doc_get_root(d);
    if (!d || !yyjson_is_obj(v) || *str(v, "error")) {
        yyjson_doc_free(d);
        fail_transfer(l, "pull_failed");
        return;
    }
    const char *digest = str(v, "digest"), *status = str(v, "status");
    if (!strncmp(digest, "sha256:", 7))
        digest += 7;
    bool aggregate = !strcmp(digest, "model");
    if ((aggregate || (sha(digest) && (!l->pull_aggregate || l->pull_resuming))) &&
        yyjson_is_uint(yyjson_obj_get(v, "total"))) {
        if (l->pull_resuming) {
            bool same = false;
            for (unsigned i = 0; i < l->pull_layer_count; i++)
                if (!strcmp(l->pull_layers[i].digest, digest) &&
                    l->pull_layers[i].total == yyjson_get_uint(yyjson_obj_get(v, "total")))
                    same = true;
            if (!same) {
                l->pull_layer_count = 0;
                l->pull_aggregate = false;
            }
            l->pull_resuming = false;
        }
        /* Ollama's tensor transfer reports one model-wide counter. Never add it
         * to individual layer counters from an earlier preparation stage. */
        if (aggregate && !l->pull_aggregate) {
            l->pull_layer_count = 0;
            l->pull_aggregate = true;
        }
        unsigned i = 0;
        for (; i < l->pull_layer_count; i++)
            if (!strcmp(l->pull_layers[i].digest, digest))
                break;
        if (i == l->pull_layer_count) {
            if (i == 128) {
                yyjson_doc_free(d);
                fail_transfer(l, "response_limit");
                return;
            }
            l->pull_layers[i].received = l->pull_layers[i].total = 0;
            l->pull_layers[i].resumed = false;
            copy(l->pull_layers[i].digest, sizeof l->pull_layers[i].digest, digest);
            l->pull_layer_count++;
        }
        uint64_t total = yyjson_get_uint(yyjson_obj_get(v, "total"));
        uint64_t received = yyjson_get_uint(yyjson_obj_get(v, "completed"));
        if (received > total || total > INT64_MAX) {
            yyjson_doc_free(d);
            fail_transfer(l, "download_size");
            return;
        }
        /* Zero can be the provider checking its partial, not a loss of bytes.
         * Retain the last checkpoint until it reports actual progress. Its first
         * positive counter is authoritative even after a rollback; aggregate
         * "model" is a progress identity, never a content-integrity claim. */
        if (l->pull_layers[i].total != total || received > l->pull_layers[i].received ||
            (l->pull_layers[i].resumed && received))
            l->pull_layers[i].received = received;
        if (received)
            l->pull_layers[i].resumed = false;
        l->pull_layers[i].total = total;
        l->received = l->total = 0;
        for (i = 0; i < l->pull_layer_count; i++) {
            if (l->pull_layers[i].total > INT64_MAX - l->total) {
                yyjson_doc_free(d);
                fail_transfer(l, "download_size");
                return;
            }
            l->received += l->pull_layers[i].received;
            l->total += l->pull_layers[i].total;
        }
        if (!l->pull_aggregate && l->total < l->pull_expected) l->total = l->pull_expected;
        transfer_progress(l);
    }
    l->pull_done = !strcmp(status, "success");
    if (l->pull_done || strstr(status, "verifying") || strstr(status, "writing manifest")) {
        uint64_t actual = 0;
        for (unsigned i = 0; i < l->pull_layer_count; i++) actual += l->pull_layers[i].total;
        if (actual) l->total = actual;
    }
    /* Progress notifications can stop short of the final byte. The provider's verified
     * success, not a timer or extrapolation, proves all announced layers are available. */
    if (l->pull_done)
        l->received = l->total;
    if (strstr(status, "verifying"))
        save_job(l, "verifying", NULL);
    else if (strstr(status, "writing manifest") || l->pull_done)
        save_job(l, "registering", NULL);
    l->pull_size = 0;
    yyjson_doc_free(d);
}
void ot_library_bytes(ot_library *l, uint64_t id, const void *data, size_t size) {
    if (!id || !size)
        return;
    advice_page *page = advice_page_for_id(l, id);
    if (page) {
        if (!append(&page->body, &page->size, &page->capacity, data, size, 1024 * 1024))
            ot_library_end(l, id, "response_limit");
        return;
    }
    if (id == l->advice_id) {
        if (!append(&l->advice_body, &l->advice_size, &l->advice_capacity, data, size, 1024 * 1024))
            ot_library_end(l, id, "response_limit");
        return;
    }
    if (id == l->runtime_id) {
        if (!append(&l->runtime_body, &l->runtime_size, &l->runtime_capacity, data, size, 1024 * 1024))
            ot_library_end(l, id, "response_limit");
        return;
    }
    if (id == l->query_id) {
        if (!append(&l->query, &l->query_size, &l->query_capacity, data, size, 4 * 1024 * 1024)) {
            cancel(l, &l->query_id);
            changed(l, "response_limit");
        }
        return;
    }
    if (id == l->prepare_id) {
        if (!append(&l->prepare, &l->prepare_size, &l->prepare_capacity, data, size, 1024 * 1024))
            ot_library_end(l, id, "response_limit");
        return;
    }
    if (id == l->health_id) {
        if (!append(&l->health, &l->health_size, &l->health_capacity, data, size, 1024 * 1024))
            ot_library_end(l, id, "response_limit");
        return;
    }
    if (id != l->transfer_id)
        return;
    if (!strncmp(l->url, "ollama:", 7)) {
        const unsigned char *bytes = data;
        for (size_t i = 0; i < size && l->transfer_id; i++) {
            if (bytes[i] == '\n') {
                pull_frame(l);
                continue;
            }
            if (l->pull_size == sizeof l->pull_line - 1) {
                fail_transfer(l, "response_limit");
                return;
            }
            l->pull_line[l->pull_size++] = (char)bytes[i];
        }
        return;
    }
    if (size > INT64_MAX - l->received || (l->total && size > l->total - l->received)) {
        fail_transfer(l, "download_size");
        return;
    }
    for (size_t offset = 0; offset < size;) {
        size_t n = sizeof l->transfer_buffer - l->buffered;
        if (n > size - offset)
            n = size - offset;
        memcpy(l->transfer_buffer + l->buffered, (const unsigned char *)data + offset, n);
        l->buffered += n;
        offset += n;
        if (l->buffered == sizeof l->transfer_buffer && !flush_transfer(l)) {
            fail_transfer(l, "file_operation");
            return;
        }
    }
    l->received += size;
    transfer_progress(l);
}
static void finish_pull(ot_library *l) {
    if (l->pull_job==l->job && !l->pull_published) {
        l->worker_kind=7;atomic_store(&l->worker_done,false);atomic_store(&l->worker_cancel,false);
        if (!pthread_create(&l->worker,NULL,hash_partial,l)) {l->working=true;save_job(l,"registering",NULL);}
        else save_job(l,"paused","worker_failed");
        return;
    }
    ot_model_info m = {.bytes = l->total};
    bool ok = sqlite3_exec(l->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
              save_model(l, l->url + 7, 2, l->url + 7, &m, 0, true);
    sqlite3_stmt *q =
        ok ? query(l, "UPDATE models SET metadata=json_patch(metadata,json_remove((SELECT metadata FROM "
                      "downloads WHERE id=?),'$.pullProgress','$.pullStore','$.pullCleanup','$.discard','$.setup','$.setupBorrowed')) WHERE kind=2 AND reference=?")
           : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, l->job);
        sqlite3_bind_text(q, 2, l->url + 7, -1, SQLITE_TRANSIENT);
    }
    ok = done(q) && save_job(l, "complete", NULL) &&
         sqlite3_exec(l->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!ok) {
        sqlite3_exec(l->db, "ROLLBACK", NULL, NULL, NULL);
        fail_transfer(l, "storage_write");
        return;
    }
    if (l->pull_job==l->job) (void)request_cleanup(l,l->job,false);
    l->job = 0;
}
static void finish_download(ot_library *l) {
    if (l->total && l->total != l->received) {
        fail_transfer(l, "download_size");
        return;
    }
    if (!flush_transfer(l)) {
        fail_transfer(l, "file_operation");
        return;
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char hash[65];
    CC_SHA256_Final(digest, &l->digest);
    for (unsigned i = 0; i < 32; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    if (strcasecmp(hash, l->checksum)) {
        fail_transfer(l, "checksum_mismatch");
        l->etag[0] = 0;
        save_job(l, "paused", "checksum_mismatch");
        return;
    }
    struct stat completed, current;
    bool flushed = l->file >= 0 && fsync(l->file) == 0 && !fstat(l->file, &completed) &&
                   S_ISREG(completed.st_mode) && completed.st_size >= 0 &&
                   (uint64_t)completed.st_size == l->received;
    if (l->file >= 0) {
        if (close(l->file))
            flushed = false;
        l->file = -1;
    }
    char part[4096], final[4096];
    ot_model_info m;
    if (!flushed || !path_for(l, l->job, ".part", part) || !path_for(l, l->job, ".gguf", final)) {
        fail_transfer(l, "file_operation");
        return;
    }
    if (l->package) {
        copy(l->worker_path, sizeof l->worker_path, part);
        l->worker_kind = 5;
        atomic_store(&l->worker_done, false);
        atomic_store(&l->worker_cancel, false);
        if (pthread_create(&l->worker, NULL, hash_partial, l))
            save_job(l, "paused", "worker_failed");
        else {
            l->working = true;
            save_job(l, "registering", NULL);
        }
        return;
    }
    if (lstat(part, &current) || !same_file(&completed, &current)) {
        fail_transfer(l, "partial_changed");
        return;
    }
    if (!ot_gguf(part, &m)) {
        fail_transfer(l, "invalid_gguf");
        return;
    }
    if (lstat(part, &current) || !same_file(&completed, &current)) {
        fail_transfer(l, "partial_changed");
        return;
    }
    /* The file and SQLite cannot share a transaction. Keep the verified partial
     * until the database commits; repeating completion accepts only its own inode. */
    if (link(part, final)) {
        struct stat source, target;
        if (errno != EEXIST || lstat(part, &source) || lstat(final, &target) ||
            source.st_dev != target.st_dev || source.st_ino != target.st_ino) {
            fail_transfer(l, "file_operation");
            return;
        }
    }
    int dir = open(l->directory, O_RDONLY);
    bool durable = dir >= 0 && fsync(dir) == 0;
    if (dir >= 0)
        close(dir);
    if (!durable) {
        fail_transfer(l, "file_operation");
        return;
    }
    l->total = l->received;
    sqlite3_stmt *job = query(l, "SELECT metadata FROM downloads WHERE id=?");
    if (job)
        sqlite3_bind_int64(job, 1, l->job);
    const char *json = job && sqlite3_step(job) == SQLITE_ROW ? col(job, 0) : "{}";
    yyjson_doc *metadata = yyjson_read(json, strlen(json), 0);
    sqlite3_finalize(job);
    yyjson_val *fields = yyjson_doc_get_root(metadata);
    int64_t replace = yyjson_get_sint(yyjson_obj_get(fields, "replaceId"));
    bool began = metadata && sqlite3_exec(l->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    bool replaced = began;
    bool mismatch = false;
    if (began && replace) {
        sqlite3_stmt *q = query(l, "UPDATE models SET reference=?1 WHERE id=?2 AND kind=1 AND "
                                   "reference=?3 AND device=?4 AND inode=?5");
        if (q) {
            sqlite3_bind_text(q, 1, final, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 2, replace);
            sqlite3_bind_text(q, 3, str(fields, "replacePath"), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 4, yyjson_get_sint(yyjson_obj_get(fields, "replaceDevice")));
            sqlite3_bind_int64(q, 5, yyjson_get_sint(yyjson_obj_get(fields, "replaceInode")));
        }
        bool updated = done(q);
        replaced = updated && sqlite3_changes(l->db) == 1;
        mismatch = updated && !replaced;
    }
    bool committed =
        began && replaced && save_model(l, *l->job_name ? l->job_name : m.name, 1, final, &m, l->job, true) &&
        save_job(l, "complete", NULL) && sqlite3_exec(l->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    yyjson_doc_free(metadata);
    if (!committed) {
        sqlite3_exec(l->db, "ROLLBACK", NULL, NULL, NULL);
        fail_transfer(l, mismatch ? "replacement_changed" : "storage_write");
        return;
    }
    unlink(part);
    if (replace) {
        cleanup_replacement(l, l->job);
        if (l->requested == replace)
            l->refresh_model = replace;
    }
    l->job = 0;
}
void ot_library_end(ot_library *l, uint64_t id, const char *error) {
    if (!id)
        return;
    advice_page *page = advice_page_for_id(l, id);
    if (page) {
        if (error && l->io.cancel) l->io.cancel(l->io.context, id);
        if (error) {free(page->body);page->body=NULL;page->size=page->capacity=0;}
        page->complete = true;
        copy(page->error, sizeof page->error, error ? error : "");
        return;
    }
    if (id == l->advice_id) {
        advice_deliver(l, error);
        return;
    }
    if (id && id == l->remove_id) {
        cancel(l, &l->remove_id);
        if (!error) {
            if (!ot_remove_model(l->app, l->removing, true))
                error = "storage_write";
        }
        l->removing = 0;
        if (error)
            l->download_uri[0] = 0;
        changed(l, error);
        return;
    }
    if (id && id == l->runtime_id) {
        if (error)
            cancel(l, &l->runtime_id);
        else
            l->runtime_id = 0;
        if (runtime_current(l)) {
            ot_runtime_info observed = {0};
            bool ok = false;
            if (!error && l->running_ollama)
                ok = ot_runtime_ollama(&observed, l->runtime_body, l->runtime_size,
                                       ot_configuration(l->app)->model);
            else if (!error) {
                yyjson_doc *d = yyjson_read(l->runtime_body, l->runtime_size, 0);
                yyjson_val *sleep = yyjson_obj_get(yyjson_doc_get_root(d), "is_sleeping");
                observed = l->engine.runtime;
                ok = yyjson_is_obj(yyjson_doc_get_root(d));
                if (yyjson_is_bool(sleep) && yyjson_get_bool(sleep))
                    observed.kind = OT_RUNTIME_ASLEEP;
                yyjson_doc_free(d);
            }
            /* A failed observation cannot disprove an already verified execution
             * mode. Process exit and real request failures have their own paths. */
            if (ok) {
                l->runtime = observed;
                runtime_buffers(l, &l->runtime);
                const ot_preferences *preferences = ot_get_preferences(l->app);
                if (l->engine.pid > 0 && l->runtime.allocation > ot_resources_get(l->app)->memory_total / 100 *
                                                                     preferences->memory_percent) {
                    engine_failure(l, "memory_budget");
                } else if (l->runtime.kind != OT_RUNTIME_UNKNOWN && l->runtime.kind != OT_RUNTIME_ASLEEP) {
                    if (preferences->gpu_layers == 0 && l->runtime.kind != OT_RUNTIME_CPU)
                        engine_failure(l, "cpu_unverified");
                    else if (preferences->require_gpu) {
                        if (l->runtime.kind == OT_RUNTIME_GPU || l->runtime.kind == OT_RUNTIME_HYBRID)
                            ot_connection_ready(l->app, true);
                        else
                            engine_failure(l, "gpu_required");
                    }
                }
                memory_record(l);
                l->memory_revision++;
            }
        }
        free(l->runtime_body);
        l->runtime_body = NULL;
        l->runtime_size = l->runtime_capacity = 0;
        return;
    }
    if (id == l->query_id && !strcmp(l->query_mode, "urlMetadata")) {
        l->query_id = 0;
        l->preview_ready = !error;
        changed(l, error);
        return;
    }
    if (id == l->query_id) {
        l->query_id = 0;
        if (!strncmp(l->query_mode, "ollama", 6)) {
            if (error || !l->query_size || memchr(l->query, 0, l->query_size))
                changed(l, error ? error : "invalid_response");
            else
                ollama_catalog(l);
            return;
        }
        yyjson_doc *d = error ? NULL : yyjson_read(l->query, l->query_size, 0);
        yyjson_val *v = yyjson_doc_get_root(d);
        if (!d)
            changed(l, error ? error : "invalid_response");
        else if (!strcmp(l->query_mode, "tags")) {
            if (yyjson_is_arr(yyjson_obj_get(v, "models"))) {
                if (!*l->ollama)
                    copy(l->ollama, sizeof l->ollama, "http://127.0.0.1:11434");
                tags(l, v);
                changed(l, NULL);
            } else
                changed(l, "invalid_response");
        } else if ((!strcmp(l->query_mode, "search") && yyjson_is_arr(v)) ||
                   (!strcmp(l->query_mode, "variants") && yyjson_is_arr(yyjson_obj_get(v, "siblings"))))
            catalog(l, v);
        else
            changed(l, "invalid_response");
        yyjson_doc_free(d);
        return;
    }
    if (id == l->prepare_id) {
        l->prepare_id = 0;
        yyjson_doc *d = error ? NULL : yyjson_read(l->prepare, l->prepare_size, 0);
        yyjson_val *v = yyjson_doc_get_root(d);
        if (d && yyjson_is_arr(yyjson_obj_get(v, "models"))) {
            snprintf(l->download_uri, sizeof l->download_uri, "http://127.0.0.1:%u",
                     l->download_external ? 11434 : l->download_engine.port);
            l->download_preparing = false;
            if (!l->pull_job) tags(l, v);
            transfer(l);
        } else if (l->download_external) {
            l->download_external = false;
            char path[4096];
            if (!engine_path(l, 2, path))
                download_preparation_failed(l, "engine_missing");
            else if (!ot_engine_start(&l->download_engine, path, l->profile, "", true, false, NULL,
                                      ot_get_preferences(l->app), ot_resources_get(l->app)))
                download_preparation_failed(l, "engine_start");
        }
        yyjson_doc_free(d);
        return;
    }
    if (id == l->health_id) {
        l->health_id = 0;
        yyjson_doc *d = error ? NULL : yyjson_read(l->health, l->health_size, 0);
        yyjson_val *v = yyjson_doc_get_root(d);
        if (l->health_stage == HEALTH_MODEL) {
            if (*str(v, "remote_host") || *str(v, "remote_model")) {
                yyjson_doc_free(d);
                engine_failure(l, "remote_model");
                return;
            }
            yyjson_val *capabilities = yyjson_obj_get(v, "capabilities");
            if (l->running_ollama && yyjson_is_arr(capabilities)) {
                bool completion = false;
                size_t index, count;
                yyjson_val *capability;
                yyjson_arr_foreach(capabilities, index, count,
                                   capability) if (yyjson_equals_str(capability, "completion")) completion =
                    true;
                if (!completion) {
                    sqlite3_stmt *q =
                        query(l, "UPDATE models SET "
                                 "metadata=json_set(metadata,'$.chatSupported',json('false')) WHERE id=?");
                    if (q)
                        sqlite3_bind_int64(q, 1, l->pending);
                    bool saved = done(q);
                    yyjson_doc_free(d);
                    engine_failure(l, saved ? "model_not_chat" : "storage_write");
                    return;
                }
            }
            if (l->running_ollama && l->pending > 0 && error && !strcmp(error, "not_found")) {
                sqlite3_stmt *q = query(l, "UPDATE models SET available=0 WHERE id=?");
                if (q)
                    sqlite3_bind_int64(q, 1, l->pending);
                if (!done(q)) {
                    engine_failure(l, "storage_write");
                    yyjson_doc_free(d);
                    return;
                }
            }
            if (d && yyjson_is_obj(v) && !*str(v, "error")) {
                model_capabilities(l, v);
                model_request(l, HEALTH_WARM);
            } else
                engine_failure(l, "model_unavailable");
        } else if (l->health_stage == HEALTH_WARM) {
            if (d && yyjson_get_bool(yyjson_obj_get(v, "done")) && !*str(v, "error")) {
                if (!l->cpu_probing || cpu_verified(l, v))
                    connect_pending(l, v);
            } else
                engine_failure(l, "model_unavailable");
        } else if (l->health_stage == HEALTH_PROPS) {
            if (d)
                model_capabilities(l, v);
            ot_preferences p;
            if (model_preferences(l, 0, &p) && p.gpu_layers == 0)
                model_request(l, HEALTH_CPU);
            else
                connect_pending(l, v);
        } else if (l->health_stage == HEALTH_CPU) {
            if (d && cpu_verified(l, v))
                connect_pending(l, v);
            else if (!d)
                engine_failure(l, "cpu_not_suitable");
        } else {
            bool ok = d && (l->running_ollama ? yyjson_is_arr(yyjson_obj_get(v, "models"))
                                              : !strcmp(str(v, "status"), "ok"));
            if (ok && l->pending > 0) {
                if (l->running_ollama) {
                    if (!l->health_external)
                        snprintf(l->ollama, sizeof l->ollama, "http://127.0.0.1:%u", l->engine.port);
                    else if (!*l->ollama)
                        copy(l->ollama, sizeof l->ollama, "http://127.0.0.1:11434");
                    tags(l, v);
                    model_request(l, HEALTH_MODEL);
                } else
                    model_request(l, HEALTH_PROPS);
            } else if (ok)
                connect_pending(l, v);
            else if (l->health_external) {
                l->health_external = false;
                l->ollama[0] = 0;
                launch_pending(l);
            }
        }
        yyjson_doc_free(d);
        return;
    }
    if (id != l->transfer_id)
        return;
    l->transfer_id = 0;
    if (error) {
        fail_transfer(l, error);
        return;
    }
    if (!strncmp(l->url, "ollama:", 7)) {
        pull_frame(l);
        if (!l->pull_done) {
            fail_transfer(l, "pull_incomplete");
            return;
        }
        finish_pull(l);
    } else
        finish_download(l);
}
uint64_t ot_library_revision(const ot_library *l) {
    return l->revision;
}
uint64_t ot_library_memory_revision(const ot_library *l) {
    return l->memory_revision + l->revision;
}
char *ot_library_memory(const ot_library *l) {
    const ot_memory_view *v = &l->memory_view;
    const ot_preferences *actual = ot_get_preferences(l->app);
    bool known = l->process_memory_known && l->engine.pid > 0 && l->engine.child > 0 &&
                 l->process_memory_child == l->engine.child && !l->engine.stopping;
    ot_runtime_info runtime = runtime_current(l) ? l->runtime : (ot_runtime_info){0};
    bool reported = runtime.allocation && v->model == l->selected && v->context == runtime.context &&
                    v->gpu_layers == actual->gpu_layers && v->memory_percent == actual->memory_percent &&
                    v->threads == actual->threads;
    ot_memory_plan plan = reported ? (ot_memory_plan){.total = runtime.allocation,
                                                      .weights = runtime.weights,
                                                      .cache = runtime.cache,
                                                      .compute = runtime.compute}
                                   : v->plan;
    uint64_t projected = 0;
    const ot_runtime_info *previous = l->memory_previous_model == v->model && l->memory_previous.compute &&
                                              l->memory_previous.weights == runtime.weights
                                          ? &l->memory_previous
                                          : NULL;
    bool anchored = !reported && !v->plan.observed && (l->running_ollama || previous) && known &&
                    runtime.allocation && v->model == l->selected && v->gpu_layers == actual->gpu_layers &&
                    v->memory_percent == actual->memory_percent && v->threads == actual->threads &&
                    ot_memory_project(v->context, &runtime, previous, &projected);
    if (anchored)
        plan = (ot_memory_plan){.total = projected};
    const ot_memory_plan *p = &plan;
    const char *source = reported                  ? (runtime.weights ? "buffers" : "reported")
                         : anchored || p->observed ? "observed"
                         : p->engine_plan          ? "engine"
                                                   : "metadata";
    char out[1536];
    snprintf(
        out, sizeof out,
        "{\"estimate\":{\"id\":%lld,\"context\":%u,\"gpuLayers\":%d,\"memoryPercent\":%u,\"threads\":%u,"
        "\"pending\":%s,"
        "\"bytes\":%llu,\"weights\":%llu,\"cache\":%llu,\"compute\":%llu,"
        "\"source\":\"%s\",\"knownCache\":%s},\"runtime\":{\"id\":%lld,\"context\":%u,"
        "\"gpuLayers\":%d,\"processKnown\":%s,\"processBytes\":%llu,\"acceleration\":\"%s\","
        "\"effectiveContext\":%u,\"batch\":%u,\"microBatch\":%u,\"windowCells\":%u,\"gpuLayersObserved\":%u,"
        "\"layersObserved\":%u,\"otherResidentModels\":%u,\"allocationBytes\":%llu,"
        "\"gpuAllocationBytes\":%llu,\"weightsBytes\":%llu,\"cacheBytes\":%llu,"
        "\"linearCacheBytes\":%llu,\"computeBytes\":%llu}}",
        (long long)v->model, v->context, v->gpu_layers, v->memory_percent, v->threads,
        v->pending ? "true" : "false", (unsigned long long)p->total, (unsigned long long)p->weights,
        (unsigned long long)p->cache, (unsigned long long)p->compute, source,
        p->known_cache ? "true" : "false", (long long)l->selected, actual->context, actual->gpu_layers,
        known ? "true" : "false", (unsigned long long)(known ? l->process_memory : 0),
        ot_runtime_name(runtime.kind), runtime.context, runtime.batch, runtime.micro_batch,
        runtime.window_cells, runtime.gpu_layers, runtime.total_layers, runtime.other_models,
        (unsigned long long)runtime.allocation, (unsigned long long)runtime.gpu_allocation,
        (unsigned long long)runtime.weights, (unsigned long long)runtime.cache,
        (unsigned long long)runtime.linear_cache, (unsigned long long)runtime.compute);
    return strdup(out);
}
char *ot_library_health(const ot_library *l) {
    char out[2304];
    size_t sources[3] = {0};
    for (size_t i = 0; i < l->advice_count; i++)
        if (l->advice[i].kind == 1 || l->advice[i].kind == 2) sources[l->advice[i].kind]++;
    char *memory = ot_library_memory(l);
    snprintf(out, sizeof out,
             "{\"engine\":\"%s\",\"owned\":%s,\"pid\":%d,\"download\":%s,\"downloadOwned\":%s,"
             "\"downloadPid\":%d,\"preparing\":%s,\"received\":%llu,\"total\":%llu,\"error\":\"%s\","
             "\"activityError\":%s,\"engineError\":\"%s\",\"memory\":%s,"
             "\"suggestions\":{\"open\":%s,\"fetching\":%s,\"candidates\":%zu,\"llama\":%zu,\"ollama\":%zu,\"requests\":%u,\"prefetchHits\":%u,\"peakRequests\":%u,\"elapsedMs\":%.1f},"
             "\"catalogCache\":{\"bytes\":%zu,\"hits\":%llu,\"misses\":%llu}}",
             l->engine_state, l->engine.pid > 0 ? "true" : "false", l->engine.pid > 0 ? l->engine.child : 0,
             (l->transfer_id || l->working || l->download_preparing) ? "true" : "false",
             l->download_engine.pid > 0 ? "true" : "false",
             l->download_engine.pid > 0 ? l->download_engine.child : 0,
             l->download_preparing ? "true" : "false", (unsigned long long)l->received,
             (unsigned long long)l->total, l->error, l->activity_error ? "true" : "false", l->engine_error,
             memory ? memory : "{}", l->advice_visible ? "true" : "false", l->advice_id ? "true" : "false",
             l->advice_count, sources[1], sources[2], l->advice_requests, l->advice_hits, l->advice_peak,
             l->advice_cycle_started > 0 ? 1000 * ((l->advice_finished > 0 ? l->advice_finished : now()) - l->advice_cycle_started) : 0, l->catalog_cache_bytes, (unsigned long long)l->catalog_hits,
             (unsigned long long)l->catalog_misses);
    free(memory);
    return strdup(out);
}
static void advice_view(ot_library *l, yyjson_mut_doc *d, yyjson_mut_val *v) {
    yyjson_mut_val *recommendations = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "recommendations", recommendations);
    yyjson_mut_val *advice = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "adviceRecommendations", advice);
    const ot_preferences *preferences = ot_get_preferences(l->app);
    yyjson_mut_obj_add_bool(d, v, "suggesting", l->advice_id != 0);
    yyjson_mut_obj_add_uint(d, v, "adviceRequest", l->advice_request_epoch);
    yyjson_mut_obj_add_bool(d, v, "suggestionsReady", l->advice_ready);
    yyjson_mut_obj_add_strcpy(d, v, "suggestionError", l->advice_error);
    yyjson_mut_obj_add_sint(d, v, "adviceUpdated", l->advice_updated);
    sqlite3_stmt *dismissed = query(l, "SELECT json_array_length(dismissed) FROM advice_state WHERE id=1");
    unsigned ignored = dismissed && sqlite3_step(dismissed) == SQLITE_ROW ?
                       (unsigned)sqlite3_column_int(dismissed, 0) : 0;
    sqlite3_finalize(dismissed);
    yyjson_mut_obj_add_uint(d, v, "adviceDismissed", ignored);
    if (!ot_network_allowed(preferences, OT_NETWORK_CATALOGUES)) return;
    /* Local entries and the online catalogue form one shortlist. Publishing the
     * local subset during collection makes cards jump when remote results arrive. */
    if (l->advice_id && !l->advice_ready)
        return;
    ot_model_candidate *candidates = calloc(OT_ADVICE_MODELS + 32, sizeof *candidates);
    if (!candidates) return;
    size_t count = 0;
    int64_t current_model = l->selected ? l->selected : ot_local_model(l->app);
    const char *local_reference = advice_local_reference(l);
    if (l->advice_snapshot) count = ot_model_catalogue_read(l->advice_snapshot, candidates, OT_ADVICE_MODELS);
    else if (l->advice_ready)
        for (size_t i = 0; i < l->advice_count; i++) candidates[count++] = l->advice[i];
    for (size_t i = count; i > 0; i--)
        if (!(preferences->setup_engines & (unsigned)candidates[i-1].kind)) candidates[i-1] = candidates[--count];
    /* The bounded comparison must retain the active identity, even after many
     * newer imports. Otherwise an already installed model can become its own upgrade. */
    sqlite3_stmt *local = query(l, "SELECT id,kind,name,reference,bytes,context,metadata FROM models "
                                   "WHERE hidden=0 AND available=1 "
                                   "ORDER BY (id=?1) DESC,(kind=2 AND reference=?2) DESC,id DESC LIMIT 32");
    if (local) {
        sqlite3_bind_int64(local, 1, current_model);
        sqlite3_bind_text(local, 2, local_reference, -1, SQLITE_TRANSIENT);
    }
    while (local && sqlite3_step(local) == SQLITE_ROW) {
        yyjson_doc *meta = yyjson_read(col(local, 6), strlen(col(local, 6)), 0);
        yyjson_val *fields = yyjson_doc_get_root(meta);
        bool usable = !yyjson_is_true(yyjson_obj_get(fields, "cloud")) &&
                      !yyjson_is_true(yyjson_obj_get(fields, "fileChanged")) &&
                      !yyjson_is_false(yyjson_obj_get(fields, "chatSupported"));
        int kind = sqlite3_column_int(local, 1);
        usable &= (kind == 1 || kind == 2) && (preferences->setup_engines & (unsigned)kind);
        if (!usable && kind == 2)
            for (size_t i = 0; i < count; i++)
                if (!strcmp(candidates[i].name, col(local, 3)) &&
                    !advice_tag_changed(&candidates[i], str(fields, "digest"))) {
                    candidates[i] = candidates[--count];
                    break;
                }
        if (usable && (kind == 1 || kind == 2)) {
            ot_model_candidate *m = NULL;
            for (size_t i = 0; i < count; i++)
                if (candidates[i].kind == kind &&
                    (kind == 2 ? !strcmp(candidates[i].name, col(local, 3)) &&
                                     !advice_tag_changed(&candidates[i], str(fields, "digest"))
                               : *str(fields, "checksum") &&
                                     !strcmp(candidates[i].checksum, str(fields, "checksum")))) {
                    m = &candidates[i];
                    break;
                }
            if (!m && count < OT_ADVICE_MODELS + 32) {
                m = &candidates[count++];
                *m = (ot_model_candidate){.kind = kind};
                copy(m->name, sizeof m->name, col(local, kind == 2 ? 3 : 2));
                copy(m->description, sizeof m->description, col(local, 2));
            }
            if (m) {
                m->installed = sqlite3_column_int64(local, 0);
                if (!current_model && kind == 2 && !strcmp(local_reference, col(local, 3)))
                    current_model = m->installed;
                m->bytes = (uint64_t)sqlite3_column_int64(local, 4);
                m->context = (unsigned)sqlite3_column_int(local, 5);
                m->reasoning |= yyjson_is_true(yyjson_obj_get(fields, "reasoning"));
                m->approximate = false;
                if (!*m->checksum) copy(m->checksum, sizeof m->checksum, str(fields, "checksum"));
                const char *digest = str(fields, "digest");
                if (!strncmp(digest, "sha256:", 7)) digest += 7;
                if (sha(digest)) copy(m->catalogue_digest, sizeof m->catalogue_digest, digest);
                copy(m->quantization, sizeof m->quantization, str(fields, "quantization"));
                m->verified = kind == 2 ? ot_model_verified(col(local, 3), str(fields, "digest"))
                                        : ot_model_file_verified(str(fields, "checksum"));
            }
        }
        if (usable && kind == 2)
            for (size_t i = 0; i < count; i++)
                if (!strcmp(candidates[i].name, col(local, 3)) &&
                    advice_tag_changed(&candidates[i], str(fields, "digest")))
                    candidates[i].update_of = sqlite3_column_int64(local, 0);
        yyjson_doc_free(meta);
    }
    sqlite3_finalize(local);
    ot_resources available = model_resources(l);
    ot_advice_profile learned = {0};
    if (preferences->advice_learning)
        (void)ot_store_advice_profile(ot_app_store(l->app), &learned, preferences->advice_since);
    yyjson_mut_obj_add_uint(d, v, "adviceSamples", learned.samples);
    double scores[OT_ADVICE_MODELS + 32];
    ot_advice_profile targets[32] = {0};
    size_t matched[32], measurements = 0;
    for (size_t i = 0; i < count; i++) {
        ot_model_candidate *m = &candidates[i];
        if (preferences->advice_learning && m->installed && measurements < 32) {
            ot_advice_profile *target = &targets[measurements];
            target->runtime = (unsigned)m->kind;
            ot_preferences measured = *preferences, saved = ot_preferences_default();
            if (ot_store_generation(ot_app_store(l->app), m->installed, &saved) > 0)
                ot_preferences_model_values(&measured, &saved);
            ot_advice_environment(target, &measured, &available);
            copy(target->identity, sizeof target->identity, m->kind == 1 ? m->checksum : m->catalogue_digest);
            matched[measurements++] = i;
        }
    }
    if (ot_store_advice_performance(ot_app_store(l->app), targets, measurements))
        for (size_t i = 0; i < measurements; i++) {
            ot_model_candidate *m = &candidates[matched[i]];
            m->usage_count = targets[i].samples;
            m->first_token_ms = targets[i].first_token_ms;
            m->tokens_per_second = targets[i].tokens_per_second;
        }
    for (size_t i = 0; i < count; i++)
        scores[i] = ot_model_candidate_rank(&candidates[i], &available, preferences, &learned);
    ot_model_evidence_scores(candidates, scores, count);
    /* Use the shared capacity policy, preferring tested content only among close,
     * comparable candidates. The qualification table never supplies the catalogue. */
    size_t order[OT_ADVICE_MODELS + 32];
    size_t ranked = ot_model_candidate_order(candidates, scores, count, order);
    double nearest_size = HUGE_VAL;
    if (preferences->advice_size_gb)
        for (size_t i = 0; i < count; i++)
            if (scores[i] >= 0 && candidates[i].bytes)
                nearest_size = fmin(nearest_size, fabs(log((double)candidates[i].bytes /
                    ((double)preferences->advice_size_gb * 1000000000))));
    /* A size preference means nearby alternatives, not three arbitrary sizes.
     * If that range is absent, retain the closest available match rather than an empty view. */
    double size_window = fmax(log(1.5), nearest_size + .01);
    size_t baseline = SIZE_MAX, leader = SIZE_MAX, winner = SIZE_MAX;
    for (size_t rank = 0; rank < ranked; rank++) {
        size_t i = order[rank];
        if (current_model > 0 ? candidates[i].installed == current_model : candidates[i].installed > 0) {
            baseline = i;
            break;
        }
    }
    for (size_t rank = 0; rank < ranked; rank++) {
        size_t i = order[rank];
        const ot_model_candidate *m = &candidates[i];
        char key[280]; advice_key(m, key);
        if (preferences->advice_size_gb && m->bytes && fabs(log((double)m->bytes /
                ((double)preferences->advice_size_gb * 1000000000))) > size_window) continue;
        if (ot_model_candidate_upgrade(m, scores[i],
                baseline == SIZE_MAX ? NULL : &candidates[baseline],
                baseline == SIZE_MAX ? 0 : scores[baseline])) {
            if (leader == SIZE_MAX) leader = i;
            if (!advice_ignored(l, key)) { winner = i; break; }
        }
    }
    /* One qualified next step, never three padded alternatives. Setup retains
     * its independent shortlist because it is choosing an initial model. */
    if (preferences->setup == 2 && preferences->advice_notices && winner != SIZE_MAX && winner == leader) {
        const ot_model_candidate *first = &candidates[winner];
        char key[280]; advice_key(first, key);
        yyjson_mut_val *notice = strcmp(key, preferences->advice_seen) ? yyjson_mut_obj(d) : NULL;
        if (notice) {
            yyjson_mut_obj_add_val(d, v, "recommendationNotice", notice);
            yyjson_mut_obj_add_strcpy(d, notice, "key", key);
            yyjson_mut_obj_add_strcpy(d, notice, "name", first->name);
            yyjson_mut_obj_add_strcpy(d, notice, "reason", baseline != SIZE_MAX &&
                (double)first->bytes < (double)candidates[baseline].bytes * .75
                ? "resources" : first->metric_count ? "evidence" : "needs");
        }
    }
    size_t emitted = 0, emitted_order[3];
    bool advice_emitted = winner == SIZE_MAX;
    for (size_t rank = 0; rank < ranked && (emitted < 3 || !advice_emitted); rank++) {
        const ot_model_candidate *m = &candidates[order[rank]];
        if (scores[order[rank]] < 0 && rank >= 3)
            continue;
        if (preferences->advice_size_gb && m->bytes && fabs(log((double)m->bytes /
                ((double)preferences->advice_size_gb * 1000000000))) > size_window) continue;
        bool repeated = false;
        for (size_t i = 0; i < emitted; i++) {
            const ot_model_candidate *shown = &candidates[emitted_order[i]];
            bool replacement = !strcmp(m->name,shown->name) && (m->update_of || shown->update_of);
            if (!replacement && ot_model_candidate_same_family(m,shown)) repeated = true;
        }
        bool shortlist = !repeated && emitted < 3;
        if (!shortlist && order[rank] != winner) continue;
        if (shortlist) emitted_order[emitted++] = order[rank];
        yyjson_mut_val *item = yyjson_mut_obj(d);
        if (shortlist) yyjson_mut_arr_append(recommendations, item);
        char key[280];
        if (m->installed)
            snprintf(key, sizeof key, "local:%lld", (long long)m->installed);
        else if (m->kind == 1)
            snprintf(key, sizeof key, "gguf:%s", m->checksum);
        else
            copy(key, sizeof key, m->name);
        yyjson_mut_obj_add_strcpy(d, item, "option", key);
        char identity[280]; advice_key(m, identity);
        yyjson_mut_obj_add_strcpy(d, item, "adviceKey", identity);
        yyjson_mut_obj_add_strcpy(d, item, "name", m->name);
        model_label(d, item, m->name, NULL);
        yyjson_mut_obj_add_uint(d, item, "bytes", m->bytes);
        ot_model_info suggested = {.bytes = m->bytes, .context = m->context, .complex_cache = true};
        if (m->installed)
            model_info(l, m->installed, &suggested);
        ot_preferences recipe = ot_model_preset(preferences, &suggested, &available, OT_PRESET_RECOMMENDED);
        ot_preferences saved_values;
        if (m->installed && ot_store_generation(ot_app_store(l->app), m->installed, &saved_values) > 0)
            ot_preferences_model_values(&recipe, &saved_values);
        yyjson_mut_obj_add_uint(d, item, "context", recipe.context);
        yyjson_mut_obj_add_uint(d, item, "input", recipe.input);
        yyjson_mut_obj_add_uint(d, item, "output", recipe.output);
        yyjson_mut_obj_add_bool(d, item, "reasoning", m->reasoning);
        yyjson_mut_obj_add_bool(d, item, "verified", m->verified);
        yyjson_mut_obj_add_bool(d, item, "fits", scores[order[rank]] >= 0);
        yyjson_mut_obj_add_bool(d, item, "approximate", m->approximate);
        yyjson_mut_obj_add_strcpy(d, item, "quantization", m->quantization);
        if (m->kind == 1) yyjson_mut_obj_add_strcpy(d, item, "fileName", m->file);
        yyjson_mut_val *evidence = yyjson_mut_arr(d);
        yyjson_mut_obj_add_val(d, item, "evidence", evidence);
        for (unsigned i = 0; i < m->metric_count && i < OT_ADVICE_METRICS; i++) {
            yyjson_mut_val *metric = yyjson_mut_obj(d);
            yyjson_mut_arr_append(evidence, metric);
            yyjson_mut_obj_add_strcpy(d, metric, "benchmark", m->metrics[i].benchmark);
            yyjson_mut_obj_add_strcpy(d, metric, "task", m->metrics[i].task);
            yyjson_mut_obj_add_real(d, metric, "value", m->metrics[i].value);
            yyjson_mut_obj_add_bool(d, metric, "verified", m->metrics[i].verified);
            yyjson_mut_obj_add_bool(d, metric, "exact", m->metrics[i].exact);
        }
        yyjson_mut_obj_add_bool(d, item, "baseEvidence", m->base_evidence);
        yyjson_mut_obj_add_bool(d, item, "space",
                                m->installed || !available.disk_available ||
                                    (m->bytes < available.disk_available &&
                                     (UINT64_C(2) << 30) < available.disk_available - m->bytes));
        yyjson_mut_obj_add_bool(d, item, "recommended", rank == 0 && scores[order[rank]] >= 0);
        yyjson_mut_obj_add_sint(d, item, "installed", m->installed);
        yyjson_mut_obj_add_sint(d, item, "updateOf", m->update_of);
        yyjson_mut_obj_add_sint(d, item, "kind", m->kind);
        if (*m->repository) {
            char source[1024];
            snprintf(source, sizeof source, "https://huggingface.co/%s", m->repository);
            yyjson_mut_obj_add_strcpy(d, item, "sourceURL", source);
            yyjson_mut_obj_add_strcpy(d, item, "license", m->license);
        }
        if (m->kind == 2 && ot_model_name(m->name) && !strchr(m->name, '/')) {
            char family[257], source[320];
            copy(family, sizeof family, m->name);
            char *colon = strchr(family, ':');
            if (colon)
                *colon = 0;
            snprintf(source, sizeof source, "https://ollama.com/library/%s", family);
            yyjson_mut_obj_add_strcpy(d, item, "sourceURL", source);
        }
        if (order[rank] == winner) {
            yyjson_mut_arr_append(advice, shortlist ? yyjson_mut_val_mut_copy(d,item) : item);
            advice_emitted = true;
        }
    }
    free(candidates);
}
char *ot_library_view(ot_library *l) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, v);
    yyjson_mut_obj_add_strcpy(d, v, "error", l->error);
    yyjson_mut_obj_add_strcpy(d, v, "engineError", l->engine_error);
    yyjson_mut_obj_add_bool(d, v, "activityError", l->activity_error);
    yyjson_mut_obj_add_strcpy(d, v, "engine", l->engine_state);
    yyjson_mut_obj_add_bool(d, v, "owned", l->engine.pid > 0);
    yyjson_mut_obj_add_bool(d, v, "downloadPreparing", l->download_preparing && !l->removing);
    yyjson_mut_obj_add_sint(d, v, "removing", l->removing);
    yyjson_mut_obj_add_bool(d, v, "ollama", *l->ollama != 0);
    yyjson_mut_obj_add_bool(d, v, "discovering", l->working && l->worker_kind == 2);
    yyjson_mut_obj_add_bool(d, v, "busy", l->pending != 0 || l->removing != 0);
    advice_view(l, d, v);
    const char *target = ot_get_preferences(l->app)->setup_target;
    if (*target || ot_get_preferences(l->app)->setup_target_engines) {
        bool engines_only = !*target;
        int64_t id = engines_only ? 0 : named_model(l, target);
        sqlite3_stmt *q = query(l, "SELECT name,kind FROM models WHERE id=?");
        if (q)
            sqlite3_bind_int64(q, 1, id);
        bool found = q && sqlite3_step(q) == SQLITE_ROW;
        char name[257];
        copy(name, sizeof name, found ? col(q, 0) : target);
        int kind = engines_only ? 0 : found ? sqlite3_column_int(q, 1) : !strncmp(target, "gguf:", 5) ? 1 : 2;
        sqlite3_finalize(q);
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, v, "setupModel", item);
        yyjson_mut_obj_add_strcpy(d, item, "target", target);
        yyjson_mut_obj_add_bool(d, item, "enginesOnly", engines_only);
        unsigned required=0;
        if (setup_access(l,ot_get_preferences(l->app),&required))
            yyjson_mut_obj_add_uint(d,item,"resumeAccess",required);
        yyjson_mut_obj_add_bool(d, item, "enginesReady", setup_engines_ready(l, ot_get_preferences(l->app)));
        yyjson_mut_obj_add_sint(d, item, "id", id);
        yyjson_mut_obj_add_uint(d, item, "engines", ot_get_preferences(l->app)->setup_target_engines);
        char url[512];
        snprintf(url, sizeof url, "ollama:%s", target);
        q = query(l, kind == 1 ? "SELECT id,name FROM downloads WHERE checksum=? ORDER BY id DESC LIMIT 1"
                              : "SELECT id,name FROM downloads WHERE url=? ORDER BY id DESC LIMIT 1");
        if (q) sqlite3_bind_text(q, 1, kind == 1 ? target + 5 : url, -1, SQLITE_TRANSIENT);
        if (q && sqlite3_step(q) == SQLITE_ROW) {
            yyjson_mut_obj_add_sint(d, item, "job", sqlite3_column_int64(q, 0));
            if (!found) copy(name, sizeof name, col(q, 1));
        }
        sqlite3_finalize(q);
        yyjson_mut_obj_add_strcpy(d, item, "name", name);
        yyjson_mut_obj_add_sint(d, item, "kind", kind);
    }
    yyjson_mut_obj_add_sint(d, v, "selected", l->selected);
    yyjson_mut_obj_add_sint(d, v, "pending", l->pending);
    yyjson_mut_obj_add_sint(d, v, "requested", l->requested);
    yyjson_mut_obj_add_bool(d, v, "searching", l->query_id != 0);
    yyjson_mut_obj_add_bool(d, v, "previewing", l->working && l->worker_kind == 3);
    if (l->import_ready) {
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, v, "importCandidate", item);
        yyjson_mut_obj_add_uint(d, item, "token", l->import_token);
        const char *base = strrchr(l->import_path, '/');
        yyjson_mut_obj_add_strcpy(d, item, "file", base ? base + 1 : l->import_path);
        yyjson_mut_obj_add_strcpy(d, item, "name",
                                  *l->import_info.name ? l->import_info.name
                                  : base               ? base + 1
                                                       : l->import_path);
        yyjson_mut_obj_add_uint(d, item, "bytes", l->import_info.bytes);
        yyjson_mut_obj_add_uint(d, item, "context", l->import_info.context);
        yyjson_mut_obj_add_strcpy(d, item, "family", l->import_info.architecture);
    }
    if (l->preview_ready) {
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, v, "downloadCandidate", item);
        yyjson_mut_obj_add_uint(d, item, "token", l->preview_token);
        yyjson_mut_obj_add_strcpy(d, item, "name", l->preview_name);
        yyjson_mut_obj_add_strcpy(d, item, "url", l->preview_url);
        yyjson_mut_obj_add_strcpy(d, item, "checksum", l->preview_checksum);
        yyjson_mut_obj_add_uint(d, item, "bytes", l->preview_bytes);
    }
    ot_model_info model;
    int64_t current_model=l->selected ? l->selected : l->requested ? l->requested :
        *ot_configuration(l->app)->model ? ot_local_model(l->app) : 0;
    model_info(l,current_model,&model);
    const ot_server_info *server = current_model ? NULL : ot_connection_model(l->app);
    if (server) model = server->model;
    ot_resources resources = model_resources(l);
    yyjson_mut_val *profiles = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, v, "profiles", profiles);
    for (unsigned i = 0; i < OT_PRESET_COUNT; i++) {
        ot_preferences p = ot_model_preset(ot_get_preferences(l->app), &model, server ? NULL : &resources, i);
        yyjson_mut_obj_add_val(d, profiles, ot_model_profile_names[i], ot_preferences_model_json(d, &p));
    }
    ot_preferences defaults = ot_preferences_default();
    defaults = ot_model_preset(&defaults, &model, server ? NULL : &resources, OT_PRESET_RECOMMENDED);
    yyjson_mut_obj_add_val(d, v, "modelDefaults", ot_preferences_model_json(d, &defaults));
    yyjson_mut_obj_add_strcpy(d, v, "catalogKind", l->query_mode);
    yyjson_mut_obj_add_bool(d, v, "catalogHasMore", l->catalog_has_more);
    yyjson_mut_obj_add_strcpy(
        d, v, "catalogRepository",
        !strcmp(l->query_mode, "variants") || !strcmp(l->query_mode, "ollamaVariants") ? l->repo : "");
    yyjson_mut_val *engines = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "engines", engines);
    yyjson_mut_obj_add_uint(d, v, "engineScan", l->engine_scan);
    for (unsigned i = 0; i < 2; i++) {
        if (!*l->detected_engines[i])
            continue;
        yyjson_mut_val *e = yyjson_mut_obj(d);
        yyjson_mut_arr_append(engines, e);
        yyjson_mut_obj_add_uint(d, e, "kind", i + 1);
        yyjson_mut_obj_add_str(d, e, "name", i ? "Ollama" : "llama.cpp");
        yyjson_mut_obj_add_strcpy(d, e, "path", l->detected_engines[i]);
        char managed[4096];
        bool owned = ot_package_find(ot_package_for_kind((int)i + 1), l->components, managed) &&
                     !strcmp(managed, l->detected_engines[i]);
        yyjson_mut_obj_add_bool(d, e, "managed", owned);
        yyjson_mut_obj_add_bool(d, e, "manual", l->custom_engines[i]);
        yyjson_mut_obj_add_uint(
            d, e, "validation",
            l->working && l->worker_kind == 4 && l->validation_kind == i + 1 ? 3 : l->validation[i]);
        yyjson_mut_obj_add_bool(d, e, "idleSleep", i || l->llama_sleep);
    }
    yyjson_mut_val *packages = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "enginePackages", packages);
    for (int kind = 1; kind <= 2; kind++) {
        const ot_package *package = ot_package_for_kind(kind);
        if (!package) continue;
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_arr_append(packages, item);
        yyjson_mut_obj_add_uint(d, item, "kind", (unsigned)package->kind);
        yyjson_mut_obj_add_str(d, item, "name", package->name);
        yyjson_mut_obj_add_str(d, item, "version", package->version);
        yyjson_mut_obj_add_uint(d, item, "bytes", package->bytes);
    }
    const char *catalog_json = l->catalog ? l->catalog : "[]";
    yyjson_doc *catalog_doc = yyjson_read(catalog_json, strlen(catalog_json), 0);
    yyjson_mut_val *catalog_items = yyjson_val_mut_copy(d, yyjson_doc_get_root(catalog_doc));
    if (!strcmp(l->query_mode, "variants") || !strcmp(l->query_mode, "ollamaVariants"))
        catalog_resources(l, d, catalog_items);
    yyjson_mut_obj_add_val(d, v, "catalog", catalog_items);
    yyjson_doc_free(catalog_doc);
    yyjson_mut_val *models = yyjson_mut_arr(d), *hidden = yyjson_mut_arr(d), *jobs = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "models", models);
    yyjson_mut_obj_add_val(d, v, "hiddenModels", hidden);
    yyjson_mut_obj_add_val(d, v, "jobs", jobs);
    sqlite3_stmt *q = query(
        l,
        /* Count each identity/shape once. Correlated counts become quadratic when
         * many quantizations share a size or geometry. Hidden references still
         * report matches against visible models, exactly as ordinary rows do. */
        "WITH identities AS MATERIALIZED (SELECT device,inode,count(*) AS n FROM models "
        "WHERE kind=1 AND hidden=0 AND inode>0 GROUP BY device,inode), "
        "shapes AS MATERIALIZED (SELECT bytes,layers,embedding,count(*) AS n FROM models "
        "WHERE kind=1 AND hidden=0 AND bytes>0 GROUP BY bytes,layers,embedding) "
        "SELECT m.id,m.name,m.kind,m.bytes,m.managed,m.context,m.layers,m.reference,m.metadata,"
        "m.hidden,m.origin,m.available,coalesce(i.n,0),coalesce(s.n,0) "
        "FROM models m LEFT JOIN identities i ON m.kind=1 AND m.device=i.device AND m.inode=i.inode "
        "LEFT JOIN shapes s ON m.kind=1 AND m.bytes=s.bytes AND m.layers=s.layers AND m.embedding=s.embedding "
        "ORDER BY (m.id=?1 OR m.id=?2) DESC,m.hidden,m.name LIMIT 1000");
    if (q) {
        sqlite3_bind_int64(q, 1, ot_local_model(l->app));
        sqlite3_bind_int64(q, 2, l->requested);
    }
    if (q)
        while (sqlite3_step(q) == SQLITE_ROW) {
            yyjson_mut_val *m = yyjson_mut_obj(d);
            bool is_hidden = sqlite3_column_int(q, 9) != 0;
            yyjson_mut_arr_append(is_hidden ? hidden : models, m);
            yyjson_mut_obj_add_bool(d, m, "hidden", is_hidden);
            yyjson_mut_obj_add_strcpy(d, m, "origin", col(q, 10));
            yyjson_mut_obj_add_bool(d, m, "available", sqlite3_column_int(q, 11) != 0);
            yyjson_mut_obj_add_bool(d, m, "canDelete",
                                    sqlite3_column_int(q, 2) == 2 ||
                                        (sqlite3_column_int(q, 2) == 1 && sqlite3_column_int(q, 11) != 0 &&
                                         (sqlite3_column_int64(q, 4) > 0 || !strcmp(col(q, 10), "manual"))));
            yyjson_mut_obj_add_int(d, m, "sameFile", sqlite3_column_int(q, 12));
            yyjson_mut_obj_add_int(d, m, "possibleDuplicates", sqlite3_column_int(q, 13));
            yyjson_mut_obj_add_sint(d, m, "id", sqlite3_column_int64(q, 0));
            yyjson_mut_obj_add_strcpy(d, m, "name", col(q, 1));
            yyjson_mut_obj_add_int(d, m, "kind", sqlite3_column_int(q, 2));
            yyjson_mut_obj_add_uint(d, m, "bytes", (uint64_t)sqlite3_column_int64(q, 3));
            yyjson_mut_obj_add_bool(d, m, "managed", sqlite3_column_int(q, 4) > 0);
            yyjson_mut_obj_add_int(d, m, "context", sqlite3_column_int64(q, 5));
            yyjson_mut_obj_add_int(d, m, "layers", sqlite3_column_int64(q, 6));
            int64_t cpu_id = sqlite3_column_int64(q, 0);
            if (cpu_id == l->selected || cpu_id == l->requested) {
                unsigned capacity = cpu_capacity(l, cpu_id);
                double rate = 0;
                bool cached = cpu_cached(l, cpu_id, ot_get_preferences(l->app), &rate);
                if (cached && rate >= 8 && capacity < ot_get_preferences(l->app)->context)
                    capacity = ot_get_preferences(l->app)->context;
                yyjson_mut_obj_add_uint(d, m, "cpuMaxContext", cached && rate < 8 ? 0 : capacity);
                yyjson_mut_obj_add_bool(d, m, "cpuQualified", cached && rate >= 8);
            }
            const char *reference = col(q, 7), *base = strrchr(reference, '/');
            yyjson_mut_obj_add_strcpy(d, m, "file", base ? base + 1 : reference);
            const char *metadata = col(q, 8);
            yyjson_doc *meta = strlen(metadata) <= 20480 ? yyjson_read(metadata, strlen(metadata), 0) : NULL;
            if (meta && yyjson_is_obj(yyjson_doc_get_root(meta))) {
                model_label(d, m, col(q, 1), yyjson_doc_get_root(meta));
                bool verified = sqlite3_column_int(q, 2) == 2
                                    ? ot_model_verified(reference, str(yyjson_doc_get_root(meta), "digest"))
                                    : ot_model_file_verified(str(yyjson_doc_get_root(meta), "checksum"));
                verified &= sqlite3_column_int(q, 11) != 0 &&
                            !yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(meta), "fileChanged"));
                yyjson_mut_obj_add_bool(d, m, "verifiedChat", verified);
                yyjson_mut_val *visible = yyjson_val_mut_copy(d, yyjson_doc_get_root(meta));
                yyjson_mut_obj_remove_key(visible, "memoryObservation");
                yyjson_mut_obj_remove_key(visible, "cpuQualification");
                yyjson_mut_obj_add_val(d, m, "metadata", visible);
                if (yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(meta), "fileChanged"))) {
                    yyjson_mut_obj_remove_key(m, "canDelete");
                    yyjson_mut_obj_add_bool(d, m, "canDelete", false);
                }
            } else
                model_label(d, m, col(q, 1), NULL);
            yyjson_doc_free(meta);
        }
    sqlite3_finalize(q);
    q = query(l, "SELECT (SELECT count(*) FROM models)>1000 OR "
                 "(SELECT count(*) FROM downloads WHERE coalesce(json_extract(metadata,'$.discard'),0)=0)>1000");
    bool projection_limited = q && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    yyjson_mut_obj_add_bool(d, v, "projectionLimited", projection_limited);
    q = query(l, "SELECT id,name,state,error,received,total,url LIKE 'ollama:%',created,updated,metadata,"
                 "EXISTS(SELECT 1 FROM models WHERE managed=downloads.id),url,checksum FROM downloads WHERE coalesce(json_extract(metadata,'$.discard'),0)=0 ORDER "
                 "BY (state IN('queued','preparing','downloading','verifying','registering','paused')) DESC,updated DESC,id "
                 "DESC LIMIT 1000");
    if (q)
        while (sqlite3_step(q) == SQLITE_ROW) {
            yyjson_mut_val *m = yyjson_mut_obj(d);
            yyjson_mut_arr_append(jobs, m);
            yyjson_mut_obj_add_sint(d, m, "id", sqlite3_column_int64(q, 0));
            yyjson_mut_obj_add_strcpy(d, m, "name", col(q, 1));
            bool component = ot_package_for_download(col(q, 11), col(q, 12)) != NULL;
            yyjson_mut_obj_add_bool(d, m, "component", component);
            const char *state = col(q, 2);
            bool retry_registration = !strcmp(state, "paused") && !strcmp(col(q, 3), "storage_write") &&
                                      sqlite3_column_int64(q, 5) > 0 &&
                                      sqlite3_column_int64(q, 4) == sqlite3_column_int64(q, 5);
            const char *phase = retry_registration ? "registration_failed" : state;
            yyjson_mut_obj_add_strcpy(d, m, "state", state);
            yyjson_mut_obj_add_strcpy(d, m, "phase", phase);
            yyjson_mut_obj_add_bool(d, m, "retryRegistration", retry_registration);
            bool offline_resume = !sqlite3_column_int(q, 6) && !strcmp(state, "paused") &&
                                  sqlite3_column_int64(q, 5) > 0 &&
                                  sqlite3_column_int64(q, 4) == sqlite3_column_int64(q, 5) &&
                                  strcmp(col(q, 3), "checksum_mismatch") && strcmp(col(q, 3), "invalid_gguf");
            yyjson_mut_obj_add_bool(d, m, "resumeOffline", offline_resume);
            yyjson_mut_obj_add_str(d, m, "destination", sqlite3_column_int(q, 6) ? "engine" : "overtone");
            yyjson_mut_obj_add_strcpy(d, m, "error", col(q, 3));
            yyjson_mut_obj_add_uint(d, m, "received", (uint64_t)sqlite3_column_int64(q, 4));
            yyjson_mut_obj_add_uint(d, m, "total", (uint64_t)sqlite3_column_int64(q, 5));
            yyjson_mut_obj_add_str(d, m, "source",
                                   component                  ? "component"
                                   : sqlite3_column_int(q, 6) ? "ollama"
                                                              : "gguf");
            yyjson_mut_obj_add_int(d, m, "created", sqlite3_column_int64(q, 7));
            yyjson_mut_obj_add_int(d, m, "updated", sqlite3_column_int64(q, 8));
            yyjson_mut_obj_add_bool(
                d, m, "registered",
                sqlite3_column_int(q, 10) != 0 ||
                    ((component || sqlite3_column_int(q, 6)) && !strcmp(state, "complete")));
            bool live = l->job == sqlite3_column_int64(q, 0) && l->transfer_id != 0;
            if (live) {
                yyjson_mut_obj_remove_key(m, "received");
                yyjson_mut_obj_remove_key(m, "total");
                yyjson_mut_obj_add_uint(d, m, "received", l->received);
                yyjson_mut_obj_add_uint(d, m, "total", l->total);
                yyjson_mut_obj_add_real(d, m, "bytesPerSecond", l->bytes_per_second);
            }
            bool pull = !strncmp(col(q, 11), "ollama:", 7);
            bool active_pull = pull && l->job == sqlite3_column_int64(q, 0) && !l->pull_done &&
                               (!strcmp(state, "downloading") || !strcmp(state, "preparing"));
            yyjson_mut_obj_add_bool(d, m, "progressKnown", !active_pull || l->pull_expected || l->pull_aggregate);
            const char *meta = col(q, 9);
            yyjson_doc *metadata = yyjson_read(meta, strlen(meta), 0);
            if (metadata) {
                model_label(d, m, col(q, 1), yyjson_doc_get_root(metadata));
                yyjson_mut_obj_add_val(d, m, "metadata",
                                       yyjson_val_mut_copy(d, yyjson_doc_get_root(metadata)));
                if (!sqlite3_column_int(q, 6) && !strncmp(str(yyjson_doc_get_root(metadata), "sourceURL"),
                                                          "https://huggingface.co/", 23)) {
                    yyjson_mut_obj_remove_key(m, "source");
                    yyjson_mut_obj_add_str(d, m, "source", "hf");
                }
            } else
                model_label(d, m, col(q, 1), NULL);
            yyjson_doc_free(metadata);
        }
    sqlite3_finalize(q);
    char *out = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return out;
}
