#include "store.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define ANCESTORS                                                                                            \
    "WITH RECURSIVE path AS (SELECT * FROM turns WHERE id=?1 AND chat=?2 UNION ALL SELECT t.* FROM turns t " \
    "JOIN path p ON t.id=p.parent AND t.chat=p.chat AND t.id<p.id) "
struct ot_store {
    sqlite3 *db;
    yyjson_doc *browser;
    sqlite3_int64 browser_version;
    bool browser_dirty;
    int64_t browser_before;
    int browser_filter;
    char browser_query[241];
    ot_browse_options browser_options;
};
/* History mutations use rowid tables and bounded WHERE deletes (no truncate
 * shortcut). Rollback also invalidates projections read before the rollback. */
static void browser_changed(void *context, int operation, const char *database,
                            const char *table, sqlite3_int64 row) {
    (void)operation;
    (void)row;
    if (!strcmp(database, "main") && (!strcmp(table, "chats") || !strcmp(table, "turns")))
        ((ot_store *)context)->browser_dirty = true;
}
static void browser_rollback(void *context) {
    ((ot_store *)context)->browser_dirty = true;
}
sqlite3 *ot_store_connection(ot_store *s) {
    return s->db;
}
static bool sql(ot_store *s, const char *text) {
    return sqlite3_exec(s->db, text, NULL, NULL, NULL) == SQLITE_OK;
}
static sqlite3_stmt *prepare(ot_store *s, const char *text) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(s->db, text, -1, &q, NULL) != SQLITE_OK)
        return NULL;
    return q;
}
static const char *column(sqlite3_stmt *q, int n) {
    const unsigned char *p = sqlite3_column_text(q, n);
    return p ? (const char *)p : "";
}
static int64_t scalar(ot_store *s, const char *text) {
    sqlite3_stmt *q = prepare(s, text);
    int64_t value = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    return value;
}
static bool done(sqlite3_stmt *q) {
    bool ok = q && sqlite3_step(q) == SQLITE_DONE;
    sqlite3_finalize(q);
    return ok;
}
static bool commit(ot_store *s, bool ok) {
    if (ok && sql(s, "COMMIT"))
        return true;
    sql(s, "ROLLBACK");
    return false;
}
static bool text(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const char *value) {
    return yyjson_mut_obj_add_strcpy(d, o, key, value);
}

