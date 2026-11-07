#include "location.h"
#include "store.h"
#include "yyjson.h"
#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static bool failed(ot_location *l, const char *code) {
    snprintf(l->error, sizeof l->error, "%s", code);
    return false;
}
static bool path(char out[4096], const char *root, const char *name) {
    return snprintf(out, 4096, "%s/%s", root, name) < 4096;
}
static bool sync_directory(const char *root) {
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    bool ok = fd >= 0 && !fsync(fd);
    if (fd >= 0)
        close(fd);
    return ok;
}
static bool write_atomic(const char *root, const char *name, const char *bytes, size_t size) {
    char temporary[4096], final[4096];
    if (!path(temporary, root, ".location.XXXXXX") || !path(final, root, name))
        return false;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    bool ok = write(fd, bytes, size) == (ssize_t)size && !fsync(fd);
    if (close(fd))
        ok = false;
    if (ok)
        ok = !rename(temporary, final) && sync_directory(root);
    if (!ok)
        unlink(temporary);
    return ok;
}
static bool marker(const char *root, const char *expected) {
    char name[4096], value[33] = {0};
    if (!path(name, root, "location.id"))
        return false;
    int fd = open(name, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == 32 &&
              st.st_uid == getuid() && read(fd, value, 32) == 32 && !strcmp(value, expected);
    if (fd >= 0)
        close(fd);
    return ok;
}
static bool persist(ot_location *l) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = d ? yyjson_mut_obj(d) : NULL;
    if (!o) {
        yyjson_mut_doc_free(d);
        return failed(l, "storage_write");
    }
    yyjson_mut_doc_set_root(d, o);
    bool complete = yyjson_mut_obj_add_strcpy(d, o, "active", l->active) &&
                    yyjson_mut_obj_add_strcpy(d, o, "pending", l->pending) &&
                    yyjson_mut_obj_add_strcpy(d, o, "marker", l->marker) &&
                    yyjson_mut_obj_add_uint(d, o, "device", l->device) &&
                    yyjson_mut_obj_add_uint(d, o, "inode", l->inode) &&
                    yyjson_mut_obj_add_uint(d, o, "bytes", l->bytes) &&
                    yyjson_mut_obj_add_uint(d, o, "files", l->files);
    size_t size = 0;
    char *json = complete ? yyjson_mut_write(d, 0, &size) : NULL;
    bool ok = json && write_atomic(l->base, "location.json", json, size);
    free(json);
    yyjson_mut_doc_free(d);
    return ok || failed(l, "storage_write");
}
bool ot_location_open(ot_location *l, const char *base) {
    *l = (ot_location){0};
    if (!base || !realpath(base, l->base))
        return failed(l, "invalid_path");
    snprintf(l->active, sizeof l->active, "%s", l->base);
    char filename[4096], buffer[16385];
    if (!path(filename, l->base, "location.json"))
        return failed(l, "invalid_path");
    int fd = open(filename, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        unsigned char random[16];
        arc4random_buf(random, sizeof random);
        for (unsigned i = 0; i < 16; i++)
            snprintf(l->marker + 2 * i, 3, "%02x", random[i]);
        return true;
    }
    struct stat st;
    ssize_t n = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid()
                    ? read(fd, buffer, sizeof buffer)
                    : -1;
    if (fd >= 0)
        close(fd);
    yyjson_doc *d = n > 0 && n < 16385 ? yyjson_read(buffer, (size_t)n, 0) : NULL;
    yyjson_val *o = yyjson_doc_get_root(d);
    const char *keys[] = {"active", "pending", "marker"};
    char *outputs[] = {l->active, l->pending, l->marker};
    size_t capacities[] = {sizeof l->active, sizeof l->pending, sizeof l->marker};
    bool ok = yyjson_is_obj(o);
    for (unsigned i = 0; ok && i < 3; i++) {
        yyjson_val *v = yyjson_obj_get(o, keys[i]);
        const char *s = yyjson_get_str(v);
        size_t len = yyjson_get_len(v);
        ok = s && len < capacities[i] && !memchr(s, 0, len) &&
             (i == 2 ? len == 32 && strspn(s, "0123456789abcdef") == 32 : !len || s[0] == '/');
        if (ok)
            memcpy(outputs[i], s, len + 1);
    }
    l->device = yyjson_get_uint(yyjson_obj_get(o, "device"));
    l->inode = yyjson_get_uint(yyjson_obj_get(o, "inode"));
    l->bytes = yyjson_get_uint(yyjson_obj_get(o, "bytes"));
    l->files = yyjson_get_uint(yyjson_obj_get(o, "files"));
    yyjson_doc_free(d);
    return (ok && *l->active && marker(l->active, l->marker)) || failed(l, "location_invalid");
}
static bool known_model(const char *name) {
    size_t digits = strspn(name, "0123456789");
    return digits && (!strcmp(name + digits, ".gguf") || !strcmp(name + digits, ".part"));
}
typedef bool (*visit)(void *, const char *, const struct stat *, bool);
/* Private pull stores contain resumable blobs and nested manifests. Traverse only
 * these owned directories, never links or external caches; directories bracket files. */
