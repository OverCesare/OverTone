#include "package.h"
#include <CommonCrypto/CommonDigest.h>
#include "engine.h"
#include "process.h"
#include "utf8.h"
#include "yyjson.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static const ot_package ollama = {
    2,
    "Ollama",
    "0.40.13",
    "https://github.com/ollama/ollama/releases/download/v0.40.13/ollama-darwin.tgz",
    "bae29252232b9e1358cf5801a7d65a07442af18fb5467cb68c7fc3711f59c511",
    "ollama",
    186689993,
    1024ULL * 1024 * 1024,
    0};
static const ot_package llama = {
    1,
    "llama.cpp",
    "b11960",
    "https://github.com/ggml-org/llama.cpp/releases/download/b11960/llama-b11960-bin-macos-arm64.tar.gz",
    "58e5a5e1d7c2bf66cedd9c29e0323f3e570660a70a1e2156d32eed08f4cc1e3c",
    "llama-server",
    16465074,
    512ULL * 1024 * 1024,
    1};
const ot_package *ot_package_for_kind(int kind) {
    return kind == ollama.kind ? &ollama : kind == llama.kind ? &llama : NULL;
}
const ot_package *ot_package_for_download(const char *url, const char *checksum) {
    for (int kind = 1; kind <= 2; kind++) {
        const ot_package *p = ot_package_for_kind(kind);
        if (!strcmp(url, p->url) && !strcmp(checksum, p->checksum))
            return p;
    }
    return NULL;
}
static bool folder(const char *path) {
    struct stat st;
    return (!mkdir(path, 0700) || errno == EEXIST) && !lstat(path, &st) && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid() && !(st.st_mode & 0077);
}
static bool installed_directory(const ot_package *p, const char *profile, char out[4096]) {
    return p && snprintf(out, 4096, "%s/engines/%s-%s", profile, p->entry, p->version) < 4096;
}
bool ot_package_find(const ot_package *p, const char *profile, char path[4096]) {
    char root[4096], directory[4096], marker[4096], candidate[4096], hash[65] = {0};
    struct stat st;
    if (!p || snprintf(root, sizeof root, "%s/engines", profile) >= (int)sizeof root || lstat(root, &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077) ||
        !installed_directory(p, profile, directory) || lstat(directory, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0077) ||
        snprintf(marker, sizeof marker, "%s/package.sha256", directory) >= (int)sizeof marker)
        return false;
    int fd = open(marker, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
              st.st_size == 64 && read(fd, hash, 64) == 64 && !strcmp(hash, p->checksum);
    if (fd >= 0)
        close(fd);
    ok = ok && snprintf(candidate, sizeof candidate, "%s/%s", directory, p->entry) < (int)sizeof candidate &&
         !lstat(candidate, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 0100) &&
         ot_process_executable(candidate);
    if (ok)
        memcpy(path, candidate, strlen(candidate) + 1);
    return ok;
}
/* Only small regular license documents from a verified managed package. Never
 * follow links or expose arbitrary engine-directory files to the renderer. */
char *ot_package_notices(const ot_package *p, const char *profile) {
    char executable[4096];
    if (!ot_package_find(p, profile, executable)) return NULL;
    char *slash = strrchr(executable, '/');
    if (!slash) return NULL;
    *slash = 0;
    int root = open(executable, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *directory = root >= 0 ? fdopendir(root) : NULL;
    if (!directory) { if (root >= 0) close(root); return NULL; }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *documents = doc ? yyjson_mut_arr(doc) : NULL;
    if (!documents) { closedir(directory); yyjson_mut_doc_free(doc); return NULL; }
    yyjson_mut_doc_set_root(doc, documents);
    size_t bytes = 0, entries = 0;
    struct dirent *entry;
    while (entries++ < 256 && (entry = readdir(directory)) && yyjson_mut_arr_size(documents) < 32) {
        char name[256];
        size_t length = strlen(entry->d_name);
        if (!length || length >= sizeof name || entry->d_name[0] == '.') continue;
        for (size_t i = 0; i <= length; i++) name[i] = (char)toupper((unsigned char)entry->d_name[i]);
        if (!strstr(name, "LICENSE") && !strstr(name, "NOTICE") && !strstr(name, "COPYING")) continue;
        int fd = openat(dirfd(directory), entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat st;
        bool regular = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                       st.st_size > 0 && st.st_size <= 262144 && (uint64_t)st.st_size <= 1048576 - bytes;
        size_t size = regular ? (size_t)st.st_size : 0, used = 0;
        char *text = size ? malloc(size + 1) : NULL;
        while (text && used < size) {
            ssize_t read_bytes = read(fd, text + used, size - used);
            if (read_bytes < 0 && errno == EINTR) continue;
            if (read_bytes <= 0) break;
            used += (size_t)read_bytes;
        }
        if (fd >= 0) close(fd);
        if (text && used == size && ot_utf8_valid(text, size)) {
            text[size] = 0;
            yyjson_mut_val *document = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, document, "name", entry->d_name);
            yyjson_mut_obj_add_strncpy(doc, document, "text", text, size);
            yyjson_mut_arr_append(documents, document);
            bytes += size;
        }
        free(text);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    closedir(directory);
    return json;
}
static bool relative_path(const char *name) {
    size_t length = strlen(name);
    if (!length || length > 240 || *name == '/' || strstr(name, "..") || strstr(name, "//"))
        return false;
    unsigned depth = 0;
    for (const char *s = name; *s; s++) {
        if (*s == '/') {
            if (++depth > 3)
                return false;
        } else if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
                     *s == '_' || *s == '-' || *s == '.'))
            return false;
    }
    return true;
}
static bool write_all(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        bytes += (size_t)n;
        size -= (size_t)n;
    }
    return true;
}
static bool clean_stages(const char *root, const atomic_bool *cancel) {
    DIR *entries = opendir(root);
    if (!entries)
        return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && !atomic_load(cancel)) {
        errno = 0;
        entry = readdir(entries);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        if (strncmp(entry->d_name, ".install-", 9))
            continue;
        char path[4096];
        struct stat st;
        if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path) {
            ok = false;
            break;
        }
        if (!lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 0077))
            ok = removefile(path, NULL, REMOVEFILE_RECURSIVE) == 0;
    }
    closedir(entries);
    return ok && !atomic_load(cancel);
}
static bool verify_tree(int directory, unsigned depth, unsigned *files, uint64_t *bytes, uint64_t limit,
                        const atomic_bool *cancel) {
    int duplicate = dup(directory);
    if (duplicate < 0)
        return false;
    DIR *entries = fdopendir(duplicate);
    if (!entries) {
        close(duplicate);
        return false;
    }
    bool ok = true;
    struct dirent *entry;
    while (ok && !atomic_load(cancel)) {
        errno = 0;
        entry = readdir(entries);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat st;
        ok = ++*files <= 256 && relative_path(entry->d_name) &&
             !fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) && st.st_uid == getuid();
        if (!ok)
            break;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            ok = depth < 3 && child >= 0 && verify_tree(child, depth + 1, files, bytes, limit, cancel) &&
                 !fsync(child);
            if (child >= 0)
                close(child);
        } else if (S_ISREG(st.st_mode)) {
            ok = st.st_nlink == 1 && st.st_size >= 0 && (uint64_t)st.st_size <= limit - *bytes;
            if (ok) {
                *bytes += (uint64_t)st.st_size;
                int file = openat(directory, entry->d_name, O_RDONLY | O_NOFOLLOW);
                ok = file >= 0 && !fsync(file);
                if (file >= 0)
                    close(file);
            }
        } else if (S_ISLNK(st.st_mode)) {
            char target[256];
            ssize_t n = readlinkat(directory, entry->d_name, target, sizeof target - 1);
            ok = n > 0 && n < (ssize_t)sizeof target - 1;
            if (ok) {
                target[n] = 0;
                ok = relative_path(target) && !strchr(target, '/') && !fstatat(directory, target, &st, 0) &&
                     S_ISREG(st.st_mode);
            }
        } else
            ok = false;
    }
    closedir(entries);
    return ok && !atomic_load(cancel);
}
static bool verify(int fd, const ot_package *p, const atomic_bool *cancel) {
    struct stat before, after;
    unsigned char buffer[262144], digest[CC_SHA256_DIGEST_LENGTH];
    char hash[65];
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size != p->bytes)
        return false;
    CC_SHA256_CTX sha;
    CC_SHA256_Init(&sha);
    ssize_t n;
    while (!atomic_load(cancel) && (n = read(fd, buffer, sizeof buffer)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        CC_SHA256_Update(&sha, buffer, (CC_LONG)n);
    }
    CC_SHA256_Final(digest, &sha);
    for (unsigned i = 0; i < sizeof digest; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    return !atomic_load(cancel) && !strcmp(hash, p->checksum) && !fstat(fd, &after) &&
           before.st_ino == after.st_ino && before.st_size == after.st_size &&
           before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec && lseek(fd, 0, SEEK_SET) == 0;
}
bool ot_package_install(const ot_package *p, const char *profile, const char *source,
                        const atomic_bool *cancel, char path[4096]) {
    if (!p || atomic_load(cancel))
        return false;
    if (ot_package_find(p, profile, path))
        return true;
    struct statvfs space;
    if (statvfs(profile, &space) || (uint64_t)space.f_bavail * space.f_frsize < p->unpacked_limit)
        return false;
    char root[4096], stage[4096], final[4096];
    if (snprintf(root, sizeof root, "%s/engines", profile) >= (int)sizeof root || !folder(root) ||
        !installed_directory(p, profile, final) ||
        snprintf(stage, sizeof stage, "%s/.install-XXXXXX", root) >= (int)sizeof stage ||
        !clean_stages(root, cancel))
        return false;
    int fd = open(source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    bool ok = verify(fd, p, cancel) && mkdtemp(stage) != NULL;
    bool staged = ok;
    int output = ok ? open(stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : -1;
    /* macOS ships bsdtar/libarchive as a system tool. Reuse its parser and secure
     * extraction defaults instead of vendoring an archive library or private ABI. */
    char tar[4096];
    ok = ok && realpath("/usr/bin/tar", tar) != NULL;
    char *list_args[] = {tar, "-tf", "-", NULL};
    char *listing = ok ? ot_process_capture(tar, list_args, 30, cancel, true, fd) : NULL;
    unsigned count = 0;
    ok = ok && listing;
    if (listing) {
        char *save = NULL;
        for (char *name = strtok_r(listing, "\n", &save); name; name = strtok_r(NULL, "\n", &save))
            if (!relative_path(name) || ++count > 256) {
                ok = false;
                break;
            }
        ok = ok && count > 0;
    }
    free(listing);
    ok = ok && lseek(fd, 0, SEEK_SET) == 0;
    char *extract_args[] = {tar,
                            "-xf",
                            "-",
                            "-C",
                            stage,
                            "--no-same-owner",
                            "--no-same-permissions",
                            "--no-acls",
                            "--no-xattrs",
                            "--no-fflags",
                            "--safe-writes",
                            "--exclude=*/._*",
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL};
    if (p->strip_components) {
        /* The official llama distribution has one root directory. Keep only runtime,
         * planner, their shared libraries and notices, never the unrelated CLI tools. */
        size_t i = 0;
        while (extract_args[i])
            i++;
        extract_args[i++] = "--strip-components=1";
        extract_args[i++] = "--include=*/llama-server";
        extract_args[i++] = "--include=*/llama-fit-params";
        extract_args[i++] = "--include=*/*.dylib";
        extract_args[i++] = "--include=*/LICENSE*";
        extract_args[i] = NULL;
    }
    char *result = ok ? ot_process_capture(tar, extract_args, 120, cancel, true, fd) : NULL;
    ok = ok && result;
    free(result);
    close(fd);
    unsigned files = 0;
    uint64_t bytes = 0;
    ok = ok && verify_tree(output, 0, &files, &bytes, p->unpacked_limit, cancel);
    char executable[4096];
    ok = ok && snprintf(executable, sizeof executable, "%s/%s", stage, p->entry) < (int)sizeof executable &&
         ot_engine_validate(executable, p->kind == 2) && !atomic_load(cancel);
    if (ok) {
        int marker = openat(output, "package.sha256", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        ok = marker >= 0 && write_all(marker, p->checksum, 64) && fsync(marker) == 0;
        if (marker >= 0)
            close(marker);
        ok = ok && fsync(output) == 0;
    }
    if (output >= 0)
        close(output);
    ok = ok && !atomic_load(cancel);
    if (ok) {
        ok = renameatx_np(AT_FDCWD, stage, AT_FDCWD, final, RENAME_EXCL) == 0;
        if (ok) {
            staged = false;
            int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            ok = dir >= 0 && fsync(dir) == 0;
            if (dir >= 0)
                close(dir);
            ok = ok && ot_package_find(p, profile, path);
        } else if (errno == EEXIST) {
            /* Repair only our private versioned directory, never an external engine.
             * The exchange is atomic: a crash leaves a complete installation in final. */
            struct stat st;
            if (ot_package_find(p, profile, path))
                ok = true;
            else if (!lstat(final, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
                     !(st.st_mode & 0077) && !renameatx_np(AT_FDCWD, stage, AT_FDCWD, final, RENAME_SWAP)) {
                int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                ok = dir >= 0 && !fsync(dir) && ot_package_find(p, profile, path);
                if (dir >= 0)
                    close(dir);
            }
        }
    }
    if (staged)
        removefile(stage, NULL, REMOVEFILE_RECURSIVE);
    return ok;
}