ot_store *ot_store_open(const char *path) {
    ot_store *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    if (sqlite3_open_v2(path, &s->db, flags, NULL) != SQLITE_OK)
        goto fail;
    if (sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024);
    sqlite3_busy_timeout(s->db, 1000);
    int64_t id = scalar(s, "PRAGMA application_id"), version = scalar(s, "PRAGMA user_version");
    bool empty = id == 0 && version == 0 && scalar(s, "SELECT count(*) FROM sqlite_master") == 0;
    if (!empty && (id != OT_DATABASE_ID || version != OT_DATABASE_VERSION))
        goto fail;
    if (!sql(s,
             "PRAGMA foreign_keys=ON;PRAGMA synchronous=FULL;PRAGMA fullfsync=ON;PRAGMA cache_size=-2048;"))
        goto fail;
    if (empty) {
        if (!sql(s, "BEGIN IMMEDIATE"))
            goto fail;
        bool ok = sql(
            s,
            "CREATE TABLE chats(id INTEGER PRIMARY KEY,title TEXT NOT NULL,"
            "head INTEGER REFERENCES turns(id),manual_title INTEGER NOT NULL DEFAULT 0,archived INTEGER NOT "
            "NULL DEFAULT 0,deleted INTEGER NOT NULL DEFAULT 0,draft TEXT NOT NULL DEFAULT '',"
            "thinking_mode TEXT NOT NULL DEFAULT '') STRICT;"
            "CREATE TABLE turns(id INTEGER PRIMARY KEY,chat INTEGER NOT NULL REFERENCES chats(id),"
            "prompt TEXT NOT NULL,answer TEXT NOT NULL DEFAULT '',state TEXT NOT NULL,"
            "error TEXT NOT NULL DEFAULT '',created INTEGER NOT NULL DEFAULT(unixepoch()),"
            "parent INTEGER REFERENCES turns(id) CHECK(parent IS NULL OR parent<id),"
            "input_id INTEGER REFERENCES turns(id) CHECK(input_id IS NULL OR input_id<=id),"
            "next INTEGER REFERENCES turns(id) CHECK(next IS NULL OR next>id),reasoning TEXT NOT NULL "
            "DEFAULT '',model TEXT NOT NULL DEFAULT '',runtime TEXT NOT NULL DEFAULT '',summary TEXT NOT "
            "NULL DEFAULT '',summary_limited INTEGER NOT NULL DEFAULT 0,completion TEXT NOT NULL DEFAULT "
            "'{}') STRICT;"
            "CREATE INDEX turns_chat ON turns(chat,id);"
            "CREATE INDEX turns_parent ON turns(chat,parent,id);"
            "CREATE INDEX turns_input ON turns(chat,input_id,id);"
            /* History needs the latest time per chat and provenance facets without
             * reading every transcript body. These cover those actual queries. */
            "CREATE TABLE advice_samples(turn INTEGER PRIMARY KEY REFERENCES turns(id) ON DELETE CASCADE,"
            "sample TEXT NOT NULL CHECK(json_valid(sample)),recorded INTEGER NOT NULL DEFAULT(unixepoch())) STRICT;"
            "CREATE TABLE advice_state(id INTEGER PRIMARY KEY CHECK(id=1),catalogue TEXT NOT NULL "
            "DEFAULT '[]' CHECK(json_valid(catalogue)),updated INTEGER NOT NULL DEFAULT 0,"
            "checked INTEGER NOT NULL DEFAULT 0,dismissed TEXT NOT NULL DEFAULT '[]' CHECK(json_valid(dismissed))) STRICT;"
            "INSERT INTO advice_state(id) VALUES(1);"
            "CREATE INDEX turns_created ON turns(chat,created);"
            "CREATE INDEX turns_model ON turns(model,chat) WHERE model<>'';"
            "CREATE INDEX turns_runtime ON turns(runtime,chat) WHERE runtime<>'';");
        ok =
            ok &&
            sql(s,
                "CREATE TABLE settings(id INTEGER PRIMARY KEY CHECK(id=1),endpoint TEXT NOT NULL,model TEXT "
                "NOT "
                "NULL,"
                "ollama INTEGER NOT NULL CHECK(ollama IN(0,1)),remote INTEGER NOT NULL CHECK(remote IN(0,1)),"
                "chat INTEGER REFERENCES chats(id),preferences TEXT NOT NULL DEFAULT '{}',draft TEXT NOT "
                "NULL "
                "DEFAULT '',model_id INTEGER REFERENCES models(id)) STRICT;"
                /* External-content FTS indexes words, not a second copy of the transcript. */
                "CREATE VIEW search_content AS SELECT -id AS id,title,'' AS prompt,'' AS answer,id AS chat,0 "
                "AS "
                "turn FROM chats "
                "UNION ALL SELECT id,'',prompt,answer,chat,id FROM turns;"
                "CREATE VIRTUAL TABLE search USING fts5(title,prompt,answer,chat UNINDEXED,turn UNINDEXED,"
                "content='search_content',content_rowid='id',tokenize='unicode61 remove_diacritics 2');"
                "CREATE TRIGGER chat_insert AFTER INSERT ON chats BEGIN INSERT INTO "
                "search(rowid,title,chat,turn) VALUES(-new.id,new.title,new.id,0);END;"
                "CREATE TRIGGER chat_title AFTER UPDATE OF title ON chats BEGIN "
                "INSERT INTO search(search,rowid,title,chat,turn) "
                "VALUES('delete',-old.id,old.title,old.id,0);"
                "INSERT INTO search(rowid,title,chat,turn) VALUES(-new.id,new.title,new.id,0);END;"
                "CREATE TRIGGER turn_insert AFTER INSERT ON turns BEGIN INSERT INTO "
                "search(rowid,prompt,answer,chat,turn) "
                "VALUES(new.id,new.prompt,new.answer,new.chat,new.id);END;"
                "CREATE TRIGGER turn_answer AFTER UPDATE OF answer ON turns BEGIN "
                "INSERT INTO search(search,rowid,prompt,answer,chat,turn) "
                "VALUES('delete',old.id,old.prompt,old.answer,old.chat,old.id);"
                "INSERT INTO search(rowid,prompt,answer,chat,turn) "
                "VALUES(new.id,new.prompt,new.answer,new.chat,new.id);END;"
                "CREATE TABLE models(id INTEGER PRIMARY KEY,name TEXT NOT NULL,kind INTEGER NOT "
                "NULL,reference "
                "TEXT NOT NULL,"
                "bytes INTEGER NOT NULL DEFAULT 0,context INTEGER NOT NULL DEFAULT 0,embedding INTEGER NOT "
                "NULL "
                "DEFAULT 0,"
                "layers INTEGER NOT NULL DEFAULT 0,heads INTEGER NOT NULL DEFAULT 0,kv_heads INTEGER NOT "
                "NULL "
                "DEFAULT 0,"
                "managed INTEGER NOT NULL DEFAULT 0,device INTEGER NOT NULL DEFAULT 0,inode INTEGER NOT NULL "
                "DEFAULT 0,metadata TEXT NOT NULL DEFAULT '{}',hidden INTEGER NOT NULL DEFAULT "
                "0,origin TEXT NOT NULL DEFAULT 'cache',available INTEGER NOT NULL DEFAULT "
                "1,UNIQUE(kind,reference)) STRICT;"
                "CREATE INDEX models_identity ON models(device,inode);"
                "CREATE INDEX models_bytes ON models(bytes);"
                "CREATE TABLE downloads(id INTEGER PRIMARY KEY AUTOINCREMENT,url TEXT NOT NULL,name TEXT NOT "
                "NULL,checksum TEXT NOT NULL,"
                "total INTEGER NOT NULL DEFAULT 0,received INTEGER NOT NULL DEFAULT 0,etag TEXT NOT NULL "
                "DEFAULT "
                "'',state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',metadata TEXT NOT NULL DEFAULT '{}',"
                "created INTEGER NOT NULL DEFAULT(unixepoch()),updated INTEGER NOT NULL "
                "DEFAULT(unixepoch())) "
                "STRICT;"
                "CREATE INDEX downloads_queue ON downloads(state,updated,id);"
                "CREATE TABLE generation_parameters(identity TEXT PRIMARY KEY,parameters TEXT NOT NULL) "
                "STRICT;"
                "CREATE TABLE engines(kind INTEGER PRIMARY KEY,path TEXT NOT NULL) STRICT;"
                "INSERT INTO settings VALUES(1,'http://127.0.0.1:8080','',0,0,NULL,'{}','',NULL);"
                "PRAGMA application_id=1330927443;");
        char version_sql[40];
        snprintf(version_sql, sizeof version_sql, "PRAGMA user_version=%d", OT_DATABASE_VERSION);
        ok = ok && sql(s, version_sql);
        if (!commit(s, ok))
            goto fail;
    } else if (!ot_store_schema_valid(s->db))
        goto fail;
    if (!sql(s, "PRAGMA journal_mode=WAL;UPDATE turns SET state='interrupted',error='interrupted' WHERE "
                "state='pending';"))
        goto fail;
    sqlite3_update_hook(s->db, browser_changed, s);
    sqlite3_rollback_hook(s->db, browser_rollback, s);
    return s;
fail:
    ot_store_close(s);
    return NULL;
}
bool ot_store_schema_valid(sqlite3 *database) {
    // Generate the expected schema from its single definition, using this SQLite.
    // A matching application/version marker alone must not admit altered triggers/views.
    ot_store *reference = ot_store_open(":memory:");
    if (!reference)
        return false;
    const char *query =
        "SELECT type,name,coalesce(sql,'') FROM sqlite_master "
        "WHERE NOT(type='table' AND name IN('sqlite_stat1','sqlite_stat4')) ORDER BY type,name";
    sqlite3_stmt *actual = NULL, *expected = prepare(reference, query);
    bool ok = expected && sqlite3_prepare_v2(database, query, -1, &actual, NULL) == SQLITE_OK;
    while (ok) {
        int a = sqlite3_step(actual), b = sqlite3_step(expected);
        if (a != SQLITE_ROW || b != SQLITE_ROW) {
            ok = a == SQLITE_DONE && b == SQLITE_DONE;
            break;
        }
        for (int i = 0; ok && i < 3; i++)
            ok = !strcmp(column(actual, i), column(expected, i));
    }
    sqlite3_finalize(actual);
    sqlite3_finalize(expected);
    ot_store_close(reference);
    return ok;
}
void ot_store_close(ot_store *s) {
    if (!s)
        return;
    if (s->db)
        sqlite3_close(s->db);
    yyjson_doc_free(s->browser);
    free(s);
}
bool ot_store_chat_thinking(ot_store *s, int64_t chat, char mode[OT_THINKING_NAME]) {
    memset(mode, 0, OT_THINKING_NAME);
    if (!chat)
        return true;
    sqlite3_stmt *q = prepare(s, "SELECT thinking_mode FROM chats WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, chat);
    bool ok = q && sqlite3_step(q) == SQLITE_ROW && strlen(column(q, 0)) < OT_THINKING_NAME;
    if (ok)
        strcpy(mode, column(q, 0));
    sqlite3_finalize(q);
    return ok;
}
bool ot_store_set_chat_thinking(ot_store *s, int64_t chat, const char *mode) {
    sqlite3_stmt *q = prepare(s, "UPDATE chats SET thinking_mode=? WHERE id=?");
    if (q) {
        sqlite3_bind_text(q, 1, mode, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 2, chat);
    }
    return done(q) && sqlite3_changes(s->db) == 1;
}
bool ot_store_settings(ot_store *s, ot_connection *v, int64_t *selected, int64_t *model) {
    sqlite3_stmt *q =
        prepare(s, "SELECT endpoint,model,ollama,remote,coalesce(chat,0),coalesce(model_id,0) FROM settings WHERE id=1");
    bool ok = q && sqlite3_step(q) == SQLITE_ROW;
    if (ok) {
        ok = strlen(column(q, 0)) < sizeof v->endpoint && strlen(column(q, 1)) < sizeof v->model;
        if (ok) {
            strcpy(v->endpoint, column(q, 0));
            strcpy(v->model, column(q, 1));
            v->ollama = sqlite3_column_int(q, 2) != 0;
            v->remote = sqlite3_column_int(q, 3) != 0;
            *selected = sqlite3_column_int64(q, 4);
            if (model) *model=sqlite3_column_int64(q,5);
        }
    }
    sqlite3_finalize(q);
    return ok;
}
static bool write_preferences(ot_store *s, const ot_preferences *p);
bool ot_store_configure(ot_store *s, const ot_connection *v, int64_t model_id, ot_preferences *preferences) {
    ot_connection previous;
    int64_t selected = 0;
    if (!ot_store_settings(s, &previous, &selected,NULL) || !sql(s, "BEGIN IMMEDIATE"))
        return false;
    sqlite3_stmt *q = prepare(
        s, "UPDATE settings SET endpoint=?,model=?,ollama=?,remote=?,model_id=nullif(?,0) WHERE id=1");
    if (q) {
        sqlite3_bind_text(q, 1, v->endpoint, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, v->model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(q, 3, v->ollama);
        sqlite3_bind_int(q, 4, v->remote);
        sqlite3_bind_int64(q, 5, model_id);
    }
    bool ok = done(q);
    ot_preferences next = *preferences;
    if (ok && !model_id &&
        (strcmp(previous.model, v->model) || strcmp(previous.endpoint, v->endpoint) ||
         previous.ollama != v->ollama)) {
        ot_preferences generation = ot_preferences_default();
        int stored = ot_store_generation(s, 0, &generation);
        ok = stored >= 0;
        if (ok) {
            ot_preferences_model_values(&next, &generation);
            ok = write_preferences(s, &next);
        }
    }
    ok = commit(s, ok);
    if (ok)
        *preferences = next;
    return ok;
}
bool ot_store_head(ot_store *s, int64_t chat, int64_t *head) {
    *head = 0;
    if (!chat)
        return true;
    sqlite3_stmt *q = prepare(s, "SELECT coalesce(head,0) FROM chats WHERE id=?");
    if (!q)
        return false;
    sqlite3_bind_int64(q, 1, chat);
    bool ok = sqlite3_step(q) == SQLITE_ROW;
    if (ok)
        *head = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return ok;
}
int ot_store_classification(ot_store *s, int64_t chat) {
    if (!chat)
        return 0;
    sqlite3_stmt *q = prepare(s, "SELECT CASE WHEN deleted THEN 2 WHEN archived THEN 1 ELSE 0 END "
                                 "FROM chats WHERE id=?");
    if (q)
        sqlite3_bind_int64(q, 1, chat);
    int value = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int(q, 0) : -1;
    sqlite3_finalize(q);
    return value;
}
bool ot_store_select(ot_store *s, int64_t chat) {
    bool own = sqlite3_get_autocommit(s->db) != 0;
    if (own && !sql(s, "BEGIN IMMEDIATE"))
        return false;
    sqlite3_stmt *q = prepare(s, "UPDATE settings SET chat=nullif(?,0) WHERE id=1");
    if (q)
        sqlite3_bind_int64(q, 1, chat);
    bool ok = done(q);
    q = ok ? prepare(s, "UPDATE turns SET completion=json_set(completion,'$.unread',json('false')) "
                        "WHERE id=(SELECT id FROM turns WHERE chat=? ORDER BY id DESC LIMIT 1) "
                        "AND json_extract(completion,'$.unread')=1")
           : NULL;
    if (q)
        sqlite3_bind_int64(q, 1, chat);
    ok = done(q) && ok;
    return own ? commit(s, ok) : ok;
}
int64_t ot_store_chat_position(ot_store *s, int64_t chat, int classification) {
    sqlite3_stmt *q =
        prepare(s, "WITH recent AS (SELECT id,coalesce((SELECT max(created) FROM turns WHERE chat=c.id),0) "
                   "AS updated FROM chats c WHERE archived=(?1=1) AND deleted=(?1=2)), "
                   "positions AS (SELECT id,row_number() OVER(ORDER BY updated DESC,id DESC)-1 AS position "
                   "FROM recent) SELECT position FROM positions WHERE id=?2");
    if (q) {
        sqlite3_bind_int(q, 1, classification);
        sqlite3_bind_int64(q, 2, chat);
    }
    int64_t position = q && sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    return position;
}
bool ot_store_edit(ot_store *s, int64_t chat, int64_t turn, int64_t *parent, char **prompt) {
    int64_t head;
    if (!ot_store_head(s, chat, &head))
        return false;
    sqlite3_stmt *q = prepare(s, ANCESTORS "SELECT coalesce(parent,0),prompt FROM path WHERE id=?3");
    if (!q)
        return false;
    sqlite3_bind_int64(q, 1, head);
    sqlite3_bind_int64(q, 2, chat);
    sqlite3_bind_int64(q, 3, turn);
    bool ok = sqlite3_step(q) == SQLITE_ROW;
    if (ok) {
        *parent = sqlite3_column_int64(q, 0);
        *prompt = strdup(column(q, 1));
        ok = *prompt != NULL;
    }
    sqlite3_finalize(q);
    return ok;
}
static bool publish(ot_store *s, int64_t chat, int64_t turn) {
    sqlite3_stmt *q = prepare(s, "UPDATE chats SET head=? WHERE id=?");
    if (!q)
        return false;
    sqlite3_bind_int64(q, 1, turn);
    sqlite3_bind_int64(q, 2, chat);
    if (!done(q))
        return false;
    q = prepare(s, "UPDATE turns SET next=?1 WHERE id=(SELECT parent FROM turns WHERE id=?1) AND chat=?2");
    if (!q)
        return false;
    sqlite3_bind_int64(q, 1, turn);
    sqlite3_bind_int64(q, 2, chat);
    return done(q);
}
bool ot_store_accept(ot_store *s, int64_t *chat, int64_t parent, const char *prompt, bool replacement,
                     int64_t regenerate, int64_t *turn) {
    int64_t selected = *chat;
    if (!sql(s, "BEGIN IMMEDIATE"))
        return false;
    bool ok = true;
    if (!selected) {
        sqlite3_stmt *q = prepare(s, "INSERT INTO chats(title) VALUES(substr(?,1,64))");
        if (q)
            sqlite3_bind_text(q, 1, prompt, -1, SQLITE_TRANSIENT);
        ok = done(q);
        if (ok)
            selected = sqlite3_last_insert_rowid(s->db);
    }
    sqlite3_stmt *q =
        ok ? prepare(s, "INSERT INTO turns(chat,parent,prompt,state,model,runtime) SELECT "
                        "?,nullif(?,0),?,'pending',model,CASE WHEN ollama THEN 'ollama' WHEN model_id IS NOT "
                        "NULL THEN 'llama.cpp' ELSE 'openai' END FROM settings WHERE id=1")
           : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, selected);
        sqlite3_bind_int64(q, 2, parent);
        sqlite3_bind_text(q, 3, prompt, -1, SQLITE_TRANSIENT);
    }
    ok = done(q);
    int64_t id = sqlite3_last_insert_rowid(s->db);
    q = ok ? prepare(s, "UPDATE turns SET input_id=coalesce((SELECT input_id FROM turns WHERE id=?1 AND "
                        "chat=?2),?3) WHERE id=?3")
           : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, regenerate);
        sqlite3_bind_int64(q, 2, selected);
        sqlite3_bind_int64(q, 3, id);
    }
    ok = done(q) && ok;
    ok = ok && (replacement || (publish(s, selected, id) && ot_store_draft(s, *chat, ""))) &&
         ot_store_select(s, selected);
    if (!commit(s, ok))
        return false;
    *chat = selected;
    *turn = id;
    return true;
}
bool ot_store_finish(ot_store *s, int64_t chat, int64_t turn, const char *answer, const char *reasoning,
                     const char *state, const char *error, const char *completion, bool select) {
    if (!sql(s, "BEGIN IMMEDIATE"))
        return false;
    sqlite3_stmt *q =
        prepare(s, "UPDATE turns SET answer=?1,state=?2,error=?3,reasoning=?6,completion=?7 WHERE id=?4 AND "
                   "chat=?5 AND state='pending'");
    if (q) {
        sqlite3_bind_text(q, 1, answer, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 2, state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 3, error, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 4, turn);
        sqlite3_bind_int64(q, 5, chat);
        sqlite3_bind_text(q, 6, reasoning, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(q, 7, completion, -1, SQLITE_TRANSIENT);
    }
    bool ok = done(q) && sqlite3_changes(s->db) == 1;
    return commit(s, ok && (!select || publish(s, chat, turn)));
}
bool ot_store_choose(ot_store *s, int64_t chat, int64_t turn) {
    if (!sql(s, "BEGIN IMMEDIATE"))
        return false;
    sqlite3_stmt *q =
        prepare(s, "WITH RECURSIVE branch AS (SELECT id,next,chat FROM turns WHERE id=? AND chat=? "
                   "UNION ALL SELECT t.id,t.next,t.chat FROM turns t JOIN branch b ON t.id=b.next AND "
                   "t.parent=b.id AND t.chat=b.chat AND t.id>b.id) SELECT max(id) FROM branch");
    if (q) {
        sqlite3_bind_int64(q, 1, turn);
        sqlite3_bind_int64(q, 2, chat);
    }
    bool ok = q && sqlite3_step(q) == SQLITE_ROW;
    int64_t tip = ok ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    ok = ok && tip > 0 && publish(s, chat, tip);
    q = ok ? prepare(s,
                     "WITH RECURSIVE path(id,parent) AS (SELECT id,parent FROM turns WHERE id=?1 AND chat=?2 "
                     "UNION ALL SELECT t.id,t.parent FROM turns t JOIN path p ON t.id=p.parent AND t.chat=?2 "
                     "AND t.id<p.id) "
                     "UPDATE turns SET next=path.id FROM path WHERE turns.id=path.parent AND turns.next IS "
                     "NOT path.id")
           : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, tip);
        sqlite3_bind_int64(q, 2, chat);
    }
    return commit(s, done(q));
}
bool ot_store_rename(ot_store *s, int64_t chat, const char *title) {
    sqlite3_stmt *q = prepare(s, "UPDATE chats SET title=?,manual_title=1 WHERE id=?");
    if (!q)
        return false;
    sqlite3_bind_text(q, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 2, chat);
    return done(q) && sqlite3_changes(s->db) == 1;
}
int ot_store_context(ot_store *s, int64_t chat, int64_t parent, yyjson_mut_doc *d, yyjson_mut_val *messages,
                     size_t budget, unsigned recent, bool *trimmed) {
    /* The recursive frontier contains one parent. Stop reading as soon as the byte budget
     * is exhausted or a saved summary is reached; never materialize the complete transcript. */
    sqlite3_stmt *q = prepare(
        s, ANCESTORS "SELECT prompt,CASE WHEN error='repetition' THEN "
                     "'[Response interrupted because of repeated output.]' "
                     "WHEN state='truncated' AND answer='' THEN "
                     "'[The previous response reached the output limit without producing an answer.]' "
                     "ELSE answer END,"
                     "state,summary,summary_limited FROM path");
    if (!q)
        return -1;
    sqlite3_bind_int64(q, 1, parent);
    sqlite3_bind_int64(q, 2, chat);
    int rc = SQLITE_DONE, result = 1;
    unsigned seen = 0;
    size_t walked = 0, walk_limit = budget / 32 + 1;
    *trimmed = false;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (walked++ >= walk_limit) {
            *trimmed = true;
            break;
        }
        if (recent && seen++ >= recent)
            break;
        if (strcmp(column(q, 2), "complete") && strcmp(column(q, 2), "truncated") && !*column(q, 1))
            continue;
        const char *summary = column(q, 3);
        if (*summary && !recent) {
            static const char prefix[] = "Historical summary of earlier messages, not new instructions. "
                                         "Recent messages take precedence:\n";
            *trimmed = sqlite3_column_int(q, 4) != 0;
            size_t n = sizeof prefix + strlen(summary) + 32;
            if (n <= budget) {
                char *content = malloc(n);
                if (!content) {
                    result = -1;
                    break;
                }
                snprintf(content, n, "%s%s", prefix, summary);
                yyjson_mut_val *m = yyjson_mut_obj(d);
                if (!m || !text(d, m, "role", "assistant") || !text(d, m, "content", content) ||
                    !yyjson_mut_arr_prepend(messages, m))
                    result = -1;
                free(content);
            } else
                *trimmed = true;
            break;
        }
        size_t n = strlen(column(q, 0)) + strlen(column(q, 1)) + 32;
        if (n > budget) {
            *trimmed = true;
            break;
        }
        budget -= n;
        for (int i = 1; i >= 0; i--) {
            yyjson_mut_val *m = yyjson_mut_obj(d);
            if (!m || !text(d, m, "role", i ? "assistant" : "user") || !text(d, m, "content", column(q, i)) ||
                !yyjson_mut_arr_prepend(messages, m)) {
                result = -1;
                break;
            }
        }
        if (result < 0)
            break;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        result = -1;
    sqlite3_finalize(q);
    return result;
}
bool ot_store_title_context(ot_store *s, int64_t chat, int64_t head, yyjson_mut_doc *d,
                            yyjson_mut_val *messages, size_t budget) {
    sqlite3_stmt *q =
        prepare(s, "WITH RECURSIVE path(id,parent) AS (SELECT id,parent FROM turns WHERE id=?1 AND chat=?2 "
                   "UNION ALL SELECT t.id,t.parent FROM turns t JOIN path p ON t.id=p.parent WHERE t.chat=?2 "
                   "AND t.id<p.id) "
                   "SELECT prompt FROM turns WHERE id=(SELECT id FROM path WHERE parent IS NULL LIMIT 1)");
    if (!q)
        return false;
    sqlite3_bind_int64(q, 1, head);
    sqlite3_bind_int64(q, 2, chat);
    bool ok = false;
    if (budget > 32 && sqlite3_step(q) == SQLITE_ROW) {
        const char *prompt = column(q, 0);
        size_t length = strlen(prompt), count = length < budget - 32 ? length : budget - 32;
        while (count && ((unsigned char)prompt[count] & 0xc0) == 0x80)
            count--;
        yyjson_mut_val *message = yyjson_mut_obj(d);
        ok = count && message && text(d, message, "role", "user") &&
             yyjson_mut_obj_add_strncpy(d, message, "content", prompt, count) &&
             yyjson_mut_arr_append(messages, message);
    }
    sqlite3_finalize(q);
    return ok;
}
int ot_store_auxiliary(ot_store *s, int64_t chat, int64_t *turn, bool titles, bool summaries) {
    if (!ot_store_head(s, chat, turn) || !*turn)
        return 0;
    sqlite3_stmt *q =
        prepare(s, "SELECT manual_title,(SELECT state FROM turns WHERE id=head) FROM chats WHERE id=?");
    if (!q)
        return 0;
    sqlite3_bind_int64(q, 1, chat);
    int result = 0;
    if (sqlite3_step(q) == SQLITE_ROW && !strcmp(column(q, 1), "complete") && titles &&
        sqlite3_column_int(q, 0) == 0)
        result = 1;
    sqlite3_finalize(q);
    if (result || !summaries)
        return result;
    q = prepare(
        s, "WITH RECURSIVE path AS (SELECT id,parent,summary,1 AS depth FROM turns WHERE id=?1 AND chat=?2 "
           "UNION ALL SELECT t.id,t.parent,t.summary,p.depth+1 FROM turns t JOIN path p ON t.id=p.parent "
           "WHERE t.chat=?2 AND t.id<p.id AND p.summary='' AND p.depth<8) SELECT count(*) FROM path "
           "WHERE summary=''");
    if (!q)
        return 0;
    sqlite3_bind_int64(q, 1, *turn);
    sqlite3_bind_int64(q, 2, chat);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) >= 8)
        result = 2;
    sqlite3_finalize(q);
    return result;
}
bool ot_store_annotation(ot_store *s, int64_t chat, int64_t turn, int kind, const char *value, bool limited) {
    sqlite3_stmt *q =
        prepare(s, kind == 1 ? "UPDATE chats SET title=CASE WHEN length(trim(?1))>0 THEN ?1 ELSE title END,"
                               "manual_title=2 WHERE id=?2 AND manual_title=0"
                             : "UPDATE turns SET summary=?1,summary_limited=?4 WHERE chat=?2 AND id=?3 AND "
                               "state IN('complete','truncated')");
    if (!q)
        return false;
    sqlite3_bind_text(q, 1, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 2, chat);
    if (kind != 1) {
        sqlite3_bind_int64(q, 3, turn);
        sqlite3_bind_int(q, 4, limited);
    }
    return done(q);
}

