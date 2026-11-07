#include "pull.h"
#include "yyjson.h"
#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool path(char out[4096], const char *a, const char *b) {
    return snprintf(out, 4096, "%s/%s", a, b) < 4096;
}
static bool directory(const char *name) {
    struct stat st;
    return !lstat(name, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 0022);
}
static bool directories(const char *name) {
    char copy[4096];
    size_t size = strlen(name);
    if (!size || name[0] != '/' || size >= sizeof copy)
        return false;
    memcpy(copy, name, size + 1);
    for (size_t i = 1; i <= size; i++)
        if (copy[i] == '/' || !copy[i]) {
            char saved = copy[i];
            copy[i] = 0;
            struct stat st;
            bool ok = (!mkdir(copy, 0700) || errno == EEXIST) && !lstat(copy, &st) && S_ISDIR(st.st_mode);
            copy[i] = saved;
            if (!ok)
                return false;
        }
    return directory(name);
}
static bool shared_store(char out[4096]) {
    const char *root = getenv("OLLAMA_MODELS"), *home = getenv("HOME");
    if (root && *root)
        return root[0] == '/' && snprintf(out, 4096, "%s", root) < 4096;
    return home && snprintf(out, 4096, "%s/.ollama/models", home) < 4096;
}
bool ot_pull_path(const char *profile, int64_t job, char out[4096]) {
    return job > 0 && snprintf(out, 4096, "%s/models/pull-%lld", profile, (long long)job) < 4096;
}
static bool blob_name(const char *name) {
    if (strlen(name) != 71 || strncmp(name, "sha256-", 7))
        return false;
    for (size_t i = 7; i < 71; i++)
        if (!strchr("0123456789abcdef", name[i]))
            return false;
    return true;
}
bool ot_pull_prepare(const char *profile, int64_t job, const atomic_bool *cancel, char out[4096]) {
    char parent[4096], blobs[4096], shared[4096], source[4096];
    if (atomic_load(cancel) || !path(parent, profile, "models") || !directory(parent) ||
        !ot_pull_path(profile, job, out))
        return false;
    bool fresh = mkdir(out, 0700) == 0;
    if ((!fresh && errno != EEXIST) || !directory(out) || !path(blobs, out, "blobs") || !directories(blobs))
        return false;
    if (!fresh)
        return true;
    /* Hard links reuse immutable cached blobs without copying gigabytes. Across
     * filesystems, leave cache reuse to a later acquisition rather than copy it. */
    if (!shared_store(shared) || !path(source, shared, "blobs"))
        return false;
    DIR *dir = opendir(source);
    if (!dir)
        return errno == ENOENT;
    struct dirent *entry;
    unsigned visited = 0;
    bool ok = true;
    while (!atomic_load(cancel) && visited++ < 16384 && (entry = readdir(dir))) {
        char a[4096], b[4096];
        struct stat st;
        if (!blob_name(entry->d_name))
            continue;
        if (!path(a, source, entry->d_name) || !path(b, blobs, entry->d_name)) {
            ok = false;
            break;
        }
        if (lstat(a, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0022))
            continue;
        if (link(a, b) && errno != EXDEV && errno != EEXIST) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok && !atomic_load(cancel);
}
static char *read_manifest(const char *file, size_t *size) {
    int fd = open(file, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    char *data = NULL;
    if (fd < 0)
        return NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size > 0 &&
        st.st_size <= 8 * 1024 * 1024) {
        *size = (size_t)st.st_size;
        data = malloc(*size + 1);
        size_t offset = 0;
        while (data && offset < *size) {
            ssize_t n = read(fd, data + offset, *size - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                free(data);
                data = NULL;
                break;
            }
            offset += (size_t)n;
        }
        if (data)
            data[*size] = 0;
    }
    close(fd);
    return data;
}
static bool verified_blob(const char *store, yyjson_val *entry, const atomic_bool *cancel) {
    const char *digest = yyjson_get_str(yyjson_obj_get(entry, "digest"));
    yyjson_val *bytes = yyjson_obj_get(entry, "size");
    char file[4096], name[72];
    if (!digest || strlen(digest) != 71 || strncmp(digest, "sha256:", 7) || !yyjson_is_uint(bytes))
        return false;
    snprintf(name, sizeof name, "sha256-%s", digest + 7);
    if (!blob_name(name) || snprintf(file, sizeof file, "%s/blobs/%s", store, name) >= (int)sizeof file)
        return false;
    int fd = open(file, O_RDONLY | O_NOFOLLOW);
    struct stat before, after;
    bool ok = false;
    if (fd < 0)
        return false;
    unsigned char *buffer = malloc(1024 * 1024);
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    bool owned = !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == getuid();
    if (buffer && owned && before.st_size >= 0 && (uint64_t)before.st_size == yyjson_get_uint(bytes)) {
        ssize_t n;
        ok = true;
        while (!atomic_load(cancel) && (n = read(fd, buffer, 1024 * 1024))) {
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                ok = false;
                break;
            }
            CC_SHA256_Update(&hash, buffer, (CC_LONG)n);
        }
        unsigned char raw[32];
        char actual[65];
        CC_SHA256_Final(raw, &hash);
        for (unsigned i = 0; i < 32; i++)
            snprintf(actual + i * 2, 3, "%02x", raw[i]);
        ok = ok && !atomic_load(cancel) && !strcmp(actual, digest + 7) && !fstat(fd, &after) &&
             before.st_size == after.st_size && before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
             before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
    }
    if (!ok && owned && buffer && !atomic_load(cancel)) {
        struct stat named;
        if (!lstat(file, &named) && named.st_dev == before.st_dev && named.st_ino == before.st_ino)
            unlink(file);
    }
    free(buffer);
    close(fd);
    return ok;
}
static bool durable_copy(const char *from, const char *to) {
    struct stat a, b;
    if (lstat(from, &a) || !S_ISREG(a.st_mode) || a.st_uid != getuid())
        return false;
    if (!lstat(to, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        return true;
    char temporary[4096];
    if (snprintf(temporary, sizeof temporary, "%s.XXXXXX", to) >= (int)sizeof temporary)
        return false;
    int reserved = mkstemp(temporary);
    if (reserved < 0)
        return false;
    close(reserved);
    bool ok = unlink(temporary) == 0;
    if (ok && link(from, temporary))
        ok = errno == EXDEV && copyfile(from, temporary, NULL, COPYFILE_DATA | COPYFILE_EXCL) == 0;
    int fd = ok ? open(temporary, O_RDONLY | O_NOFOLLOW) : -1;
    ok = ok && fd >= 0 && !fsync(fd);
    if (fd >= 0)
        close(fd);
    if (ok)
        ok = rename(temporary, to) == 0;
    if (!ok)
        unlink(temporary);
    return ok;
}
static bool manifest(const char *store, const char *shared, const char *relative, const atomic_bool *cancel,
                     bool publish) {
    char source[4096];
    size_t size = 0;
    if (!path(source, store, relative))
        return false;
    char *data = read_manifest(source, &size);
    yyjson_doc *doc = data ? yyjson_read(data, size, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc), *config = yyjson_obj_get(root, "config"),
               *layers = yyjson_obj_get(root, "layers");
    bool ok = doc && yyjson_is_arr(layers) && yyjson_arr_size(layers) <= 16384;
    size_t count = yyjson_arr_size(layers);
    for (size_t i = 0; ok && i <= count; i++) {
        yyjson_val *entry = i ? yyjson_arr_get(layers, i - 1) : config;
        if (!publish)
            ok = verified_blob(store, entry, cancel);
        else {
            const char *hash = yyjson_get_str(yyjson_obj_get(entry, "digest"));
            char from[4096], to[4096];
            char name[72];
            ok = hash && strlen(hash) == 71 && !strncmp(hash, "sha256:", 7) &&
                 snprintf(name, sizeof name, "sha256-%s", hash + 7) == 71 && blob_name(name) &&
                 yyjson_is_uint(yyjson_obj_get(entry, "size")) &&
                 snprintf(from, sizeof from, "%s/blobs/sha256-%s", store, hash + 7) < (int)sizeof from &&
                 snprintf(to, sizeof to, "%s/blobs/sha256-%s", shared, hash + 7) < (int)sizeof to &&
                 durable_copy(from, to);
        }
    }
    if (ok && publish) {
        char destination[4096], parent[4096];
        ok = path(destination, shared, relative);
        if (ok) {
            memcpy(parent, destination, strlen(destination) + 1);
            char *slash = strrchr(parent, '/');
            *slash = 0;
            ok = directories(parent) && durable_copy(source, destination);
            int fd = ok ? open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : -1;
            ok = ok && fd >= 0 && !fsync(fd);
            if (fd >= 0)
                close(fd);
        }
    }
    yyjson_doc_free(doc);
    free(data);
    return ok;
}
static bool manifests(const char *store, const char *shared, const char *relative, unsigned depth,
                      unsigned *count, const atomic_bool *cancel, bool publish) {
    char folder[4096];
    if (!path(folder, store, relative))
        return false;
    DIR *dir = opendir(folder);
    if (!dir)
        return false;
    struct dirent *entry;
    bool ok = true;
    while (ok && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        char child[4096], file[4096];
        struct stat st;
        ok = path(child, relative, entry->d_name) && path(file, store, child) && !lstat(file, &st) &&
             st.st_uid == getuid();
        if (!ok)
            break;
        if (S_ISDIR(st.st_mode))
            ok = depth < 5 && manifests(store, shared, child, depth + 1, count, cancel, publish);
        else
            ok = S_ISREG(st.st_mode) && ++*count <= 64 && manifest(store, shared, child, cancel, publish);
    }
    closedir(dir);
    return ok;
}
bool ot_pull_publish(const char *profile, int64_t job, const atomic_bool *cancel) {
    char store[4096], shared[4096], blobs[4096];
    unsigned count = 0;
    if (!ot_pull_path(profile, job, store) || !directory(store) || !shared_store(shared) ||
        !manifests(store, shared, "manifests", 0, &count, cancel, false) || !count || atomic_load(cancel))
        return false;
    /* Verification is cancellable. Publication is a short atomic-manifest commit:
     * after this boundary success is a completed installation, even if Back races it. */
    if (!path(blobs, shared, "blobs") || !directories(blobs))
        return false;
    count = 0;
    return manifests(store, shared, "manifests", 0, &count, cancel, true);
}
bool ot_pull_discard(const char *profile, int64_t job) {
    char store[4096];
    struct stat st;
    if (!ot_pull_path(profile, job, store))
        return false;
    if (lstat(store, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077))
        return false;
    return removefile(store, NULL, REMOVEFILE_RECURSIVE) == 0;
}