static bool inventory_pull(const char *root, const char *relative, unsigned depth, unsigned *count,
                           visit callback, void *context) {
    char full[4096];
    struct stat st;
    if (depth > 8 || ++*count > 100000 || !path(full, root, relative) || lstat(full, &st) ||
        st.st_uid != getuid() || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
        !callback(context, relative, &st, false))
        return false;
    if (S_ISREG(st.st_mode))
        return true;
    DIR *dir = opendir(full);
    if (!dir)
        return false;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[4096];
        ok = path(child, relative, entry->d_name) &&
             inventory_pull(root, child, depth + 1, count, callback, context);
    }
    closedir(dir);
    return ok && callback(context, relative, &st, true);
}
static bool pull_folder(const char *name) {
    if (strncmp(name, "pull-", 5) || name[5] < '1' || name[5] > '9') return false;
    size_t digits = strspn(name + 5, "0123456789");
    return digits <= 19 && !name[5 + digits] && strtoull(name + 5, NULL, 10) <= INT64_MAX;
}
/* The product owns these files only; external model/cache references are never copied. */
static bool inventory(const char *root, visit callback, void *context) {
    const char *names[] = {"workspace.sqlite", "restore.sqlite", "previous.sqlite"};
    char full[4096];
    struct stat st;
    for (unsigned i = 0; i < 3; i++) {
        if (!path(full, root, names[i]))
            return false;
        if (lstat(full, &st)) {
            if (errno == ENOENT)
                continue;
            return false;
        }
        if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || !callback(context, names[i], &st, false))
            return false;
    }
    if (!path(full, root, "models"))
        return false;
    if (lstat(full, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid())
        return false;
    DIR *dir = opendir(full);
    if (!dir)
        return false;
    bool ok = true;
    struct dirent *entry;
    unsigned count = 0;
    while (ok && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        char relative[512];
        ok = snprintf(relative, sizeof relative, "models/%s", entry->d_name) < (int)sizeof relative &&
             path(full, root, relative) && !lstat(full, &st) && st.st_uid == getuid();
        if (ok && pull_folder(entry->d_name))
            ok = S_ISDIR(st.st_mode) && !(st.st_mode & 0077) &&
                 inventory_pull(root, relative, 0, &count, callback, context);
        else if (ok)
            ok = ++count <= 100000 && known_model(entry->d_name) && S_ISREG(st.st_mode) &&
                 callback(context, relative, &st, false);
    }
    closedir(dir);
    return ok;
}
static bool count_file(void *context, const char *name, const struct stat *st, bool closing) {
    (void)name;
    (void)closing;
    if (S_ISDIR(st->st_mode)) return true;
    ot_location *l = context;
    if (st->st_size < 0 || UINT64_MAX - l->bytes < (uint64_t)st->st_size)
        return false;
    l->bytes += (uint64_t)st->st_size;
    l->files++;
    return true;
}
static bool nested(const char *a, const char *b) {
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (!b[n] || b[n] == '/');
}
static bool empty_directory(const char *root) {
    DIR *dir = opendir(root);
    if (!dir)
        return false;
    bool empty = true;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            empty = false;
            break;
        }
    closedir(dir);
    return empty;
}
bool ot_location_plan(ot_location *l, const char *target) {
    ot_location plan = *l;
    plan.error[0] = 0;
    struct stat st;
    struct statfs fs;
    if (!target || !realpath(target, plan.pending) || strcmp(target, plan.pending) || lstat(target, &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != getuid() || nested(plan.active, plan.pending) ||
        nested(plan.pending, plan.active) || statfs(plan.pending, &fs) || !(fs.f_flags & MNT_LOCAL))
        return failed(l, "location_invalid");
    if (!empty_directory(plan.pending) && !marker(plan.pending, plan.marker))
        return failed(l, "location_not_empty");
    char probe[4096];
    if (!path(probe, plan.pending, ".overtone-write.XXXXXX"))
        return failed(l, "invalid_path");
    int probe_fd = mkstemp(probe);
    if (probe_fd < 0)
        return failed(l, "location_readonly");
    bool writable = !fsync(probe_fd);
    if (close(probe_fd))
        writable = false;
    if (unlink(probe))
        writable = false;
    if (!writable)
        return failed(l, "location_readonly");
    plan.bytes = plan.files = 0;
    if (!inventory(plan.active, count_file, &plan))
        return failed(l, "location_inventory");
    uint64_t space = (uint64_t)fs.f_bavail * fs.f_bsize;
    if (space < plan.bytes || space - plan.bytes < 64 * 1024 * 1024)
        return failed(l, "disk_budget");
    plan.device = (uint64_t)st.st_dev;
    plan.inode = (uint64_t)st.st_ino;
    if (!marker(plan.active, plan.marker) && !write_atomic(plan.active, "location.id", plan.marker, 32))
        return failed(l, "storage_write");
    if (!persist(&plan))
        return failed(l, plan.error);
    *l = plan;
    return true;
}
bool ot_location_cancel(ot_location *l) {
    ot_location plan = *l;
    plan.pending[0] = plan.error[0] = 0;
    plan.bytes = plan.files = plan.device = plan.inode = 0;
    if (!persist(&plan))
        return failed(l, plan.error);
    *l = plan;
    return true;
}
typedef struct {
    ot_location *l;
    char stage[4096];
    const atomic_bool *cancel;
    ot_location_progress progress;
    void *context;
    uint64_t completed;
} transfer;
static bool cancelled(const transfer *t) {
    return t->cancel && atomic_load(t->cancel);
}
static bool hash(const char *filename, unsigned char digest[32], const transfer *t) {
    int fd = open(filename, O_RDONLY | O_NOFOLLOW);
    unsigned char *buffer = malloc(1024 * 1024);
    CC_SHA256_CTX sha;
    CC_SHA256_Init(&sha);
    bool ok = fd >= 0 && buffer;
    while (ok && !cancelled(t)) {
        ssize_t n = read(fd, buffer, 1024 * 1024);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        CC_SHA256_Update(&sha, buffer, (CC_LONG)n);
    }
    free(buffer);
    if (fd >= 0)
        close(fd);
    CC_SHA256_Final(digest, &sha);
    return ok && !cancelled(t);
}
static bool copy_file(void *context, const char *name, const struct stat *original, bool closing) {
    transfer *t = context;
    char source[4096], destination[4096];
    if (cancelled(t) || !path(source, t->l->active, name) || !path(destination, t->stage, name))
        return false;
    if (S_ISDIR(original->st_mode))
        return closing ? sync_directory(destination) : mkdir(destination, 0700) == 0;
    if (t->progress)
        t->progress(t->context, "copying", t->completed, t->l->bytes);
    bool database = !strchr(name, '/') && strstr(name, ".sqlite") != NULL;
    bool cloned = false, ok;
    if (database)
        ok = ot_store_backup(source, destination);
    else {
        copyfile_state_t state = copyfile_state_alloc();
        ok = state && !copyfile(source, destination, state, COPYFILE_DATA | COPYFILE_EXCL | COPYFILE_CLONE);
        if (ok && copyfile_state_get(state, COPYFILE_STATE_WAS_CLONED, &cloned))
            cloned = false;
        if (state)
            copyfile_state_free(state);
    }
    if (ok && !database) {
        unsigned char a[32], b[32];
        struct stat after, copied;
        if (t->progress)
            t->progress(t->context, "verifying", t->completed, t->l->bytes);
        /* An APFS clone shares the original extents atomically; hashing both would
         * reread every weight byte twice. Ordinary copies still verify their data. */
        ok = (cloned || (hash(source, a, t) && hash(destination, b, t) && !memcmp(a, b, 32))) &&
             !cancelled(t) && !lstat(destination, &copied) && S_ISREG(copied.st_mode) &&
             copied.st_size == original->st_size && !lstat(source, &after) &&
             after.st_dev == original->st_dev && after.st_ino == original->st_ino &&
             after.st_size == original->st_size &&
             after.st_mtimespec.tv_sec == original->st_mtimespec.tv_sec &&
             after.st_mtimespec.tv_nsec == original->st_mtimespec.tv_nsec;
    }
    if (ok) {
        int fd = open(destination, O_RDONLY | O_NOFOLLOW);
        ok = fd >= 0 && !fsync(fd);
        if (fd >= 0)
            close(fd);
        t->completed += (uint64_t)original->st_size;
    }
    return ok;
}
static bool remove_file(void *context, const char *name, const struct stat *st, bool closing) {
    if (S_ISDIR(st->st_mode) && !closing) return true;
    char filename[4096];
    return path(filename, context, name) && !(closing ? rmdir(filename) : unlink(filename));
}
static bool commit_file(void *context, const char *name, const struct stat *st, bool closing) {
    transfer *t = context;
    char from[4096], to[4096];
    if (!path(from, t->stage, name) || !path(to, t->l->pending, name)) return false;
    if (S_ISDIR(st->st_mode))
        return closing ? sync_directory(to) && !rmdir(from) : mkdir(to, 0700) == 0;
    return rename(from, to) == 0;
}
static bool rebase(const char *database, const transfer *t) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *q = NULL, *update = NULL, *pending = NULL;
    const char *sql = "SELECT id,reference,device,inode FROM models WHERE kind=1 AND managed>0 "
                      "AND substr(reference,1,length(?1)+8)=?1||'/models/' ORDER BY id";
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(db, "UPDATE models SET reference=?1,device=?2,inode=?3,metadata="
                                 "json_set(metadata,'$.fileModified',?5,'$.fileModifiedNsec',?6) WHERE id=?4",
                                 -1, &update, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(db, "UPDATE downloads SET metadata=json_set(metadata,'$.replacePath',?1,"
                                 "'$.replaceDevice',?2,'$.replaceInode',?3) WHERE "
                                 "json_extract(metadata,'$.replaceId')=?4 AND json_extract(metadata,'$.replacePath')=?5 "
                                 "AND json_extract(metadata,'$.replaceDevice')=?6 AND json_extract(metadata,'$.replaceInode')=?7",
                                 -1, &pending, NULL) == SQLITE_OK;
    if (ok)
        sqlite3_bind_text(q, 1, t->l->active, -1, SQLITE_TRANSIENT);
    int rc = SQLITE_DONE;
    while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        const char *reference = (const char *)sqlite3_column_text(q, 1),
                   *relative = reference + strlen(t->l->active) + 1;
        char actual[4096], target[4096];
        struct stat st;
        ok = !strncmp(relative, "models/", 7) && known_model(relative + 7) &&
             path(actual, t->stage, relative) && path(target, t->l->pending, relative) &&
             !lstat(actual, &st) && S_ISREG(st.st_mode);
        if (ok) {
            sqlite3_bind_text(update, 1, target, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, st.st_dev);
            sqlite3_bind_int64(update, 3, (sqlite3_int64)st.st_ino);
            sqlite3_bind_int64(update, 4, sqlite3_column_int64(q, 0));
            sqlite3_bind_int64(update, 5, st.st_mtimespec.tv_sec);
            sqlite3_bind_int64(update, 6, st.st_mtimespec.tv_nsec);
            sqlite3_bind_text(pending, 1, target, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(pending, 2, st.st_dev);
            sqlite3_bind_int64(pending, 3, (sqlite3_int64)st.st_ino);
            sqlite3_bind_int64(pending, 4, sqlite3_column_int64(q, 0));
            sqlite3_bind_text(pending, 5, reference, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(pending, 6, sqlite3_column_int64(q, 2));
            sqlite3_bind_int64(pending, 7, sqlite3_column_int64(q, 3));
            ok = sqlite3_step(pending) == SQLITE_DONE && sqlite3_step(update) == SQLITE_DONE;
            sqlite3_reset(update);
            sqlite3_reset(pending);
        }
    }
    ok = ok && rc == SQLITE_DONE;
    sqlite3_finalize(q);
    sqlite3_finalize(update);
    sqlite3_finalize(pending);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    else
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    return ok;
}
bool ot_location_apply(ot_location *l, const atomic_bool *cancel, ot_location_progress progress,
                       void *context) {
    if (!*l->pending)
        return true;
    struct stat st;
    if (lstat(l->pending, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (uint64_t)st.st_dev != l->device || (uint64_t)st.st_ino != l->inode ||
        (!empty_directory(l->pending) && !marker(l->pending, l->marker)))
        return failed(l, "location_changed");
    char lockpath[4096];
    if (!path(lockpath, l->pending, "profile.lock"))
        return failed(l, "invalid_path");
    int lock = open(lockpath, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0)
            close(lock);
        return failed(l, "busy");
    }
    transfer t = {.l = l, .cancel = cancel, .progress = progress, .context = context};
    bool ok = !chmod(l->pending, 0700) &&
              (marker(l->pending, l->marker) || write_atomic(l->pending, "location.id", l->marker, 32)) &&
              path(t.stage, l->pending, ".overtone-transfer");
    char modeldir[4096];
    if (ok && !lstat(t.stage, &st)) {
        ok = S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 0077) &&
             inventory(t.stage, remove_file, t.stage);
        if (ok && path(modeldir, t.stage, "models"))
            ok = !rmdir(modeldir) || errno == ENOENT;
        if (ok)
            ok = !rmdir(t.stage);
    }
    if (ok)
        ok = !mkdir(t.stage, 0700) && path(modeldir, t.stage, "models") && !mkdir(modeldir, 0700);
    if (ok) {
        l->bytes = l->files = 0;
        struct statfs fs;
        ok = inventory(l->active, count_file, l) && !statfs(l->pending, &fs);
        uint64_t available = ok ? (uint64_t)fs.f_bavail * fs.f_bsize : 0;
        ok = ok && available >= l->bytes && available - l->bytes >= 64 * 1024 * 1024 &&
             inventory(l->active, copy_file, &t);
    }
    char database[4096];
    if (ok)
        ok = !cancelled(&t) && path(database, t.stage, "workspace.sqlite") && rebase(database, &t) &&
             ot_store_validate(database) && sync_directory(modeldir) && sync_directory(t.stage);
    if (ok && progress)
        progress(context, "registering", l->bytes, l->bytes);
    /* From here cancellation is deferred: retry can always rebuild this target from the source. */
    if (ok) {
        ok = inventory(l->pending, remove_file, l->pending) && path(modeldir, l->pending, "models") &&
             (!mkdir(modeldir, 0700) || errno == EEXIST) && inventory(t.stage, commit_file, &t) &&
             sync_directory(modeldir) && sync_directory(l->pending);
    }
    if (ok) {
        ot_location committed = *l;
        snprintf(committed.active, sizeof committed.active, "%s", l->pending);
        committed.pending[0] = committed.error[0] = 0;
        ok = persist(&committed);
        if (ok)
            *l = committed;
    }
    if (!ok && *t.stage)
        inventory(t.stage, remove_file, t.stage);
    if (path(modeldir, t.stage, "models"))
        rmdir(modeldir);
    rmdir(t.stage);
    close(lock);
    return ok || failed(l, cancelled(&t) ? "cancelled" : "location_transfer");
}