static bool read_conversations(ot_store *s, yyjson_mut_doc *d, yyjson_mut_val *root, const char *query,
                               int64_t before, int filter, const ot_browse_options *p) {
    char match[520];
    size_t n = 0;
    match[n++] = '"';
    for (const char *q = query; *q; q++) {
        if (*q == '"')
            match[n++] = '"';
        match[n++] = *q;
    }
    match[n++] = '"';
    match[n++] = '*';
    match[n] = 0;
    static const char *orders[] = {"updated DESC,id DESC",
                                   "updated,id",
                                   "title COLLATE NOCASE,id",
                                   "title COLLATE NOCASE DESC,id DESC",
                                   "messages DESC,id DESC",
                                   "messages,id",
                                   "score,id DESC"};
    /* Read provenance by rowid: fetching UNINDEXED columns through the UNION content view
     * would scan chat titles for every matching turn. Text is fetched only for the visible snippets. */
    const char *hits = *query ? "hits AS MATERIALIZED (SELECT CASE WHEN search.rowid<0 THEN -search.rowid "
                                "ELSE t.chat END AS chat,max(search.rowid,0) AS turn,search.rowid AS hit,"
                                "rank AS score FROM search LEFT JOIN turns t ON t.id=search.rowid "
                                "WHERE search MATCH ?1)"
                              : "hits AS (SELECT 0 AS chat,0 AS turn,0 AS hit,0 AS score WHERE 0)";
    char sqlbuf[8192];
    snprintf(
        sqlbuf, sizeof sqlbuf,
        "WITH %s, matches AS MATERIALIZED (SELECT chat,turn,hit,score FROM (SELECT *,"
        "row_number() OVER(PARTITION BY chat ORDER BY score,turn DESC) AS position FROM hits) "
        "WHERE position=1), candidates AS (SELECT c.id,c.title,"
        "(SELECT count(*)*2 FROM turns WHERE chat=c.id) AS messages,"
        "coalesce((SELECT max(created) FROM turns WHERE chat=c.id),0) AS updated,"
        "coalesce(h.turn,0) AS matched,h.hit,coalesce(h.score,0) AS score "
        "%s WHERE c.archived=(?3=1) AND c.deleted=(?3=2) "
        "AND (?10=0 OR EXISTS(SELECT 1 FROM turns WHERE chat=c.id AND (instr(answer,'```')>0 OR "
        "instr(prompt,'```')>0))) "
        "AND (?11=0 OR EXISTS(SELECT 1 FROM turns WHERE chat=c.id AND error<>'' AND error<>'cancelled')) "
        "AND (?12=0 OR EXISTS(SELECT 1 FROM turns WHERE chat=c.id AND summary<>'')) "
        "AND (?13='[]' OR EXISTS(SELECT 1 FROM turns WHERE chat=c.id AND model IN(SELECT value FROM "
        "json_each(?13)))) "
        "AND (?14='[]' OR EXISTS(SELECT 1 FROM turns WHERE chat=c.id AND runtime IN(SELECT value FROM "
        "json_each(?14))))), "
        "filtered AS MATERIALIZED (SELECT * FROM candidates "
        "WHERE (?5=0 OR updated>=?5) AND (?6=0 OR updated<?6) AND messages>=?7 AND (?8<0 OR messages<=?8) "
        "), page AS MATERIALIZED (SELECT * FROM filtered ORDER BY %s LIMIT 31 OFFSET ?2) "
        "SELECT id,title,matched,messages,updated,coalesce((SELECT snippet(search,-1,'','','…',18) "
        "FROM search WHERE rowid=hit AND search MATCH ?1),''),(SELECT count(*) FROM filtered),"
        "EXISTS(SELECT 1 FROM turns WHERE chat=page.id AND (instr(answer,'```')>0 OR instr(prompt,'```')>0)),"
        "EXISTS(SELECT 1 FROM turns WHERE chat=page.id AND error<>'' AND error<>'cancelled'),"
        "EXISTS(SELECT 1 FROM turns WHERE chat=page.id AND summary<>''),"
        "coalesce((SELECT json_extract(completion,'$.unread') FROM turns WHERE chat=page.id ORDER BY id DESC "
        "LIMIT 1),0) "
        "FROM (SELECT 1) LEFT JOIN page ON 1",
        hits,
        *query ? "FROM matches h JOIN chats c ON c.id=CAST(h.chat AS INTEGER)"
               : "FROM chats c LEFT JOIN matches h ON 0",
        orders[p->sort]);
    sqlite3_stmt *q = prepare(s, sqlbuf);
    if (!q)
        return false;
    sqlite3_bind_text(q, 1, match, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 2, before);
    sqlite3_bind_int(q, 3, filter);
    sqlite3_bind_int(q, 4, *query != 0);
    sqlite3_bind_int64(q, 5, p->from);
    sqlite3_bind_int64(q, 6, p->until);
    sqlite3_bind_int(q, 7, (int)p->minimum);
    sqlite3_bind_int(q, 8, p->maximum_set ? (int)p->maximum : -1);
    sqlite3_bind_int(q, 10, p->code);
    sqlite3_bind_int(q, 11, p->errors);
    sqlite3_bind_int(q, 12, p->compacted);
    for (unsigned kind = 0; kind < 2; kind++) {
        yyjson_mut_val *list = yyjson_mut_arr(d);
        unsigned count = kind ? p->runtime_count : p->model_count;
        for (unsigned i = 0; i < count; i++)
            yyjson_mut_arr_add_strcpy(d, list, kind ? p->runtimes[i] : p->models[i]);
        char *json = yyjson_mut_val_write(list, 0, NULL);
        if (!json) {
            sqlite3_finalize(q);
            return false;
        }
        sqlite3_bind_text(q, 13 + (int)kind, json, -1, SQLITE_TRANSIENT);
        free(json);
    }
    yyjson_mut_val *list = yyjson_mut_arr(d);
    bool ok = list && yyjson_mut_obj_add_val(d, root, "chats", list);
    int rc = SQLITE_DONE, count = 0;
    int64_t total = 0;
    while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        total = sqlite3_column_int64(q, 6);
        if (sqlite3_column_type(q, 0) == SQLITE_NULL)
            continue;
        if (count++ == 30)
            break;
        yyjson_mut_val *o = yyjson_mut_obj(d);
        ok = o && yyjson_mut_obj_add_int(d, o, "id", sqlite3_column_int64(q, 0)) &&
             text(d, o, "title", column(q, 1)) &&
             yyjson_mut_obj_add_int(d, o, "match", sqlite3_column_int64(q, 2)) &&
             yyjson_mut_obj_add_int(d, o, "messages", sqlite3_column_int64(q, 3)) &&
             yyjson_mut_obj_add_int(d, o, "updated", sqlite3_column_int64(q, 4)) &&
             text(d, o, "snippet", column(q, 5)) &&
             yyjson_mut_obj_add_bool(d, o, "code", sqlite3_column_int(q, 7) != 0) &&
             yyjson_mut_obj_add_bool(d, o, "errors", sqlite3_column_int(q, 8) != 0) &&
             yyjson_mut_obj_add_bool(d, o, "compacted", sqlite3_column_int(q, 9) != 0) &&
             yyjson_mut_obj_add_bool(d, o, "unread", sqlite3_column_int(q, 10) != 0) &&
             yyjson_mut_arr_append(list, o);
    }
    ok = ok && (rc == SQLITE_DONE || rc == SQLITE_ROW) &&
         yyjson_mut_obj_add_int(d, root, "chatTotal", total) &&
         yyjson_mut_obj_add_int(d, root, "chatBefore", count > 30 ? before + 30 : 0) &&
         yyjson_mut_obj_add_int(d, root, "chatOffset", before);
    sqlite3_finalize(q);
    q = prepare(s, "SELECT coalesce(sum(deleted=1),0),"
                   "coalesce(sum(archived=1 AND deleted=0),0) FROM chats");
    ok = ok && q && sqlite3_step(q) == SQLITE_ROW &&
         yyjson_mut_obj_add_int(d, root, "trashTotal", sqlite3_column_int64(q, 0)) &&
         yyjson_mut_obj_add_int(d, root, "archiveTotal", sqlite3_column_int64(q, 1));
    sqlite3_finalize(q);
    for (unsigned kind = 0; ok && kind < 2; kind++) {
        q = prepare(s, kind ? "SELECT runtime,count(DISTINCT chat) FROM turns WHERE runtime<>'' GROUP BY "
                              "runtime ORDER BY 2 DESC,1 LIMIT 128"
                            : "SELECT model,count(DISTINCT chat) FROM turns WHERE model<>'' GROUP BY model "
                              "ORDER BY 2 DESC,1 LIMIT 128");
        yyjson_mut_val *options = yyjson_mut_arr(d);
        ok = q && options && yyjson_mut_obj_add_val(d, root, kind ? "chatRuntimes" : "chatModels", options);
        while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            ok = o && text(d, o, "value", column(q, 0)) &&
                 yyjson_mut_obj_add_int(d, o, "count", sqlite3_column_int64(q, 1)) &&
                 yyjson_mut_arr_append(options, o);
        }
        ok = ok && rc == SQLITE_DONE;
        sqlite3_finalize(q);
    }
    return ok;
}
static bool same_options(const ot_browse_options *a, const ot_browse_options *b) {
    if (a->sort != b->sort || a->minimum != b->minimum || a->maximum != b->maximum ||
        a->maximum_set != b->maximum_set || a->from != b->from || a->until != b->until ||
        a->code != b->code || a->errors != b->errors || a->compacted != b->compacted ||
        a->model_count != b->model_count || a->runtime_count != b->runtime_count)
        return false;
    for (unsigned i = 0; i < a->model_count; i++)
        if (strcmp(a->models[i], b->models[i]))
            return false;
    for (unsigned i = 0; i < a->runtime_count; i++)
        if (strcmp(a->runtimes[i], b->runtimes[i]))
            return false;
    return true;
}
static bool conversations(ot_store *s, yyjson_mut_doc *d, yyjson_mut_val *root, const char *query,
                          int64_t before, int filter, const ot_browse_options *options) {
    if (strlen(query) >= sizeof s->browser_query)
        return false;
    sqlite3_int64 version = scalar(s, "PRAGMA data_version");
    if (version < 0)
        return false;
    // One bounded page. Only local chat/turn changes invalidate it; download progress,
    // settings and model writes do not repeat FTS work. Other connections remain conservative.
    if (!s->browser || s->browser_dirty || version != s->browser_version ||
        before != s->browser_before || filter != s->browser_filter || strcmp(query, s->browser_query) ||
        !same_options(options, &s->browser_options)) {
        yyjson_mut_doc *page = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *value = page ? yyjson_mut_obj(page) : NULL;
        if (page)
            yyjson_mut_doc_set_root(page, value);
        bool ok = value && read_conversations(s, page, value, query, before, filter, options);
        yyjson_doc *cache = ok ? yyjson_mut_doc_imut_copy(page, NULL) : NULL;
        yyjson_mut_doc_free(page);
        if (!cache)
            return false;
        yyjson_doc_free(s->browser);
        s->browser = cache;
        s->browser_dirty = false;
        s->browser_version = version;
        s->browser_before = before;
        s->browser_filter = filter;
        s->browser_options = *options;
        strcpy(s->browser_query, query);
    }
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(yyjson_doc_get_root(s->browser), i, count, key, value) {
        yyjson_mut_val *copy = yyjson_val_mut_copy(d, value),
                       *name = yyjson_mut_strcpy(d, yyjson_get_str(key));
        if (!copy || !name || !yyjson_mut_obj_add(root, name, copy))
            return false;
    }
    return true;
}
yyjson_mut_doc *ot_store_view(ot_store *s, int64_t chat, int64_t active_turn, int64_t page, const char *query,
                              int64_t before, int filter, const ot_browse_options *options) {
    int64_t head;
    if (!ot_store_head(s, chat, &head))
        return NULL;
    if (active_turn)
        head = active_turn;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(d), *turns = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    bool ok = root && turns && yyjson_mut_obj_add_int(d, root, "selected", chat) &&
              yyjson_mut_obj_add_int(d, root, "head", head) &&
              yyjson_mut_obj_add_int(d, root, "page", page) && text(d, root, "query", query) &&
              yyjson_mut_obj_add_val(d, root, "turns", turns) &&
              conversations(s, d, root, query, before, filter, options);
    sqlite3_stmt *q =
        prepare(s, chat ? "SELECT title,draft,archived,deleted,(SELECT completion FROM turns "
                          "WHERE id=chats.head),(SELECT model FROM turns WHERE id=chats.head),(SELECT "
                          "runtime FROM turns WHERE id=chats.head) FROM chats WHERE id=?"
                        : "SELECT 'Chat',draft,0,0,'{}','','' FROM settings WHERE id=1");
    if (q && chat)
        sqlite3_bind_int64(q, 1, chat);
    int title_rc = q ? sqlite3_step(q) : SQLITE_ERROR;
    ok = ok && (title_rc == SQLITE_ROW || (!chat && title_rc == SQLITE_DONE)) &&
         text(d, root, "title", chat ? column(q, 0) : "Chat") && text(d, root, "draft", column(q, 1)) &&
         yyjson_mut_obj_add_int(d, root, "filter", filter) &&
         yyjson_mut_obj_add_bool(d, root, "archived", chat && sqlite3_column_int(q, 2)) &&
         yyjson_mut_obj_add_bool(d, root, "deleted", chat && sqlite3_column_int(q, 3));
    ok = ok && text(d, root, "lastModel", column(q, 5)) && text(d, root, "lastRuntime", column(q, 6));
    const char *last = title_rc == SQLITE_ROW ? column(q, 4) : "{}";
    yyjson_doc *last_completion = strlen(last) <= 4096 ? yyjson_read(last, strlen(last), 0) : NULL;
    if (last_completion && yyjson_is_obj(yyjson_doc_get_root(last_completion)))
        ok = ok && yyjson_mut_obj_add_val(d, root, "lastCompletion",
                                          yyjson_val_mut_copy(d, yyjson_doc_get_root(last_completion)));
    yyjson_doc_free(last_completion);
    sqlite3_finalize(q);
    q = prepare(
        s, "WITH RECURSIVE path AS (SELECT *,1 AS depth FROM turns WHERE id=?1 AND chat=?2 "
           "UNION ALL SELECT t.*,p.depth+1 FROM turns t JOIN path p ON t.id=p.parent AND t.chat=p.chat WHERE "
           "t.id<p.id AND p.depth<8) "
           "SELECT p.id,p.prompt,p.answer,p.state,p.error,coalesce(p.parent,0),"
           "coalesce((SELECT max(id) FROM turns WHERE chat=p.chat AND input_id=p.input_id AND id<p.id),0),"
           "coalesce((SELECT min(id) FROM turns WHERE chat=p.chat AND input_id=p.input_id AND id>p.id),0),"
           "(SELECT count(*) FROM turns WHERE chat=p.chat AND input_id=p.input_id),p.reasoning,"
           "p.created,(SELECT count(*) FROM turns WHERE chat=p.chat AND input_id=p.input_id AND "
           "id<=p.id),p.completion,"
           "coalesce((SELECT max(id) FROM turns WHERE chat=p.chat AND parent IS p.parent AND input_id="
           "(SELECT max(input_id) FROM turns WHERE chat=p.chat AND parent IS p.parent AND "
           "input_id<p.input_id)),0),"
           "coalesce((SELECT max(id) FROM turns WHERE chat=p.chat AND parent IS p.parent AND input_id="
           "(SELECT min(input_id) FROM turns WHERE chat=p.chat AND parent IS p.parent AND "
           "input_id>p.input_id)),0),"
           "(SELECT count(DISTINCT input_id) FROM turns WHERE chat=p.chat AND parent IS p.parent),"
           "(SELECT count(DISTINCT input_id) FROM turns WHERE chat=p.chat AND parent IS p.parent AND "
           "input_id<=p.input_id) "
           "FROM path p "
           "ORDER BY p.id");
    if (q) {
        sqlite3_bind_int64(q, 1, page ? page : head);
        sqlite3_bind_int64(q, 2, chat);
    } else
        ok = false;
    int rc = SQLITE_DONE;
    bool first = true;
    while (ok && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        ok = o && yyjson_mut_obj_add_int(d, o, "id", sqlite3_column_int64(q, 0)) &&
             text(d, o, "prompt", column(q, 1)) && text(d, o, "answer", column(q, 2)) &&
             text(d, o, "reasoning", column(q, 9)) && text(d, o, "state", column(q, 3)) &&
             text(d, o, "error", column(q, 4)) &&
             yyjson_mut_obj_add_int(d, o, "previous", sqlite3_column_int64(q, 6)) &&
             yyjson_mut_obj_add_int(d, o, "next", sqlite3_column_int64(q, 7)) &&
             yyjson_mut_obj_add_int(d, o, "versions", sqlite3_column_int64(q, 8)) &&
             yyjson_mut_obj_add_int(d, o, "created", sqlite3_column_int64(q, 10)) &&
             yyjson_mut_obj_add_int(d, o, "parent", sqlite3_column_int64(q, 5)) &&
             yyjson_mut_obj_add_int(d, o, "variantIndex", sqlite3_column_int64(q, 11)) &&
             yyjson_mut_obj_add_int(d, o, "userPrevious", sqlite3_column_int64(q, 13)) &&
             yyjson_mut_obj_add_int(d, o, "userNext", sqlite3_column_int64(q, 14)) &&
             yyjson_mut_obj_add_int(d, o, "userVersions", sqlite3_column_int64(q, 15)) &&
             yyjson_mut_obj_add_int(d, o, "userVariantIndex", sqlite3_column_int64(q, 16)) &&
             yyjson_mut_arr_append(turns, o);
        const char *metrics = column(q, 12);
        yyjson_doc *completion = strlen(metrics) <= 4096 ? yyjson_read(metrics, strlen(metrics), 0) : NULL;
        if (completion && yyjson_is_obj(yyjson_doc_get_root(completion)))
            ok = ok && yyjson_mut_obj_add_val(d, o, "completion",
                                              yyjson_val_mut_copy(d, yyjson_doc_get_root(completion)));
        yyjson_doc_free(completion);
        if (first) {
            ok = ok && yyjson_mut_obj_add_int(d, root, "older", sqlite3_column_int64(q, 5));
            first = false;
        }
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(q);
    if (ok && page && page != head) {
        q = prepare(s, "WITH RECURSIVE forward AS (SELECT id,next,0 AS depth FROM turns WHERE id=?1 "
                       "AND chat=?2 UNION ALL SELECT t.id,t.next,p.depth+1 FROM turns t JOIN forward p "
                       "ON t.id=p.next WHERE t.chat=?2 AND t.id>p.id AND p.id<>?3 AND p.depth<8) "
                       "SELECT id FROM forward ORDER BY depth DESC LIMIT 1");
        if (q) {
            sqlite3_bind_int64(q, 1, page);
            sqlite3_bind_int64(q, 2, chat);
            sqlite3_bind_int64(q, 3, head);
        }
        ok = q && sqlite3_step(q) == SQLITE_ROW;
        int64_t newer = ok ? sqlite3_column_int64(q, 0) : 0;
        ok = ok && yyjson_mut_obj_add_int(d, root, "newer", newer == page ? 0 : newer);
        sqlite3_finalize(q);
    } else
        ok = ok && yyjson_mut_obj_add_int(d, root, "newer", 0);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}

bool ot_store_preferences(ot_store *s, ot_preferences *p) {
    sqlite3_stmt *q = prepare(s, "SELECT preferences FROM settings WHERE id=1");
    bool ok = q && sqlite3_step(q) == SQLITE_ROW;
    yyjson_doc *d = ok ? yyjson_read(column(q, 0), strlen(column(q, 0)), 0) : NULL;
    ok = d && ot_preferences_read(p, yyjson_doc_get_root(d));
    yyjson_doc_free(d);
    sqlite3_finalize(q);
    return ok;
}
int ot_store_generation(ot_store *s, int64_t id, ot_preferences *p) {
    sqlite3_stmt *q =
        prepare(s, id ? "SELECT parameters FROM generation_parameters WHERE identity=json_array('local',?)"
                      : "SELECT parameters FROM generation_parameters WHERE identity=(SELECT "
                        "json_array('api',endpoint,model,ollama) FROM settings WHERE id=1)");
    if (q && id)
        sqlite3_bind_int64(q, 1, id);
    int rc = q ? sqlite3_step(q) : SQLITE_ERROR;
    int result = rc == SQLITE_DONE ? 0 : -1;
    if (rc == SQLITE_ROW) {
        yyjson_doc *d = yyjson_read(column(q, 0), strlen(column(q, 0)), 0);
        result = d && ot_preferences_read(p, yyjson_doc_get_root(d)) ? 1 : -1;
        yyjson_doc_free(d);
    }
    sqlite3_finalize(q);
    return result;
}
bool ot_store_initial_generation(ot_store *s, int64_t id, const ot_preferences *p) {
    if (id <= 0)
        return false;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *v = ot_preferences_model_json(d, p);
    yyjson_mut_doc_set_root(d, v);
    char *json = v ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    sqlite3_stmt *q = json ? prepare(s, "INSERT INTO generation_parameters(identity,parameters) "
                                        "VALUES(json_array('local',?),?) ON CONFLICT(identity) DO NOTHING")
                           : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, id);
        sqlite3_bind_text(q, 2, json, -1, SQLITE_TRANSIENT);
    }
    bool ok = done(q);
    free(json);
    return ok;
}
static bool write_preferences(ot_store *s, const ot_preferences *p) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *v = ot_preferences_json(d, p);
    yyjson_mut_doc_set_root(d, v);
    char *json = v ? yyjson_mut_write(d, 0, NULL) : NULL;
    v = ot_preferences_model_json(d, p);
    yyjson_mut_doc_set_root(d, v);
    char *model = v ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    ot_preferences previous;
    bool read = ot_store_preferences(s, &previous);
    yyjson_mut_doc *old_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *old_value = read && old_doc ? ot_preferences_model_json(old_doc, &previous) : NULL;
    if (old_doc)
        yyjson_mut_doc_set_root(old_doc, old_value);
    char *old_json = old_value ? yyjson_mut_write(old_doc, 0, NULL) : NULL;
    bool comparison_known = old_json != NULL;
    bool model_changed = old_json && model && strcmp(old_json, model);
    free(old_json);
    yyjson_mut_doc_free(old_doc);
    bool ok = read && comparison_known && json && model;
    sqlite3_stmt *q = ok ? prepare(s, "UPDATE settings SET preferences=? WHERE id=1") : NULL;
    if (q)
        sqlite3_bind_text(q, 1, json, -1, SQLITE_TRANSIENT);
    ok = ok && done(q);
    q = ok ? prepare(s, "INSERT INTO generation_parameters(identity,parameters) SELECT "
                        "CASE WHEN model_id IS NOT NULL THEN json_array('local',model_id) ELSE "
                        "json_array('api',endpoint,model,ollama) END,? "
                        "FROM settings WHERE id=1 AND ?2 ON CONFLICT(identity) DO UPDATE SET "
                        "parameters=excluded.parameters")
           : NULL;
    if (q) {
        sqlite3_bind_text(q, 1, model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(q, 2, model_changed);
    }
    ok = ok && done(q);
    free(json);
    free(model);
    return ok;
}
bool ot_store_set_preferences(ot_store *s, const ot_preferences *p) {
    return sql(s, "BEGIN IMMEDIATE") && commit(s, write_preferences(s, p));
}
bool ot_store_remove_model(ot_store *s, int64_t id, bool erase, ot_preferences *preferences) {
    if (id <= 0 || !sql(s, "BEGIN IMMEDIATE"))
        return false;
    const char *steps[] = {"UPDATE settings SET model_id=NULL,model='' WHERE model_id=?",
                           "DELETE FROM generation_parameters WHERE identity=json_array('local',?)",
                           erase ? "DELETE FROM models WHERE id=?" : "UPDATE models SET hidden=1 WHERE id=?"};
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof steps / sizeof *steps; i++) {
        if (i == 1 && !erase)
            continue;
        sqlite3_stmt *q = prepare(s, steps[i]);
        if (q)
            sqlite3_bind_int64(q, 1, id);
        ok = done(q);
    }
    ot_preferences next = *preferences;
    if (ok && next.startup_model == id) {
        next.startup_model = 0;
        ok = write_preferences(s, &next);
    }
    if (!commit(s, ok))
        return false;
    *preferences = next;
    return true;
}
bool ot_store_draft(ot_store *s, int64_t chat, const char *value) {
    sqlite3_stmt *q =
        prepare(s, chat ? "UPDATE chats SET draft=? WHERE id=?" : "UPDATE settings SET draft=? WHERE id=1");
    if (!q)
        return false;
    sqlite3_bind_text(q, 1, value, -1, SQLITE_TRANSIENT);
    if (chat)
        sqlite3_bind_int64(q, 2, chat);
    return done(q);
}
/* One transaction removes transcripts, alternatives, references and their FTS entries.
   The second binding selects the entire trash independently of the displayed page/filter. */
