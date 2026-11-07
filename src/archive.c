#include "store.h"
#include "utf8.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
/* File work uses its own read connection; it does not borrow the live owner's connection. */
static sqlite3 *read_database(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    if (sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024);
    const char *checks[] = {"PRAGMA application_id", "PRAGMA user_version"};
    const int expected[] = {OT_DATABASE_ID, OT_DATABASE_VERSION};
    bool ok = true;
    for (int i = 0; ok && i < 2; i++) {
        sqlite3_stmt *q = NULL;
        ok = sqlite3_prepare_v2(db, checks[i], -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW &&
             sqlite3_column_int(q, 0) == expected[i];
        sqlite3_finalize(q);
    }
    if (!ok || !ot_store_schema_valid(db)) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 1000);
    return db;
}
/* Validate the stored value by its meaning, independent of table scan order. */
static bool valid_text_column(const char *name, const char *value, size_t bytes) {
    if (!name || !ot_utf8_valid(value, bytes))
        return false;
    bool preferences = !strcmp(name, "preferences") || !strcmp(name, "parameters");
    bool array = !strcmp(name, "catalogue") || !strcmp(name, "dismissed");
    if (!preferences && !array && strcmp(name, "metadata") && strcmp(name, "completion") &&
        strcmp(name, "sample"))
        return true;
    yyjson_doc *document = yyjson_read(value, bytes, 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    ot_preferences parsed;
    bool valid = preferences ? ot_preferences_read(&parsed, root)
                 : array     ? yyjson_is_arr(root)
                             : yyjson_is_obj(root);
    yyjson_doc_free(document);
    return valid;
}
bool ot_store_validate(const char *path) {
    sqlite3 *db = read_database(path);
    if (!db)
        return false;
    sqlite3_stmt *q = NULL;
    bool ok = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &q, NULL) == SQLITE_OK &&
              sqlite3_step(q) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(q, 0), "ok");
    sqlite3_finalize(q);
    q = NULL;
    ok = ok && sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &q, NULL) == SQLITE_OK &&
         sqlite3_step(q) == SQLITE_DONE;
    sqlite3_finalize(q);
    q = NULL;
    /* Foreign keys establish existence, not ownership of a conversation branch.
     * Reject coherent SQLite files whose head/edges would hide or mix transcripts. */
    const char *relations =
        "SELECT 1 FROM chats c LEFT JOIN turns h ON h.id=c.head WHERE h.chat<>c.id "
        "OR c.manual_title NOT IN (0,1,2) OR c.archived NOT IN (0,1) OR c.deleted NOT IN (0,1) OR (c.archived=1 AND c.deleted=1) "
        "UNION ALL SELECT 1 FROM turns t LEFT JOIN turns p ON p.id=t.parent "
        "LEFT JOIN turns n ON n.id=t.next LEFT JOIN turns i ON i.id=t.input_id "
        "WHERE t.state NOT IN ('pending','complete','truncated','failed','cancelled','interrupted') "
        "OR t.summary_limited NOT IN (0,1) OR (t.parent IS NOT NULL AND p.chat<>t.chat) "
        "OR (t.next IS NOT NULL AND (n.chat<>t.chat OR n.parent IS NOT t.id)) "
        "OR (t.input_id IS NOT NULL AND (i.chat<>t.chat OR i.parent IS NOT t.parent OR i.prompt<>t.prompt)) "
        "LIMIT 1";
    ok = ok && sqlite3_prepare_v2(db, relations, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_DONE;
    sqlite3_finalize(q);
    q = NULL;
    const char *tables[] = {"chats",
                            "turns",
                            "settings",
                            "models",
                            "downloads",
                            "engines",
                            "generation_parameters",
                            "advice_samples",
                            "advice_state"};
    bool settings_found = false, advice_found = false;
    for (unsigned i = 0; ok && i < sizeof tables / sizeof *tables; i++) {
        char query[80];
        snprintf(query, sizeof query, "SELECT * FROM %s", tables[i]);
        ok = sqlite3_prepare_v2(db, query, -1, &q, NULL) == SQLITE_OK;
        int rc = SQLITE_DONE;
        while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
            if (!strcmp(tables[i], "settings"))
                settings_found = true;
            else if (!strcmp(tables[i], "advice_state"))
                advice_found = true;
            for (int j = 0; ok && j < sqlite3_column_count(q); j++) {
                int type = sqlite3_column_type(q, j);
                ok = type != SQLITE_BLOB;
                if (type != SQLITE_TEXT)
                    continue;
                const char *value = (const char *)sqlite3_column_text(q, j);
                size_t n = (size_t)sqlite3_column_bytes(q, j);
                ok = valid_text_column(sqlite3_column_name(q, j), value, n);
            }
        }
        ok = ok && rc == SQLITE_DONE;
        sqlite3_finalize(q);
        q = NULL;
    }
    ok = ok && settings_found && advice_found;
    sqlite3_close(db);
    return ok;
}
static bool durable(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    bool ok = fsync(fd) == 0;
#ifdef F_FULLFSYNC
    if (ok)
        ok = fcntl(fd, F_FULLFSYNC) == 0;
#endif
    close(fd);
    return ok;
}
static bool sync_parent(const char *path) {
    char parent[4096];
    if (strlen(path) >= sizeof parent)
        return false;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    bool ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    return ok;
}
static bool no_companions(const char *path) {
    const char *suffix[] = {"-wal", "-shm", "-journal"};
    char file[4120];
    for (unsigned i = 0; i < 3; i++) {
        if (snprintf(file, sizeof file, "%s%s", path, suffix[i]) >= (int)sizeof file)
            return false;
        struct stat st;
        if (!lstat(file, &st) || errno != ENOENT)
            return false;
    }
    return true;
}
bool ot_store_backup(const char *source, const char *destination) {
    sqlite3 *input = read_database(source), *output = NULL;
    if (!input)
        return false;
    /* Do not open a new database beside someone else's WAL or journal. SQLite
     * could replay it; deleting it afterwards would also destroy unrelated data. */
    if (!no_companions(destination)) {
        sqlite3_close(input);
        return false;
    }
    /* Keep all SQLite side files inside a private staging directory. Only the
     * verified standalone database becomes visible at the requested destination. */
    char directory[4096], staged[4096];
    if (snprintf(directory, sizeof directory, "%s.XXXXXX", destination) >= (int)sizeof directory ||
        strlen(directory) + sizeof "/snapshot" > sizeof staged || !mkdtemp(directory)) {
        sqlite3_close(input);
        return false;
    }
    snprintf(staged, sizeof staged, "%s/snapshot", directory);
    int fd = open(staged, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    bool ok = fd >= 0;
    if (fd >= 0)
        close(fd);
    ok = ok && sqlite3_open_v2(staged, &output, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
    sqlite3_backup *copy = ok ? sqlite3_backup_init(output, "main", input, "main") : NULL;
    if (copy) {
        int rc = sqlite3_backup_step(copy, -1);
        ok = sqlite3_backup_finish(copy) == SQLITE_OK && rc == SQLITE_DONE;
    } else
        ok = false;
    /* A portable backup must not need a WAL/SHM companion beside the selected file. */
    if (ok)
        ok = sqlite3_exec(output, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(output);
    sqlite3_close(input);
    ok = ok && ot_store_validate(staged) && durable(staged) && no_companions(destination) &&
         !link(staged, destination);
    /* Flushing the file alone does not persist its new directory entry. Keep a
     * complete backup even if the final directory flush reports an I/O error. */
    if (ok)
        ok = sync_parent(destination);
    /* These companions belong to our private directory, never to the destination. */
    const char *suffix[] = {"-wal", "-shm", "-journal"};
    for (unsigned i = 0; i < 3; i++) {
        char file[4120];
        snprintf(file, sizeof file, "%s%s", staged, suffix[i]);
        unlink(file);
    }
    unlink(staged);
    rmdir(directory);
    return ok;
}
bool ot_store_export(const char *source, const char *destination) {
    sqlite3 *db = read_database(source);
    if (!db)
        return false;
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        sqlite3_close(db);
        return false;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(destination);
        sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK;
    const char *queries[] = {"SELECT id,title,coalesce(head,0),archived,deleted FROM chats ORDER BY id",
                             "SELECT "
                             "id,chat,coalesce(parent,0),coalesce(next,0),prompt,answer,state,error,created,"
                             "reasoning,completion,input_id FROM "
                             "turns ORDER BY id"};
    const char *keys[2][12] = {{"id", "title", "head", "archived", "deleted"},
                               {"id", "chat", "parent", "next", "prompt", "answer", "state", "error",
                                "created", "reasoning", "completion", "inputId"}};
    for (int table = 0; ok && table < 2; table++) {
        sqlite3_stmt *q = NULL;
        ok = sqlite3_prepare_v2(db, queries[table], -1, &q, NULL) == SQLITE_OK;
        int rc = SQLITE_DONE;
        while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
            if (!v) {
                yyjson_mut_doc_free(d);
                ok = false;
                break;
            }
            yyjson_mut_doc_set_root(d, v);
            ok = yyjson_mut_obj_add_str(d, v, "type", table ? "turn" : "chat");
            for (int i = 0; ok && i < (table ? 12 : 5); i++) {
                if (sqlite3_column_type(q, i) == SQLITE_INTEGER)
                    ok = yyjson_mut_obj_add_int(d, v, keys[table][i], sqlite3_column_int64(q, i));
                else
                    ok = yyjson_mut_obj_add_strcpy(d, v, keys[table][i],
                                                   (const char *)sqlite3_column_text(q, i));
            }
            char *line = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
            yyjson_mut_doc_free(d);
            ok = line && fputs(line, file) != EOF && fputc('\n', file) != EOF;
            free(line);
        }
        ok = ok && rc == SQLITE_DONE;
        sqlite3_finalize(q);
    }
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    if (fclose(file))
        ok = false;
    if (ok)
        ok = durable(destination) && sync_parent(destination);
    if (!ok)
        unlink(destination);
    return ok;
}

bool ot_store_apply_restore(const char *profile) {
    char live[4096], stage[4096], previous[4096], temporary[4096];
    if (snprintf(live, sizeof live, "%s/workspace.sqlite", profile) >= (int)sizeof live ||
        snprintf(stage, sizeof stage, "%s/restore.sqlite", profile) >= (int)sizeof stage ||
        snprintf(previous, sizeof previous, "%s/previous.sqlite", profile) >= (int)sizeof previous ||
        snprintf(temporary, sizeof temporary, "%s/previous.pending", profile) >= (int)sizeof temporary)
        return false;
    struct stat st;
    if (lstat(stage, &st))
        return errno == ENOENT;
    if (!S_ISREG(st.st_mode) || !ot_store_validate(stage))
        return false;
    if (!lstat(live, &st)) {
        if (!S_ISREG(st.st_mode))
            return false;
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(live, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            return false;
        }
        bool ok = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL) == SQLITE_OK;
        sqlite3_close(db);
        if (!ok)
            return false;
        unlink(temporary);
        if (!ot_store_backup(live, temporary) || rename(temporary, previous))
            return false;
    } else if (errno != ENOENT)
        return false;
    if (rename(stage, live))
        return false;
    int dir = open(profile, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    bool ok = dir >= 0 && fsync(dir) == 0;
    if (dir >= 0)
        close(dir);
    return ok;
}
