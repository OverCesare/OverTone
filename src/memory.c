#include "memory.h"
#include "yyjson.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static uint64_t saturated(long double value) {
    return value >= 0x1p64L ? UINT64_MAX : (uint64_t)value;
}
ot_memory_plan ot_memory_approximate(const ot_model_info *m, unsigned context) {
    ot_memory_plan p = {.weights =
                            m->tensor_bytes && m->tensor_bytes <= m->bytes ? m->tensor_bytes : m->bytes};
    unsigned key = m->key_length ? m->key_length : m->heads ? m->embedding / m->heads : 0;
    unsigned value = m->value_length ? m->value_length : key;
    p.known_cache = !m->complex_cache && key && value && m->layers && m->heads;
    long double cache = key && value && m->layers && m->heads
                            ? (long double)context * 2 * m->layers * ((long double)key + value) *
                                  (m->kv_heads ? m->kv_heads : m->heads)
                            : 0;
    p.cache = saturated(cache);
    /* Unknown graph memory is not a fixed percentage of the weights. Keep the metadata
     * projection separate from an actual allocation/planner result. */
    p.total = saturated((long double)p.weights + (long double)p.cache);
    return p;
}
bool ot_memory_project(unsigned context, const ot_runtime_info *r, const ot_runtime_info *previous,
                       uint64_t *bytes) {
    if (!context || !r || !r->context || !r->allocation || !r->weights || !r->cache || !bytes)
        return false;
    /* The runner supplies cache precision/head dimensions and separates full/SWA banks.
     * Project only the context-dependent part; weights and fixed states stay observed.
     * Moving below the observed sliding-window capacity needs a new engine plan. */
    if (r->window_cells && (r->context <= r->window_cells || context <= r->window_cells))
        return false;
    long double projected = (long double)r->allocation +
                            ((long double)context - r->context) * (long double)r->linear_cache / r->context;
    if (previous) {
        if (!previous->context || previous->context == r->context || previous->weights != r->weights ||
            !r->batch || !r->micro_batch || !r->total_layers || r->gpu_layers > r->total_layers ||
            previous->batch != r->batch || previous->micro_batch != r->micro_batch ||
            previous->gpu_layers != r->gpu_layers || previous->total_layers != r->total_layers ||
            previous->window_cells != r->window_cells || previous->cache < previous->linear_cache ||
            r->cache < r->linear_cache || r->cache - r->linear_cache != previous->cache - previous->linear_cache ||
            r->linear_cache > UINT64_MAX / previous->context ||
            previous->linear_cache > UINT64_MAX / r->context ||
            r->linear_cache * previous->context != previous->linear_cache * r->context ||
            !previous->compute || !r->compute)
            return false;
        long double compute =
            (long double)r->compute + ((long double)context - r->context) *
                                          ((long double)r->compute - (long double)previous->compute) /
                                          ((long double)r->context - previous->context);
        if (compute < 0 || compute >= 0x1p64L)
            return false;
        projected += compute - (long double)r->compute;
    }
    if (projected <= 0 || projected >= 0x1p64L)
        return false;
    *bytes = (uint64_t)projected;
    return true;
}
static const char *text(yyjson_val *object, const char *key) {
    yyjson_val *field = yyjson_obj_get(object, key);
    const char *value = yyjson_get_str(field);
    return value && strlen(value) == yyjson_get_len(field) ? value : "";
}
static bool observation_valid(const ot_runtime_info *r) {
    if (r->context < 1024 || r->context > 262144 || !r->allocation || r->linear_cache > r->cache ||
        r->window_cells > r->context)
        return false;
    if (!r->weights && !r->cache && !r->compute)
        return true; /* A service can report the total without component geometry. */
    return r->weights && r->weights <= UINT64_MAX - r->cache &&
           r->weights + r->cache <= UINT64_MAX - r->compute &&
           r->allocation == r->weights + r->cache + r->compute;
}
static const char *const observation_fields[] = {
    "context", "batch", "microBatch", "gpuLayers", "totalLayers", "windowCells",
    "allocation", "weights", "cache", "linearCache", "compute"};