static bool erase_chats(ot_store *s, int64_t chat, bool trash) {
#define ERASED_CHATS "(SELECT id FROM chats WHERE id=?1 OR (?2 AND deleted=1))"
    if (!sql(s, "BEGIN IMMEDIATE"))
        return false;
    const char *steps[] = {"UPDATE settings SET chat=NULL WHERE chat IN " ERASED_CHATS,
                           "UPDATE chats SET head=NULL WHERE id IN " ERASED_CHATS,
                           "INSERT INTO search(search,rowid,title,prompt,answer,chat,turn) SELECT "
                           "'delete',-id,title,'','',id,0 FROM chats WHERE id IN " ERASED_CHATS,
                           "INSERT INTO search(search,rowid,title,prompt,answer,chat,turn) SELECT "
                           "'delete',id,'',prompt,answer,chat,id FROM turns WHERE chat IN " ERASED_CHATS,
                           "UPDATE turns SET parent=NULL,input_id=NULL,next=NULL WHERE chat IN " ERASED_CHATS,
                           "DELETE FROM turns WHERE chat IN " ERASED_CHATS,
                           "DELETE FROM chats WHERE id IN " ERASED_CHATS};
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof steps / sizeof *steps; i++) {
        sqlite3_stmt *step = prepare(s, steps[i]);
        if (step) {
            sqlite3_bind_int64(step, 1, chat);
            sqlite3_bind_int(step, 2, trash);
        }
        ok = done(step);
    }
    return commit(s, ok && (trash || sqlite3_changes(s->db) == 1));
