#include "inventory.h"
#include "yyjson.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    ot_inventory result;
    const atomic_bool *cancel;
    unsigned visited;
    double deadline;
} scan;
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static const char *string(yyjson_val *v, const char *key) {
    yyjson_val *field = yyjson_obj_get(v, key);
    const char *s = yyjson_get_str(field);
    return s && strlen(s) == yyjson_get_len(field) ? s : "";
}
static bool under(const char *path, const char *root) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && path[n] == '/';
}
static bool digest(const char *s) {
    if (strlen(s) != 71 || strncmp(s, "sha256:", 7))
        return false;
    for (unsigned i = 7; i < 71; i++)
        if (!strchr("0123456789abcdef", s[i]))
            return false;
    return true;
}
static bool blob(const char *root, yyjson_val *entry, uint64_t *bytes) {
    const char *hash = string(entry, "digest");
    if (!digest(hash))
        return false;
    char path[4096];
    if (snprintf(path, sizeof path, "%s/blobs/sha256-%s", root, hash + 7) >= (int)sizeof path)
        return false;
    struct stat st;
    uint64_t expected = yyjson_get_uint(yyjson_obj_get(entry, "size"));
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != expected ||
        expected > UINT64_MAX - *bytes)
        return false;
    *bytes += expected;
    return true;
}
static bool ollama(const char *root, const char *path, ot_inventory_item *item) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    char *data = NULL;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 1024 * 1024;
    if (ok)
        data = malloc((size_t)st.st_size);
    size_t size = 0;
    while (data && size < (size_t)st.st_size) {
        ssize_t n = read(fd, data + size, (size_t)st.st_size - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    yyjson_doc *d = data && size == (size_t)st.st_size ? yyjson_read(data, size, 0) : NULL;
    if (d) {
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(data, (CC_LONG)size, hash);
        for (unsigned i = 0; i < sizeof hash; i++)
            snprintf(item->manifest_digest + i * 2, 3, "%02x", hash[i]);
    }
    free(data);
    if (!d)
        return false;
    yyjson_val *v = yyjson_doc_get_root(d), *layers = yyjson_obj_get(v, "layers");
    ok = yyjson_is_arr(layers) && yyjson_arr_size(layers) <= 4096 &&
         blob(root, yyjson_obj_get(v, "config"), &item->info.bytes);
    bool model = false;
    size_t i, n;
    yyjson_val *layer;
    yyjson_arr_foreach(layers, i, n, layer) {
        if (!ok)
            break;
        ok = blob(root, layer, &item->info.bytes);
        if (!strcmp(string(layer, "mediaType"), "application/vnd.ollama.image.model") ||
            !strcmp(string(layer, "mediaType"), "application/vnd.ollama.image.tensor"))
            model = true;
    }
    yyjson_doc_free(d);
    if (!ok || !model)
        return false;
    const char *relative = path + strlen(root) + strlen("/manifests/");
    const char *slash = strchr(relative, '/');
    if (!slash)
        return false;
    bool official = (size_t)(slash - relative) == strlen("registry.ollama.ai") &&
                    !strncmp(relative, "registry.ollama.ai", strlen("registry.ollama.ai"));
    const char *name = official ? slash + 1 : relative;
    if (official && !strncmp(name, "library/", 8))
        name += 8;
    if (strlen(name) > 256)
        return false;
    strcpy(item->reference, name);
    char *tag = strrchr(item->reference, '/');
    if (!tag)
        return false;
    *tag = ':';
    strcpy(item->name, item->reference);
    item->kind = 2;
    snprintf(item->manifest, sizeof item->manifest, "%s", path);
    item->manifest_root_length = strlen(root);
    return true;
}
bool ot_inventory_manifest(const char *path, size_t root_length) {
    size_t length = strlen(path);
    if (!root_length || root_length >= 4096 || root_length > length ||
        strncmp(path + root_length, "/manifests/", 11))
        return false;
    char root_path[4096];
    memcpy(root_path, path, root_length);
    root_path[root_length] = 0;
    ot_inventory_item item = {0};
    return ollama(root_path, path, &item);
}
static void walk(scan *s, const char *root, const char *directory, unsigned depth, bool manifests) {
    if (depth > 8)
        return;
    DIR *dir = opendir(directory);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        if (atomic_load(s->cancel) || ++s->visited > 10000 || s->result.count == 256 || now() > s->deadline) {
            s->result.limited = true;
            break;
        }
        char path[4096], canonical[4096];
        if (snprintf(path, sizeof path, "%s/%s", directory, entry->d_name) >= (int)sizeof path)
            continue;
        struct stat st;
        if (lstat(path, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk(s, root, path, depth + 1, manifests);
            if (s->result.limited)
                break;
            continue;
        }
        if (!realpath(path, canonical) || !under(canonical, root))
            continue;
        ot_inventory_item item = {0};
        if (manifests) {
            if (!S_ISREG(st.st_mode) || !ollama(root, canonical, &item))
                continue;
        } else {
            size_t n = strlen(entry->d_name);
            if (n < 5 || strcmp(entry->d_name + n - 5, ".gguf") || !ot_gguf(canonical, &item.info))
                continue;
            item.kind = 1;
            snprintf(item.reference, sizeof item.reference, "%s", canonical);
            snprintf(item.name, sizeof item.name, "%s", *item.info.name ? item.info.name : entry->d_name);
        }
        bool duplicate = false;
        for (size_t i = 0; i < s->result.count; i++)
            if (s->result.items[i].kind == item.kind &&
                !strcmp(s->result.items[i].reference, item.reference)) {
                duplicate = true;
                break;
            }
        if (!duplicate)
            s->result.items[s->result.count++] = item;
    }
    closedir(dir);
}
static void root(scan *s, const char *path, bool manifests) {
    if (!path || !*path || s->result.limited)
        return;
    char canonical[4096], start[4096];
    if (!realpath(path, canonical))
        return;
    if (snprintf(start, sizeof start, "%s%s", canonical, manifests ? "/manifests" : "") >= (int)sizeof start)
        return;
    walk(s, canonical, start, 0, manifests);
}
ot_inventory ot_inventory_read(const atomic_bool *cancel) {
    scan s = {.cancel = cancel, .deadline = now() + 5};
    s.result.items = calloc(256, sizeof *s.result.items);
    if (!s.result.items)
        return s.result;
    const char *home = getenv("HOME");
    if (!home)
        return s.result;
    char path[4096];
    const char *override = getenv("OLLAMA_MODELS");
    if (override && *override)
        root(&s, override, true);
    else {
        snprintf(path, sizeof path, "%s/.ollama/models", home);
        root(&s, path, true);
    }
    override = getenv("HF_HUB_CACHE");
    if (!override || !*override)
        override = getenv("HUGGINGFACE_HUB_CACHE");
    if (override && *override)
        root(&s, override, false);
    else {
        const char *hf = getenv("HF_HOME"), *cache = getenv("XDG_CACHE_HOME");
        if (hf && *hf)
            snprintf(path, sizeof path, "%s/hub", hf);
        else if (cache && *cache)
            snprintf(path, sizeof path, "%s/huggingface/hub", cache);
        else
            snprintf(path, sizeof path, "%s/.cache/huggingface/hub", home);
        root(&s, path, false);
    }
    return s.result;
}