static void observation_values(const ot_runtime_info *r, uint64_t values[11]) {
    const uint64_t v[] = {r->context, r->batch, r->micro_batch, r->gpu_layers, r->total_layers,
                          r->window_cells, r->allocation, r->weights, r->cache, r->linear_cache, r->compute};
    memcpy(values, v, sizeof v);
}
static yyjson_doc *observations_read(const char *saved, const char *key, ot_runtime_info samples[2]) {
    if (!saved || !key || !*key || strlen(saved) > 4096 || strlen(key) > 1024)
        return NULL;
    yyjson_doc *d = yyjson_read(saved, strlen(saved), 0);
    yyjson_val *root = yyjson_doc_get_root(d), *rows = yyjson_obj_get(root, "samples");
    bool ok = yyjson_get_uint(yyjson_obj_get(root, "version")) == 1 && !strcmp(text(root, "key"), key) &&
              yyjson_is_arr(rows) && yyjson_arr_size(rows) > 0 && yyjson_arr_size(rows) <= 2;
    for (size_t i = 0; ok && i < yyjson_arr_size(rows); i++) {
        uint64_t v[11];
        for (size_t j = 0; j < 11; j++) {
            yyjson_val *value = yyjson_obj_get(yyjson_arr_get(rows, i), observation_fields[j]);
            v[j] = yyjson_get_uint(value);
            ok = ok && yyjson_is_uint(value) && (j >= 6 || v[j] <= UINT32_MAX);
        }
        samples[i] = (ot_runtime_info){.context = (unsigned)v[0], .batch = (unsigned)v[1],
            .micro_batch = (unsigned)v[2], .gpu_layers = (unsigned)v[3], .total_layers = (unsigned)v[4],
            .window_cells = (unsigned)v[5], .allocation = v[6], .weights = v[7], .cache = v[8],
            .linear_cache = v[9], .compute = v[10]};
        ok = ok && observation_valid(&samples[i]);
    }
    if (ok)
        return d;
    memset(samples, 0, 2 * sizeof *samples);
    yyjson_doc_free(d);
    return NULL;
}
char *ot_memory_observe(const char *saved, const char *key, const ot_runtime_info *observed) {
    if (!key || !*key || strlen(key) > 1024 || !observed || !observation_valid(observed))
        return NULL;
    ot_runtime_info samples[2] = {0};
    yyjson_doc *old = observations_read(saved, key, samples);
    uint64_t before[11], after[11];
    observation_values(&samples[0], before);
    observation_values(observed, after);
    if (old && !memcmp(before, after, sizeof before)) {
        yyjson_doc_free(old);
        return NULL;
    }
    if (samples[0].context != observed->context)
        samples[1] = samples[0];
    samples[0] = *observed;
    yyjson_doc_free(old);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *rows = yyjson_mut_arr(d);
    bool ok = root && rows && yyjson_mut_obj_add_uint(d, root, "version", 1) &&
              yyjson_mut_obj_add_strcpy(d, root, "key", key) && yyjson_mut_obj_add_val(d, root, "samples", rows);
    for (size_t i = 0; ok && i < 2 && samples[i].context; i++) {
        yyjson_mut_val *row = yyjson_mut_obj(d);
        observation_values(&samples[i], after);
        ok = row && yyjson_mut_arr_append(rows, row);
        for (size_t j = 0; ok && j < 11; j++)
            ok = yyjson_mut_obj_add_uint(d, row, observation_fields[j], after[j]);
    }
    yyjson_mut_doc_set_root(d, root);
    char *json = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    if (json && strlen(json) > 4096) {
        free(json);
        json = NULL;
    }
    return json;
}
bool ot_memory_recall(const char *saved, const char *key, unsigned context, ot_memory_plan *result) {
    if (!result)
        return false;
    ot_runtime_info samples[2] = {0};
    yyjson_doc *d = observations_read(saved, key, samples);
    if (!d)
        return false;
    yyjson_doc_free(d);
    for (unsigned i = 0; i < 2; i++)
        if (context == samples[i].context) {
            *result = (ot_memory_plan){.total = samples[i].allocation, .weights = samples[i].weights,
                .cache = samples[i].cache, .compute = samples[i].compute, .observed = true,
                .known_cache = samples[i].weights != 0};
            return true;
        }
    const ot_runtime_info *a = &samples[0], *b = &samples[1];
    uint64_t projected;
    bool between = (context > a->context && context < b->context) ||
                   (context > b->context && context < a->context);
    if (!between || !ot_memory_project(context, a, b, &projected))
        return false;
    *result = (ot_memory_plan){.total = projected, .observed = true, .known_cache = true};
    return true;
}
bool ot_memory_parse(const char *json, size_t length, ot_memory_plan *result) {
    if (!json || !result || length > 65536)
        return false;
    yyjson_doc *d = yyjson_read(json, length, 0);
    yyjson_val *root = yyjson_doc_get_root(d), *data = yyjson_obj_get(root, "data"),
               *rows = yyjson_obj_get(data, "rows");
    bool ok = !strcmp(text(root, "type"), "fit_memory_breakdown") && !strcmp(text(data, "unit"), "MiB") &&
              yyjson_is_arr(rows) && yyjson_arr_size(rows) > 0 && yyjson_arr_size(rows) <= 64;
    ot_memory_plan p = {.engine_plan = true, .known_cache = true};
    size_t i, n;
    yyjson_val *row;
    yyjson_arr_foreach(rows, i, n, row) {
        if (!ok)
            break;
        const char *kind = text(row, "kind"), *name = text(row, "name");
        ok = (!strcmp(kind, "host") || !strcmp(kind, "device") || !strcmp(kind, "buffer_type")) && *name &&
             strlen(name) <= 128;
        for (size_t j = 0; ok && j < i; j++) {
            yyjson_val *other = yyjson_arr_get(rows, j);
            if (!strcmp(kind, text(other, "kind")) && !strcmp(name, text(other, "name")))
                ok = false;
        }
        const char *keys[] = {"model", "context", "compute"};
        uint64_t *outputs[] = {&p.weights, &p.cache, &p.compute};
        for (unsigned j = 0; ok && j < 3; j++) {
            yyjson_val *v = yyjson_obj_get(row, keys[j]);
            uint64_t size = yyjson_get_uint(v);
            ok = yyjson_is_uint(v) && size <= (UINT64_MAX - *outputs[j]) / 1048576;
            if (ok)
                *outputs[j] += size * 1048576;
        }
    }
    if (ok) {
        ok = p.weights <= UINT64_MAX - p.cache && p.weights + p.cache <= UINT64_MAX - p.compute;
        if (ok)
            p.total = p.weights + p.cache + p.compute;
        ok = ok && p.total > 0;
    }
    yyjson_doc_free(d);
    if (ok)
        *result = p;
    return ok;
}
typedef struct {
    dev_t device;
    ino_t inode;
    off_t size;
    struct timespec modified;
} identity;
static bool identify(const char *path, identity *id) {
    struct stat st;
    if (!*path || lstat(path, &st) || !S_ISREG(st.st_mode))
        return false;
    *id = (identity){st.st_dev, st.st_ino, st.st_size, st.st_mtimespec};
    return true;
}
static bool same(identity a, identity b) {
    return a.device == b.device && a.inode == b.inode && a.size == b.size &&
           a.modified.tv_sec == b.modified.tv_sec && a.modified.tv_nsec == b.modified.tv_nsec;
}
typedef struct {
    uint64_t serial;
    char file[4096], engine[4096];
    ot_preferences preferences;
    ot_resources resources;
    ot_memory_view view;
} request;
typedef struct {
    identity file, engine;
    unsigned context, threads, memory_percent;
    uint64_t memory_total, gpu_recommended;
    bool gpu;
    int layers;
    ot_memory_plan plan;
} cached;
struct ot_memory {
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    atomic_bool cancel;
    bool stopping, queued;
    request latest;
    ot_memory_view view;
    uint64_t revision, delivered;
    ot_memory_probe probe;
};
static void *work(void *pointer) {
    ot_memory *m = pointer;
    cached cache[16] = {0};
    unsigned next = 0;
    pthread_mutex_lock(&m->mutex);
    while (!m->stopping) {
        while (!m->queued && !m->stopping)
            pthread_cond_wait(&m->condition, &m->mutex);
        if (m->stopping)
            break;
        /* Metadata is already projected synchronously. Let a moving slider settle before
         * opening the GGUF or starting an optional engine planner. New input replaces this
         * deadline; there is never one job or subprocess per input event. */
        uint64_t serial = m->latest.serial;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 250000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        int waited = 0;
        while (!m->stopping && m->queued && serial == m->latest.serial && waited != ETIMEDOUT)
            if ((waited = pthread_cond_timedwait(&m->condition, &m->mutex, &deadline)) && waited != ETIMEDOUT)
                break;
        if (m->stopping)
            break;
        if (!m->queued || serial != m->latest.serial)
            continue;
        request r = m->latest;
        m->queued = false;
        atomic_store(&m->cancel, false);
        pthread_mutex_unlock(&m->mutex);
        identity file = {0}, engine = {0};
        bool valid = identify(r.file, &file), can_probe = identify(r.engine, &engine), hit = false;
        unsigned threads = ot_thread_budget(&r.resources, &r.preferences);
        if (valid)
            for (unsigned i = 0; i < 16; i++) {
                cached *c = &cache[i];
                if (c->plan.total && same(c->file, file) && same(c->engine, engine) &&
                    c->context == r.view.context && c->layers == r.view.gpu_layers && c->threads == threads &&
                    c->memory_percent == r.preferences.memory_percent && c->gpu == r.resources.gpu &&
                    c->memory_total == r.resources.memory_total &&
                    c->gpu_recommended == r.resources.gpu_recommended) {
                    r.view.plan = c->plan;
                    hit = true;
                    break;
                }
            }
        if (valid && !hit && !atomic_load(&m->cancel)) {
            ot_model_info actual;
            valid = ot_gguf(r.file, &actual);
            if (valid)
                r.view.plan = ot_memory_approximate(&actual, r.view.context);
            if (valid && can_probe && m->probe)
                m->probe(r.engine, r.file, &r.preferences, &r.resources, &m->cancel, &r.view.plan);
            identity after_file, after_engine;
            valid = valid && identify(r.file, &after_file) && same(file, after_file) &&
                    (!can_probe || (identify(r.engine, &after_engine) && same(engine, after_engine)));
            if (valid && !atomic_load(&m->cancel) && (r.view.plan.engine_plan || !can_probe))
                cache[next++ % 16] =
                    (cached){.file = file, .engine = engine, .context = r.view.context,
                             .threads = threads, .layers = r.view.gpu_layers, .plan = r.view.plan,
                             .memory_percent = r.preferences.memory_percent, .gpu = r.resources.gpu,
                             .memory_total = r.resources.memory_total,
                             .gpu_recommended = r.resources.gpu_recommended};
        }
        pthread_mutex_lock(&m->mutex);
        if (!m->stopping && r.serial == m->latest.serial) {
            if (!valid)
                r.view.plan = (ot_memory_plan){0};
            r.view.pending = false;
            m->view = r.view;
            m->revision++;
        }
    }
    pthread_mutex_unlock(&m->mutex);
    return NULL;
}
ot_memory *ot_memory_open(ot_memory_probe probe) {
    ot_memory *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    if (pthread_mutex_init(&m->mutex, NULL)) {
        free(m);
        return NULL;
    }
    if (pthread_cond_init(&m->condition, NULL)) {
        pthread_mutex_destroy(&m->mutex);
        free(m);
        return NULL;
    }
    atomic_init(&m->cancel, false);
    m->probe = probe;
    if (pthread_create(&m->worker, NULL, work, m)) {
        pthread_cond_destroy(&m->condition);
        pthread_mutex_destroy(&m->mutex);
        free(m);
        return NULL;
    }
    return m;
}
void ot_memory_request(ot_memory *m, int64_t model, const char *file, const char *engine,
                       const ot_model_info *info, const ot_preferences *p, const ot_resources *resources,
                       const ot_memory_plan *observed) {
    if (!m)
        return;
    pthread_mutex_lock(&m->mutex);
    request *r = &m->latest;
    atomic_store(&m->cancel, true);
    r->serial++;
    snprintf(r->file, sizeof r->file, "%s", file ? file : "");
    snprintf(r->engine, sizeof r->engine, "%s", engine ? engine : "");
    r->preferences = *p;
    r->resources = *resources;
    r->view = (ot_memory_view){.model = model, .context = p->context, .gpu_layers = p->gpu_layers,
                               .pending = !observed && *r->file != 0,
                               .plan = observed ? *observed : ot_memory_approximate(info, p->context),
                               .memory_percent = p->memory_percent, .threads = p->threads};
    m->view = r->view;
    m->revision++;
    m->queued = r->view.pending;
    pthread_cond_signal(&m->condition);
    pthread_mutex_unlock(&m->mutex);
}
void ot_memory_cancel(ot_memory *m) {
    if (!m)
        return;
    pthread_mutex_lock(&m->mutex);
    atomic_store(&m->cancel, true);
    m->latest.serial++;
    m->queued = false;
    m->view.pending = false;
    m->revision++;
    pthread_mutex_unlock(&m->mutex);
}
bool ot_memory_poll(ot_memory *m, ot_memory_view *view) {
    if (!m)
        return false;
    pthread_mutex_lock(&m->mutex);
    bool changed = m->revision != m->delivered;
    if (changed) {
        *view = m->view;
        m->delivered = m->revision;
    }
    pthread_mutex_unlock(&m->mutex);
    return changed;
}
void ot_memory_close(ot_memory *m) {
    if (!m)
        return;
    pthread_mutex_lock(&m->mutex);
    m->stopping = true;
    atomic_store(&m->cancel, true);
    pthread_cond_signal(&m->condition);
    pthread_mutex_unlock(&m->mutex);
    pthread_join(m->worker, NULL);
    pthread_cond_destroy(&m->condition);
    pthread_mutex_destroy(&m->mutex);
    free(m);
}