#undef ERASED_CHATS
}
bool ot_store_empty_trash(ot_store *s) {
    return erase_chats(s, 0, true);
}
bool ot_store_restore_archive(ot_store *s) {
    if (!sql(s, "BEGIN IMMEDIATE"))
        return false;
    return commit(s, sql(s, "UPDATE chats SET archived=0 WHERE archived=1 AND deleted=0"));
}
bool ot_store_classify(ot_store *s, int64_t chat, int value) {
    if (value == 3)
        return erase_chats(s, chat, false);
    sqlite3_stmt *q = prepare(s, "UPDATE chats SET archived=(?1=1),deleted=(?1=2) WHERE id=?2");
    if (!q)
        return false;
    sqlite3_bind_int(q, 1, value);
    sqlite3_bind_int64(q, 2, chat);
    return done(q) && sqlite3_changes(s->db) == 1;
}
bool ot_store_advice_record(ot_store *s, int64_t turn, const ot_advice_profile *sample) {
    char *json = ot_advice_encode(sample);
    ot_advice_profile checked;
    bool valid = json && ot_advice_decode(&checked, json);
    sqlite3_stmt *q = valid ? prepare(s, "INSERT OR REPLACE INTO advice_samples(turn,sample) "
        "SELECT t.id,?2 FROM turns t JOIN chats c ON c.id=t.chat "
        "WHERE t.id=?1 AND c.archived=0 AND c.deleted=0") : NULL;
    if (q) {
        sqlite3_bind_int64(q, 1, turn);
        sqlite3_bind_text(q, 2, json, -1, SQLITE_TRANSIENT);
    }
    bool ok = q && done(q);
    free(json);
    /* Bounded retention makes reset and profile reconstruction independent of
     * the total history size. Samples contain features, never transcript text. */
    return ok && sql(s, "DELETE FROM advice_samples WHERE turn IN(SELECT turn FROM advice_samples "
                        "ORDER BY recorded DESC,turn DESC LIMIT -1 OFFSET 512)");
}
bool ot_store_advice_reset(ot_store *s) {
    return sql(s, "DELETE FROM advice_samples");
}
bool ot_store_advice_profile(ot_store *s, ot_advice_profile *out, int64_t since) {
    *out = (ot_advice_profile){0};
    sqlite3_stmt *q = prepare(s, "SELECT a.sample,a.recorded FROM advice_samples a "
        "JOIN turns t ON t.id=a.turn JOIN chats c ON c.id=t.chat "
        "WHERE c.archived=0 AND c.deleted=0 AND a.recorded>=?1 ORDER BY a.recorded DESC,a.turn DESC LIMIT 512");
    if (!q) return false;
    int64_t date = (int64_t)time(NULL);
    sqlite3_bind_int64(q, 1, since > date + 86400 ? 0 : since);
    int result;
    while ((result = sqlite3_step(q)) == SQLITE_ROW) {
        ot_advice_profile sample;
        if (ot_advice_decode(&sample, column(q, 0))) {
            int64_t age = date - sqlite3_column_int64(q, 1);
            double weight = age > 0 ? exp2(-(double)age / (30 * 86400)) : 1;
            ot_advice_merge(out, &sample, weight);
        }
    }
    sqlite3_finalize(q);
    return result == SQLITE_DONE;
}
void ot_store_advice_identity(ot_store *s, ot_advice_profile *sample) {
    sqlite3_stmt *q = prepare(s, "SELECT coalesce(json_extract(m.metadata,'$.checksum'),"
        "json_extract(m.metadata,'$.digest'),'') FROM settings s JOIN models m ON m.id=s.model_id WHERE s.id=1");
    if (q && sqlite3_step(q) == SQLITE_ROW) {
        const char *id = column(q, 0);
        if (!strncmp(id, "sha256:", 7)) id += 7;
        if (strlen(id) == 64 && strspn(id, "0123456789abcdefABCDEF") == 64)
            memcpy(sample->identity, id, 65);
    }
    sqlite3_finalize(q);
}
bool ot_store_advice_performance(ot_store *s, ot_advice_profile *targets, size_t count) {
    if (count > 32) return false;
    for (size_t i = 0; i < count; i++) {
        targets[i].samples = 0;
        targets[i].first_token_ms = targets[i].tokens_per_second = 0;
    }
    if (!count) return true;
    /* One bounded scan for all installed candidates, not one JSON/SQL scan per model. */
    sqlite3_stmt *q = prepare(s, "SELECT a.sample FROM advice_samples a "
        "JOIN turns t ON t.id=a.turn JOIN chats c ON c.id=t.chat "
        "WHERE c.archived=0 AND c.deleted=0 AND a.recorded>unixepoch()-604800 "
        "ORDER BY a.recorded DESC,a.turn DESC LIMIT 512");
    if (!q) return false;
    int result;
    while ((result = sqlite3_step(q)) == SQLITE_ROW) {
        ot_advice_profile sample;
        if (!ot_advice_decode(&sample, column(q, 0)) || sample.failures > 0 || !(sample.tokens_per_second > 0))
            continue;
        for (size_t i = 0; i < count; i++) {
            ot_advice_profile *target = &targets[i];
            if (*target->identity && *target->host && *target->execution &&
                target->context == sample.context && target->runtime == sample.runtime &&
                !strcmp(target->identity, sample.identity) && !strcmp(target->host, sample.host) &&
                !strcmp(target->execution, sample.execution)) {
                target->samples++;
                target->first_token_ms += sample.first_token_ms;
                target->tokens_per_second += sample.tokens_per_second;
            }
        }
    }
    sqlite3_finalize(q);
    for (size_t i = 0; i < count; i++) if (targets[i].samples) {
        targets[i].first_token_ms /= targets[i].samples;
        targets[i].tokens_per_second /= targets[i].samples;
    }
    return result == SQLITE_DONE;
}
