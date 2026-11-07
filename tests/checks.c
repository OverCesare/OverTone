#include "core.h"
#include "rewrite.h"
#include "store.h"
#include "engine.h"
#include "gguf.h"
#include "utf8.h"
#include "library.h"
#include "location.h"
#include "diagnostics.h"
#include "package.h"
#include "process.h"
#include "pull.h"
#include <removefile.h>
#include "recommendations.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "yyjson.h"
#include <copyfile.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <sqlite3.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t operation;
    unsigned requests, cancels;
    char *body;
} transport;
static void requested(void *p, uint64_t op, const char *body) {
    transport *t = p;
    t->operation = op;
    t->requests++;
    free(t->body);
    t->body = strdup(body);
    assert(t->body);
}
static void cancelled(void *p, uint64_t op) {
    transport *t = p;
    assert(op == t->operation);
    t->cancels++;
}
static ot_app *open_at(const char *path, transport *t, const char *function, int line) {
    ot_app *a = ot_open(path, (ot_io){t, requested, cancelled, NULL});
    if (!a)
        fprintf(stderr, "%s:%d: cannot open test workspace\n", function, line);
    assert(a);
    return a;
}
#define open_app(path, state) open_at(path, state, __func__, __LINE__)
static void apply_preferences(ot_app *app, const ot_preferences *preferences) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    assert(d);
    yyjson_mut_doc_set_root(d, ot_preferences_json(d, preferences));
    char *json = yyjson_mut_write(d, 0, NULL);
    assert(json && ot_set_preferences(app, json));
    free(json);
    yyjson_mut_doc_free(d);
}
static bool send_message(ot_app *a, const char *prompt, const char *model, bool ollama) {
    ot_connection c = *ot_configuration(a);
    strcpy(c.model, model);
    c.ollama = ollama;
    /* Provider fixtures explicitly apply their configured parameters after choosing a protocol. */
    ot_preferences fixture = *ot_get_preferences(a);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    assert(d);
    yyjson_mut_doc_set_root(d, ot_preferences_json(d, &fixture));
    char *json = yyjson_mut_write(d, 0, NULL);
    assert(json);
    bool ok = ot_configure(a, &c, 0) && ot_set_preferences(a, json) && ot_submit(a, prompt, 0);
    free(json);
    yyjson_mut_doc_free(d);
    return ok;
}
static void statement(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}
static void has(ot_app *a, const char *value) {
    char *view = ot_view(a);
    assert(view);
    if (!strstr(view, value))
        fprintf(stderr, "Missing expected fixture value: %s\n", value);
    assert(strstr(view, value));
    free(view);
}
static void feed(ot_app *a, transport *t, const char *data, size_t chunk) {
    size_t size = strlen(data);
    for (size_t i = 0; i < size; i += chunk)
        ot_bytes(a, t->operation, data + i, size - i < chunk ? size - i : chunk);
}
static const char *answer = "data: {\"choices\":[{\"delta\":{\"content\":\"Ciao 🌍\\n```c\\nint n = "
                            "1;\\n```\"}}]}\r\n\r\ndata: [DONE]\n\n";

static int64_t value(ot_app *a, const char *key) {
    char *raw = ot_view(a);
    assert(raw);
    yyjson_doc *d = yyjson_read(raw, strlen(raw), 0);
    assert(d);
    int64_t n = yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(d), key));
    yyjson_doc_free(d);
    free(raw);
    return n;
}
static size_t count(ot_app *a, const char *key) {
    char *raw = ot_view(a);
    assert(raw);
    yyjson_doc *d = yyjson_read(raw, strlen(raw), 0);
    assert(d);
    size_t n = yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), key));
    yyjson_doc_free(d);
    free(raw);
    return n;
}
static void absent(ot_app *a, const char *value) {
    char *raw = ot_view(a);
    if (raw && strstr(raw, value))
        fprintf(stderr, "Unexpected fixture value: %s\n", value);
    assert(raw && !strstr(raw, value));
    free(raw);
}
static void complete(ot_app *a, transport *t, const char *content) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    assert(doc);
    yyjson_mut_doc_set_root(doc, yyjson_mut_strcpy(doc, content));
    char *encoded = yyjson_mut_write(doc, 0, NULL);
    assert(encoded);
    char frame[512];
    int n = snprintf(frame, sizeof frame,
                     ot_configuration(a)->ollama
                         ? "{\"message\":{\"content\":%s},\"done\":true}\n"
                         : "data: {\"choices\":[{\"delta\":{\"content\":%s}}]}\n\ndata: [DONE]\n\n",
                     encoded);
    free(encoded);
    yyjson_mut_doc_free(doc);
    assert(n > 0 && (size_t)n < sizeof frame);
    feed(a, t, frame, 3);
    ot_end(a, t->operation, NULL);
}
static void stream_terminal_edges(void) {
    transport t = {0};
    ot_app *a = open_app(":memory:", &t);
    assert(send_message(a, "Prima domanda", "fixture", false));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"content_filter\"}]}\n\n", 7);
    ot_end(a, t.operation, NULL);
    ot_background(a);
    assert(!ot_is_idle(a));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{\"content\":\"Titolo incompleto\"}}]}\n\n", 7);
    ot_end(a, t.operation, NULL);
    has(a, "\"title\":\"Prima domanda\""); /* A previous finish reason cannot complete a new request. */
    assert(ot_select(a, 0) && send_message(a, "Limite a EOF", "fixture", true));
    char frame[2200];
    memset(frame, 'A', 2048);
    frame[2048] = 0;
    char wire[2300];
    snprintf(wire, sizeof wire, "{\"message\":{\"content\":\"%s\"},\"done\":false}\n", frame);
    for (unsigned i = 0; i < 64; i++)
        feed(a, &t, wire, 29);
    feed(a, &t, "{\"message\":{\"content\":\"Z\"},\"done\":true}", 7);
    ot_end(a, t.operation, NULL);
    has(a, "\"error\":\"stream_limit\"");
    has(a, "\"state\":\"failed\"");
    ot_close(a);
    free(t.body);
}
static ot_thinking detect_thinking(const char *json, bool ollama) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    ot_thinking result = ot_thinking_detect(yyjson_doc_get_root(doc), ollama);
    yyjson_doc_free(doc);
    return result;
}
static void feedback_ownership(void) {
    transport t = {0};
    ot_app *a = open_app(":memory:", &t);
    ot_chat_visible(a, true);
    assert(send_message(a, "Prima chat", "fixture", false));
    complete(a, &t, "Risposta valida");
    int64_t first = value(a, "selected");
    assert(ot_select(a, 0) && send_message(a, "Seconda chat", "fixture", false));
    complete(a, &t, "Altra risposta");
    int64_t second = value(a, "selected"), head = value(a, "head");
    assert(ot_select(a, first) && ot_submit(a, "Richiesta in background", 0));
    int64_t failed = value(a, "activeTurn");
    assert(ot_select(a, second));
    ot_end(a, t.operation, "http_503");
    assert(value(a, "selected") == second && value(a, "head") == head);
    assert(value(a, "errorChat") == first && value(a, "errorTurn") == failed);
    has(a, "\"unread\":true");
    int64_t revision = value(a, "errorRevision");
    assert(ot_page(a, second, head));
    assert(value(a, "errorRevision") == revision); /* Paging is not a new failure. */
    assert(ot_select(a, second) && ot_submit(a, NULL, head));
    int64_t alternate = value(a, "activeTurn");
    ot_end(a, t.operation, "http_503");
    assert(value(a, "errorChat") == second && value(a, "errorTurn") == alternate);
    assert(value(a, "head") == head); /* A failed alternative must remain reportable. */
    assert(ot_select(a, second));
    assert(value(a, "errorChat") == 0 && value(a, "errorTurn") == 0);
    assert(!ot_page(a, second, -1));
    assert(value(a, "errorChat") == second && value(a, "errorTurn") == 0);
    revision = value(a, "errorRevision");
    ot_resources_update(a, ot_resources_get(a));
    assert(value(a, "errorRevision") == revision);
    assert(!ot_page(a, second, -1));
    assert(value(a, "errorRevision") > revision); /* Same code, distinct failed attempt. */
    ot_close(a);
    free(t.body);
}
static void thinking_controls(void) {
    struct {
        const char *json;
        bool ollama, known;
        unsigned count;
    } cases[] = {{"{}", true, false, 0},
                 {"{\"capabilities\":[\"completion\"]}", true, true, 0},
                 {"{\"capabilities\":[\"thinking\\u0000extra\"]}", true, false, 0},
                 {"{\"capabilities\":[17]}", true, false, 0},
                 {"{\"thinking\":{\"values\":[false]}}", true, true, 0},
                 {"{\"thinking\":{\"values\":[true]}}", true, true, 1},
                 {"{\"thinking\":{\"values\":[false,true]}}", true, true, 2},
                 {"{\"thinking\":{\"values\":[\"high\"]}}", true, true, 1},
                 {"{\"thinking\":{\"values\":[\"minimal\",\"maximum\"]}}", true, true, 2},
                 {"{\"thinking\":{\"values\":[]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"low\",4]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"low\",\"low\"]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"auto\"]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"a b\"]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"low\\u0000high\"]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"12345678901234567890123456789012\"]}}", true, false, 0},
                 {"{\"thinking\":{\"values\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}}", true,
                  false, 0},
                 {"{\"capabilities\":[\"thinking\"],\"details\":{\"family\":\"qwen3\"}}", true, true, 2},
                 {"{\"capabilities\":[\"thinking\"],\"details\":{\"family\":\"gptoss\"}}", true, true, 3},
                 {"{\"capabilities\":[\"thinking\"],\"thinking\":{\"values\":[false]}}", true, true, 0},
                 {"{\"chat_template\":\"enable_thinking\"}", false, true, 2},
                 {"{\"chat_template\":\"reasoning_effort == 'high'\"}", false, true, 1},
                 {"{\"chat_template\":\"reasoning_effort\"}", false, false, 0},
                 {"{\"chat_template\":\"plain template\"}", false, false, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ot_thinking result = detect_thinking(cases[i].json, cases[i].ollama);
        assert(result.known == cases[i].known && result.count == cases[i].count);
    }
    ot_thinking modes = detect_thinking("{\"thinking\":{\"values\":[false,\"minimal\",\"maximum\"]}}", true);
    assert(ot_thinking_find(&modes, "off")->type == OT_THINKING_FALSE);
    assert(ot_thinking_find(&modes, "maximum")->type == OT_THINKING_LEVEL);
    assert(!ot_thinking_find(&modes, "high"));
    transport t = {0};
    ot_app *app = open_app(":memory:", &t);
    ot_connection connection = *ot_configuration(app);
    strcpy(connection.model, "fixture");
    connection.ollama = true;
    assert(ot_configure(app, &connection, 0));
    ot_thinking_capability(app, &modes);
    assert(ot_set_preferences(app, "{\"thinkingMode\":\"maximum\"}"));
    has(app, "\"thinkingOptions\":[\"off\",\"minimal\",\"maximum\"]");
    assert(ot_submit(app, "Prova livello dichiarato", 0));
    assert(strstr(t.body, "\"think\":\"maximum\""));
    complete(app, &t, "Risposta");
    has(app, "\"requestedThinking\":\"maximum\"");
    assert(!ot_chat_thinking(app, "high"));
    assert(ot_chat_thinking(app, "minimal"));
    assert(ot_submit(app, "Livello della conversazione", 0));
    assert(strstr(t.body, "\"think\":\"minimal\""));
    complete(app, &t, "Risposta");
    connection.ollama = false;
    assert(ot_configure(app, &connection, 0));
    ot_thinking_capability(app, &modes);
    assert(ot_submit(app, "Trasporto compatibile", 0));
    assert(strstr(t.body, "\"reasoning_effort\":\"minimal\""));
    complete(app, &t, "Risposta");
    modes = detect_thinking("{\"thinking\":{\"values\":[false]}}", true);
    ot_thinking_capability(app, &modes);
    assert(ot_submit(app, "Modello senza ragionamento", 0));
    assert(!strstr(t.body, "reasoning_effort") && !strstr(t.body, "enable_thinking"));
    complete(app, &t, "Risposta");
    assert(ot_select(app, 0));
    assert(ot_submit(app, "Crea una calcolatrice in C", 0));
    complete(app, &t, "Risposta");
    ot_background(app);
    complete(app, &t, "```c\nint main(void) { return 0; }\n```");
    has(app, "\"title\":\"Crea una calcolatrice in C\"");
    uint64_t last_title = t.operation;
    ot_background(app);
    assert(t.operation == last_title); /* Rejected code does not cause an idle retry loop. */
    assert(ot_select(app, 0));
    assert(ot_submit(app, "Perché il cielo appare blu?", 0));
    complete(app, &t, "Risposta");
    ot_background(app);
    complete(app, &t,
             "Il cielo appare blu perché la luce solare attraversa l'atmosfera e viene diffusa "
             "in modo diverso secondo la sua lunghezza d'onda. Questa è una spiegazione, "
             "non un titolo della conversazione.");
    has(app, "\"title\":\"Perché il cielo appare blu?\"");
    last_title = t.operation;
    ot_background(app);
    assert(t.operation == last_title);
    ot_close(app);
    free(t.body);
}
static void browsing_and_readiness(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    ot_connection c = *ot_configuration(a);
    strcpy(c.model, "qwen");
    c.ollama = true;
    assert(ot_configure(a, &c, 0));
    ot_connection_ready(a, false);
    assert(!ot_submit(a, "Non inviare a una configurazione precedente", 0) && !t.requests);
    ot_connection_ready(a, true);
    ot_thinking modes = {.known = true};
    ot_thinking_capability(a, &modes);
    assert(ot_set_preferences(a, "{\"thinkingMode\":\"high\"}"));
    assert(ot_submit(a, "Conserva la risposta interrotta", 0));
    assert(ot_set_preferences(a, "{\"thinkingMode\":\"high\",\"theme\":\"dark\"}"));
    assert(!strcmp(ot_get_preferences(a)->theme, "dark"));
    assert(!ot_set_preferences(a, "{\"context\":16384}"));
    assert(ot_get_preferences(a)->context == 8192);
    assert(value(a, "inputLimitBytes") > 0 && value(a, "inputLimitBytes") < 8192);
    assert(ot_page(a, value(a, "selected"), 0) && !t.cancels);
    assert(!ot_page(a, 999, 0) && !t.cancels);
    assert(!strstr(t.body, "\"think\":")); /* Unsupported preference uses the model default. */
    feed(a, &t, "{\"message\":{\"content\":\"Testo parziale durevole\"}}\n", 3);
    ot_stop(a);
    ot_end(a, t.operation, "cancelled");
    ot_close(a);
    a = open_app(path, &t);
    has(a, "Testo parziale durevole");
    modes =
        (ot_thinking){.known = true, .count = 1, .options = {{.value = "high", .type = OT_THINKING_LEVEL}}};
    ot_thinking_capability(a, &modes);
    assert(ot_submit(a, "Continua", 0));
    assert(strstr(t.body, "Testo parziale durevole") && strstr(t.body, "\"think\":\"high\""));
    feed(a, &t, "{\"message\":{\"content\":\"Completato\"},\"done\":true}\n", 5);
    ot_end(a, t.operation, NULL);
    statement(path, "UPDATE turns SET created=100;"
                    "INSERT INTO chats(id,title) VALUES(2,'Vuota'),(3,'Codice'),(4,'Errore');"
                    "INSERT INTO turns(chat,prompt,answer,state,error,created,model,runtime) VALUES"
                    "(3,'esempio','```c int n;```','complete','',200,'modelA','openai'),"
                    "(4,'richiesta','','failed','http_500',300,'modelB','openai');");
    assert(value(a, "chatTotal") == 4);
    assert(ot_search_filters(a, "{\"maximum\":0}") && count(a, "chats") == 1);
    assert(ot_search_filters(a, "{\"minimum\":4}") && count(a, "chats") == 1);
    assert(ot_search_filters(a, "{\"code\":true}") && count(a, "chats") == 1);
    assert(ot_search_filters(a, "{\"errors\":true}") && count(a, "chats") == 1);
    assert(ot_search_filters(a, "{\"models\":[\"modelA\"]}") && count(a, "chats") == 1);
    assert(ot_search_filters(a, "{\"runtimes\":[\"openai\"]}") && count(a, "chats") == 2);
    assert(ot_search_filters(a, "{\"from\":200,\"until\":300}") && count(a, "chats") == 1);
    assert(!ot_search_filters(a, "{\"from\":300,\"until\":200}") && count(a, "chats") == 1);
    assert(!ot_search_filters(a, "{\"models\":[\"modelA\\u0000other\"]}"));
    assert(ot_search_filters(a, "{}") && ot_search(a, "esempio", 0) && count(a, "chats") == 1);
    has(a, "modelA");
    int64_t selected = value(a, "selected");
    assert(ot_draft(a, selected, "Bozza di navigazione"));
    assert(ot_classify(a, 1));
    ot_close(a);
    a = open_app(path, &t);
    assert(value(a, "selected") == selected && value(a, "filter") == 1);
    assert(!ot_submit(a, "Non scrivere dopo la riapertura", 0));
    has(a, "chat_read_only");
    has(a, "Bozza di navigazione");
    statement(path, "WITH RECURSIVE n(x) AS (VALUES(100) UNION ALL SELECT x+1 FROM n WHERE x<139) "
                    "INSERT INTO chats(id,title) SELECT x,'Più recente' FROM n;"
                    "INSERT INTO turns(chat,prompt,answer,state,created) "
                    "SELECT id,'Test','Test','complete',400 FROM chats WHERE id BETWEEN 100 AND 139;");
    assert(ot_classify(a, 0));
    assert(value(a, "selected") == selected && value(a, "filter") == 0);
    assert(value(a, "chatRevealIndex") == 42 && value(a, "chatOffset") == 30);
    assert(ot_draft(a, selected, "Scrittura nuovamente disponibile"));
    ot_close(a);
    free(t.body);
    unlink(path);
}
static void language_hint(void *context, const char *prompt, char tag[16]) {
    (void)context;
    assert(prompt && *prompt);
    strcpy(tag, "it");
}
static void empty_trash(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(ot_empty_trash(a) && value(a, "trashTotal") == 0);
    assert(ot_select(a, 0) && send_message(a, "Conversazione attiva", "model", false));
    assert(value(a, "filter") == 0);
    complete(a, &t, "Da conservare");
    int64_t active = value(a, "selected");
    assert(ot_select(a, 0) && ot_submit(a, "Conversazione archiviata", 0));
    complete(a, &t, "Da conservare in archivio");
    int64_t archived = value(a, "selected");
    assert(ot_classify(a, 1));
    int64_t discarded[2];
    for (unsigned i = 0; i < 2; i++) {
        assert(ot_select(a, 0) && ot_submit(a, "Contenuto da eliminare", 0));
        complete(a, &t, "Risposta nel cestino");
        int64_t head = value(a, "head");
        assert(ot_submit(a, NULL, head));
        complete(a, &t, "Variante nel cestino");
        discarded[i] = value(a, "selected");
        assert(ot_classify(a, 2));
    }
    statement(path, "WITH RECURSIVE n(i) AS (VALUES(100) UNION ALL SELECT i+1 FROM n WHERE i<134) "
                    "INSERT INTO chats(id,title,deleted) SELECT i,'Cestino oltre pagina uno',1 FROM n");
    assert(value(a, "trashTotal") == 37 && value(a, "chatBefore") > 0);
    assert(ot_search(a, "nessuna-corrispondenza", 0));
    assert(value(a, "chatTotal") == 0 && value(a, "trashTotal") == 37);
    statement(path, "CREATE TRIGGER fail_empty BEFORE DELETE ON chats BEGIN "
                    "SELECT RAISE(ABORT,'fixture'); END");
    assert(!ot_empty_trash(a));
    has(a, "storage_write");
    assert(value(a, "selected") == discarded[1] && value(a, "trashTotal") == 37);
    statement(path, "DROP TRIGGER fail_empty");
    assert(ot_empty_trash(a));
    assert(value(a, "selected") == 0 && value(a, "filter") == 2 && value(a, "trashTotal") == 0 &&
           value(a, "chatTotal") == 0);
    for (unsigned i = 0; i < 2; i++)
        assert(!ot_select(a, discarded[i]));
    assert(ot_select(a, archived));
    has(a, "Da conservare in archivio");
    has(a, "\"archived\":true");
    assert(ot_select(a, active));
    has(a, "Da conservare");
    statement(path, "INSERT INTO search(search,rank) VALUES('integrity-check',1)");
    assert(ot_submit(a, "Generazione indipendente", 0));
    assert(ot_select(a, archived) && ot_classify(a, 2));
    assert(ot_empty_trash(a) && !t.cancels && value(a, "activeChat") == active);
    complete(a, &t, "Risposta conservata dopo svuotamento");
    assert(ot_select(a, active));
    has(a, "Risposta conservata dopo svuotamento");
    assert(ot_submit(a, "Generazione da eliminare", 0) && ot_classify(a, 2));
    uint64_t operation = t.operation;
    assert(ot_select(a, 0) && ot_empty_trash(a));
    assert(t.cancels == 1 && !value(a, "activeChat") && !value(a, "trashTotal"));
    ot_end(a, operation, "late_callback");
    assert(!ot_select(a, active) && ot_empty_trash(a));
    statement(path, "INSERT INTO search(search,rank) VALUES('integrity-check',1)");
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
}
static char *diagnostic_snapshot(void *context, const char *request) {
    return !strcmp(request, "inspect") || !strcmp(request, "control fill prompt Caffè 中文") ? strdup(context) : NULL;
}
static int diagnostic_client(const struct sockaddr_un *address) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct timeval timeout = {.tv_sec = 1};
    assert(fd >= 0 && !setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) &&
           !connect(fd, (const struct sockaddr *)address, sizeof *address));
    return fd;
}
static void diagnostic_rejected(int fd) {
    char byte;
    ssize_t n = read(fd, &byte, 1);
    assert(!n || (n < 0 && errno == ECONNRESET));
    assert(!close(fd));
}
static void diagnostic_lifecycle(const char *directory) {
    ot_diagnostics *server = ot_diagnostics_open(directory, diagnostic_snapshot, "{\"ok\":true}");
    assert(server);
    char manifest[4096], socket_path[108] = {0};
    snprintf(manifest, sizeof manifest, "%s/socket.path", directory);
    FILE *file = fopen(manifest, "r");
    assert(file && fgets(socket_path, sizeof socket_path, file) && !fclose(file));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, socket_path);
    int fd = diagnostic_client(&address);
    assert(write(fd, "inspect\n", 8) == 8 && !shutdown(fd, SHUT_WR));
    char received[32] = {0};
    size_t size = 0;
    ssize_t n;
    while ((n = read(fd, received + size, sizeof received - size - 1)) > 0)
        size += (size_t)n;
    assert(!n && !strcmp(received, "{\"ok\":true}") && !close(fd));
    const char unicode[] = "control fill prompt Caffè 中文\n";
    fd = diagnostic_client(&address);
    assert(write(fd, unicode, sizeof unicode - 1) == sizeof unicode - 1 && !shutdown(fd, SHUT_WR));
    memset(received, 0, sizeof received);
    assert(read(fd, received, sizeof received - 1) > 0 && !strcmp(received, "{\"ok\":true}") && !close(fd));
    /* Reject malformed, oversized and stalled requests; never call the snapshot. */
    const char *bad[] = {"inspect\nextra\n", "inspect\r\n", "inspect\001\n", "inspect\177\n", "control fill prompt \xc3\n", "control fill prompt \xc0\xaf\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        fd = diagnostic_client(&address);
        assert(write(fd, bad[i], strlen(bad[i])) == (ssize_t)strlen(bad[i]));
        assert(!shutdown(fd, SHUT_WR));
        diagnostic_rejected(fd);
    }
    fd = diagnostic_client(&address);
    char oversized[OT_DIAGNOSTICS_REQUEST_BYTES];
    memset(oversized, 'a', sizeof oversized);
    assert(write(fd, oversized, sizeof oversized) == sizeof oversized);
    diagnostic_rejected(fd);
    fd = diagnostic_client(&address);
    assert(write(fd, "i", 1) == 1);
    diagnostic_rejected(fd);
    fd = diagnostic_client(&address);
    ot_diagnostics_close(server); /* Must wake and join an otherwise idle listener. */
    diagnostic_rejected(fd);
    assert(access(manifest, F_OK) && access(socket_path, F_OK));
    server = ot_diagnostics_open(directory, diagnostic_snapshot, "{}");
    assert(server);
    ot_diagnostics_close(server); /* Also valid before its thread first waits. */
}
static void restore_archive(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(ot_restore_archive(a) && value(a, "archiveTotal") == 0);
    assert(send_message(a, "Da conservare nel cestino", "model", false));
    complete(a, &t, "Risposta nel cestino");
    int64_t discarded = value(a, "selected");
    assert(ot_classify(a, 2));
    assert(ot_select(a, 0) && ot_submit(a, "Da ripristinare", 0));
    complete(a, &t, "Risposta archiviata");
    int64_t first = value(a, "head"), archived = value(a, "selected");
    assert(ot_submit(a, NULL, first));
    complete(a, &t, "Alternativa da conservare");
    int64_t head = value(a, "head");
    assert(ot_classify(a, 1));
    statement(path, "WITH RECURSIVE n(i) AS (VALUES(100) UNION ALL SELECT i+1 FROM n WHERE i<134) "
                    "INSERT INTO chats(id,title,archived) "
                    "SELECT i,'Archivio oltre pagina uno',1 FROM n;"
                    "INSERT INTO turns(chat,prompt,state,created) "
                    "SELECT id,'Archivio oltre pagina uno','complete',9999999999 "
                    "FROM chats WHERE id>=100");
    assert(ot_classify(a, 1) && value(a, "chatOffset") > 0);
    assert(value(a, "archiveTotal") == 36 && value(a, "trashTotal") == 1);
    assert(ot_search(a, "nessuna-corrispondenza", 0) && !value(a, "chatTotal"));
    statement(path, "CREATE TRIGGER fail_restore BEFORE UPDATE OF archived ON chats "
                    "WHEN OLD.archived=1 AND OLD.id=134 BEGIN SELECT RAISE(FAIL,'fixture'); END");
    assert(!ot_restore_archive(a));
    has(a, "storage_write");
    assert(value(a, "archiveTotal") == 36 && value(a, "selected") == archived && value(a, "head") == head &&
           value(a, "filter") == 1);
    statement(path, "DROP TRIGGER fail_restore");
    assert(ot_restore_archive(a));
    assert(!value(a, "archiveTotal") && value(a, "trashTotal") == 1 && value(a, "selected") == archived &&
           value(a, "head") == head && !value(a, "filter") && value(a, "chatOffset") > 0);
    has(a, "\"archived\":false");
    has(a, "Alternativa da conservare");
    assert(ot_search(a, "Alternativa", 0) && value(a, "chatTotal") == 1);
    assert(ot_select(a, discarded));
    has(a, "\"deleted\":true");
    assert(ot_select(a, archived) && ot_submit(a, "Generazione in archivio", 0));
    assert(ot_classify(a, 1) && ot_restore_archive(a));
    assert(!t.cancels && value(a, "activeChat") == archived);
    complete(a, &t, "Risposta dopo il ripristino");
    has(a, "Risposta dopo il ripristino");
    assert(ot_restore_archive(a) && !value(a, "archiveTotal"));
    statement(path, "INSERT INTO search(search,rank) VALUES('integrity-check',1)");
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
}
static void independent_generation(const char *path) {
    transport t = {0};
    ot_app *a = ot_open(path, (ot_io){&t, requested, cancelled, language_hint});
    assert(a && send_message(a, "Prima domanda", "model", false));
    yyjson_doc *request = yyjson_read(t.body, strlen(t.body), 0);
    yyjson_val *messages = yyjson_obj_get(yyjson_doc_get_root(request), "messages"),
               *system = yyjson_arr_get_first(messages), *question = yyjson_arr_get_last(messages);
    assert(yyjson_equals_str(yyjson_obj_get(system, "role"), "system") &&
           strstr(yyjson_get_str(yyjson_obj_get(system, "content")), "Italian") &&
           yyjson_equals_str(yyjson_obj_get(question, "role"), "user") &&
           yyjson_equals_str(yyjson_obj_get(question, "content"), "Prima domanda"));
    yyjson_doc_free(request);
    time_t before_reply = time(NULL);
    complete(a, &t, "Prima risposta");
    char *completed_view = ot_view(a);
    yyjson_doc *completed_doc = yyjson_read(completed_view, strlen(completed_view), 0);
    yyjson_val *first = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(completed_doc), "turns"));
    assert(yyjson_equals_str(yyjson_obj_get(first, "prompt"), "Prima domanda"));
    int64_t received = yyjson_get_sint(yyjson_obj_get(yyjson_obj_get(first, "completion"), "receivedAt"));
    assert(received >= (int64_t)before_reply && received <= (int64_t)time(NULL));
    yyjson_doc_free(completed_doc);
    free(completed_view);
    has(a, "Prima domanda");
    ot_background(a);
    has(a, "\"auxiliary\":true");
    assert(value(a, "activeChat") == 0);
    complete(a, &t, "Prima domanda");
    int64_t first_chat = value(a, "selected"), first_turn = value(a, "head");
    assert(ot_select(a, 0) && ot_submit(a, "Altra chat", 0));
    complete(a, &t, "Risposta separata");
    int64_t second_chat = value(a, "selected");
    assert(ot_select(a, first_chat) && ot_submit(a, NULL, first_turn));
    int64_t generated = value(a, "activeTurn");
    assert(value(a, "activeChat") == first_chat && generated != first_turn);
    uint64_t operation = t.operation;
    assert(ot_select(a, second_chat) && ot_rename(a, "Titolo durante lo stream"));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{\"content\":\"Nuovo testo parziale\"}}]}\n\n", 3);
    has(a, "Risposta separata");
    absent(a, "Nuovo testo parziale");
    char *progress = ot_progress(a);
    assert(progress && strstr(progress, "Nuovo testo parziale"));
    free(progress);
    assert(ot_classify(a, 1) && !t.cancels && value(a, "activeChat") == first_chat);
    assert(ot_select(a, first_chat) && ot_choose(a, first_turn));
    assert(value(a, "head") == first_turn);
    absent(a, "Nuovo testo parziale");
    assert(ot_choose(a, generated));
    has(a, "Nuovo testo parziale");
    assert(ot_choose(a, first_turn));
    feed(a, &t, "data: [DONE]\n\n", 1);
    ot_end(a, operation, NULL);
    assert(!t.cancels && value(a, "head") == first_turn);
    assert(ot_choose(a, generated));
    has(a, "Nuovo testo parziale");
    assert(ot_submit(a, "Da eliminare mentre genera", 0));
    operation = t.operation;
    assert(ot_classify(a, 3) && t.cancels == 1 && !value(a, "activeChat"));
    ot_end(a, operation, "late_callback");
    assert(!ot_select(a, first_chat));
    assert(ot_select(a, second_chat));
    has(a, "Titolo durante lo stream");
    assert(ot_classify(a, 0) && ot_select(a, second_chat) &&
           ot_submit(a, "Completa mentre leggo un'altra chat", 0));
    assert(ot_select(a, 0));
    complete(a, &t, "Risposta non ancora letta");
    assert(value(a, "activeChat") == 0);
    has(a, "\"unread\":true");
    ot_background(a); /* Auxiliary title work must not advertise a running chat response. */
    assert(value(a, "activeChat") == 0);
    ot_close(a);
    a = open_app(path, &t);
    has(a, "\"unread\":true");
    assert(ot_select(a, second_chat));
    absent(a, "\"unread\":true");
    assert(ot_submit(a, "Completa mentre sono nella libreria modelli", 0));
    ot_chat_visible(a, false);
    complete(a, &t, "Completata fuori dalla schermata chat");
    has(a, "\"unread\":true");
    ot_close(a);
    a = open_app(path, &t);
    has(a, "\"unread\":true");
    ot_chat_visible(a, true);
    absent(a, "\"unread\":true");
    assert(ot_set_preferences(a, "{\"startupModel\":-1,\"memoryPercent\":100}"));
    assert(ot_get_preferences(a)->startup_model == -1 && ot_get_preferences(a)->memory_percent == 100);
    ot_start_without_model(a);
    assert(!*ot_configuration(a)->model && !ot_submit(a, "Non avviare", 0));
    ot_close(a);
    a = open_app(path, &t);
    assert(ot_get_preferences(a)->startup_model == -1 && *ot_configuration(a)->model);
    ot_close(a);
    free(t.body);
    unlink(path);
}
static void repeated_generation(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(send_message(a, "Spiega il risultato", "model", true));
    for (unsigned i = 0; !t.cancels && i < 80; i++)
        feed(a, &t, "{\"message\":{\"content\":\"Il risultato è corretto e posso proseguire. \"}}\n", 3);
    assert(t.cancels == 1);
    ot_end(a, t.operation, "cancelled");
    has(a, "\"error\":\"repetition\"");
    assert(ot_submit(a, "Genera dati ripetitivi in un blocco di codice", 0));
    feed(a, &t, "{\"message\":{\"content\":\"```text\\n\"}}\n", 3);
    for (unsigned i = 0; i < 40; i++)
        feed(a, &t, "{\"message\":{\"content\":\"Il risultato è corretto e posso proseguire.\\n\"}}\n", 3);
    assert(t.cancels == 1);
    feed(a, &t, "{\"message\":{\"content\":\"```\"},\"done\":true}\n", 3);
    ot_end(a, t.operation, NULL);
    has(a, "\"state\":\"complete\"");
    assert(send_message(a, "Mostra il programma completo", "model", false));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Let's design. ```c\\n\"}}]}\n\n", 3);
    unsigned previous_cancels = t.cancels;
    for (unsigned i = 0; t.cancels == previous_cancels && i < 80; i++)
        feed(a, &t,
             "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Need `db_clear` to free each bucket. "
             "`db_close` calls `db_clear`, then frees the array.\\n\"}}]}\n\n",
             3);
    assert(t.cancels == previous_cancels + 1);
    ot_end(a, t.operation, "cancelled");
    has(a, "\"error\":\"repetition\"");
    assert(send_message(a, "Dati seguiti da spiegazione", "model", true));
    feed(a, &t, "{\"message\":{\"content\":\"```text\\n\"}}\n", 3);
    previous_cancels = t.cancels;
    for (unsigned i = 0; i < 40; i++)
        feed(a, &t, "{\"message\":{\"content\":\"Il risultato è corretto e posso proseguire.\\n\"}}\n", 3);
    assert(t.cancels == previous_cancels);
    feed(a, &t, "{\"message\":{\"content\":\"``\"}}\n", 3);
    feed(a, &t, "{\"message\":{\"content\":\"`\\n\"}}\n", 3);
    for (unsigned i = 0; t.cancels == previous_cancels && i < 80; i++)
        feed(a, &t, "{\"message\":{\"content\":\"Il risultato è corretto e posso proseguire. \"}}\n", 3);
    assert(t.cancels == previous_cancels + 1);
    ot_end(a, t.operation, "cancelled");
    has(a, "\"error\":\"repetition\"");
    assert(ot_submit(a, "Una richiesta rimasta senza risposta", 0));
    feed(a, &t,
         "{\"message\":{\"thinking\":\"Ragionamento senza risposta "
         "finale\"},\"done_reason\":\"length\",\"done\":true}\n",
         3);
    ot_end(a, t.operation, NULL);
    yyjson_mut_doc *context = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *messages = yyjson_mut_arr(context);
    bool trimmed;
    assert(ot_store_context(ot_app_store(a), value(a, "selected"), value(a, "head"), context, messages,
                            100000, 1, &trimmed) == 1);
    assert(yyjson_mut_arr_size(messages) == 2);
    yyjson_mut_val *prior = yyjson_mut_arr_get(messages, 1);
    assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(prior, "role")), "assistant"));
    assert(yyjson_mut_get_len(yyjson_mut_obj_get(prior, "content")) > 0);
    yyjson_mut_doc_free(context);
    assert(send_message(a, "中文说明", "model", true));
    previous_cancels = t.cancels;
    for (unsigned i = 0; t.cancels == previous_cancels && i < 80; i++)
        feed(a, &t, "{\"message\":{\"content\":\"这个结果正确可以继续处理这个问题。\"}}\n", 3);
    assert(t.cancels == previous_cancels + 1);
    ot_end(a, t.operation, "cancelled");
    has(a, "\"error\":\"repetition\"");
    ot_close(a);
    free(t.body);
    unlink(path);
}
static void continuity(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(send_message(a, "Primo", "chosen-model", false));
    assert(strstr(t.body, "\"stream_options\":{\"include_usage\":true}"));
    feed(a, &t,
         "data: "
         "{\"choices\":[{\"delta\":{\"content\":\"Caffè\"},\"finish_reason\":\"stop\"}],\"usage\":null}\n\n"
         "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":31,\"completion_tokens\":7},"
         "\"timings\":{\"cache_n\":17,\"prompt_ms\":12.5,\"predicted_ms\":25.25}}\n\ndata: [DONE]\n\n",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "\"inputTokens\":31");
    has(a, "\"outputTokens\":7");
    has(a, "\"evalMs\":25.25");
    has(a, "\"cachedInputTokens\":17");
    ot_close(a);
    a = open_app(path, &t);
    has(a, "Caffè");
    has(a, "\"outputTokens\":7");
    assert(ot_search(a, "CAFFE", 0));
    assert(count(a, "chats") == 1);
    assert(ot_search(a, "", 0));
    assert(ot_select(a, 0));
    assert(send_message(a, "Alpha", "chosen-model", false));
    complete(a, &t, "Originale città");
    int64_t original = value(a, "head"), chat = value(a, "selected");
    assert(ot_submit(a, "Beta", 0));
    complete(a, &t, "Boccia blu");
    int64_t continuation = value(a, "head");
    assert(ot_submit(a, NULL, original));
    assert(!strstr(t.body, "Beta") && !strstr(t.body, "Originale"));
    complete(a, &t, "Alternativa verde");
    int64_t alternative = value(a, "head");
    assert(ot_submit(a, "Delta", 0));
    complete(a, &t, "Ramo nuovo");
    int64_t alternative_tip = value(a, "head");
    assert(ot_choose(a, original));
    assert(value(a, "head") == continuation);
    has(a, "Boccia blu");
    absent(a, "Ramo nuovo");
    assert(ot_choose(a, alternative));
    assert(value(a, "head") == alternative_tip);
    has(a, "Ramo nuovo");
    absent(a, "Boccia blu");
    char *snapshot = ot_view(a);
    yyjson_doc *snapshot_doc = yyjson_read(snapshot, strlen(snapshot), 0);
    assert(snapshot_doc);
    yyjson_val *snapshot_root = yyjson_doc_get_root(snapshot_doc);
    yyjson_val *first_turn = yyjson_arr_get_first(yyjson_obj_get(snapshot_root, "turns"));
    assert(yyjson_get_int(yyjson_obj_get(first_turn, "variantIndex")) == 2);
    assert(yyjson_get_int(yyjson_obj_get(first_turn, "versions")) == 2);
    assert(yyjson_get_int(yyjson_obj_get(first_turn, "userVersions")) == 1);
    assert(yyjson_get_int(yyjson_obj_get(first_turn, "created")) > 0);
    yyjson_val *first_chat = yyjson_arr_get_first(yyjson_obj_get(snapshot_root, "chats"));
    assert(yyjson_get_int(yyjson_obj_get(first_chat, "messages")) == 8);
    assert(yyjson_get_int(yyjson_obj_get(first_chat, "updated")) > 0);
    yyjson_doc_free(snapshot_doc);
    free(snapshot);
    unsigned requests = t.requests;
    assert(!ot_submit(a, "Non appartiene al ramo", continuation));
    assert(t.requests == requests);
    assert(ot_choose(a, original));
    assert(ot_submit(a, "Alpha modificato", original));
    int64_t cancelled = value(a, "head");
    ot_stop(a);
    ot_end(a, t.operation, NULL);
    assert(value(a, "head") == continuation);
    has(a, "Originale città");
    assert(ot_choose(a, cancelled));
    has(a, "Alpha modificato");
    has(a, "cancelled");
    has(a, "\"userVersions\":2");
    has(a, "\"userVariantIndex\":2");
    has(a, "\"versions\":1");
    assert(ot_choose(a, original));
    assert(ot_submit(a, "Beta modificato", continuation));
    assert(strstr(t.body, "Originale città") && !strstr(t.body, "Boccia blu") &&
           !strstr(t.body, "Alternativa verde"));
    complete(a, &t, "Risposta nuova");
    int64_t edited = value(a, "head");
    assert(ot_rename(a, "Titolo manuale viaggio"));
    ot_connection c = *ot_configuration(a);
    strcpy(c.endpoint, "https://api.example.invalid");
    c.remote = true;
    assert(ot_configure(a, &c, 0));
    ot_close(a);
    a = open_app(path, &t);
    assert(value(a, "head") == edited && value(a, "selected") == chat);
    assert(strcmp(ot_configuration(a)->endpoint, c.endpoint) == 0 && ot_configuration(a)->remote);
    has(a, "Titolo manuale viaggio");
    statement(path, "CREATE TRIGGER reject_settings BEFORE UPDATE ON settings WHEN new.model='rejected' "
                    "BEGIN SELECT RAISE(ABORT,'fault');END");
    strcpy(c.model, "rejected");
    assert(!ot_configure(a, &c, 0));
    assert(strcmp(ot_configuration(a)->model, "chosen-model") == 0);
    assert(ot_search(a, "boccia", 0));
    assert(count(a, "chats") == 1); /* Finds a nonselected version. */
    char *raw = ot_view(a);
    yyjson_doc *d = yyjson_read(raw, strlen(raw), 0);
    yyjson_val *found = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(d), "chats"));
    assert(yyjson_get_sint(yyjson_obj_get(found, "match")) == continuation);
    yyjson_doc_free(d);
    free(raw);
    assert(ot_search(a, "VIAGGIO", 0));
    assert(count(a, "chats") == 1);
    assert(ot_search(a, "\" OR *", 0));
    raw = ot_view(a);
    assert(raw);
    free(raw); /* Literal phrase, not FTS syntax. */
    assert(ot_search(a, "", 0));
    for (unsigned i = 0; i < 35; i++) {
        assert(ot_select(a, 0));
        assert(ot_submit(a, "Pagina", 0));
        complete(a, &t, "Testo");
    }
    assert(count(a, "chats") == 30);
    int64_t before = value(a, "chatBefore");
    assert(before > 0);
    assert(ot_search(a, "", before));
    assert(count(a, "chats") == 7);
    assert(value(a, "chatBefore") == 0);
    assert(ot_search(a, "", 9000));
    assert(count(a, "chats") == 0 && value(a, "chatTotal") == 37);
    assert(ot_search(a, "", 0));
    for (unsigned i = 0; i < 35; i++) {
        assert(ot_submit(a, "Turno", 0));
        complete(a, &t, "Risposta");
    }
    size_t window = count(a, "turns");
    assert(window > 0 && window < 36);
    int64_t older = value(a, "older");
    assert(older > 0);
    assert(ot_page(a, value(a, "selected"), older));
    size_t reading = count(a, "turns");
    assert(reading > 0 && reading <= window);
    assert(value(a, "newer") == value(a, "head"));
    assert(ot_submit(a, "Invio mentre leggo una pagina precedente", 0));
    assert(value(a, "page") == older && count(a, "turns") == reading);
    complete(a, &t, "La lettura rimane sulla pagina precedente");
    assert(value(a, "page") == older && count(a, "turns") == reading);
    assert(ot_page(a, value(a, "selected"), 0));
    assert(count(a, "turns") == window);
    assert(ot_submit(a, "Invio dal fondo dopo il prefetch", 0));
    assert(value(a, "page") == 0 && count(a, "turns") == window);
    complete(a, &t, "Risposta nuova visibile alla coda");
    has(a, "Risposta nuova visibile alla coda");
    size_t visited = count(a, "turns");
    while ((older = value(a, "older"))) {
        assert(ot_page(a, value(a, "selected"), older));
        size_t page_size = count(a, "turns");
        assert(page_size > 0 && page_size <= window && value(a, "older") < older);
        visited += page_size;
        assert(visited <= 38);
    }
    assert(visited == 38); /* Every ancestor appears once, independently of page size. */
    while (value(a, "newer")) {
        int64_t next = value(a, "newer"), previous = value(a, "page");
        assert(next > previous && next <= value(a, "head"));
        assert(ot_page(a, value(a, "selected"), next));
        assert(value(a, "page") == next || !value(a, "page"));
        assert(value(a, "older") <= previous); /* An overlapping tail is valid; a skipped page is not. */
    }
    assert(!value(a, "page"));
    has(a, "Risposta nuova visibile alla coda");
    assert(ot_select(a, chat));
    assert(ot_choose(a, original));
    assert(value(a, "head") == edited);
    ot_close(a);
    free(t.body);
    statement(path, "INSERT INTO search(search,rank) VALUES('integrity-check',1)");
    unlink(path);
}

static void model_parameters(const char *path) {
    transport transport_state = {0};
    ot_app *app = open_app(path, &transport_state);
    assert(ot_set_preferences(app, "{\"language\":\"ja\",\"inputLanguage\":\"ja\"}"));
    ot_close(app);
    app = open_app(path, &transport_state);
    assert(!strcmp(ot_get_preferences(app)->language, "ja") &&
           !strcmp(ot_get_preferences(app)->input_language, "ja"));
    assert(!ot_set_preferences(app, "{\"language\":\"../en\"}"));
    assert(!ot_set_preferences(app, "{\"startupModel\":18446744073709551615}"));
    assert(!ot_set_preferences(app, "{\"gpuLayers\":18446744073709551614}"));
    sqlite3 *db = ot_store_connection(ot_app_store(app));
    assert(sqlite3_exec(db, "INSERT INTO models(id,name,kind,reference) VALUES(1,'A',1,'a'),(2,'B',1,'b')",
                        NULL, NULL, NULL) == SQLITE_OK);
    ot_connection connection = *ot_configuration(app);
    strcpy(connection.model, "A");
    assert(ot_configure(app, &connection, 1));
    assert(ot_set_preferences(
        app, "{\"context\":4096,\"output\":512,\"temperature\":0.3,\"theme\":\"dark\",\"startupModel\":2}"));
    strcpy(connection.model, "B");
    assert(ot_configure(app, &connection, 2));
    assert(ot_set_preferences(
        app,
        "{\"context\":16384,\"output\":1024,\"temperature\":0.9,\"theme\":\"dark\",\"startupModel\":2}"));
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT identity,parameters FROM generation_parameters ORDER BY identity",
                              -1, &query, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) {
        assert(sqlite3_step(query) == SQLITE_ROW);
        const char *text = (const char *)sqlite3_column_text(query, 1);
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        assert(doc && !yyjson_obj_get(yyjson_doc_get_root(doc), "theme") &&
               !yyjson_obj_get(yyjson_doc_get_root(doc), "system") &&
               !yyjson_obj_get(yyjson_doc_get_root(doc), "startupModel"));
        ot_preferences stored;
        assert(ot_preferences_read(&stored, yyjson_doc_get_root(doc)));
        assert(stored.context == (i ? 16384u : 4096u));
        ot_preferences active = *ot_get_preferences(app);
        ot_preferences_model_values(&active, &stored);
        assert(!strcmp(active.theme, "dark") && active.startup_model == 2);
        assert(fabs(active.temperature - (i ? .9 : .3)) < .00001);
        yyjson_doc_free(doc);
    }
    sqlite3_finalize(query);
    assert(sqlite3_exec(db,
                        "CREATE TRIGGER fail_parameters BEFORE UPDATE OF parameters ON generation_parameters "
                        "BEGIN SELECT "
                        "RAISE(ABORT,'test fault');END",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(!ot_set_preferences(app, "{\"context\":32768,\"output\":1024,\"theme\":\"light\"}"));
    ot_preferences restored;
    assert(ot_store_preferences(ot_app_store(app), &restored) && !strcmp(restored.theme, "dark") &&
           restored.context == 16384);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_parameters", NULL, NULL, NULL) == SQLITE_OK);
    strcpy(connection.model, "api-A");
    assert(ot_configure(app, &connection, 0));
    assert(ot_set_preferences(
        app, "{\"context\":4096,\"output\":512,\"temperature\":0.2,\"theme\":\"dark\",\"startupModel\":2}"));
    strcpy(connection.model, "api-B");
    assert(ot_configure(app, &connection, 0));
    assert(ot_get_preferences(app)->context == 8192);
    assert(ot_set_preferences(
        app, "{\"context\":16384,\"output\":512,\"temperature\":0.8,\"theme\":\"dark\",\"startupModel\":2}"));
    strcpy(connection.model, "api-A");
    assert(ot_configure(app, &connection, 0));
    assert(ot_get_preferences(app)->context == 4096 &&
           fabs(ot_get_preferences(app)->temperature - .2) < .00001);
    ot_close(app);
    app = open_app(path, &transport_state);
    assert(ot_get_preferences(app)->startup_model == 2);
    assert(ot_remove_model(app, 2, false));
    assert(ot_get_preferences(app)->startup_model == 0 && ot_get_preferences(app)->context == 4096);
    assert(ot_store_generation(ot_app_store(app), 2, &restored) == 1 && restored.context == 16384);
    assert(ot_remove_model(app, 2, true));
    assert(ot_store_generation(ot_app_store(app), 2, &restored) == 0);
    assert(ot_store_generation(ot_app_store(app), 1, &restored) == 1 && restored.context == 4096);
    ot_close(app);
    unlink(path);
}
static void workspace(const char *path, const char *directory) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(!ot_get_preferences(a)->internet && strstr(ot_get_preferences(a)->system, "emojis"));
    assert(
        ot_set_preferences(a, "{\"system\":\"Use my custom style.\",\"input\":512,\"keepAlive\":\"35m\"}"));
    assert(!strcmp(ot_get_preferences(a)->system, "Use my custom style."));
    assert(ot_get_preferences(a)->input == 512);
    assert(ot_set_preferences(a, "{\"system\":null}"));
    assert(!strcmp(ot_get_preferences(a)->system, ot_preferences_default().system));
    assert(ot_set_preferences(a, "{\"system\":\"\"}"));
    assert(!strcmp(ot_get_preferences(a)->system, ot_preferences_default().system));
    assert(!ot_set_preferences(a, "{\"context\":4096,\"input\":4000,\"output\":512}"));
    assert(!ot_set_preferences(a, "{\"keepAlive\":\"241m\"}"));
    assert(ot_set_preferences(a, "{\"requireGpu\":true}") && ot_get_preferences(a)->require_gpu);
    assert(!ot_set_preferences(a, "{\"requireGpu\":1}"));
    assert(ot_get_preferences(a)->tooltips);
    assert(ot_set_preferences(a, "{\"tooltips\":false}") && !ot_get_preferences(a)->tooltips);
    assert(!ot_set_preferences(a, "{\"tooltips\":0}"));
    assert(ot_set_preferences(a, "{\"region\":\"en-US\",\"dateFormat\":\"ymd\",\"hourFormat\":\"24\"}"));
    ot_preferences regional;
    assert(ot_store_preferences(ot_app_store(a), &regional) && !strcmp(regional.region, "en-US") &&
           !strcmp(regional.date_format, "ymd") && !strcmp(regional.hour_format, "24"));
    assert(!ot_set_preferences(a, "{\"region\":\"invalid\"}"));
    assert(!ot_set_preferences(a, "{\"dateFormat\":\"bad\"}"));
    assert(!ot_set_preferences(a, "{\"hourFormat\":12}"));
    yyjson_mut_doc *model = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *parameters = ot_preferences_model_json(model, &regional);
    assert(parameters && !yyjson_mut_obj_get(parameters, "region") &&
           !yyjson_mut_obj_get(parameters, "dateFormat") && !yyjson_mut_obj_get(parameters, "hourFormat"));
    yyjson_mut_doc_free(model);
    assert(ot_set_preferences(a, "{\"requireGpu\":false}") && !ot_get_preferences(a)->require_gpu);
    assert(ot_set_preferences(
        a, "{\"context\":8192,\"output\":1024,\"theme\":\"dark\",\"temperature\":0.2,\"tooltips\":false}"));
    assert(!ot_set_preferences(a, "{\"context\":1024,\"output\":2048}"));
    assert(!ot_set_preferences(a, "{\"memoryPercent\":200}"));
    assert(send_message(a, "Domanda iniziale", "model", false));
    complete(a, &t, "Risposta iniziale");
    ot_background(a);
    assert(t.requests == 2);
    assert(strstr(t.body, "Domanda iniziale") && !strstr(t.body, "Risposta iniziale"));
    uint64_t title_operation = t.operation;
    assert(ot_submit(a, "Priorità interattiva", 0));
    assert(t.cancels == 1);
    ot_end(a, title_operation, "stale");
    complete(a, &t, "Risposta successiva");
    ot_background(a);
    assert(strstr(t.body, "Domanda iniziale") && !strstr(t.body, "Priorità interattiva") &&
           !strstr(t.body, "Risposta successiva"));
    complete(
        a, &t,
        "Titolo: \"Titolo generato sulla rotazione sincrona della Luna\".\nUna spiegazione da non usare");
    has(a, "Titolo generato sulla rotazione sincrona della Luna");
    absent(a, "Una spiegazione da non usare");
    assert(ot_rename(a, "Titolo scelto"));
    assert(ot_draft(a, value(a, "selected"), "Bozza conservata"));
    int64_t chat = value(a, "selected");
    ot_thinking thinking = {
        .known = true,
        .count = 2,
        .options = {{.value = "off", .type = OT_THINKING_FALSE}, {.value = "on", .type = OT_THINKING_TRUE}}};
    ot_thinking_capability(a, &thinking);
    assert(ot_chat_thinking(a, "off"));
    assert(!strcmp(ot_get_preferences(a)->thinking_mode, "auto"));
    has(a, "\"thinkingMode\":\"off\"");
    assert(ot_select(a, 0));
    has(a, "\"thinkingMode\":\"auto\"");
    assert(ot_select(a, chat));
    has(a, "\"thinkingMode\":\"off\"");
    assert(ot_classify(a, 1));
    assert(value(a, "selected") == chat && value(a, "filter") == 1);
    assert(count(a, "chats") == 1);
    has(a, "Bozza conservata");
    int64_t archived_head = value(a, "head");
    uint64_t archived_operation = t.operation;
    for (int classification = 1; classification <= 2; classification++) {
        assert(ot_classify(a, classification));
        assert(value(a, "selected") == chat && value(a, "filter") == classification);
        assert(value(a, "head") == archived_head);
        assert(!ot_draft(a, chat, "Non salvare"));
        assert(!ot_submit(a, "Non inviare", 0));
        has(a, "chat_read_only");
        assert(!ot_submit(a, NULL, 0));
        assert(!ot_submit(a, "Non modificare", archived_head));
        assert(!ot_improve(a, "Non migliorare"));
        assert(!ot_chat_thinking(a, "on"));
        ot_background(a);
        assert(t.operation == archived_operation);
        has(a, "Bozza conservata");
    }
    assert(ot_classify(a, 2));
    assert(count(a, "chats") == 1);
    assert(ot_classify(a, 1));
    assert(value(a, "selected") == chat && value(a, "filter") == 1);
    assert(ot_classify(a, 0));
    assert(value(a, "selected") == chat && value(a, "filter") == 0);
    assert(ot_draft(a, chat, "Bozza conservata"));
    assert(ot_select(a, 0));
    assert(send_message(a, "Da eliminare definitivamente", "model", false));
    complete(a, &t, "Contenuto temporaneo");
    int64_t disposable_chat = value(a, "selected");
    assert(ot_classify(a, 3));
    assert(!ot_select(a, disposable_chat));
    statement(path, "INSERT INTO search(search,rank) VALUES('integrity-check',1)");
    assert(ot_select(a, chat));
    ot_resources r = {.memory_total = 128ull << 30,
                      .memory_available = 40ull << 30,
                      .cores = 18,
                      .performance_cores = 12,
                      .unified = true};
    ot_resources_update(a, &r);
    assert(ot_memory_budget(&r, ot_get_preferences(a)) == (40ull << 30) * 9 / 10);
    assert(ot_thread_budget(&r, ot_get_preferences(a)) == 12);
    ot_preferences base = *ot_get_preferences(a);
    strcpy(base.keep_alive, "-1");
    ot_model_info preset_model = {.context = 40960, .layers = 24, .experts = 32, .experts_used = 4};
    for (unsigned profile = 0; profile < 4; profile++) {
        ot_preferences preset = ot_model_preset(&base, &preset_model, NULL, profile);
        assert(preset.context <= preset_model.context && preset.input + preset.output < preset.context);
        assert(preset.gpu_layers == -1 && !strcmp(preset.keep_alive, "-1"));
    }
    r.pressure = 2;
    assert(ot_memory_budget(&r, ot_get_preferences(a)) == 0);
    char backup[300], exported[300], staged[300], previous[300];
    snprintf(backup, sizeof backup, "%s/backup.sqlite", directory);
    snprintf(exported, sizeof exported, "%s/export.ndjson", directory);
    snprintf(staged, sizeof staged, "%s/restore.sqlite", directory);
    snprintf(previous, sizeof previous, "%s/previous.sqlite", directory);
    char companion[320];
    snprintf(companion, sizeof companion, "%s-wal", backup);
    FILE *foreign = fopen(companion, "w");
    assert(foreign && fputs("unrelated", foreign) >= 0 && !fclose(foreign));
    assert(!ot_store_backup(path, backup));
    foreign = fopen(companion, "r");
    assert(foreign && fgetc(foreign) == 'u' && !fclose(foreign));
    assert(!unlink(companion) && access(backup, F_OK) != 0);
    assert(ot_store_backup(path, backup));
    assert(!ot_store_backup(path, backup));
    assert(ot_store_validate(backup));
    assert(ot_store_export(path, exported));
    assert(!ot_store_export(path, exported));
    FILE *f = fopen(exported, "r");
    assert(f);
    char line[4096];
    unsigned lines = 0;
    while (fgets(line, sizeof line, f)) {
        yyjson_doc *d = yyjson_read(line, strlen(line), 0);
        assert(d);
        yyjson_doc_free(d);
        lines++;
    }
    fclose(f);
    assert(lines == 3);
    assert(ot_rename(a, "Dopo il backup"));
    ot_close(a);
    assert(ot_store_backup(backup, staged));
    assert(ot_store_apply_restore(directory));
    a = open_app(path, &t);
    has(a, "Titolo scelto");
    absent(a, "Dopo il backup");
    has(a, "\"thinkingMode\":\"off\"");
    assert(!strcmp(ot_get_preferences(a)->theme, "dark") && !ot_get_preferences(a)->tooltips);
    ot_close(a);
    assert(ot_store_validate(previous));
    statement(backup, "UPDATE settings SET draft=CAST(x'ff' AS TEXT)");
    assert(!ot_store_validate(backup));
    statement(backup, "UPDATE settings SET draft=''");
    assert(ot_store_validate(backup));
    statement(backup, "UPDATE turns SET completion='[]'");
    assert(!ot_store_validate(backup));
    statement(backup, "UPDATE turns SET completion='{}'");
    assert(ot_store_validate(backup));
    statement(backup, "CREATE TRIGGER unexpected_trigger AFTER UPDATE ON chats BEGIN SELECT 1; END");
    assert(!ot_store_validate(backup));
    ot_store *altered = ot_store_open(backup);
    assert(!altered);
    free(t.body);
    unlink(backup);
    unlink(exported);
    unlink(previous);
    unlink(path);
}


static void rewrite_contract(void) {
    char output[OT_REWRITE_LIMIT + 1];
    assert(ot_rewrite_apply("scrivi una calcolatrice in C",
        "{\"prompt\":\"Scrivi una calcolatrice in C con operazioni di base, input valido e divisione per zero gestita. Fornisci codice completo e istruzioni di compilazione.\"}", output) == 1);
    assert(strstr(output, "divisione per zero"));
    assert(ot_rewrite_apply("Spiega RAM e SSD in due frasi.",
        "{\"prompt\":\"Spiega RAM e SSD in due frasi.\"}", output) == 0);
    assert(ot_rewrite_apply("testo", "```json\n{\"prompt\":\"testo\"}\n```", output) == 0);
    assert(ot_rewrite_apply("保留雪 C17 {{nome}}", "{\"prompt\":null}", output) == 0 &&
           !strcmp(output, "保留雪 C17 {{nome}}"));
    assert(ot_rewrite_apply("null", "{\"prompt\":\"null\"}", output) == 0);
    const char *invalid[] = {"", "null", "[]", "{\"prompt\":\"null\"}", "{\"prompt\":null,\"extra\":1}", "{\"prompt\":null,\"prompt\":null}", "{\"prompt\":\"\"}",
        "{\"prompt\":\" \\n\\t\"}", "{\"prompt\":\"a\\u0000b\"}", "{\"prompt\":\"a\",\"prompt\":\"b\"}",
        "{\"prompt\":\"a\",\"extra\":1}", "{\"prompt\":12}", "{\"prompt\":\"incompleto",
        "commento {\"prompt\":\"testo\"}", "```json\n{\"prompt\":\"testo\"}\n``` garbage"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(ot_rewrite_apply("testo", invalid[i], output) < 0);
    assert(ot_rewrite_apply("Scrivi in C17.", "{\"prompt\":\"Scrivi in C18.\"}", output) < 0);
    assert(ot_rewrite_apply("Scrivi in C17.", "{\"prompt\":\"Scrivi in C170.\"}", output) < 0);
    assert(ot_rewrite_apply("Scrivi in C.", "{\"prompt\":\"Scrivi in Python.\"}", output) < 0);
    assert(ot_rewrite_apply("Usa `a-b` e 20 righe.", "{\"prompt\":\"Usa `a+b` e 20 righe.\"}", output) < 0);
    assert(ot_rewrite_apply("Usa 20 righe e C17.", "{\"prompt\":\"In C17, usa 20 righe per il codice.\"}", output) == 1);
    assert(ot_rewrite_apply("Mantieni {{nome}}.", "{\"prompt\":\"Mantieni {{utente}}.\"}", output) < 0);
    assert(ot_rewrite_apply("用三句话解释。", "{\"prompt\":\"用二句话解释。\"}", output) < 0);
    assert(ot_rewrite_apply("保留雪。", "{\"prompt\":\"保留雪，并说明。\"}", output) == 1);
    assert(ot_rewrite_apply("Vai a https://example.com/test.",
        "{\"prompt\":\"Visita https://example.com/test e riassumi.\"}", output) == 1);
    char *large = malloc(OT_REWRITE_LIMIT + 2); assert(large);
    memset(large, 'a', OT_REWRITE_LIMIT + 1); large[OT_REWRITE_LIMIT + 1] = 0;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL); assert(d);
    yyjson_mut_val *root = yyjson_mut_obj(d); yyjson_mut_doc_set_root(d, root);
    assert(yyjson_mut_obj_add_str(d, root, "prompt", large));
    char *json = yyjson_mut_write(d, 0, NULL); assert(json);
    assert(ot_rewrite_apply("testo", json, output) < 0); free(json);
    large[OT_REWRITE_LIMIT] = 0;
    json = yyjson_mut_write(d, 0, NULL); assert(json);
    assert(ot_rewrite_apply(large, json, output) < 0); /* yyjson captured the previous length. */
    free(json);
    assert(yyjson_mut_obj_remove_key(root, "prompt"));
    assert(yyjson_mut_obj_add_str(d, root, "prompt", large));
    json = yyjson_mut_write(d, 0, NULL); assert(json);
    assert(ot_rewrite_apply(large, json, output) == 0 && !strcmp(large, output));
    free(json); yyjson_mut_doc_free(d); free(large);
    puts("PASS: complete prompt proposals preserve literals, allow useful expansion and reject malformed, incomplete or oversized results");
}
static void automatic_language(void *context, const char *source, char tag[16]) {
    (void)context;
    strcpy(tag, !strncmp(source, "Write", 5) ? "en" : "it");
}
static void automatic_tasks(const char *path) {
    for (unsigned protocol = 0; protocol < 2; protocol++) {
        transport t = {0};
        ot_app *a = ot_open(path, (ot_io){&t, requested, cancelled, automatic_language});
        assert(a && ot_set_preferences(a, "{\"system\":\"CUSTOM_REPLY_STYLE\",\"summaries\":false}"));
        const char *prompt = "Write a calculator in C and explain every line.";
        assert(send_message(a, prompt, "model", protocol != 0) && strstr(t.body, "CUSTOM_REPLY_STYLE"));
        complete(a, &t, "Source response");
        ot_background(a);
        for (unsigned task = 0; task < 2; task++) {
            if (task) assert(ot_improve(a, prompt));
            yyjson_doc *d = yyjson_read(t.body, strlen(t.body), 0);
            yyjson_val *root = yyjson_doc_get_root(d), *messages = yyjson_obj_get(root, "messages"),
                       *system = yyjson_arr_get_first(messages), *instruction = yyjson_arr_get_last(messages);
            const char *policy = yyjson_get_str(yyjson_obj_get(system, "content"));
            assert(policy && !strstr(policy, "CUSTOM_REPLY_STYLE") && (task || strstr(policy, "English")) &&
                   !strstr(policy, prompt) && yyjson_arr_size(messages) == 2 &&
                   strstr(yyjson_get_str(yyjson_obj_get(instruction, "content")), prompt) &&
                   !yyjson_equals_str(yyjson_obj_get(instruction, "content"), prompt));
            if (task) {
                const char *quoted = yyjson_get_str(yyjson_obj_get(instruction, "content"));
                yyjson_doc *data = yyjson_read(quoted, strlen(quoted), 0);
                yyjson_val *source = yyjson_doc_get_root(data);
                assert(yyjson_equals_str(yyjson_obj_get(source, "DRAFT"), prompt) &&
                       yyjson_is_arr(yyjson_obj_get(source, "HISTORY")));
                yyjson_doc_free(data);
            }
            yyjson_val *parameters = protocol ? yyjson_obj_get(root, "options") : root;
            yyjson_val *temperature = yyjson_obj_get(parameters, "temperature");
            assert(yyjson_is_num(temperature) && fabs(yyjson_get_num(temperature)) < 1e-9);
            yyjson_doc_free(d);
            complete(a, &t, task ? "{\"prompt\":\"Write a calculator in C and explain each line.\"}" : "Calculator in C");
        }
        has(a, "Calculator in C");
        assert(!strcmp(ot_get_preferences(a)->system, "CUSTOM_REPLY_STYLE"));
        ot_close(a); free(t.body); assert(!unlink(path));
    }
    puts("PASS: automatic editing isolates assistant style, source language and terminal task across both protocols");
}
/* Editing borrows the selected branch as data; it must never become a turn,
 * annotation or instruction in the next normal request. */
static void rewrite_isolation(const char *path) {
    for (unsigned protocol = 0; protocol < 2; protocol++) {
        transport t = {0}; ot_app *a = open_app(path, &t);
        assert(ot_set_preferences(a, "{\"context\":8192,\"output\":1024,\"titles\":false,\"summaries\":false}"));
        assert(send_message(a, "OTHER_CHAT_PRIVATE", "model", protocol != 0));
        complete(a, &t, "Unrelated answer");
        assert(ot_select(a, 0));
        assert(ot_submit(a, "SELECTED_BRANCH", 0)); complete(a, &t, "Chosen answer");
        int64_t head = value(a, "head");
        assert(ot_submit(a, "ALTERNATE_BRANCH", head)); complete(a, &t, "Alternate answer");
        assert(ot_choose(a, head));
        assert(ot_draft(a, value(a, "selected"), "Bozza originale"));
        sqlite3 *db = NULL; sqlite3_stmt *version = NULL;
        assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        assert(sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &version, NULL) == SQLITE_OK);
        for (unsigned context = 0; context < 2; context++) {
            assert(ot_set_preferences(a, context ? "{\"improveContext\":4}" : "{\"improveContext\":0}"));
            assert(sqlite3_step(version) == SQLITE_ROW);
            int before = sqlite3_column_int(version, 0); sqlite3_reset(version);
            assert(ot_improve(a, "Continua con il programma"));
            assert(!strstr(t.body, "OTHER_CHAT_PRIVATE") && !strstr(t.body, "ALTERNATE_BRANCH"));
            assert((strstr(t.body, "SELECTED_BRANCH") != NULL) == (context != 0));
            complete(a, &t, context ? "{\"prompt\":\"Continua con il programma e verifica gli input.\"}" : "{\"prompt\":null}");
            assert(value(a, "head") == head);
            assert(sqlite3_step(version) == SQLITE_ROW && sqlite3_column_int(version, 0) == before);
            sqlite3_reset(version);
            has(a, "Bozza originale");
        }
        /* Missing capability information is not proof that thinking is disabled. */
        for (unsigned known = 0; known < 2; known++) {
            ot_thinking thinking = {.known = known != 0};
            ot_thinking_capability(a, &thinking);
            assert(ot_improve(a, "Rendi chiara la richiesta"));
            yyjson_doc *request = yyjson_read(t.body, strlen(t.body), 0);
            yyjson_val *root = yyjson_doc_get_root(request),
                       *options = protocol ? yyjson_obj_get(root, "options") : root;
            assert(yyjson_get_uint(yyjson_obj_get(options, protocol ? "num_predict" : "max_tokens")) ==
                   (known ? 1536 : 3072));
            yyjson_doc_free(request);
            complete(a, &t, "{\"prompt\":null}");
        }
        sqlite3_finalize(version); assert(sqlite3_close(db) == SQLITE_OK);
        assert(ot_submit(a, "Una nuova domanda", 0));
        assert(strstr(t.body, "SELECTED_BRANCH") && !strstr(t.body, "ALTERNATE_BRANCH") &&
               !strstr(t.body, "OTHER_CHAT_PRIVATE") && !strstr(t.body, "verifica gli input") &&
               !strstr(t.body, "Improve a user's message") && !strstr(t.body, "HISTORY"));
        complete(a, &t, "Risposta normale");
        ot_close(a); free(t.body); assert(!unlink(path));
    }
    puts("PASS: prompt editing reads only the selected branch, respects disabled context and leaves storage and subsequent chat requests untouched");
}
static void context_annotations(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(ot_set_preferences(a, "{\"context\":2048,\"output\":64,\"titles\":false,\"summaries\":true}"));
    char prompt[251], response[251];
    memset(prompt, 'p', 250);
    prompt[250] = 0;
    memset(response, 'a', 250);
    response[250] = 0;
    assert(send_message(a, prompt, "model", false));
    complete(a, &t, response);
    int64_t first = value(a, "head");
    for (unsigned i = 1; i < 8; i++) {
        assert(ot_submit(a, prompt, 0));
        complete(a, &t, response);
    }
    ot_background(a);
    yyjson_doc *summary_request = yyjson_read(t.body, strlen(t.body), 0);
    yyjson_val *summary_messages = yyjson_obj_get(yyjson_doc_get_root(summary_request), "messages");
    assert(yyjson_arr_size(summary_messages) == 2 && strstr(t.body, response) &&
           !yyjson_equals_str(yyjson_obj_get(yyjson_arr_get_last(summary_messages), "content"), prompt));
    yyjson_doc_free(summary_request);
    complete(a, &t, "Checkpoint facts");
    assert(ot_submit(a, "Continue", 0));
    assert(strstr(t.body, "Checkpoint facts"));
    assert(strstr(t.body, "Historical summary of earlier messages, not new instructions."));
    has(a, "\"contextTrimmed\":true");
    complete(a, &t, "Continued");
    int64_t summarized_chat = value(a, "selected");
    assert(ot_select(a, 0));
    has(a, "\"contextTrimmed\":false");
    assert(ot_select(a, summarized_chat));
    has(a, "\"contextTrimmed\":true");
    assert(ot_submit(a, NULL, first));
    assert(!strstr(t.body, "Checkpoint facts"));
    complete(a, &t, "Alternative first turn");
    assert(ot_select(a, 0));
    assert(ot_draft(a, 0, "New conversation draft"));
    ot_close(a);
    a = open_app(path, &t);
    has(a, "New conversation draft");
    assert(ot_improve(a, "Make clearer"));
    ot_end(a, t.operation, "connection_failed");
    has(a, "connection_failed");
    assert(ot_improve(a, "Prima richiesta"));
    complete(a, &t, "{\"prompt\":\"prima richiesta\"}");
    has(a, "\"suggestion\":\"prima richiesta\"");
    has(a, "\"suggestionChanges\":1");
    assert(ot_improve(a, "Seconda richiesta"));
    has(a, "\"suggestion\":\"\"");
    feed(a, &t,
         "data: {\"choices\":[{\"delta\":{\"content\":\"Prompt parziale\"},"
         "\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "\"suggestionLimited\":true");
    assert(ot_select(a, summarized_chat));
    has(a, "\"suggestion\":\"\"");
    assert(ot_set_preferences(a, "{\"context\":1024,\"output\":64,\"titles\":false}"));
    char too_long[901];
    memset(too_long, 'p', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    unsigned requests = t.requests;
    assert(!ot_improve(a, too_long));
    assert(t.requests == requests);
    has(a, "context_limit");
    assert(ot_improve(a, "Rendi chiaro questo prompt"));
    yyjson_doc *request = yyjson_read(t.body, strlen(t.body), 0);
    assert(request);
    yyjson_val *root = yyjson_doc_get_root(request), *messages = yyjson_obj_get(root, "messages"), *message;
    size_t message_index, message_count, bytes = 0;
    yyjson_arr_foreach(messages, message_index, message_count, message) bytes +=
        yyjson_get_len(yyjson_obj_get(message, "content")) + 32;
    uint64_t output = yyjson_get_uint(yyjson_obj_get(root, "max_tokens"));
    assert(bytes + output <= 1024 && output >= 64 && output <= 768);
    yyjson_doc_free(request);
    complete(a, &t, "{\"prompt\":\"Rendi chiaro questo prompt\"}");
    has(a, "\"suggestionClamped\":true");
    assert(ot_set_preferences(a, "{\"context\":8192,\"output\":64,\"titles\":false}"));
    char longer[2049];
    memset(longer, 'p', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    assert(ot_improve(a, longer));
    request = yyjson_read(t.body, strlen(t.body), 0);
    assert(request);
    output = yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(request), "max_tokens"));
    assert(output >= 2048 && output <= 3072);
    yyjson_doc_free(request);
    complete(a, &t, " \n\t");
    has(a, "empty_response");
    has(a, "\"suggestion\":\"\"");
    char *empty_result = ot_health(a);
    assert(empty_result && strstr(empty_result, "empty_response"));
    free(empty_result);
    ot_close(a);
    free(t.body);
    unlink(path);
}

typedef struct {
    uint64_t id, offset;
    char url[8193];
    char method[8];
    char body[2048];
    struct {uint64_t id; char url[1024];} recent[16];
    unsigned requests;
} acquisition;
static void acquisition_request(void *context, uint64_t id, const char *method, const char *url,
                                const char *body, const char *etag, uint64_t offset, ot_network_scope scope) {
    (void)scope;
    (void)etag;
    acquisition *t = context;
    unsigned slot = t->requests++ % 16;
    t->recent[slot].id = id;
    snprintf(t->recent[slot].url, sizeof t->recent[slot].url, "%s", url);
    t->id = id;
    t->offset = offset;
    snprintf(t->url, sizeof t->url, "%s", url);
    snprintf(t->method, sizeof t->method, "%s", method);
    snprintf(t->body, sizeof t->body, "%s", body ? body : "");
}
static void library_has_at(ot_library *l, const char *text, unsigned line) {
    char *json = ot_library_view(l);
    if (json && !strstr(json, text))
        fprintf(stderr, "Missing library fixture value at line %u: %s\n", line, text);
    assert(json && strstr(json, text));
    free(json);
}
#define library_has(l, text) library_has_at(l, text, __LINE__)
static uint64_t candidate_token(ot_library *l, const char *key) {
    char *raw = ot_library_view(l);
    assert(raw);
    yyjson_doc *d = yyjson_read(raw, strlen(raw), 0);
    uint64_t token = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(d), key), "token"));
    free(raw);
    yyjson_doc_free(d);
    return token;
}
static void settle_library(ot_library *l) {
    for (unsigned i = 0; i < 1000; i++) {
        ot_library_tick(l);
        char *view = ot_library_view(l);
        assert(view);
        bool busy = strstr(view, "\"discovering\":true") || strstr(view, "\"state\":\"verifying\"") ||
                    strstr(view, "\"previewing\":true");
        free(view);
        if (!busy)
            return;
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(0 && "inventory timeout");
}
static void adaptive_models(void) {
    char title[257], quant[33];
    ot_model_label("Qwen3.5-4B-3D-Coder-Q4_K_M.gguf", "", title, quant);
    assert(!strcmp(title, "Qwen3.5 4B 3D Coder") && !strcmp(quant, "Q4_K_M"));
    ot_model_label("Modello Δ", quant, title, quant);
    assert(!strcmp(title, "Modello Δ") && !strcmp(quant, "Q4_K_M"));
    ot_model_label("gpt-oss:20b", "MXFP4", title, quant);
    assert(!strcmp(title, "gpt-oss:20b") && !strcmp(quant, "MXFP4"));
    ot_model_label("Qwen3-8B-custom.gguf", "", title, quant);
    assert(!*quant && !strcmp(title, "Qwen3 8B custom"));
    char boundary[270];
    memset(boundary, 'x', 255);
    memcpy(boundary + 255, "é.gguf", 8);
    ot_model_label(boundary, "", title, quant);
    assert(strlen(title) == 255 && ot_utf8_valid(title, strlen(title)));
    boundary[257] = 0;
    ot_model_label(boundary, "", title, quant);
    assert(strlen(title) == 255 && ot_utf8_valid(title, strlen(title)));
    memset(boundary, 'x', 256);
    memcpy(boundary + 256, ".gguf", 6);
    ot_model_label(boundary, "", title, quant);
    assert(strlen(title) == 256);
    ot_model_candidate families[6], models[16];
    const char *feed_models =
        "{\"recommendations\":["
        "{\"model\":\"remote:cloud\",\"description\":\"coding\"},"
        "{\"model\":\"test:4b\",\"description\":\"coding and research\",\"vram_bytes\":6000000000},"
        "{\"model\":\"test:9b\"},{\"model\":\"bad\\u0000name\"}]}";
    assert(ot_model_feed(feed_models, strlen(feed_models), families, 6) == 1);
    assert(!strcmp(families[0].name, "test:4b"));
    const char *html = "<a href=\"/library/test:4b\">3GB–4GB 32K context window</a>"
                       "<a href=\"/library/test:9b\"><span class=\"font-mono\">abc123abc123</span>7GB 128K "
                       "context window</a>"
                       "<a href=\"/library/test:9b-a2b\"><span class=\"font-mono\">abc123abc123</span>7GB "
                       "128K context window</a>"
                       "<a href=\"/library/test:0.8b\">900MB 16K context window</a>"
                       "<a href=\"/library/test:4b-q8_0\">6GB</a>"
                       "<a href=\"/library/test:cloud\">70GB</a><a href=\"/library/other:2b\">2GB</a>";
    char *catalogue = ot_ollama_catalog(html, "test");
    assert(catalogue && ot_model_variants(catalogue, &families[0], models, 16) == 4);
    assert(strstr(models[3].name, "q8_0") && models[3].bytes == 6000000000);
    free(catalogue);
    assert(models[0].bytes == 4000000000 && models[0].context == 32768 && models[0].approximate);
    assert(models[0].memory == 6000000000 && !models[1].memory);
    assert(ot_model_variants("[{\"name\":\"other:4b\",\"bytes\":1000000000}]", &families[0], models + 4,
                             12) == 0);
    assert(!ot_model_name(NULL) && !ot_model_name("bad/../name") && ot_model_name("test:4b"));
    ot_preferences p = ot_preferences_default();
    assert(!strcmp(p.theme, "system") && !strcmp(p.accent, "teal") && !strcmp(p.profile, "recommended") &&
           p.setup_engines == 3);
    ot_resources small = {.memory_total = 8ULL << 30,
                          .memory_available = 6ULL << 30,
                          .gpu_recommended = 6ULL << 30,
                          .disk_available = 50ULL << 30,
                          .gpu = true,
                          .unified = true};
    assert(!ot_model_candidate_fits(&models[0], &small, &p));
    assert(ot_model_candidate_fits(&models[2], &small, &p));
    small.pressure = 2;
    assert(!ot_model_candidate_fits(&models[2], &small, &p));
    small.pressure = 0;
    small.gpu = false;
    assert(!ot_model_candidate_fits(&models[2], &small, &p));
    small.gpu = true;
    small.disk_available = 1ULL << 30;
    assert(!ot_model_candidate_fits(&models[2], &small, &p));
    models[2].installed = 42;
    assert(ot_model_candidate_fits(&models[2], &small, &p));
    ot_resources large = {.memory_total = 128ULL << 30,
                          .memory_available = 110ULL << 30,
                          .gpu_recommended = 96ULL << 30,
                          .disk_available = 500ULL << 30,
                          .gpu = true,
                          .unified = true};
    ot_model_candidate writing = models[1], coding = models[1];
    strcpy(writing.description, "creative writing and translation");
    strcpy(coding.description, "coding and programming");
    p.purpose = 1;
    assert(ot_model_candidate_score(&coding, &large, &p) > ot_model_candidate_score(&writing, &large, &p));
    p.purpose = 3;
    assert(ot_model_candidate_score(&writing, &large, &p) > ot_model_candidate_score(&coding, &large, &p));
    assert(!ot_model_verified(coding.name, "not-qualified"));
    double unqualified = ot_model_candidate_score(&coding, &large, &p);
    coding.verified = true;
    assert(fabs(ot_model_candidate_score(&coding, &large, &p) - unqualified) < 1e-12);
    ot_model_candidate pair[] = {writing, coding};
    pair[0].verified = false;
    pair[1].verified = true;
    double similar[] = {.8, .79}, different[] = {.8, .7};
    assert(ot_model_candidate_best(pair, similar, 2) == 1);
    assert(ot_model_candidate_best(pair, different, 2) == 0);
    pair[1].bytes = pair[0].bytes / 2;
    assert(ot_model_candidate_best(pair, similar, 2) == 0);
    assert(ot_model_verified("qwen3.5:4b",
                             "sha256:7f3251fa6878a78a606bcb3c074283306e5ad2ea411a7774a9dd98a4bc7f2d13"));
    assert(!ot_model_verified("qwen3.5:4b", "changed"));
    assert(ot_model_file_verified("d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785"));
    assert(!ot_model_file_verified("changed"));
    const unsigned capacities[] = {8, 16, 24, 32, 64, 128}, contexts[] = {1024, 4096, 32768, 262144, 0};
    size_t simulations = 0;
    for (size_t h = 0; h < sizeof capacities / sizeof *capacities; h++)
        for (unsigned pressure = 0; pressure < 3; pressure++)
            for (unsigned purpose = 0; purpose < 6; purpose++)
                for (size_t c = 0; c < sizeof contexts / sizeof *contexts; c++)
                    for (unsigned geometry = 0; geometry < 3; geometry++) {
                        ot_resources r = large;
                        r.memory_total = (uint64_t)capacities[h] << 30;
                        r.memory_available = r.memory_total * 3 / 4;
                        r.gpu_recommended = r.memory_total * 3 / 4;
                        r.pressure = pressure;
                        ot_model_info m = {.bytes = UINT64_C(3) << 30,
                                           .context = contexts[c],
                                           .layers = 32,
                                           .embedding = 4096,
                                           .heads = 32,
                                           .kv_heads = 8,
                                           .experts = geometry == 1 ? 64 : 0,
                                           .experts_used = geometry == 1 ? 4 : 0,
                                           .complex_cache = geometry == 2};
                        p.purpose = purpose;
                        ot_preferences recipe = ot_model_preset(&p, &m, &r, OT_PRESET_RECOMMENDED);
                        assert(recipe.context >= 1024 && recipe.context <= (m.context ? m.context : 32768));
                        assert(recipe.input >= 256 && recipe.output >= 64 &&
                               recipe.input + recipe.output < recipe.context);
                        assert(recipe.gpu_layers == -1 && recipe.require_gpu && !recipe.threads);
                        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                        yyjson_mut_doc_set_root(d, ot_preferences_json(d, &recipe));
                        char *json = yyjson_mut_write(d, 0, NULL);
                        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
                        ot_preferences roundtrip = ot_preferences_default();
                        assert(ot_preferences_read(&roundtrip, yyjson_doc_get_root(doc)));
                        assert(roundtrip.context == recipe.context && roundtrip.output == recipe.output);
                        yyjson_doc_free(doc);
                        free(json);
                        yyjson_mut_doc_free(d);
                        simulations++;
                    }
    ot_model_info model = {.bytes = UINT64_C(8) << 30, .context = 262144, .complex_cache = true};
    p.purpose = 1;
    ot_preferences code = ot_model_preset(&p, &model, &large, OT_PRESET_RECOMMENDED);
    p.purpose = 3;
    ot_preferences prose = ot_model_preset(&p, &model, &large, OT_PRESET_RECOMMENDED);
    assert(code.context > prose.context && code.output >= 16384);
    printf("PASS: %zu adaptive hardware/activity/geometry parameter simulations and bounded catalogues\n",
           simulations);
}
static void connection_contracts(const char *path) {
    char *list = ot_server_models("{\"models\":[{\"name\":\"fixture:4b\"},{\"name\":\"fixture:4b\"},{\"name\":\"bad\\nmodel\"}]}", true);
    assert(list);
    yyjson_doc *d = yyjson_read(list, strlen(list), 0);
    assert(yyjson_arr_size(yyjson_doc_get_root(d)) == 1);
    yyjson_doc_free(d); free(list);
    assert(!ot_server_models("{}", false) && !ot_server_models(NULL, true));
    list = ot_server_models("{\"data\":[{\"id\":\"fixture-model\",\"meta\":{\"n_ctx_train\":32768}}]}", false);
    assert(list && strstr(list, "32768")); free(list);
    ot_server_info info;
    const char *metadata = "{\"capabilities\":[\"completion\",\"thinking\"],\"model_info\":{"
        "\"general.architecture\":\"fixture\",\"fixture.context_length\":131072,"
        "\"fixture.block_count\":32},\"thinking\":{\"values\":[false,true]},"
        "\"details\":{\"quantization_level\":\"Q4_K_M\"}}";
    assert(ot_server_metadata(metadata, true, "fixture:4b", &info));
    assert(info.model.context == 131072 && info.model.layers == 32 && info.thinking.count == 2);
    assert(!strcmp(info.model.quantization, "Q4_K_M"));
    ot_server_residency("{\"models\":[{\"name\":\"fixture:4b\",\"context_length\":8192}]}", "fixture:4b", &info);
    assert(info.resident_known && info.resident && info.loaded_context == 8192);
    ot_server_residency("{\"models\":[{\"name\":\"fixture:latest\",\"context_length\":4096}]}", "FIXTURE", &info);
    assert(info.resident_known && info.resident && info.loaded_context == 4096);
    ot_server_residency("{\"models\":[null]}", "fixture", &info);
    assert(info.resident && info.loaded_context == 4096);
    ot_server_residency("{\"models\":[{\"name\":\"fixture\\u0000other\"}]}", "fixture", &info);
    assert(info.resident && info.loaded_context == 4096);
    ot_server_residency("{\"models\":[]}", "fixture:4b", &info);
    assert(info.resident_known && !info.resident && !info.loaded_context);
    assert(ot_server_metadata("{\"capabilities\":[\"embedding\"]}", true, "embedding", &info) == OT_SERVER_NOT_CHAT);
    assert(ot_server_metadata("{invalid", true, "fixture", &info) == OT_SERVER_INVALID);
    assert(!ot_server_metadata("{}", false, NULL, &info));
    assert(!ot_server_metadata("{}", false, "fixture", NULL));
    assert(ot_server_metadata("{\"default_generation_settings\":{\"n_ctx\":8192},\"is_sleeping\":true}", false, "fixture", &info));
    assert(info.model.context == 8192 && info.loaded_context == 8192 && info.resident_known && !info.resident);
    transport t = {0}; ot_app *app = open_app(path,&t);
    ot_connection c = {.endpoint="http://127.0.0.1:1",.model="fixture"};
    assert(ot_configure(app,&c,0)); ot_connection_observe(app,&info);
    assert(ot_connection_model(app) && ot_connection_model(app)->model.context == 8192);
    ot_connection_failed(app,"connection_auth");
    assert(!ot_ready(app)); has(app,"connection_auth");
    ot_connection_observe(app,&info);
    assert(ot_ready(app)); has(app,"\"error\":\"\"");
    strcpy(info.model.name,"stale-model"); ot_connection_observe(app,&info);
    assert(!strcmp(ot_connection_model(app)->model.name,"fixture"));
    ot_start_without_model(app); assert(!ot_connection_model(app));
    ot_close(app); free(t.body); unlink(path);
}
static void advice_contracts(const char *path) {
    ot_advice_profile sample, decoded, aggregate = {0};
    ot_advice_request(&sample, "scrivi codice: una funzione C con #include e test", "it-IT");
    assert(sample.samples == 1 && sample.tasks[1] > 0 && sample.languages[0] > .9);
    char *json = ot_advice_encode(&sample);
    assert(json && !strstr(json, "scrivi") && !strstr(json, "#include"));
    assert(ot_advice_decode(&decoded, json)); free(json);
    assert(!ot_advice_decode(&decoded, "{}") && !ot_advice_decode(&decoded, "null"));
    ot_advice_merge(&aggregate, &sample, .5);
    assert(aggregate.samples == 1 && fabs(aggregate.tasks[1] - sample.tasks[1] / 2) < 1e-9);
    assert(ot_advice_language("en-US") == 2 && !ot_advice_language("unknown"));

    ot_model_candidate reference = {.kind=2,.installed=1}, alternative = {.kind=2};
    strcpy(reference.name,"current:4b"); strcpy(alternative.name,"alternative:4b");
    assert(!ot_model_candidate_upgrade(&alternative,.81,&reference,.75));
    assert(ot_model_candidate_upgrade(&alternative,.90,&reference,.75));
    assert(!ot_model_candidate_upgrade(&reference,1,&reference,.1));
    assert(!ot_model_candidate_upgrade(&alternative,(double)NAN,&reference,.75));
    assert(!ot_model_candidate_upgrade(&alternative,-1,NULL,0));
    assert(ot_model_candidate_upgrade(&alternative,.1,&reference,-1));
    assert(ot_model_candidate_upgrade(&alternative,.1,NULL,0));
    alternative.installed=1;
    assert(!ot_model_candidate_upgrade(&alternative,1,&reference,.1));
    alternative.installed=0;
    ot_model_candidate variants[4] = {0};
    ot_model_candidate family = {.kind = 1}; strcpy(family.repository,"example/model-GGUF");
    const char *hf = "{\"id\":\"example/model-GGUF\",\"sha\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"gated\":false,\"private\":false,\"gguf\":{\"total\":4000000000,\"chat_template\":\"template\"},"
        "\"cardData\":{\"base_model\":\"example/model\",\"language\":[\"it\",\"en\"]},\"siblings\":["
        "{\"rfilename\":\"Model-4B-Q2_K.gguf\",\"lfs\":{\"size\":1500000000,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}},"
        "{\"rfilename\":\"Model-4B-Q4_K_M.gguf\",\"lfs\":{\"size\":2500000000,\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}},"
        "{\"rfilename\":\"Model-4B-BF16.gguf\",\"lfs\":{\"size\":8100000000,\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}},"
        "{\"rfilename\":\"mmproj.gguf\",\"lfs\":{\"size\":500000000,\"sha256\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"}}]}";
    assert(ot_model_hf_variants(hf,&family,variants,4) == 3);
    assert(variants[0].bits > 2 && variants[0].bits < 4 && variants[2].bits > 16);
    assert(variants[0].languages == 3 && !variants[0].context);
    char *component = strdup(hf); assert(component);
    char *file_component = strstr(component,"Model-4B-Q2_K.gguf"); assert(file_component);
    memcpy(file_component,"body-part-0.gguf  ",17);
    ot_model_candidate safe_variants[4];
    assert(ot_model_hf_variants(component,&family,safe_variants,4) == 2); free(component);
    char incompatible[4096];
    snprintf(incompatible,sizeof incompatible,"{\"library_name\":\"mlx\",%s",hf+1);
    assert(!ot_model_hf_variants(incompatible,&family,safe_variants,4));

    json = ot_model_catalogue_write(variants,3); assert(json);
    ot_model_candidate restored[4]; assert(ot_model_catalogue_read(json,restored,4) == 3); free(json);
    assert(!strcmp(restored[1].checksum,variants[1].checksum));
    assert(!ot_model_catalogue_read("[{\"kind\":1,\"name\":\"bad/../file\"}]",restored,4));
    ot_resources hardware = {.memory_total=32ULL<<30,.memory_available=28ULL<<30,
        .gpu_recommended=24ULL<<30,.disk_available=100ULL<<30,.gpu=true,.unified=true};
    ot_preferences p = ot_preferences_default();
    assert(ot_model_candidate_fits(&variants[0],&hardware,&p)); /* Missing context is nonfatal here. */
    assert(p.advice_learning && !p.advice_size_gb);
    ot_model_candidate unrelated = variants[1], legal = variants[1];
    strcpy(unrelated.name,"flawless-Q4_K_M.gguf"); strcpy(unrelated.description,"flawless general conversation");
    strcpy(legal.name,"legal-Q4_K_M.gguf"); strcpy(legal.description,"legal law");
    p.purpose=5;
    assert(ot_model_candidate_score(&legal,&hardware,&p) > ot_model_candidate_score(&unrelated,&hardware,&p)+.2);
    p.purpose=0;p.setup_engines=2;
    assert(!ot_model_candidate_fits(&variants[1],&hardware,&p));p.setup_engines=3;

    ot_model_candidate near_below = variants[0], near_above = variants[0], small = variants[0], big = variants[0];
    near_below.bytes = UINT64_C(7700000000); near_above.bytes = UINT64_C(8300000000);
    small.bytes = UINT64_C(4000000000); big.bytes = UINT64_C(16000000000);
    p.advice_size_gb = 8;
    assert(ot_model_candidate_fits(&near_above,&hardware,&p)); /* Approximate target, never a hard cap. */
    assert(ot_model_candidate_score(&near_below,&hardware,&p) > ot_model_candidate_score(&small,&hardware,&p));
    assert(ot_model_candidate_score(&near_above,&hardware,&p) > ot_model_candidate_score(&big,&hardware,&p));
    yyjson_doc *size_doc = yyjson_read("{\"adviceSizeGB\":512}",strlen("{\"adviceSizeGB\":512}"),0);
    ot_preferences size_roundtrip;
    assert(size_doc && ot_preferences_read(&size_roundtrip,yyjson_doc_get_root(size_doc)) && size_roundtrip.advice_size_gb==512);
    yyjson_doc_free(size_doc);
    p.advice_size_gb = 0;
    ot_model_candidate family_a = {.kind=2}, family_b = {.kind=2};
    strcpy(family_a.name,"example:4b-q4");strcpy(family_b.name,"example:4b-q8");
    assert(ot_model_candidate_same_family(&family_a,&family_b));
    strcpy(family_b.name,"different:4b-q4");
    assert(!ot_model_candidate_same_family(&family_a,&family_b));
    ot_advice_profile old_needs={.samples=512,.tasks={0,.001,0,0,0,0}};
    assert(fabs(ot_model_candidate_rank(&variants[0],&hardware,&p,&old_needs)-
                ot_model_candidate_score(&variants[0],&hardware,&p))<.001);
    yyjson_mut_doc *settings=yyjson_mut_doc_new(NULL);assert(settings);
    yyjson_mut_val *model_settings=ot_preferences_model_json(settings,&p);
    assert(yyjson_mut_obj_size(model_settings)==11 && !yyjson_mut_obj_get(model_settings,"adviceLearning"));
    yyjson_mut_doc_free(settings);
    ot_model_candidate pair[] = {variants[0],variants[1]};
    pair[0].metric_count = pair[1].metric_count = 1;
    for (unsigned i=0;i<2;i++) {
        strcpy(pair[i].metrics[0].benchmark,"example/quality");
        strcpy(pair[i].metrics[0].task,"accuracy");
        strcpy(pair[i].metrics[0].source,"https://example.com/evaluation");
        pair[i].metrics[0].verified = pair[i].metrics[0].exact = true;
        pair[i].metrics[0].higher_better = true;
        pair[i].tokens_per_second=50;pair[i].first_token_ms=100;
    }
    pair[0].metrics[0].value=90; pair[1].metrics[0].value=80;
    assert(ot_model_candidate_dominates(&pair[0],&pair[1]));
    double scores[] = {.7,.8}; pair[1].verified=true;
    assert(ot_model_candidate_best(pair,scores,2) == 0); /* Qualification cannot revive a dominated choice. */
    pair[0].tokens_per_second=0;
    assert(!ot_model_candidate_dominates(&pair[0],&pair[1]));
    pair[0].tokens_per_second=50;pair[0].metrics[0].exact=false;
    assert(!ot_model_candidate_dominates(&pair[0],&pair[1]));
    pair[0].metrics[0].exact=true;
    double percent[] = {.5,.5}, fraction[] = {.5,.5};
    ot_model_evidence_scores(pair,percent,2);
    pair[0].metrics[0].value=.9;pair[1].metrics[0].value=.8;
    ot_model_evidence_scores(pair,fraction,2);
    assert(fabs(percent[0]-fraction[0]) < 1e-9 && fraction[0]>fraction[1]);
    strcpy(pair[1].metrics[0].configuration,"with tools");
    double unmatched[]={.5,.5};ot_model_evidence_scores(pair,unmatched,2);
    assert(unmatched[0]==.5 && unmatched[1]==.5 && !ot_model_candidate_dominates(&pair[0],&pair[1]));
    ot_model_evidence(&variants[0],"{\"id\":\"example/model\",\"evalResults\":[{\"verified\":true,\"data\":{"
        "\"dataset\":{\"id\":\"example/quality\",\"task_id\":\"accuracy\"},\"value\":90,"
        "\"source\":{\"url\":\"https://example.com/eval\"}}}]}",true);
    assert(variants[0].metric_count==1 && !variants[0].metrics[0].exact && variants[0].base_evidence);
    ot_model_evidence(&variants[0],"{\"id\":\"foreign/model\",\"gguf\":{\"context_length\":42}}",true);
    assert(!variants[0].context); /* An unrelated response cannot rewrite facts. */

    transport t = {0};ot_app *a=open_app(path,&t);
    ot_connection c={.endpoint="http://127.0.0.1:1",.model="fixture"};assert(ot_configure(a,&c,0));
    assert(ot_set_preferences(a,"{\"adviceLearning\":true}"));
    assert(ot_submit(a,"scrivi una funzione in C",0));feed(a,&t,answer,7);ot_end(a,t.operation,NULL);
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==1);
    assert(ot_classify(a,1));
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==0);
    assert(ot_classify(a,0));
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==1);
    assert(ot_submit(a,"another question",0));assert(ot_reset_advice(a));
    feed(a,&t,answer,9);ot_end(a,t.operation,NULL);
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==0);
    assert(ot_submit(a,"user stops",0));ot_stop(a);ot_end(a,t.operation,"cancelled");
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==0);
    assert(ot_set_preferences(a,"{\"adviceLearning\":false}"));
    assert(ot_submit(a,"not for learning",0));feed(a,&t,answer,5);ot_end(a,t.operation,NULL);
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==0);
    assert(ot_set_preferences(a,"{\"adviceLearning\":true}"));
    assert(ot_submit(a,"reset toggle while running",0));
    assert(ot_set_preferences(a,"{\"adviceLearning\":false}"));
    assert(ot_set_preferences(a,"{\"adviceLearning\":true}"));
    feed(a,&t,answer,5);ot_end(a,t.operation,NULL);
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==0);
    statement(path,"WITH RECURSIVE n(x) AS(VALUES(1000) UNION ALL SELECT x+1 FROM n WHERE x<1514) "
        "INSERT INTO turns(id,chat,prompt,answer,state) SELECT x,1,'fixture','','complete' FROM n");
    strcpy(hardware.cpu_name,"fixtureCPU");strcpy(hardware.system_build,"fixtureOS");
    ot_advice_environment(&sample,&p,&hardware);memset(sample.identity,'a',64);sample.identity[64]=0;
    sample.runtime=1;sample.tokens_per_second=20;sample.first_token_ms=100;
    json=ot_advice_encode(&sample);assert(json);
    char *bulk=sqlite3_mprintf("INSERT INTO advice_samples(turn,sample,recorded) SELECT id,%Q,unixepoch()-86400 "
        "FROM turns WHERE id>=1000",json);free(json);assert(bulk);statement(path,bulk);sqlite3_free(bulk);
    assert(ot_store_advice_record(ot_app_store(a),1000,&sample));
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,0) && aggregate.samples==512);
    assert(ot_store_advice_profile(ot_app_store(a),&aggregate,(int64_t)time(NULL)-60) && aggregate.samples==1);
    ot_advice_profile measured[3] = {sample, sample, sample};
    p.gpu_layers=0;ot_advice_environment(&measured[1],&p,&hardware);
    measured[2].identity[0]=0;
    assert(ot_store_advice_performance(ot_app_store(a),measured,3) && measured[0].samples==512 &&
           fabs(measured[0].tokens_per_second-20)<1e-9 && fabs(measured[0].first_token_ms-100)<1e-9 &&
           !measured[1].samples && !measured[2].samples);
    assert(ot_reset_advice(a));
    ot_close(a);free(t.body);unlink(path);
    puts("PASS: variant/evidence frontier, optional metadata, private samples, archive isolation, reset-in-flight and opt-out");
}
static void engine_capabilities_cache(const char *directory, const char *executable) {
    char root[300], path[340], engine[340], counter[340];
    snprintf(root,sizeof root,"%s/engine-capabilities",directory); assert(!mkdir(root,0700));
    snprintf(path,sizeof path,"%s/workspace.sqlite",root);
    snprintf(engine,sizeof engine,"%s/engine",root);
    snprintf(counter,sizeof counter,"%s/count",root);
    assert(!copyfile(executable,engine,NULL,COPYFILE_DATA|COPYFILE_STAT));
    transport t = {0}; ot_app *app = open_app(path,&t);
    char *sql = sqlite3_mprintf("INSERT INTO engines(kind,path) VALUES(1,%Q)",engine);
    assert(sql); statement(path,sql); sqlite3_free(sql);
    assert(!setenv("OT_CHECK_HELP_COUNT",counter,1));
    ot_library *l = ot_library_open(app,root,(ot_library_io){0}); assert(l);
    struct stat count;
    assert(!stat(counter,&count) && count.st_size == 1);
    ot_library_command(l,"{\"action\":\"detectEngines\"}");
    assert(!stat(counter,&count) && count.st_size == 1);
    library_has(l,"\"idleSleep\":true");
    assert(!setenv("OT_CHECK_HELP_LEGACY","1",1));
    struct timespec updated[2];
    assert(!clock_gettime(CLOCK_REALTIME,&updated[0])); updated[1] = updated[0];
    assert(!utimensat(AT_FDCWD,engine,updated,0));
    ot_library_command(l,"{\"action\":\"detectEngines\"}");
    assert(!stat(counter,&count) && count.st_size == 2);
    library_has(l,"\"idleSleep\":false");
    ot_library_close(l); ot_close(app); free(t.body);
    assert(!unsetenv("OT_CHECK_HELP_COUNT") && !unsetenv("OT_CHECK_HELP_LEGACY") &&
           !unlink(engine) && !unlink(counter) && !unlink(path));
    snprintf(path,sizeof path,"%s/workspace.sqlite-wal",root); unlink(path);
    snprintf(path,sizeof path,"%s/workspace.sqlite-shm",root); unlink(path);
    snprintf(path,sizeof path,"%s/models",root); assert(!rmdir(path) && !rmdir(root));
}
static void setup_invariants(const char *path, const char *directory, const char *executable) {
    transport chat = {0};
    ot_app *app = open_app(path, &chat);
    assert(ot_set_preferences(app, "{\"setupEngines\":2}"));
    ot_resources large = {.memory_total = 32ULL << 30,
                          .memory_available = 24ULL << 30,
                          .gpu_recommended = 24ULL << 30,
                          .gpu = true,
                          .unified = true};
    ot_resources_update(app, &large);
    acquisition network = {0};
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &network, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"finish\"}");
    assert(ot_get_preferences(app)->setup == 0);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"fixture:4b\"}");
    library_has(l, "internet_disabled");
    assert(!network.id && !ot_get_preferences(app)->internet && !*ot_get_preferences(app)->setup_target);
    assert(ot_set_preferences(app,"{\"internet\":true,\"network\":0}"));
    unsigned blocked_requests = network.requests;
    ot_library_command(l,"{\"action\":\"search\",\"query\":\"Qwen\"}");
    ot_library_command(l,"{\"action\":\"pull\",\"name\":\"qwen3:0.6b\"}");
    ot_library_command(l,"{\"action\":\"installEngine\",\"kind\":1}");
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"suggest\"}");
    assert(network.requests == blocked_requests);
    assert(!ot_set_preferences(app,"{\"network\":16}") && !ot_set_preferences(app,"{\"network\":-1}"));
    assert(ot_set_preferences(app,"{\"internet\":false,\"network\":15,\"setupEngines\":2}"));
    const ot_preferences *network_preferences = ot_get_preferences(app);
    assert(!ot_network_allowed(network_preferences,OT_NETWORK_CATALOGUES));
    assert(ot_network_allowed(network_preferences,OT_NETWORK_LOCAL));
    ot_preferences restricted = *network_preferences;
    ot_network_enable(&restricted,OT_NETWORK_SERVERS);
    assert(restricted.network == OT_NETWORK_SERVERS && restricted.internet);
    assert(!ot_network_allowed(&restricted,OT_NETWORK_MODELS));
    ot_network_enable(&restricted,OT_NETWORK_CATALOGUES);
    assert(ot_network_allowed(&restricted,OT_NETWORK_SERVERS) && ot_network_allowed(&restricted,OT_NETWORK_CATALOGUES));

    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"purpose\",\"purpose\":1}");
    assert(ot_get_preferences(app)->purpose == 1);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"purpose\",\"purpose\":99}");
    assert(ot_get_preferences(app)->purpose == 1 && !network.id);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"size\",\"gb\":8}");
    assert(ot_get_preferences(app)->advice_size_gb == 8 && !network.id);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"size\",\"gb\":513}");
    assert(ot_get_preferences(app)->advice_size_gb == 8 && !network.id);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"size\",\"gb\":0}");
    assert(!ot_get_preferences(app)->advice_size_gb);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"skip\"}");
    assert(ot_get_preferences(app)->setup == 1);
    ot_library_close(l);
    ot_close(app);
    app = open_app(path, &chat);
    assert(ot_get_preferences(app)->setup == 1 && ot_get_preferences(app)->purpose == 1);
    /* The guide must reuse a library entry and preserve its saved model parameters. */
    statement(
        path,
        "INSERT INTO models(id,name,kind,reference,available) VALUES(42,'fixture:setup',2,'fixture:setup',1);"
        "INSERT INTO generation_parameters(identity,parameters) VALUES('[\"local\",42]',"
        "'{\"context\":3072,\"profile\":\"custom\",\"keepAlive\":\"-1\"}');");
    char *engine_sql = sqlite3_mprintf("INSERT INTO engines(kind,path) VALUES(2,%Q)", executable);
    assert(engine_sql);
    statement(path, engine_sql);
    sqlite3_free(engine_sql);
    large.gpu = false;
    ot_resources_update(app, &large);
    network = (acquisition){0};
    l = ot_library_open(app, directory, (ot_library_io){.context = &network, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"use\",\"id\":42}");
    ot_library_tick(l);
    assert(!strcmp(ot_get_preferences(app)->setup_target, "local:42"));
    const char *found = "{\"models\":[{\"name\":\"fixture:setup\",\"size\":68}]}";
    ot_library_bytes(l, network.id, found, strlen(found));
    ot_library_end(l, network.id, NULL);
    assert(strstr(network.url, "/api/show"));
    ot_library_bytes(l, network.id, "{}", 2);
    ot_library_end(l, network.id, NULL);
    assert(strstr(network.url, "/api/generate"));
    yyjson_doc *preparation = yyjson_read(network.body, strlen(network.body), 0);
    assert(yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(preparation), "keep_alive")) == -1);
    yyjson_doc_free(preparation);
    const char *warmed = "{\"done\":true}";
    ot_library_bytes(l, network.id, warmed, strlen(warmed));
    ot_library_end(l, network.id, NULL);
    ot_library_tick(l);
    assert(ot_ready(app) && ot_get_preferences(app)->setup == 3 && *ot_get_preferences(app)->setup_target);
    ot_preferences persisted = {0};
    assert(ot_store_preferences(ot_app_store(app), &persisted) && persisted.setup == 3 &&
           !strcmp(persisted.setup_target,"local:42"));
    assert(strstr(network.url, "/api/ps"));
    ot_preferences required_gpu = *ot_get_preferences(app);
    required_gpu.require_gpu = true;
    apply_preferences(app, &required_gpu);
    ot_library_end(l, network.id, "engine_timeout");
    assert(ot_ready(app));
    library_has(l, "\"engine\":\"ready\"");
    library_has(l, "\"engineError\":\"\"");
    required_gpu.require_gpu = false;
    apply_preferences(app, &required_gpu);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"finish\"}");
    assert(ot_get_preferences(app)->setup == 2 && !*ot_get_preferences(app)->setup_target);
    assert(ot_get_preferences(app)->context == 3072 && !ot_get_preferences(app)->internet);
    assert(!strcmp(ot_get_preferences(app)->thinking_mode, "off"));
    ot_preferences resolved = *ot_get_preferences(app), restored = ot_preferences_default();
    assert(ot_store_initial_generation(ot_app_store(app), 44, &resolved));
    assert(ot_store_generation(ot_app_store(app), 44, &restored) == 1 && restored.context == 3072);
    resolved.context = 8192;
    assert(ot_store_initial_generation(ot_app_store(app), 42, &resolved));
    assert(ot_store_generation(ot_app_store(app), 42, &restored) == 1 && restored.context == 3072);
    statement(path, "INSERT INTO models(id,name,kind,reference,available) "
                    "VALUES(43,'embedding:fixture',2,'embedding:fixture',1)");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"use\",\"id\":43}");
    ot_library_tick(l);
    ot_library_bytes(l, network.id, found, strlen(found));
    ot_library_end(l, network.id, NULL);
    assert(strstr(network.url, "/api/show"));
    const char *embedding = "{\"capabilities\":[\"embedding\"]}";
    ot_library_bytes(l, network.id, embedding, strlen(embedding));
    ot_library_end(l, network.id, NULL);
    library_has(l, "model_not_chat");
    assert(!ot_ready(app));
    assert(ot_set_preferences(app, "{\"setupTarget\":\"fixture:pending\",\"setupEngines\":2}"));
    ot_library_manual(l);
    assert(!*ot_get_preferences(app)->setup_target);
    /* Engines-only setup completes without a model or any external request. */
    unsigned engines_only_requests=network.requests;
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"enginesOnly\"}");
    assert(ot_get_preferences(app)->setup==3 && !*ot_get_preferences(app)->setup_target &&
           ot_get_preferences(app)->setup_target_engines==2 && !ot_get_preferences(app)->internet);
    library_has(l,"\"enginesOnly\":true");library_has(l,"\"enginesReady\":true");
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"finish\"}");
    assert(ot_get_preferences(app)->setup==2 && !ot_get_preferences(app)->setup_target_engines &&
           network.requests==engines_only_requests && !ot_ready(app));
    /* A paused engine dependency blocks only its model, not unrelated downloads. */
    statement(path, "UPDATE engines SET path='/missing/fixture-ollama' WHERE kind=2");
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"enginesOnly\"}");
    assert(ot_get_preferences(app)->setup==2 && !ot_get_preferences(app)->setup_target_engines &&
           network.requests==engines_only_requests);
    library_has(l,"internet_disabled");
    large.gpu = true;
    ot_resources_update(app, &large);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"suggest\",\"allowDownloads\":true}");
    assert(strstr(network.url, "model-recommendations"));
    library_has(l, "\"recommendations\":[]"); /* Never show only the existing local model mid-fetch. */
    const char *feed_models = "{\"recommendations\":[{\"model\":\"fixture:4b\",\"description\":\"coding\","
                              "\"vram_bytes\":6000000000}]}";
    uint64_t catalogue_id = network.id;
    ot_library_bytes(l, catalogue_id, feed_models, strlen(feed_models));
    ot_library_end(l, catalogue_id, NULL);
    assert(strstr(network.url, "/library/fixture/tags"));
    library_has(l, "\"recommendations\":[]");
    const char *variants = "<a href=\"/library/fixture:4b\"><span class=\"font-mono\">bbbbbbbbbbbb</span>3GB 32K context window</a>";
    ot_library_bytes(l, network.id, variants, strlen(variants));
    ot_library_end(l, network.id, NULL);
    library_has(l, "\"suggestionsReady\":true");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"open\"}");
    library_has(l, "fixture:4b");
    /* Late catalogue callbacks cannot mutate a completed/cancelled operation. */
    ot_library_bytes(l, catalogue_id, "invalid", 7);
    ot_library_end(l, catalogue_id, "connection_failed");
    library_has(l, "\"suggestionError\":\"\"");
    statement(path,"INSERT INTO models(id,name,kind,reference,bytes,available,metadata) VALUES(101,"
        "'fixture:4b',2,'fixture:4b',3000000000,1,'{\"digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}')");
    library_has(l,"\"updateOf\":101");
    uint64_t before_update=network.id;
    ot_library_command(l,"{\"action\":\"advice\",\"mode\":\"acquire\",\"name\":\"fixture:4b\",\"kind\":2}");
    library_has(l,"replacement_changed");assert(network.id==before_update);
    statement(path,"UPDATE models SET metadata='{\"digest\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}' WHERE id=101");
    library_has(l,"\"installed\":101");
    statement(path,"DELETE FROM models WHERE id=101");
    statement(path, "INSERT INTO models(id,name,kind,reference,available,metadata) "
                    "VALUES(99,'fixture:4b',2,'fixture:4b',1,'{\"chatSupported\":false}')");
    char *advice = ot_library_view(l);
    yyjson_doc *advice_doc = yyjson_read(advice, strlen(advice), 0);
    yyjson_val *choices = yyjson_obj_get(yyjson_doc_get_root(advice_doc), "recommendations");
    for (size_t i = 0; i < yyjson_arr_size(choices); i++)
        assert(strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(choices, i), "name")), "fixture:4b"));
    yyjson_doc_free(advice_doc);
    free(advice);
    statement(path, "DELETE FROM models WHERE id=99");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"not-in-catalogue:4b\"}");
    library_has(l, "suggestion_changed");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"fixture:4b\"}");
    for (unsigned i = 0; i < 3000 && !strstr(network.url, "github.com"); i++) {
        ot_library_tick(l);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(strstr(network.url, "github.com") && ot_get_preferences(app)->internet);
    uint64_t active_download = network.id, active_revision = ot_library_revision(l);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"fixture:4b\"}");
    assert(network.id == active_download && ot_library_revision(l) > active_revision);
    statement(path, "INSERT INTO downloads(id,url,name,checksum,total,state) VALUES(999,"
                    "'ollama:unrelated:9b','unrelated:9b','',99,'paused')");
    char *setup_view = ot_library_view(l);
    yyjson_doc *setup_doc = yyjson_read(setup_view, strlen(setup_view), 0);
    yyjson_val *setup_item = yyjson_obj_get(yyjson_doc_get_root(setup_doc), "setupModel");
    assert(yyjson_equals_str(yyjson_obj_get(setup_item,"target"),"fixture:4b") &&
           yyjson_get_sint(yyjson_obj_get(setup_item,"id")) == 0 &&
           yyjson_get_uint(yyjson_obj_get(setup_item,"engines")) == 2 &&
           yyjson_get_sint(yyjson_obj_get(setup_item,"job")) == 2);
    yyjson_doc_free(setup_doc);free(setup_view);
    /* The accepted plan survives Back/Skip; it resumes only after an explicit
     * matching confirmation and current permission. None of these actions may
     * reset the checkpoint, duplicate a job or consume a stale callback. */
    const char *phases[] = {"queued","preparing","downloading","verifying","registering","paused","cancelled","complete"};
    unsigned matrix_cases=0, requests_before_follow=network.requests;
    ot_preferences matrix_original=*ot_get_preferences(app);
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"back\"}");
    ot_library_bytes(l,active_download,"late bytes",10);
    ot_library_end(l,active_download,NULL);
    for (size_t phase=0;phase<sizeof phases/sizeof *phases;phase++)
        for (unsigned mask=2;mask<=2;mask++)
            for (unsigned consent=0;consent<3;consent++) {
                char *sql=sqlite3_mprintf("UPDATE downloads SET state=%Q,received=12345 WHERE id=2",phases[phase]);
                assert(sql);statement(path,sql);sqlite3_free(sql);
                ot_preferences mp=matrix_original;
                mp.internet=consent!=0;mp.network=consent==2 ? 15U : 4U;
                mp.setup_target_engines=mask;mp.setup_paused=false;mp.setup=3;
                yyjson_mut_doc *md=yyjson_mut_doc_new(NULL);assert(md);
                yyjson_mut_doc_set_root(md,ot_preferences_json(md,&mp));
                char *prefs=yyjson_mut_write(md,0,NULL);assert(prefs && ot_set_preferences(app,prefs));
                free(prefs);yyjson_mut_doc_free(md);
                ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"back\"}");
                assert(ot_get_preferences(app)->setup_paused);
                char *raw=ot_library_view(l);assert(raw);
                yyjson_doc *doc=yyjson_read(raw,strlen(raw),0);assert(doc);
                unsigned required=(unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc),"setupModel"),"resumeAccess"));
                assert((required&OT_NETWORK_ENGINES) && (((required&OT_NETWORK_MODELS)!=0)==(strcmp(phases[phase],"complete")!=0)));
                bool allowed=ot_network_allowed(&mp,(ot_network_scope)required);
                yyjson_doc_free(doc);free(raw);
                const char *held=!strcmp(phases[phase],"complete") || !strcmp(phases[phase],"cancelled") ? phases[phase] : "paused";
                sqlite3 *db=NULL;sqlite3_stmt *q=NULL;assert(sqlite3_open(path,&db)==SQLITE_OK);
                assert(sqlite3_prepare_v2(db,"SELECT received,state,(SELECT count(*) FROM downloads) FROM downloads WHERE id=2",-1,&q,NULL)==SQLITE_OK);
                assert(sqlite3_step(q)==SQLITE_ROW && sqlite3_column_int(q,0)==12345 &&
                    !strcmp((const char*)sqlite3_column_text(q,1),held) && sqlite3_column_int(q,2)==3);
                sqlite3_finalize(q);sqlite3_close(db);
                char follow[160];snprintf(follow,sizeof follow,"{\"action\":\"setup\",\"mode\":\"follow\",\"target\":\"fixture:4b\",\"engines\":%u}",mask);
                ot_library_command(l,follow);
                assert(ot_get_preferences(app)->setup_paused==!allowed);
                ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"skip\"}");
                assert(ot_get_preferences(app)->setup==1 && ot_get_preferences(app)->setup_paused);
                ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"open\"}");
                ot_library_command(l,follow);
                assert(ot_get_preferences(app)->setup_paused==!allowed);
                ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"follow\",\"target\":\"different:4b\",\"engines\":2}");
                assert(!strcmp(ot_get_preferences(app)->setup_target,"fixture:4b"));
                ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"finish\"}");
                assert(ot_get_preferences(app)->setup==3 && network.requests==requests_before_follow);
                matrix_cases++;
            }
    assert(matrix_cases==24);
    yyjson_mut_doc *matrix_restore=yyjson_mut_doc_new(NULL);assert(matrix_restore);
    yyjson_mut_doc_set_root(matrix_restore,ot_preferences_json(matrix_restore,&matrix_original));
    char *restore_preferences=yyjson_mut_write(matrix_restore,0,NULL);
    assert(restore_preferences && ot_set_preferences(app,restore_preferences));
    free(restore_preferences);yyjson_mut_doc_free(matrix_restore);
    statement(path,"UPDATE downloads SET state='queued',received=0 WHERE id=2");
    puts("PASS: 24 setup phase/permission combinations, Back/Skip/reopen/resume/stale callback/finish");
    statement(path, "DELETE FROM downloads WHERE id=999;UPDATE sqlite_sequence SET seq=(SELECT max(id) FROM downloads) WHERE name='downloads'");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"skip\"}");
    assert(!strcmp(ot_get_preferences(app)->setup_target,"fixture:4b"));
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"open\"}");
    statement(path, "INSERT INTO downloads(id,url,name,checksum,total,state) "
                    "VALUES(3,'https://example.com/additional.gguf','additional.gguf',"
                    "'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff',68,'queued')");
    ot_library_command(l, "{\"action\":\"pause\",\"id\":1}");
    for (unsigned i = 0; i < 3000 && !strstr(network.url, "additional.gguf"); i++) {
        ot_library_tick(l);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(strstr(network.url, "additional.gguf"));
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":3}");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":2}");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":1}");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"reset\"}");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"skip\"}");
    library_has(l, "\"suggesting\":false"); /* Closing retains the durable catalogue. */
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"suggest\",\"refresh\":true}");
    assert(strstr(network.url, "model-recommendations"));
    ot_library_end(l, network.id, "connection_failed");
    assert(!strcmp(network.url, "https://ollama.com/search?o=popular") && !*network.body);
    const char *popular =
        "<a href=\"/library/embed\"><span>embedding</span></a><a href=\"/library/fallback\">Chat</a>";
    ot_library_bytes(l, network.id, popular, strlen(popular));
    ot_library_end(l, network.id, NULL);
    assert(!strcmp(network.url, "https://ollama.com/library/fallback/tags"));
    uint64_t cancelled_catalogue = network.id;
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"cancelSuggestions\"}");
    ot_library_bytes(l, cancelled_catalogue, "invalid", 7);
    ot_library_end(l, cancelled_catalogue, "connection_failed");
    library_has(l, "\"suggesting\":false");
    /* Engine changes cancel stale catalogues and filter both online and local entries. */
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"engines\",\"engines\":0}");
    assert(ot_get_preferences(app)->setup_engines == 2);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"engines\",\"engines\":1}");
    assert(ot_get_preferences(app)->setup_engines == 1 && strstr(network.url, "huggingface.co/api/models?"));
    const char *hf_feed = "[{\"id\":\"fixture/model\",\"pipeline_tag\":\"text-generation\",\"gated\":false,"
                          "\"gguf\":{\"total\":4000000000,\"context_length\":32768}}]";
    uint64_t recent_id = 0, popular_id = network.id;
    for (unsigned i = 0; i < 16; i++)
        if (strstr(network.recent[i].url, "sort=lastModified")) recent_id = network.recent[i].id;
    assert(recent_id && recent_id != popular_id);
    // The second feed may finish first, but neither partial results nor response
    // order may determine the recommendation. The cached reply is consumed once.
    assert(ot_library_headers(l, recent_id, 200, NULL, NULL, 2));
    ot_library_bytes(l, recent_id, "[]", 2);
    ot_library_end(l, recent_id, NULL);
    library_has(l, "\"recommendations\":[]");
    ot_library_bytes(l, popular_id, hf_feed, strlen(hf_feed));
    ot_library_end(l, popular_id, NULL);
    assert(strstr(network.url, "fixture/model?blobs=true"));
    library_has(l, "\"recommendations\":[]");
    const char *hf_variants =
        "{\"id\":\"fixture/model\",\"sha\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"gated\":false,\"gguf\":{\"chat_template\":\"fixture\",\"context_length\":32768},\"siblings\":["
        "{\"rfilename\":\"mmproj-Q4_K_M.gguf\",\"lfs\":{\"sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":1000}},"
        "{\"rfilename\":\"model-Q4_K_M-00001-of-00002.gguf\",\"lfs\":{\"sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":1000}},"
        "{\"rfilename\":\"model-Q4_K_M.gguf\",\"lfs\":{\"sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":3000000000}},"
        "{\"rfilename\":\"model-Q8_0.gguf\",\"lfs\":{\"sha256\":"
        "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"size\":4000000000}}]}";
    ot_library_bytes(l, network.id, hf_variants, strlen(hf_variants));
    ot_library_end(l, network.id, NULL);
    assert(strstr(network.url, "expand[]=evalResults"));
    ot_library_bytes(l, network.id, "{}", 2);
    ot_library_end(l, network.id, NULL);
    uint64_t request_before_open = network.id;
    assert(ot_set_preferences(app,"{\"setup\":2,\"internet\":true,\"network\":15,\"setupEngines\":1,\"purpose\":1}")); /* Notices belong to the configured app, not the setup shortlist. */
    ot_library_command(l, "{\"action\":\"advice\",\"mode\":\"open\"}");
    assert(network.id == request_before_open);
    char *post_setup = ot_library_view(l);
    yyjson_doc *post_doc = yyjson_read(post_setup, strlen(post_setup), 0);
    yyjson_val *post_choices = yyjson_obj_get(yyjson_doc_get_root(post_doc), "adviceRecommendations");
    assert(yyjson_arr_size(post_choices) == 1);
    const char *winner = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(post_choices, 0), "option"));
    assert(winner && !strncmp(winner, "gguf:", 5));
    /* Seeing a recommendation clears only its notice, never the recommendation.
     * Reading is local/durable and must not start a new network request. */
    yyjson_val *new_notice = yyjson_obj_get(yyjson_doc_get_root(post_doc), "recommendationNotice");
    const char *notice_key = yyjson_get_str(yyjson_obj_get(new_notice, "key"));
    assert(notice_key);
    char seen[384];
    snprintf(seen, sizeof seen, "{\"action\":\"advice\",\"mode\":\"seen\",\"key\":\"%s\"}", notice_key);
    ot_library_command(l, seen);
    assert(network.id == request_before_open && !strcmp(ot_get_preferences(app)->advice_seen, notice_key));
    char *seen_view = ot_library_view(l);
    yyjson_doc *seen_doc = yyjson_read(seen_view, strlen(seen_view), 0);
    assert(!yyjson_obj_get(yyjson_doc_get_root(seen_doc), "recommendationNotice") &&
           yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(seen_doc), "adviceRecommendations")) == 1);
    yyjson_doc_free(seen_doc); free(seen_view);
    ot_library_command(l, "{\"action\":\"advice\",\"mode\":\"seen\",\"key\":\"invalid key\"}");
    library_has(l, "invalid_input");
    assert(!strcmp(ot_get_preferences(app)->advice_seen, notice_key));
    char ignore[384];
    snprintf(ignore, sizeof ignore, "{\"action\":\"advice\",\"mode\":\"ignore\",\"key\":\"%s\"}", winner);
    ot_library_command(l, ignore);
    char *alternative = ot_library_view(l);
    yyjson_doc *alternative_doc = yyjson_read(alternative, strlen(alternative), 0);
    yyjson_val *next_choices = yyjson_obj_get(yyjson_doc_get_root(alternative_doc), "adviceRecommendations");
    /* Ignoring a quantization cannot hide the eligible alternative from the
     * same family merely because its ignored sibling had a higher rank. */
    assert(yyjson_arr_size(next_choices) == 1);
    assert(strcmp(winner, yyjson_get_str(yyjson_obj_get(yyjson_arr_get(next_choices, 0), "option"))));
    assert(!yyjson_obj_get(yyjson_doc_get_root(alternative_doc), "recommendationNotice"));
    yyjson_doc_free(alternative_doc); free(alternative);
    yyjson_doc_free(post_doc); free(post_setup);
    ot_library_command(l, "{\"action\":\"advice\",\"mode\":\"restoreNotices\"}");
    ot_library_command(l, "{\"action\":\"advice\",\"mode\":\"close\"}");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"open\"}");
    advice = ot_library_view(l);
    advice_doc = yyjson_read(advice, strlen(advice), 0);
    choices = yyjson_obj_get(yyjson_doc_get_root(advice_doc), "recommendations");
    assert(yyjson_arr_size(choices));
    for (size_t i = 0; i < yyjson_arr_size(choices); i++)
        assert(yyjson_get_int(yyjson_obj_get(yyjson_arr_get(choices, i), "kind")) == 1);
    yyjson_doc_free(advice_doc);
    free(advice);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"fixture:4b\",\"kind\":2}");
    library_has(l, "suggestion_changed");
    statement(path, "INSERT INTO engines(kind,path) VALUES(1,'/missing/fixture-llama')");
    ot_library_command(
        l, "{\"action\":\"setup\",\"mode\":\"prepare\",\"name\":\"model-Q4_K_M.gguf\",\"kind\":1}");
    assert(!strncmp(ot_get_preferences(app)->setup_target, "gguf:bbbb", 9));
    assert(strstr(network.url, "llama-b11960"));
    sqlite3 *database = NULL;
    sqlite3_stmt *download = NULL;
    assert(sqlite3_open(path, &database) == SQLITE_OK);
    assert(sqlite3_prepare_v2(database, "SELECT url FROM downloads WHERE name='model-Q4_K_M.gguf'", -1,
                              &download, NULL) == SQLITE_OK);
    assert(sqlite3_step(download) == SQLITE_ROW &&
           !strcmp((const char *)sqlite3_column_text(download, 0),
                   "https://huggingface.co/fixture/model/resolve/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"
                   "model-Q4_K_M.gguf"));
    sqlite3_finalize(download);
    sqlite3_close(database);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"engines\",\"engines\":3}");
    assert(ot_get_preferences(app)->setup_engines == 3 && strstr(network.url, "model-recommendations"));
    uint64_t stale = network.id;
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"engines\",\"engines\":1}");
    ot_library_bytes(l, stale, feed_models, strlen(feed_models));
    ot_library_end(l, stale, NULL);
    assert(network.id == stale); // Switching back reuses the complete, fresh HF cache.
    library_has(l, "\"suggesting\":false");
    assert(ot_get_preferences(app)->setup_target_engines == 1);
    // Reject an oversized read-ahead reply, cancel both feeds, and ignore every
    // late callback. A fresh accepted snapshot remains usable without I/O.
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"suggest\",\"refresh\":true}");
    uint64_t abandoned_popular = network.id, abandoned_recent = network.id - 1;
    assert(!ot_library_headers(l, abandoned_recent, 200, NULL, NULL, 1048577));
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"cancelSuggestions\"}");
    ot_library_bytes(l, abandoned_recent, hf_feed, strlen(hf_feed));
    ot_library_end(l, abandoned_recent, NULL);
    ot_library_end(l, abandoned_popular, "connection_failed");
    library_has(l, "\"suggestionsReady\":true");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"suggest\"}");
    assert(network.id == abandoned_popular); // Cancellation retains fresh cache validity too.
    statement(path, "INSERT INTO downloads(id,url,name,checksum,total,state) VALUES(6,"
                    "'https://example.com/unrelated.gguf','unrelated.gguf',"
                    "'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',68,'queued')");
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"use\",\"id\":42,\"engines\":2,\"allowDownloads\":true}");
    assert(ot_get_preferences(app)->setup_target_engines == 2);
    assert(sqlite3_open(path, &database) == SQLITE_OK);
    assert(sqlite3_prepare_v2(database, "SELECT (SELECT count(*) FROM downloads WHERE id=6 AND state='queued') - "
        "(SELECT count(*) FROM downloads WHERE id IN(4,5) AND coalesce(json_extract(metadata,'$.discard'),0)=0)", -1, &download, NULL) == SQLITE_OK);
    assert(sqlite3_step(download) == SQLITE_ROW && sqlite3_column_int(download,0) == 1);
    sqlite3_finalize(download);sqlite3_close(database);
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":1}");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":6}");

    /* A ready external connection must not require either local engine. */
    ot_library_manual(l);
    assert(ot_set_preferences(app, "{\"setupEngines\":3}"));
    ot_connection external = *ot_configuration(app);
    strcpy(external.model, "fixture-external");
    external.ollama = false;
    assert(ot_configure(app, &external, 0));
    ot_connection_ready(app, true);
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"finish\"}");
    assert(ot_get_preferences(app)->setup == 2);
    for (unsigned i=0;i<1000;i++) {
        ot_library_tick(l);
        sqlite3_stmt *pending=NULL;
        assert(sqlite3_prepare_v2(ot_store_connection(ot_app_store(app)),"SELECT count(*) FROM downloads WHERE json_extract(metadata,'$.pullCleanup')=1",-1,&pending,NULL)==SQLITE_OK);
        assert(sqlite3_step(pending)==SQLITE_ROW);bool cleaning=sqlite3_column_int(pending,0)!=0;sqlite3_finalize(pending);
        if (!cleaning) break;
        assert(i<999);struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    char source[4096], output[4096], engines[4096];
    snprintf(source, sizeof source, "%s/invalid.package", directory);
    int fd = open(source, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
    atomic_bool cancelled = false;
    const ot_package *package = ot_package_for_kind(2);
    assert(!ot_package_install(package, directory, source, &cancelled, output));
    atomic_store(&cancelled, true);
    assert(!ot_package_install(package, directory, source, &cancelled, output));
    assert(!ot_package_for_download(package->url, "changed"));
    unlink(source);
    assert(!mkfifo(source, 0600));
    atomic_store(&cancelled, false);
    assert(!ot_package_install(package, directory, source, &cancelled, output));
    assert(!ot_engine_validate(source, true));
    unlink(source);
    char installation[4096], marker[4096];
    snprintf(installation, sizeof installation, "%s/engines/ollama-%s", directory, package->version);
    assert(!mkdir(installation, 0700));
    assert(snprintf(marker, sizeof marker, "%s/package.sha256", installation) < (int)sizeof marker);
    assert(!mkfifo(marker, 0600));
    strcpy(output, "unchanged");
    assert(!ot_package_find(package, directory, output) && !strcmp(output, "unchanged"));
    assert(!unlink(marker));
    fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, package->checksum, 64) == 64 && !close(fd));
    /* Missing executable with a valid marker must not overwrite an aliased input. */
    strcpy(output, source);
    assert(!ot_package_install(package, directory, output, &cancelled, output) && !strcmp(output, source));
    char managed_executable[4096], license[4096], license_link[4096], fifo_notice[4096], large_license[4096];
    assert(snprintf(managed_executable,sizeof managed_executable,"%s/ollama",installation) < (int)sizeof managed_executable);
    assert(!link(executable,managed_executable));
    assert(snprintf(license,sizeof license,"%s/LICENSE",installation) < (int)sizeof license);
    fd = open(license,O_WRONLY|O_CREAT|O_EXCL,0600);
    assert(fd >= 0 && write(fd,"Fixture license",15)==15 && !close(fd));
    assert(snprintf(license_link,sizeof license_link,"%s/LICENSE_LINK",installation) < (int)sizeof license_link);
    assert(!symlink(license,license_link));
    assert(snprintf(fifo_notice,sizeof fifo_notice,"%s/NOTICE_FIFO",installation) < (int)sizeof fifo_notice);
    assert(!mkfifo(fifo_notice,0600));
    assert(snprintf(large_license,sizeof large_license,"%s/LICENSE_LARGE",installation) < (int)sizeof large_license);
    fd = open(large_license,O_WRONLY|O_CREAT|O_EXCL,0600);
    assert(fd >= 0);
    char large_block[4096]; memset(large_block,'A',sizeof large_block);
    for (unsigned i=0;i<65;i++) assert(write(fd,large_block,sizeof large_block)==sizeof large_block);
    assert(!close(fd));
    char *notices = ot_package_notices(package,directory); assert(notices);
    yyjson_doc *notices_doc = yyjson_read(notices,strlen(notices),0);
    assert(yyjson_arr_size(yyjson_doc_get_root(notices_doc))==1 && strstr(notices,"Fixture license"));
    yyjson_doc_free(notices_doc);free(notices);
    assert(!unlink(managed_executable) && !unlink(license) && !unlink(license_link) &&
           !unlink(fifo_notice) && !unlink(large_license));
    assert(!unlink(marker) && !rmdir(installation));
    /* A valid archive/checksum containing an invalid engine must never be published. */
    const char invalid[] = "not an executable";
    fd = open(source, O_WRONLY | O_CREAT | O_EXCL, 0700);
    assert(fd >= 0 && write(fd, invalid, sizeof invalid) == sizeof invalid && !close(fd));
    char archive_path[4096];
    snprintf(archive_path, sizeof archive_path, "%s/fixture.tar", directory);
    snprintf(engines, sizeof engines, "%s", directory);
    char tar_path[4096];
    assert(realpath("/usr/bin/tar", tar_path));
    char *tar_args[] = {tar_path, "-cf", archive_path, "-C", engines, "invalid.package", NULL};
    char *tar_result = ot_process_capture(tar_args[0], tar_args, 8, &cancelled, true, -1);
    assert(tar_result);
    free(tar_result);
    fd = open(archive_path, O_RDONLY);
    struct stat archive_stat;
    assert(fd >= 0 && !fstat(fd, &archive_stat) && archive_stat.st_size > 0 && archive_stat.st_size < 65536);
    size_t archive_size = (size_t)archive_stat.st_size;
    char *archive_bytes = malloc(archive_size);
    assert(archive_bytes && read(fd, archive_bytes, archive_size) == (ssize_t)archive_size && !close(fd));
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char checksum[65];
    CC_SHA256(archive_bytes, (CC_LONG)archive_size, digest);
    free(archive_bytes);
    for (unsigned i = 0; i < sizeof digest; i++)
        snprintf(checksum + i * 2, 3, "%02x", digest[i]);
    ot_package invalid_package = {.kind = 1,
                                  .name = "fixture",
                                  .version = "invalid",
                                  .entry = "invalid.package",
                                  .checksum = checksum,
                                  .bytes = archive_size,
                                  .unpacked_limit = 65536};
    assert(!ot_package_install(&invalid_package, directory, archive_path, &cancelled, output));
    assert(!ot_package_find(&invalid_package, directory, output));
    assert(snprintf(installation, sizeof installation, "%s/engines/invalid.package-invalid", directory) <
           (int)sizeof installation);
    assert(access(installation, F_OK) != 0 && errno == ENOENT);
    assert(!unlink(archive_path) && !unlink(source));
    snprintf(engines, sizeof engines, "%s/engines", directory);
    assert(!rmdir(engines));
    snprintf(engines, sizeof engines, "%s/models", directory);
    assert(!rmdir(engines));
}
static void write_fixture(const char *path,const char *data) {
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    size_t n=strlen(data);assert(fd>=0 && write(fd,data,n)==(ssize_t)n && !close(fd));
}
static int64_t db_integer(ot_app *app,const char *sql) {
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(ot_store_connection(ot_app_store(app)),sql,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);int64_t result=sqlite3_column_int64(q,0);sqlite3_finalize(q);return result;
}
static void cleanup_downloads(ot_app *app,ot_library *l) {
    for(unsigned i=0;db_integer(app,"SELECT count(*) FROM downloads WHERE json_extract(metadata,'$.pullCleanup')=1");i++) {
        assert(i<3000);ot_library_tick(l);struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
}
static void setup_reconsideration(const char *path,const char *directory) {
    transport chat={0};acquisition network={0};ot_app *app=open_app(path,&chat);
    ot_library *l=ot_library_open(app,directory,(ot_library_io){.context=&network,.request=acquisition_request});assert(l);settle_library(l);
    const char *phases[]={"queued","preparing","downloading","verifying","registering","paused","cancelled","complete"};
    unsigned cases=0;
    for(size_t phase=0;phase<8;phase++) for(int kind=1;kind<=2;kind++)
        for(unsigned both=0;both<2;both++) for(unsigned borrowed=0;borrowed<2;borrowed++) {
            unsigned mask=both ? 3U : (unsigned)kind;
            statement(path,"DELETE FROM downloads;DELETE FROM models;DELETE FROM engines;DELETE FROM sqlite_sequence WHERE name='downloads'");
            char hash[65];memset(hash,'a',64);hash[64]=0;char target[257];
            snprintf(target,sizeof target,kind==1 ? "gguf:%s" : "fixture:one",hash);
            ot_preferences p=ot_preferences_default();p.setup=3;p.setup_target_engines=mask;p.setup_engines=mask;
            snprintf(p.setup_target,sizeof p.setup_target,"%s",target);
            yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);assert(d);yyjson_mut_doc_set_root(d,ot_preferences_json(d,&p));
            char *json=yyjson_mut_write(d,0,NULL);assert(json && ot_set_preferences(app,json));free(json);yyjson_mut_doc_free(d);
            for(int engine=1;engine<=2;engine++) if(mask&(unsigned)engine) {
                const ot_package *package=ot_package_for_kind(engine);
                char *sql=sqlite3_mprintf("INSERT INTO downloads(id,url,name,checksum,total,state,metadata) VALUES(%d,%Q,%Q,%Q,%llu,'paused','{\"setup\":true}')",
                    engine,package->url,package->name,package->checksum,package->bytes);assert(sql);statement(path,sql);sqlite3_free(sql);
            }
            char *sql=sqlite3_mprintf("INSERT INTO downloads(id,url,name,checksum,total,state,received,metadata) VALUES(10,%Q,'previous',%Q,68,%Q,19,%Q);"
                "INSERT INTO downloads(id,url,name,checksum,total,state,received) VALUES(99,'https://example.com/manual.gguf','manual',%Q,68,'paused',17);"
                "INSERT INTO models(id,name,kind,reference,available) VALUES(42,'existing',%d,'existing',1)",
                kind==1 ? "https://example.com/one.gguf" : "ollama:fixture:one",kind==1 ? hash : "",phases[phase],
                borrowed ? "{\"setup\":true,\"setupBorrowed\":true}" : "{\"setup\":true}",hash,kind);
            assert(sql);statement(path,sql);sqlite3_free(sql);
            char part[4096];assert(snprintf(part,sizeof part,"%s/models/10.part",directory)<(int)sizeof part);
            if(kind==1 && phase!=7)write_fixture(part,"nineteen bytes here");
            unsigned requests=network.requests;
            ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"back\"}");
            assert(ot_get_preferences(app)->setup_paused && network.requests==requests);
            assert(db_integer(app,"SELECT count(*) FROM downloads WHERE json_extract(metadata,'$.setup')=1 AND state NOT IN('paused','cancelled','complete')")==0);
            assert(db_integer(app,"SELECT received FROM downloads WHERE id=10")==19);
            ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"skip\"}");
            assert(ot_get_preferences(app)->setup_paused && ot_get_preferences(app)->setup==1);
            ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"choose\"}");
            /* Draft edits are reversible: no committed task/file is discarded yet. */
            assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id=10")==1);
            char command[180];snprintf(command,sizeof command,
                "{\"action\":\"setup\",\"mode\":\"use\",\"id\":42,\"engines\":%u,\"allowDownloads\":true}",mask);
            ot_library_command(l,command);
            assert(!strcmp(ot_get_preferences(app)->setup_target,"local:42") && !ot_get_preferences(app)->setup_paused);
            ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"back\"}");cleanup_downloads(app,l);
            assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id=99 AND state='paused' AND received=17")==1);
            assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id IN(1,2)")==(both ? 2 : 1));
            if(phase==7)assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id=10 AND state='complete'")==1);
            else if(borrowed) {
                assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id=10 AND received=19 AND json_extract(metadata,'$.setup') IS NULL")==1);
                if(kind==1)assert(!access(part,F_OK));
            } else {
                assert(db_integer(app,"SELECT count(*) FROM downloads WHERE id=10")==0);
                if(kind==1)assert(access(part,F_OK));
            }
            if(kind==1 && phase!=7 && borrowed)assert(!unlink(part));
            cases++;
        }
    assert(cases==64);
    /* Persistence after Back is authoritative; even a queued row cannot bypass it. */
    statement(path,"UPDATE downloads SET state='queued' WHERE id IN(1,2)");
    ot_library_close(l);ot_close(app);app=open_app(path,&chat);
    assert(ot_get_preferences(app)->setup_paused && !strcmp(ot_get_preferences(app)->setup_target,"local:42"));
    unsigned requests=network.requests;
    l=ot_library_open(app,directory,(ot_library_io){.context=&network,.request=acquisition_request});assert(l);settle_library(l);
    assert(db_integer(app,"SELECT count(*) FROM downloads WHERE state='queued' AND json_extract(metadata,'$.setup')=1")==0);
    assert(network.requests==requests);
    ot_library_command(l,"{\"action\":\"setup\",\"mode\":\"reset\"}");cleanup_downloads(app,l);
    statement(path,"DELETE FROM downloads;DELETE FROM models;DELETE FROM engines;INSERT INTO engines(kind,path) VALUES(1,'/usr/bin/true')");
    unsigned char bytes[68]={0x47,0x47,0x55,0x46,3};bytes[8]=1;bytes[24]=1;bytes[32]='x';bytes[33]=1;bytes[37]=1;
    unsigned char digest[32];char checksum[65];CC_SHA256(bytes,sizeof bytes,digest);
    for(unsigned i=0;i<32;i++)snprintf(checksum+i*2,3,"%02x",digest[i]);
    char partial[4096];assert(snprintf(partial,sizeof partial,"%s/models/10.part",directory)<(int)sizeof partial);
    int fd=open(partial,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0 && write(fd,bytes,sizeof bytes)==sizeof bytes && !close(fd));
    char *sql=sqlite3_mprintf("INSERT INTO downloads(id,url,name,checksum,total,state,received,metadata) VALUES(10,'https://example.com/local.gguf','local',%Q,68,'paused',68,'{\"setup\":true}')",checksum);
    assert(sql);statement(path,sql);sqlite3_free(sql);
    char prefs[600];snprintf(prefs,sizeof prefs,"{\"setup\":3,\"setupPaused\":true,\"setupTarget\":\"gguf:%s\",\"setupTargetEngines\":1,\"setupEngines\":1}",checksum);
    assert(ot_set_preferences(app,prefs));requests=network.requests;
    char follow[256];snprintf(follow,sizeof follow,"{\"action\":\"setup\",\"mode\":\"follow\",\"target\":\"gguf:%s\",\"engines\":1}",checksum);
    ot_library_command(l,follow);assert(!ot_get_preferences(app)->setup_paused);
    for(unsigned i=0;!db_integer(app,"SELECT count(*) FROM downloads WHERE id=10 AND state='complete'");i++) {
        assert(i<3000);ot_library_tick(l);struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    assert(network.requests==requests && !ot_get_preferences(app)->internet);
    /* Complete engine archives also finish locally after consent is revoked.
     * Corrupt bytes must fail integrity checks rather than trigger a new request. */
    const ot_package *offline_package = ot_package_for_kind(1);
    statement(path, "UPDATE engines SET path='/missing/fixture-llama' WHERE kind=1");
    assert(snprintf(partial, sizeof partial, "%s/models/11.part", directory) < (int)sizeof partial);
    fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !ftruncate(fd, (off_t)offline_package->bytes) && !close(fd));
    sql = sqlite3_mprintf("INSERT INTO downloads(id,url,name,checksum,total,state,received,metadata) "
                         "VALUES(11,%Q,'engine',%Q,%llu,'paused',%llu,'{\"setup\":true}')",
                         offline_package->url, offline_package->checksum,
                         (unsigned long long)offline_package->bytes, (unsigned long long)offline_package->bytes);
    assert(sql); statement(path, sql); sqlite3_free(sql);
    assert(ot_set_preferences(app, "{\"setup\":3,\"setupPaused\":true,\"setupTargetEngines\":1,\"setupEngines\":1}"));
    ot_library_command(l, "{\"action\":\"setup\",\"mode\":\"follow\",\"target\":\"\",\"engines\":1}");
    assert(!ot_get_preferences(app)->setup_paused);
    for (unsigned i = 0; !db_integer(app, "SELECT count(*) FROM downloads WHERE id=11 AND error='checksum_mismatch'"); i++) {
        assert(i < 3000); ot_library_tick(l);
        struct timespec delay = {.tv_nsec = 1000000}; nanosleep(&delay, NULL);
    }
    assert(network.requests == requests && !ot_get_preferences(app)->internet);
    assert(!unlink(partial));
    ot_library_close(l);ot_close(app);free(chat.body);assert(!unlink(path));
    assert(snprintf(partial,sizeof partial,"%s/models/10.gguf",directory)<(int)sizeof partial && !unlink(partial));
    char models[4096];assert(snprintf(models,sizeof models,"%s/models",directory)<(int)sizeof models && !rmdir(models));
    puts("PASS: 64 reconsideration phase/engine/format/ownership combinations, shared dependencies, completed resources, draft edits and held restart");
}
static void local_route_lifecycle(const char *path,const char *directory) {
    transport chat={0};acquisition network={0};ot_app *app=open_app(path,&chat);
    statement(path,"INSERT INTO models(id,name,kind,reference,available) VALUES(42,'fixture:route',2,'fixture:route',1)");
    ot_connection route=*ot_configuration(app);strcpy(route.model,"fixture:route");strcpy(route.endpoint,"http://127.0.0.1:54321");route.ollama=true;
    assert(ot_configure(app,&route,42) && ot_local_model(app)==42);
    assert(ot_set_preferences(app,"{\"setup\":3,\"setupTarget\":\"local:42\",\"setupTargetEngines\":2,\"setupEngines\":2,\"setupPaused\":true}"));
    ot_close(app);app=open_app(path,&chat);
    assert(ot_local_model(app)==42 && !ot_ready(app));
    ot_connection_failed(app,"connection_failed");absent(app,"\"error\":\"connection_failed\"");
    ot_library *l=ot_library_open(app,directory,(ot_library_io){.context=&network,.request=acquisition_request});assert(l);settle_library(l);
    assert(ot_library_manages_model(l) && !ot_ready(app) && !strstr(network.url,":54321"));
    library_has(l,"\"engine\":\"stopped\"");library_has(l,"\"requested\":0");
    has(app,"\"localModel\":42");
    ot_library_close(l);ot_close(app);
    /* A held download for another model cannot suppress the independent startup selection. */
    app=open_app(path,&chat);
    assert(ot_set_preferences(app,"{\"setup\":3,\"setupTarget\":\"fixture:other\",\"setupTargetEngines\":2,\"setupEngines\":2,\"setupPaused\":true}"));
    l=ot_library_open(app,directory,(ot_library_io){.context=&network,.request=acquisition_request});assert(l);settle_library(l);
    library_has(l,"\"requested\":42");assert(!strstr(network.url,":54321"));
    ot_library_close(l);ot_close(app);
    app=open_app(path,&chat);assert(ot_set_preferences(app,"{\"startupModel\":-1}"));
    l=ot_library_open(app,directory,(ot_library_io){.context=&network,.request=acquisition_request});assert(l);settle_library(l);
    assert(!ot_local_model(app) && !*ot_configuration(app)->model && !ot_ready(app) && !ot_library_manages_model(l));
    assert(db_integer(app,"SELECT model_id FROM settings WHERE id=1")==42);
    assert(ot_configure(app,&route,0));ot_library_manual(l);
    assert(!ot_local_model(app) && !ot_library_manages_model(l));
    ot_connection_failed(app,"connection_failed");has(app,"\"error\":\"connection_failed\"");
    ot_library_close(l);ot_close(app);free(chat.body);assert(!unlink(path));
    char models[4096];assert(snprintf(models,sizeof models,"%s/models",directory)<(int)sizeof models && !rmdir(models));
    puts("PASS: durable local route, held selection, independent startup, no stale-port restore, None and explicit server errors");
}
static void pull_ownership(const char *directory) {
    char profile[4096],shared[4096],models[4096],blobs[4096],stage[4096],folder[4096],file[4096],published[4096];
    assert(snprintf(profile,sizeof profile,"%s/pull-fixture",directory)<(int)sizeof profile && !mkdir(profile,0700));
    assert(snprintf(shared,sizeof shared,"%s/shared",profile)<(int)sizeof shared && !mkdir(shared,0700));
    assert(!setenv("OLLAMA_MODELS",shared,1));
    assert(snprintf(models,sizeof models,"%s/models",profile)<(int)sizeof models && !mkdir(models,0700));
    assert(snprintf(blobs,sizeof blobs,"%s/blobs",shared)<(int)sizeof blobs && !mkdir(blobs,0700));
    const char *data="verified immutable blob";unsigned char raw[32];char hash[65];
    CC_SHA256(data,(CC_LONG)strlen(data),raw);
    for(unsigned i=0;i<32;i++)snprintf(hash+i*2,3,"%02x",raw[i]);
    assert(snprintf(published,sizeof published,"%s/sha256-%s",blobs,hash)<(int)sizeof published);
    write_fixture(published,"corrupt shared cache");
    atomic_bool cancel=false;
    assert(!ot_pull_prepare(profile,0,&cancel,stage));
    atomic_store(&cancel,true);assert(!ot_pull_prepare(profile,1,&cancel,stage));atomic_store(&cancel,false);
    assert(ot_pull_prepare(profile,1,&cancel,stage));
    assert(snprintf(folder,sizeof folder,"%s/manifests",stage)<(int)sizeof folder && !mkdir(folder,0700));
    assert(snprintf(file,sizeof file,"%s/sample",folder)<(int)sizeof file);
    write_fixture(file,"{invalid");assert(!ot_pull_publish(profile,1,&cancel));
    char manifest[1024];snprintf(manifest,sizeof manifest,
        "{\"schemaVersion\":2,\"config\":{\"digest\":\"sha256:%s\",\"size\":%zu},\"layers\":[]}",hash,strlen(data));
    write_fixture(file,manifest);assert(!ot_pull_publish(profile,1,&cancel));
    assert(!access(published,F_OK)); /* Invalid cached content is not ours to delete. */
    char private_blob[4096];assert(snprintf(private_blob,sizeof private_blob,"%s/blobs/sha256-%s",stage,hash)<(int)sizeof private_blob);
    assert(access(private_blob,F_OK));
    write_fixture(private_blob,data);
    atomic_store(&cancel,true);assert(!ot_pull_publish(profile,1,&cancel));atomic_store(&cancel,false);
    assert(ot_pull_publish(profile,1,&cancel));
    struct stat a,b;assert(!stat(private_blob,&a) && !stat(published,&b) && a.st_ino==b.st_ino && a.st_dev==b.st_dev);
    assert(ot_pull_discard(profile,1) && access(stage,F_OK) && !access(published,F_OK));
    assert(ot_pull_discard(profile,1));
    assert(ot_pull_prepare(profile,2,&cancel,stage));
    assert(snprintf(private_blob,sizeof private_blob,"%s/blobs/sha256-%s",stage,hash)<(int)sizeof private_blob);
    assert(!stat(private_blob,&a) && !stat(published,&b) && a.st_ino==b.st_ino);
    char partial[4096];assert(snprintf(partial,sizeof partial,"%s/blobs/new-partial",stage)<(int)sizeof partial);
    write_fixture(partial,"unfinished");assert(ot_pull_discard(profile,2) && !access(published,F_OK));
    assert(ot_pull_path(profile,3,stage) && !symlink(shared,stage));
    assert(!ot_pull_prepare(profile,3,&cancel,folder) && !ot_pull_discard(profile,3));assert(!unlink(stage));
    assert(!setenv("OLLAMA_MODELS",directory,1));
    assert(!removefile(profile,NULL,REMOVEFILE_RECURSIVE));
    puts("PASS: Ollama private stores, corrupt-cache repair, cancellation, atomic manifest publication, immutable cache reuse and safe discard");
}
static atomic_uint memory_probes;
static atomic_bool hold_memory_probe;
static bool memory_probe(const char *engine, const char *file, const ot_preferences *p, const ot_resources *r,
                         const atomic_bool *cancel, ot_memory_plan *result) {
    (void)engine;
    (void)file;
    (void)r;
    atomic_fetch_add(&memory_probes, 1);
    while (atomic_load(&hold_memory_probe) && !atomic_load(cancel)) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    *result = (ot_memory_plan){.weights = 4, .total = p->context, .engine_plan = true};
    return true; /* Deliberately finishes even after cancellation: stale results must be rejected. */
}
static ot_memory_view memory_settle(ot_memory *m) {
    ot_memory_view view = {0};
    for (unsigned i = 0; i < 3000; i++) {
        if (ot_memory_poll(m, &view) && !view.pending)
            return view;
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(0 && "memory worker timeout");
    return view;
}
static void model_memory(const char *file) {
    ot_runtime_buffers buffers = {0};
    ot_runtime_info allocation = {0};
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "load_tensors: CPU_Mapped model buffer size = 100.00 MiB"));
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "load_tensors: MTL0_Mapped model buffer size = 900.00 MiB"));
    assert(!ot_runtime_buffer_line(&buffers, &allocation,
                                   "llama_kv_cache_iswa: creating non-SWA KV cache, size = 8192 cells"));
    assert(ot_runtime_buffer_line(&buffers, &allocation, "llama_kv_cache: MTL0 KV buffer size = 192.00 MiB"));
    assert(!ot_runtime_buffer_line(&buffers, &allocation,
                                   "llama_kv_cache_iswa: creating SWA KV cache, size = 1280 cells"));
    assert(ot_runtime_buffer_line(&buffers, &allocation, "llama_kv_cache: MTL0 KV buffer size = 30.00 MiB"));
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "sched_reserve: MTL0 compute buffer size = 200.00 MiB"));
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "sched_reserve: MTL0 compute buffer size = 200.00 MiB"));
    assert(allocation.allocation == 1422ull * 1048576 && allocation.cache == 222ull * 1048576 &&
           allocation.linear_cache == 192ull * 1048576);
    assert(allocation.gpu_allocation == 1322ull * 1048576);
    ot_runtime_reconcile(&allocation, 950ull * 1048576);
    assert(allocation.allocation == 1372ull * 1048576 && allocation.weights == 950ull * 1048576);
    assert(
        !ot_runtime_buffer_line(&buffers, &allocation, "sched_reserve: MTL0 compute buffer size = nan MiB"));
    assert(
        !ot_runtime_buffer_line(&buffers, &allocation, "sched_reserve: MTL0 compute buffer size = -1 MiB"));
    ot_runtime_buffers large_buffers = {0};
    ot_runtime_info large_info = {0};
    assert(ot_runtime_buffer_line(&large_buffers, &large_info,
                                  "load_tensors: MTL0 model buffer size = 16000000000000 MiB"));
    ot_runtime_buffers preserved_buffers = large_buffers;
    uint64_t preserved_allocation = large_info.allocation;
    assert(!ot_runtime_buffer_line(&large_buffers, &large_info,
                                   "load_tensors: MTL0 model buffer size = 16000000000000 MiB"));
    assert(!memcmp(&large_buffers, &preserved_buffers, sizeof large_buffers) &&
           large_info.allocation == preserved_allocation);
    allocation.kind = OT_RUNTIME_GPU;
    allocation.context = 16384;
    assert(ot_runtime_buffer_line(&buffers, &allocation, "load_tensors: loading model tensors"));
    assert(!allocation.allocation && !buffers.count && allocation.kind == OT_RUNTIME_UNKNOWN &&
           !allocation.context);
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "0.00.354.182 I load_tensors: CPU_Mapped model buffer size = 100.00 MiB"));
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "0.00.354.183 I load_tensors: MTL0_Mapped model buffer size = 300.00 MiB"));
    assert(ot_runtime_buffer_line(&buffers, &allocation,
                                  "0.00.354.183 I load_tensors: MTL0_Mapped model buffer size = 500.00 MiB"));
    assert(allocation.weights == 900ull * 1048576 && allocation.gpu_allocation == 800ull * 1048576);
    assert(ot_runtime_buffer_line(&buffers, &allocation, "0.00.123 I load_tensors: loading model tensors"));
    assert(!allocation.allocation && !buffers.count);
    ot_runtime_info runtime = {0};
    assert(ot_runtime_line(&runtime, "load_tensors: offloaded 1/2 layers to GPU") &&
           runtime.kind == OT_RUNTIME_HYBRID && runtime.gpu_layers == 1);
    assert(ot_runtime_line(&runtime, "0.00.123 I llama_context: n_ctx                 = 8192") &&
           runtime.context == 8192);
    assert(!ot_runtime_line(&runtime, "llama_context: n_ctx_per_seq = 32") && runtime.context == 8192);
    assert(!ot_runtime_line(&runtime, "llama_context: n_ctx = -1") && runtime.context == 8192);
    assert(!ot_runtime_line(&runtime, "llama_context: n_ctx = 42949672960") && runtime.context == 8192);
    assert(!ot_runtime_line(&runtime, "load_tensors: offloaded 5/2 layers to GPU") &&
           runtime.kind == OT_RUNTIME_HYBRID);
    assert(ot_runtime_line(&runtime, "load_tensors: offloaded 2/2 layers to GPU") &&
           runtime.kind == OT_RUNTIME_GPU);
    assert(ot_runtime_line(&runtime, "load_tensors: offloaded 0/2 layers to GPU") &&
           runtime.kind == OT_RUNTIME_CPU);
    const char *ps = "{\"models\":[{\"name\":\"fixture\",\"digest\":\"sha256:fixture\",\"size\":1000,\"size_"
                     "vram\":200,\"context_length\":4096}]}";
    assert(ot_runtime_ollama(&runtime, ps, strlen(ps), "FIXTURE") && runtime.kind == OT_RUNTIME_GPU &&
           runtime.allocation == 1000 && runtime.gpu_allocation == 200 && runtime.context == 4096 &&
           !strcmp(runtime.model_digest, "sha256:fixture"));
    assert(ot_runtime_ollama(&runtime, ps, strlen(ps), "fixture:latest") && runtime.kind == OT_RUNTIME_GPU);
    const char *others = "{\"models\":[{\"name\":\"fixture:latest\",\"size\":1000,\"size_vram\":0},"
                         "{\"name\":\"other\",\"size\":2000,\"size_vram\":1000}]}";
    assert(ot_runtime_ollama(&runtime, others, strlen(others), "fixture") && runtime.kind == OT_RUNTIME_CPU &&
           runtime.allocation == 1000 && runtime.other_models == 1 && !*runtime.model_digest);
    assert(ot_runtime_ollama(&runtime, "{\"models\":[]}", 13, "fixture") &&
           runtime.kind == OT_RUNTIME_ASLEEP);
    const char *bad_ps = "{\"models\":[{\"name\":\"fixture\",\"size\":1000,\"size_vram\":2000}]}";
    assert(!ot_runtime_ollama(&runtime, bad_ps, strlen(bad_ps), "fixture") &&
           runtime.kind == OT_RUNTIME_ASLEEP);
    assert(!ot_runtime_ollama(&runtime, "{\"models\":null}", 15, "fixture"));
    const char *bad_rows[] = {"{\"models\":[null]}", "{\"models\":[{\"name\":\"fixture\\u0000other\"}]}",
                              "{\"models\":[{\"name\":17,\"model\":\"fixture\"}]}"};
    for (size_t i = 0; i < sizeof bad_rows / sizeof *bad_rows; i++)
        assert(!ot_runtime_ollama(&runtime, bad_rows[i], strlen(bad_rows[i]), "fixture") &&
               runtime.kind == OT_RUNTIME_ASLEEP);
    /* Order-independent identity, auxiliary geometry, a skipped array across read
     * windows and an invalid optional UTF-8 label must not corrupt the projection. */
    unsigned char *header = calloc(131072, 1);
    assert(header);
    memcpy(header, "GGUF", 4);
    header[4] = 3;
    header[8] = 1;
    header[16] = 7;
    const char *keys[] = {"qwen3.attention.key_length", "qwen3.attention.value_length",
                          "clip.attention.key_length", "qwen3.vision.embedding_length"};
    const unsigned values[] = {128, 64, 222, 255};
    size_t offset = 24;
    for (unsigned i = 0; i < 4; i++) {
        size_t length = strlen(keys[i]);
        header[offset] = (unsigned char)length;
        memcpy(header + offset + 8, keys[i], length);
        offset += length + 8;
        header[offset] = i == 1 ? 10 : 4;
        header[offset + 4] = (unsigned char)values[i];
        offset += i == 1 ? 12 : 8;
    }
    const char *strings[] = {"tokenizer.ggml.scores", "general.name", "general.architecture"};
    for (unsigned i = 0; i < 3; i++) {
        size_t length = strlen(strings[i]);
        header[offset] = (unsigned char)length;
        memcpy(header + offset + 8, strings[i], length);
        offset += length + 8;
        header[offset] = i ? 8 : 9;
        offset += 4;
        if (!i) {
            header[offset] = 6; /* float32 array, 20000 elements */
            header[offset + 4] = 0x20;
            header[offset + 5] = 0x4e;
            offset += 12 + 80000;
        } else {
            header[offset] = i == 1 ? 1 : 5;
            memcpy(header + offset + 8, i == 1 ? "\xff" : "qwen3", i == 1 ? 1u : 5u);
            offset += 8 + (i == 1 ? 1 : 5);
        }
    }
    header[offset] = 1;
    header[offset + 8] = 'x';
    header[offset + 9] = 1;
    header[offset + 13] = 1;
    size_t bytes = (offset + 33 + 31) / 32 * 32 + 4;
    char metadata_file[512];
    snprintf(metadata_file, sizeof metadata_file, "%s.metadata", file);
    FILE *fixture = fopen(metadata_file, "wb");
    assert(fixture && fwrite(header, 1, bytes, fixture) == bytes && !fclose(fixture));
    free(header);
    ot_model_info parsed;
    assert(ot_gguf(metadata_file, &parsed) && parsed.key_length == 128 && parsed.value_length == 64 &&
           !parsed.embedding && !*parsed.name && parsed.tensor_bytes == 4);
    ot_model_info unchanged = parsed;
    assert(!truncate(metadata_file, (off_t)bytes - 1));
    assert(!ot_gguf(metadata_file, &parsed) && !memcmp(&parsed, &unchanged, sizeof parsed));
    assert(!unlink(metadata_file));
    ot_model_info info = {.bytes = 600000000,
                          .tensor_bytes = 516000000,
                          .embedding = 1024,
                          .layers = 28,
                          .heads = 16,
                          .kv_heads = 8,
                          .key_length = 128,
                          .value_length = 128};
    ot_memory_plan plan = ot_memory_approximate(&info, 8192);
    assert(plan.known_cache && plan.cache == 896ull * 1048576 && plan.weights == 516000000);
    info.kv_heads = 2;
    assert(ot_memory_approximate(&info, 8192).cache == plan.cache / 4); /* GQA. */
    info.value_length = 64;
    assert(ot_memory_approximate(&info, 8192).cache == plan.cache / 4 * 3 / 4);
    info.experts = 32;
    info.experts_used = 4;
    assert(ot_memory_approximate(&info, 8192).weights == info.tensor_bytes);
    info.complex_cache = true;
    assert(!ot_memory_approximate(&info, 8192).known_cache);
    uint64_t projected = 0;
    ot_runtime_info observed = {.context = 8192,
                                .batch = 2048,
                                .micro_batch = 512,
                                .total_layers = 33,
                                .allocation = 8ull << 30,
                                .weights = 6ull << 30,
                                .cache = 1ull << 30,
                                .linear_cache = 1ull << 30};
    assert(ot_memory_project(16384, &observed, NULL, &projected) && projected == (9ull << 30));
    observed.compute = 1ull << 30;
    ot_runtime_info previous = observed;
    previous.context = 4096;
    previous.cache /= 2;
    previous.linear_cache /= 2;
    previous.compute /= 2;
    assert(ot_memory_project(16384, &observed, &previous, &projected) && projected == (10ull << 30));
    previous.batch++;
    assert(!ot_memory_project(16384, &observed, &previous, &projected));
    observed.linear_cache /= 2; /* Full and fixed sliding/recurrent banks. */
    observed.window_cells = 1280;
    assert(ot_memory_project(16384, &observed, NULL, &projected) && projected == (17ull << 29));
    assert(!ot_memory_project(1024, &observed, NULL, &projected));
    observed.allocation = UINT64_MAX;
    assert(!ot_memory_project(16384, &observed, NULL, &projected));
    observed = (ot_runtime_info){.context = 8192,
                                 .allocation = 8ull << 30,
                                 .weights = 6ull << 30,
                                 .cache = 1ull << 30,
                                 .linear_cache = 1ull << 30,
                                 .compute = 1ull << 30,
                                 .batch = 2048,
                                 .micro_batch = 512,
                                 .total_layers = 33};
    char *sample = ot_memory_observe(NULL, "model/engine/hardware/settings", &observed);
    assert(sample && ot_memory_recall(sample, "model/engine/hardware/settings", 8192, &plan));
    assert(plan.observed && plan.total == observed.allocation && plan.weights == observed.weights);
    assert(!ot_memory_observe(sample, "model/engine/hardware/settings", &observed));
    char escaped_key[1025];
    memset(escaped_key, 1, sizeof escaped_key - 1);
    escaped_key[sizeof escaped_key - 1] = 0;
    assert(!ot_memory_observe(NULL, escaped_key, &observed)); /* Encoded record stays bounded too. */
    assert(!ot_memory_recall(sample, "changed-identity", 8192, &plan));
    assert(!ot_memory_recall(sample, "model/engine/hardware/settings", 12288, &plan));
    observed.context = 16384;
    observed.cache *= 2;
    observed.linear_cache *= 2;
    observed.compute += 1ull << 29;
    observed.allocation = observed.weights + observed.cache + observed.compute;
    char *pair = ot_memory_observe(sample, "model/engine/hardware/settings", &observed);
    assert(pair && ot_memory_recall(pair, "model/engine/hardware/settings", 8192, &plan) &&
           plan.total == (8ull << 30));
    assert(ot_memory_recall(pair, "model/engine/hardware/settings", 12288, &plan) &&
           plan.total == (35ull << 28));
    assert(!ot_memory_recall(pair, "model/engine/hardware/settings", 32768,
                             &plan)); /* No blind extrapolation. */
    observed.micro_batch = 256;
    char *changed_shape = ot_memory_observe(sample, "model/engine/hardware/settings", &observed);
    assert(changed_shape && !ot_memory_recall(changed_shape, "model/engine/hardware/settings", 12288, &plan));
    observed.allocation = UINT64_MAX;
    assert(!ot_memory_observe(sample, "model/engine/hardware/settings", &observed));
    assert(!ot_memory_recall("{\"version\":1,\"key\":\"k\",\"samples\":[{\"context\":8192}]}", "k", 8192,
                             &plan));
    free(changed_shape);
    free(pair);
    free(sample);
    info = (ot_model_info){.bytes = UINT64_MAX,
                           .layers = UINT32_MAX,
                           .heads = UINT32_MAX,
                           .kv_heads = UINT32_MAX,
                           .key_length = UINT32_MAX,
                           .value_length = UINT32_MAX};
    plan = ot_memory_approximate(&info, UINT32_MAX);
    assert(plan.cache == UINT64_MAX && plan.total == UINT64_MAX);
    info = (ot_model_info){.bytes = 68};
    assert(!ot_memory_approximate(&info, 1024).known_cache);
    const char *json =
        "{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":["
        "{\"kind\":\"device\",\"name\":\"MTL0\",\"model\":409,\"context\":896,\"compute\":312,"
        "\"self\":9999,\"total\":131072,\"free\":100000},"
        "{\"kind\":\"host\",\"name\":\"Host\",\"model\":83,\"context\":0,\"compute\":32},"
        "{\"kind\":\"buffer_type\",\"name\":\"CPU_REPACK\",\"model\":353,\"context\":0,\"compute\":0}]}}";
    assert(ot_memory_parse(json, strlen(json), &plan));
    assert(plan.engine_plan && plan.weights == 845ull * 1048576 && plan.cache == 896ull * 1048576 &&
           plan.compute == 344ull * 1048576 && plan.total == 2085ull * 1048576);
    const char *invalid[] = {
        "{}",
        "null",
        "{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":[]}}",
        "{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":["
        "{\"kind\":\"host\",\"name\":\"Host\",\"model\":1,\"context\":0,\"compute\":0},"
        "{\"kind\":\"host\",\"name\":\"Host\",\"model\":1,\"context\":0,\"compute\":0}]}}",
        "{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":["
        "{\"kind\":\"host\",\"name\":\"Host\",\"model\":18446744073709551615,\"context\":0,\"compute\":0}]}}",
        "{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":["
        "{\"kind\":\"host\",\"name\":\"Host\",\"model\":1.5,\"context\":0,\"compute\":0}]}}"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        uint64_t preserved = plan.total;
        assert(!ot_memory_parse(invalid[i], strlen(invalid[i]), &plan) && plan.total == preserved);
    }
    assert(!ot_memory_parse(NULL, 0, &plan) && !ot_memory_parse(json, strlen(json), NULL));
    ot_memory *m = ot_memory_open(memory_probe);
    assert(m);
    ot_preferences p = ot_preferences_default();
    ot_resources resources = {.cores = 2};
    ot_resources enormous = {.memory_total = UINT64_MAX, .memory_available = UINT64_MAX};
    assert(ot_memory_budget(&enormous, &p) == UINT64_MAX / 100 * p.memory_percent);
    for (unsigned i = 0; i < 20; i++) {
        p.context = 1024 + i * 512;
        ot_memory_request(m, i + 1, file, file, &info, &p, &resources, NULL);
    }
    ot_memory_view view = memory_settle(m);
    assert(view.model == 20 && view.plan.total == p.context && atomic_load(&memory_probes) == 1);
    ot_memory_request(m, 21, file, file, &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 21 && atomic_load(&memory_probes) == 1); /* Identity/parameter cache hit. */
    /* Replacing a file at the same path must invalidate its cached plan. */
    unsigned char contents[68];
    FILE *input = fopen(file, "rb");
    assert(input && fread(contents, 1, sizeof contents, input) == sizeof contents && !fclose(input));
    char moved[512];
    snprintf(moved, sizeof moved, "%s.identity", file);
    assert(!rename(file, moved));
    FILE *replacement = fopen(file, "wb");
    assert(replacement && fwrite(contents, 1, sizeof contents, replacement) == sizeof contents &&
           !fclose(replacement));
    ot_memory_request(m, 21, file, file, &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 21 && atomic_load(&memory_probes) == 2);
    assert(!unlink(file) && !rename(moved, file));
    atomic_store(&hold_memory_probe, true);
    p.context = 16384;
    ot_memory_request(m, 22, file, file, &info, &p, &resources, NULL);
    for (unsigned i = 0; i < 3000 && atomic_load(&memory_probes) < 3; i++) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(atomic_load(&memory_probes) == 3);
    p.context = 32768;
    ot_memory_request(m, 23, file, file, &info, &p, &resources, NULL);
    atomic_store(&hold_memory_probe, false);
    view = memory_settle(m);
    assert(view.model == 23 && view.plan.total == 32768 && atomic_load(&memory_probes) == 4);
    ot_memory_request(m, 24, file, file, &info, &p, &resources, NULL);
    ot_memory_cancel(m);
    view = memory_settle(m);
    assert(view.model == 24 && !view.pending && atomic_load(&memory_probes) == 4);
    /* Placement depends on the budget and hardware as well as the file and context. */
    resources.memory_total = 16ull << 30;
    resources.gpu_recommended = 12ull << 30;
    resources.gpu = true;
    ot_memory_request(m, 24, file, file, &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 24 && atomic_load(&memory_probes) == 5);
    p.memory_percent = p.memory_percent == 50 ? 75 : 50;
    ot_memory_request(m, 24, file, file, &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 24 && atomic_load(&memory_probes) == 6);
    ot_memory_plan measured = {.total = 1234567, .observed = true};
    ot_memory_request(m, 24, file, file, &info, &p, &resources, &measured);
    view = memory_settle(m);
    assert(view.model == 24 && !view.pending && view.plan.observed && view.plan.total == measured.total &&
           atomic_load(&memory_probes) == 6);
    ot_memory_request(m, 24, file, file, &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 24 && atomic_load(&memory_probes) == 6);
    ot_memory_close(m);
    m = ot_memory_open(NULL);
    assert(m);
    ot_memory_request(m, 25, "/missing/overtone-model", "", &info, &p, &resources, NULL);
    view = memory_settle(m);
    assert(view.model == 25 && !view.plan.total);
    ot_memory_close(m);
}
static void models(const char *path, const char *directory) {
    /* Minimal real GGUF: one f32 tensor, no metadata, aligned data offset 64. */
    unsigned char file[68] = {0x47, 0x47, 0x55, 0x46, 3};
    file[8] = 1;
    file[24] = 1;
    file[32] = 'x';
    file[33] = 1;
    file[37] = 1;
    char model[300];
    snprintf(model, sizeof model, "%s/import.gguf", directory);
    FILE *out = fopen(model, "wb");
    assert(out && fwrite(file, 1, sizeof file, out) == sizeof file && !fclose(out));
    ot_model_info info;
    assert(ot_gguf(model, &info) && info.bytes == sizeof file);
    file[37] = 0;
    out = fopen(model, "wb");
    assert(out);
    assert(fwrite(file, 1, sizeof file, out) == sizeof file && !fclose(out));
    assert(!ot_gguf(model, &info));
    file[37] = 1;
    out = fopen(model, "wb");
    assert(out && fwrite(file, 1, sizeof file, out) == sizeof file && !fclose(out));
    model_memory(model);
    transport chat = {0};
    ot_app *app = open_app(path, &chat);
    acquisition t = {0};
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_library_command(l, "{\"action\":\"ollamaSearch\",\"query\":\"qwen\"}");
    library_has(l, "internet_disabled");
    assert(!t.id);
    assert(ot_set_preferences(app, "{\"internet\":true}"));
    ot_library_command(l, "{\"action\":\"ollamaSearch\",\"query\":\"qwen\"}");
    library_has(l, "\"searching\":true");
    const char *catalog = "<a href='/library/qwen3'>Qwen</a><a href=\"/library/qwen3\">duplicate</a>"
                          "<a href='/library/../bad'>invalid</a><a href='/library/qwen3/tags'>tags</a>href=";
    assert(ot_library_headers(l, t.id, 200, NULL, NULL, (int64_t)strlen(catalog)));
    ot_library_bytes(l, t.id, catalog, strlen(catalog));
    ot_library_end(l, t.id, NULL);
    char *catalog_view = ot_library_view(l);
    assert(catalog_view);
    yyjson_doc *catalog_doc = yyjson_read(catalog_view, strlen(catalog_view), 0);
    assert(catalog_doc);
    yyjson_val *catalog_items = yyjson_obj_get(yyjson_doc_get_root(catalog_doc), "catalog");
    assert(yyjson_arr_size(catalog_items) == 1);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(catalog_items, 0), "name")), "qwen3"));
    yyjson_doc_free(catalog_doc);
    free(catalog_view);
    ot_library_command(l, "{\"action\":\"ollamaVariants\",\"repo\":\"qwen3\"}");
    catalog = "<a href='/library/qwen3:0.6b'><span>523MB</span> • 40K context window</a>"
              "<a href='/library/qwen3:0.6b'>duplicate</a><a href='/library/other:1b'>other</a>"
              "<a href='/library/qwen3:cloud-cloud'>Cloud</a>";
    ot_library_bytes(l, t.id, catalog, strlen(catalog));
    ot_library_end(l, t.id, NULL);
    library_has(l, "\"name\":\"qwen3:0.6b\"");
    library_has(l, "\"bytes\":523000000");
    library_has(l, "\"context\":40960");
    library_has(l, "\"cloud\":true");
    char command[1000];
    snprintf(command, sizeof command, "{\"action\":\"previewImport\",\"path\":\"%s\"}", model);
    ot_library_command(l, command);
    settle_library(l);
    uint64_t token = candidate_token(l, "importCandidate");
    assert(token);
    library_has(l, "\"origin\":\"cache\""); /* Preview must not convert the auto-detected reference. */
    snprintf(command, sizeof command, "{\"action\":\"confirmImport\",\"token\":%llu}",
             (unsigned long long)(token + 1));
    ot_library_command(l, command);
    library_has(l, "import_changed");
    snprintf(command, sizeof command, "{\"action\":\"confirmImport\",\"token\":%llu}",
             (unsigned long long)token);
    ot_library_command(l, command);
    library_has(l, "import.gguf");
    library_has(l, "\"origin\":\"manual\"");
    ot_library_command(l, "{\"action\":\"remove\",\"id\":1}");
    library_has(l, "\"hidden\":true");
    assert(!access(model, F_OK));
    ot_library_close(l);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    library_has(l, "\"hidden\":true");
    ot_library_command(l, "{\"action\":\"restoreModel\",\"id\":1}");
    library_has(l, "\"hidden\":false");
    char withheld[300];
    snprintf(withheld, sizeof withheld, "%s/.held-model", directory);
    assert(!rename(model, withheld));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"available\":false");
    assert(!rename(withheld, model));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"available\":true");
    char replaced[300];
    snprintf(replaced, sizeof replaced, "%s/replacement.gguf", directory);
    int replacement = open(replaced, O_WRONLY | O_CREAT | O_EXCL, 0600);
    unsigned char changed_file[100] = {0};
    memcpy(changed_file, file, 24);
    changed_file[16] = 1;
    changed_file[24] = 17;
    memcpy(changed_file + 32, "general.file_type", 17);
    changed_file[49] = 4; changed_file[53] = 2; /* Fresh Q4_0 metadata, no context geometry. */
    memcpy(changed_file + 57, file + 24, 33);
    assert(replacement >= 0 && write(replacement, changed_file, sizeof changed_file) == sizeof changed_file && !close(replacement));
    statement(path, "UPDATE models SET context=16384,embedding=4096,layers=32,heads=32,kv_heads=8,"
                    "metadata=json_set(metadata,'$.quantization','Q8_0','$.checksum','old',"
                    "'$.chatSupported',json('false'),'$.thinking',json('{\"supported\":true}')) WHERE id=1");
    assert(!rename(replaced, model));
    ot_library_command(l, "{\"action\":\"remove\",\"id\":1,\"uninstall\":true}");
    assert(!access(model, F_OK));
    library_has(l, "file_not_owned");
    snprintf(command, sizeof command, "{\"action\":\"import\",\"path\":\"%s\"}", model);
    ot_library_command(l, command); /* Explicit reimport accepts the new identity, scanning does not. */
    assert(db_integer(app, "SELECT context=0 AND embedding=0 AND layers=0 AND heads=0 AND kv_heads=0 "
                          "AND json_extract(metadata,'$.quantization')='Q4_0' "
                          "AND json_type(metadata,'$.checksum') IS NULL "
                          "AND json_type(metadata,'$.thinking') IS NULL "
                          "AND json_type(metadata,'$.chatSupported') IS NULL FROM models WHERE id=1"));
    {
        sqlite3 *cache_db = ot_store_connection(ot_app_store(app));
        assert(sqlite3_exec(cache_db,
                            "UPDATE models SET metadata=json_set(metadata,"
                            "'$.memoryObservation',json('{\"key\":\"private-measurement\"}'),"
                            "'$.cpuQualification',json('{\"identity\":\"private-qualification\"}'),"
                            "'$.testVisible','public-marker') WHERE id=1",
                            NULL, NULL, NULL) == SQLITE_OK);
        char *visible = ot_library_view(l);
        assert(visible && strstr(visible, "public-marker") && !strstr(visible, "private-measurement") &&
               !strstr(visible, "private-qualification"));
        free(visible);
        assert(sqlite3_exec(cache_db,
                            "UPDATE models SET metadata=json_remove(metadata,"
                            "'$.memoryObservation','$.cpuQualification','$.testVisible') WHERE id=1",
                            NULL, NULL, NULL) == SQLITE_OK);
    }
    ot_preferences saved_parameters = *ot_get_preferences(app);
    ot_library_command(l, "{\"action\":\"estimate\",\"id\":1,\"context\":4096,\"gpuLayers\":-1,"
                          "\"memoryPercent\":50,\"threads\":3}");
    ot_library_tick(l);
    char *memory_view = ot_library_memory(l);
    yyjson_doc *memory_doc = yyjson_read(memory_view, strlen(memory_view), 0);
    yyjson_val *estimate = yyjson_obj_get(yyjson_doc_get_root(memory_doc), "estimate");
    assert(yyjson_get_uint(yyjson_obj_get(estimate, "memoryPercent")) == 50 &&
           yyjson_get_uint(yyjson_obj_get(estimate, "threads")) == 3 &&
           yyjson_get_uint(yyjson_obj_get(estimate, "context")) == 4096);
    yyjson_doc_free(memory_doc);
    free(memory_view);
    ot_library_command(l, "{\"action\":\"cancelEstimate\"}");
    assert(ot_get_preferences(app)->memory_percent == saved_parameters.memory_percent &&
           ot_get_preferences(app)->threads == saved_parameters.threads);
    ot_library_command(
        l, "{\"action\":\"estimate\",\"id\":1,\"context\":4096,\"gpuLayers\":-1,\"memoryPercent\":0}");
    library_has(l, "invalid_input");
    unsigned char digest[32];
    char hash[65];
    CC_SHA256(file, sizeof file, digest);
    for (unsigned i = 0; i < 32; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    snprintf(command, sizeof command,
             "{\"action\":\"previewDownload\",\"url\":\"https://example.invalid/file.gguf\","
             "\"name\":\"Downloaded\",\"checksum\":\"%s\"}",
             hash);
    ot_library_command(l, command);
    assert(!strcmp(t.method, "HEAD"));
    assert(ot_library_headers(l, t.id, 200, NULL, NULL, 68));
    ot_library_end(l, t.id, NULL);
    token = candidate_token(l, "downloadCandidate");
    assert(token);
    library_has(l, "\"jobs\":[]");
    ot_library_command(l, "{\"action\":\"cancelPreview\"}");
    snprintf(command, sizeof command, "{\"action\":\"confirmDownload\",\"token\":%llu}",
             (unsigned long long)token);
    ot_library_command(l, command);
    library_has(l, "invalid_input");
    library_has(l, "\"jobs\":[]");
    uint64_t sent = t.id;
    ot_library_command(l, "{\"action\":\"select\",\"id\":18446744073709551615}");
    library_has(l, "invalid_input");
    ot_library_command(l, "{\"action\":\"engine\",\"kind\":4294967297}");
    library_has(l, "invalid_input");
    ot_library_end(l, 0, NULL);
    assert(!ot_library_headers(l, 0, 200, NULL, NULL, 0));
    assert(t.id == sent);
    snprintf(command, sizeof command,
             "{\"action\":\"download\",\"url\":\"https://example.invalid/"
             "file.gguf\",\"name\":\"Downloaded\",\"checksum\":\"%s\",\"bytes\":68,\"license\":\"MIT\"}",
             hash);
    ot_library_command(l, command);
    assert(t.id && t.offset == 0);
    assert(ot_library_headers(l, t.id, 200, "\"version1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, 40);
    ot_library_command(l, "{\"action\":\"pause\"}");
    uint64_t old = t.id;
    ot_library_end(l, old, "cancelled");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":1}");
    library_has(l, "\"state\":\"cancelled\"");
    library_has(l, "\"received\":0");
    ot_library_command(l, command);
    assert(t.id != old && t.offset == 0); /* Cancel discards; pause below still resumes. */
    assert(ot_library_headers(l, t.id, 200, "\"version1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, 40);
    ot_library_command(l, "{\"action\":\"pause\"}");
    old = t.id;
    ot_library_end(l, old, "cancelled");
    ot_library_close(l);
    ot_close(app);
    app = open_app(path, &chat);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_library_command(l, "{\"action\":\"resume\",\"id\":1}");
    library_has(l, "\"received\":40");
    library_has(l, "\"phase\":\"preparing\"");
    for (unsigned i = 0; i < 200 && t.offset != 40; i++) {
        ot_library_tick(l);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(t.offset == 40);
    assert(ot_library_headers(l, t.id, 206, "\"version1\"", "bytes 40-67/68", 28));
    ot_library_bytes(l, t.id, file + 40, 28);
    statement(
        path,
        "CREATE TRIGGER fail_install BEFORE INSERT ON models BEGIN SELECT RAISE(ABORT,'disk fault'); END");
    ot_library_end(l, t.id, NULL);
    library_has(l, "storage_write");
    library_has(l, "\"phase\":\"registration_failed\"");
    library_has(l, "\"retryRegistration\":true");
    library_has(l, "\"destination\":\"overtone\"");
    char failed_part[512], failed_final[512];
    snprintf(failed_part, sizeof failed_part, "%s/models/1.part", directory);
    snprintf(failed_final, sizeof failed_final, "%s/models/1.gguf", directory);
    assert(!access(failed_part, F_OK) && !access(failed_final, F_OK));
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":1}");
    assert(access(failed_part, F_OK) && access(failed_final, F_OK));
    ot_library_command(l, command);
    assert(t.offset == 0 && ot_library_headers(l, t.id, 200, "\"version1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, sizeof file);
    ot_library_end(l, t.id, NULL);
    library_has(l, "\"phase\":\"registration_failed\"");
    ot_library_close(l);
    ot_close(app);
    statement(path, "DROP TRIGGER fail_install");
    app = open_app(path, &chat);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    uint64_t requests = t.id;
    assert(ot_set_preferences(app, "{\"internet\":false}"));
    ot_library_command(l, "{\"action\":\"resume\",\"id\":1}");
    settle_library(l);
    assert(t.id == requests); /* A verified complete partial requires no new network transfer. */
    library_has(l, "Downloaded");
    library_has(l, "complete");
    library_has(l, "\"phase\":\"complete\"");
    library_has(l, "\"retryRegistration\":false");
    library_has(l, "\"license\":\"MIT\"");
    library_has(l, "\"source\":\"gguf\"");
    ot_library_command(l, "{\"action\":\"discard\",\"id\":1}");
    library_has(l, "\"jobs\":[]");
    char downloaded[300];
    snprintf(downloaded, sizeof downloaded, "%s/models/1.gguf", directory);
    assert(!access(downloaded, F_OK)); /* Clearing activity never uninstalls its model. */
    assert(ot_set_preferences(app, "{\"internet\":true}"));
    ot_library_command(l, command); /* Activity removal must not cause a duplicate download. */
    library_has(l, "already_installed");
    library_has(l, "\"jobs\":[]");
    assert(t.id == requests);
    ot_library_command(l, "{\"action\":\"remove\",\"id\":2,\"uninstall\":true}");
    assert(access(downloaded, F_OK) && errno == ENOENT);
    ot_library_command(l, "{\"action\":\"remove\",\"id\":1,\"uninstall\":true}");
    assert(access(model, F_OK) && errno == ENOENT);
    ot_connection connection = *ot_configuration(app);
    assert(ot_set_preferences(app, "{\"internet\":true}"));
    strcpy(connection.endpoint, "http://127.0.0.1:23456");
    strcpy(connection.model, "fixture");
    assert(ot_configure(app, &connection, 0));
    ot_library_command(l, "{\"action\":\"pull\",\"name\":\"tiny:fixture\",\"bytes\":20}");
    assert(strstr(t.url, "11434/api/tags"));
    uint64_t preparation = t.id;
    ot_library_command(l, "{\"action\":\"pull\",\"name\":\"other:fixture\"}");
    assert(t.id == preparation);
    library_has(l, "\"state\":\"queued\"");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":3}");
    ot_library_command(l, "{\"action\":\"discard\",\"id\":3}");
    assert(t.id == preparation);
    const char *empty_tags = "{\"models\":[]}";
    ot_library_bytes(l, preparation, empty_tags, strlen(empty_tags));
    ot_library_end(l, preparation, NULL);
    assert(strstr(t.url, "11434/api/pull") && t.id != preparation);
    assert(!strcmp(ot_configuration(app)->endpoint, "http://127.0.0.1:23456"));
    const char *pull = "{\"digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                       "\"total\":12,\"completed\":10}\n"
                       "{\"digest\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
                       "\"total\":5,\"completed\":4}\n";
    ot_library_bytes(l, t.id, pull, strlen(pull));
    library_has(l, "\"received\":14");
    library_has(l, "\"total\":20");
    const char *first_complete="{\"digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"total\":12,\"completed\":12}\n";
    ot_library_bytes(l,t.id,first_complete,strlen(first_complete));
    library_has(l,"\"received\":16");library_has(l,"\"total\":20");
    char *resume_view = ot_library_view(l);
    yyjson_doc *resume_doc = yyjson_read(resume_view, strlen(resume_view), 0);
    uint64_t resumable = 0;
    yyjson_val *resume_jobs = yyjson_obj_get(yyjson_doc_get_root(resume_doc), "jobs");
    for (size_t i = 0; i < yyjson_arr_size(resume_jobs); i++) {
        yyjson_val *row = yyjson_arr_get(resume_jobs, i);
        const char *name = yyjson_get_str(yyjson_obj_get(row, "name"));
        if (name && !strcmp(name, "tiny:fixture"))
            resumable = yyjson_get_uint(yyjson_obj_get(row, "id"));
    }
    yyjson_doc_free(resume_doc);
    free(resume_view);
    assert(resumable);
    snprintf(command, sizeof command, "{\"action\":\"pause\",\"id\":%llu}", (unsigned long long)resumable);
    ot_library_command(l, command);
    library_has(l, "\"received\":16");
    /* Reopening the owner must retain content-qualified progress, not just an
     * optimistic visual percentage in the old WebView. */
    ot_library_close(l);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    snprintf(command, sizeof command, "{\"action\":\"resume\",\"id\":%llu}", (unsigned long long)resumable);
    ot_library_command(l, command);
    library_has(l, "\"received\":16");
    if (strstr(t.url, "/api/tags")) {
        ot_library_bytes(l, t.id, empty_tags, strlen(empty_tags));
        ot_library_end(l, t.id, NULL);
    }
    assert(strstr(t.url, "/api/pull"));
    const char *zero = "{\"digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                       "\"total\":12,\"completed\":0}\n";
    ot_library_bytes(l, t.id, zero, strlen(zero));
    library_has(l, "\"received\":16");
    library_has(l, "\"total\":20");
    const char *rolled_back =
        "{\"digest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"total\":12,"
        "\"completed\":9}\n";
    ot_library_bytes(l, t.id, rolled_back, strlen(rolled_back));
    library_has(l, "\"received\":13");
    ot_library_bytes(l, t.id, pull, strlen(pull));
    library_has(l, "\"received\":14");
    const char *aggregate =
        "{\"status\":\"pulling model\",\"digest\":\"sha256:model\",\"total\":17,\"completed\":16}\n";
    ot_library_bytes(l, t.id, aggregate, strlen(aggregate));
    ot_library_bytes(l, t.id, pull, strlen(pull)); /* Model-wide progress supersedes layer counters. */
    library_has(l, "\"received\":16");
    library_has(l, "\"total\":17");
    const char *success = "{\"status\":\"verifying sha256 digest\"}\n{\"status\":\"success\"}\n";
    ot_library_bytes(l, t.id, success, strlen(success));
    ot_library_bytes(l, t.id, pull, strlen(pull)); /* Late progress cannot undo verified completion. */
    statement(path, "CREATE TRIGGER fail_pull_install BEFORE INSERT ON models "
                    "WHEN NEW.name='tiny:fixture' BEGIN SELECT RAISE(ABORT,'disk fault'); END");
    ot_library_end(l, t.id, NULL);
    library_has(l, "storage_write");
    library_has(l, "\"retryRegistration\":true");
    library_has(l, "\"activityError\":true");
    library_has(l, "\"received\":17");
    statement(path, "DROP TRIGGER fail_pull_install");
    requests = t.id;
    ot_preferences previous_preferences = *ot_get_preferences(app);
    assert(ot_set_preferences(app, "{\"internet\":false}"));
    ot_library_preferences_changed(l, &previous_preferences);
    library_has(l, "\"retryRegistration\":true");
    char *view = ot_library_view(l);
    yyjson_doc *snapshot = yyjson_read(view, strlen(view), 0);
    uint64_t pull_id = yyjson_get_uint(
        yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(snapshot), "jobs")), "id"));
    assert(pull_id);
    yyjson_doc_free(snapshot);
    free(view);
    snprintf(command, sizeof command, "{\"action\":\"resume\",\"id\":%llu}", (unsigned long long)pull_id);
    ot_library_command(l, command);
    assert(t.id == requests);
    library_has(l, "tiny:fixture");
    library_has(l, "\"received\":17");
    library_has(l, "\"total\":17");
    library_has(l, "\"state\":\"complete\"");
    view = ot_library_view(l);
    snapshot = yyjson_read(view, strlen(view), 0);
    yyjson_val *installed = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(snapshot), "models"));
    int64_t service_model = yyjson_get_sint(yyjson_obj_get(installed, "id"));
    assert(service_model && !strcmp(yyjson_get_str(yyjson_obj_get(installed, "name")), "tiny:fixture"));
    yyjson_doc_free(snapshot);
    free(view);
    snprintf(command, sizeof command, "{\"action\":\"remove\",\"id\":%lld,\"uninstall\":true}",
             (long long)service_model);
    ot_library_command(l, command); /* Local uninstall also works with internet access disabled. */
    assert(!strcmp(t.method, "DELETE") && strstr(t.url, "/api/delete"));
    assert(!ot_library_headers(l, t.id, 500, NULL, NULL, 0));
    library_has(l, "tiny:fixture"); /* A service failure must retain the reference for retry. */
    ot_library_command(l, command);
    assert(strstr(t.url, "/api/tags"));
    ot_library_bytes(l, t.id, empty_tags, strlen(empty_tags));
    ot_library_end(l, t.id, NULL);
    assert(!strcmp(t.method, "DELETE"));
    assert(ot_library_headers(l, t.id, 200, NULL, NULL, 2));
    ot_library_bytes(l, t.id, "{}", 2);
    ot_library_end(l, t.id, NULL);
    library_has(l, "\"models\":[]");
    statement(path, "INSERT INTO models(id,name,kind,reference,origin) "
                    "VALUES(999,'Missing fixture',2,'missing:fixture','ollama')");
    ot_library_command(l, "{\"action\":\"select\",\"id\":999}");
    assert(strstr(t.url, "/api/tags"));
    ot_library_bytes(l, t.id, empty_tags, strlen(empty_tags));
    ot_library_end(l, t.id, NULL);
    assert(strstr(t.url, "/api/show"));
    assert(!ot_library_headers(l, t.id, 404, NULL, NULL, 0));
    library_has(l, "model_unavailable");
    library_has(l, "\"available\":false");
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"engineError\":\"model_unavailable\"");
    /* A later installation restores availability from the actual service response. */
    assert(ot_set_preferences(app, "{\"keepAlive\":\"0\"}"));
    ot_library_command(l, "{\"action\":\"select\",\"id\":999}");
    const char *found = "{\"models\":[{\"name\":\"missing:fixture\",\"size\":68}]}";
    ot_library_bytes(l, t.id, found, strlen(found));
    ot_library_end(l, t.id, NULL);
    ot_library_bytes(l, t.id, "{}", 2);
    ot_library_end(l, t.id, NULL);
    assert(strstr(t.url, "/api/generate"));
    yyjson_doc *warm = yyjson_read(t.body, strlen(t.body), 0);
    const char *warm_keep = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(warm), "keep_alive"));
    assert(warm_keep && !strcmp(warm_keep, "30s"));
    assert(!strcmp(ot_get_preferences(app)->keep_alive, "0"));
    yyjson_doc_free(warm);
    const char *warmed = "{\"done\":true}";
    ot_library_bytes(l, t.id, warmed, strlen(warmed));
    ot_library_end(l, t.id, NULL);
    library_has(l, "\"engine\":\"ready\"");
    char *recovered = ot_library_view(l);
    assert(recovered && !strstr(recovered, "\"available\":false"));
    free(recovered);
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    unlink(model);
    snprintf(downloaded, sizeof downloaded, "%s/models", directory);
    assert(!rmdir(downloaded));
}

extern char **environ;
static void replacement_downloads(const char *path, const char *directory) {
    unsigned char file[68] = {0x47, 0x47, 0x55, 0x46, 3};
    file[8] = 1;
    file[24] = 1;
    file[32] = 'x';
    file[33] = 1;
    file[37] = 1;
    unsigned char digest[32];
    char hash[65], command[1024], old_path[512], new_path[512];
    CC_SHA256(file, sizeof file, digest);
    for (unsigned i = 0; i < 32; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    transport chat = {0};
    acquisition t = {0};
    ot_app *app = open_app(path, &chat);
    assert(ot_set_preferences(app, "{\"internet\":true,\"startupModel\":-1}"));
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    snprintf(command, sizeof command,
             "{\"action\":\"download\",\"url\":\"https://example.invalid/replacement.gguf\","
             "\"name\":\"Replacement\",\"checksum\":\"%s\",\"bytes\":68}",
             hash);
    ot_library_command(l, command);
    assert(ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, sizeof file);
    ot_library_end(l, t.id, NULL);
    snprintf(old_path, sizeof old_path, "%s/models/1.gguf", directory);
    snprintf(new_path, sizeof new_path, "%s/models/2.gguf", directory);
    assert(!access(old_path, F_OK));
    statement(path, "UPDATE models SET device=0,metadata=json_set(metadata,'$.fileChanged',json('true'));");
    assert(ot_set_preferences(app, "{\"startupModel\":1,\"internet\":true}"));
    ot_library_close(l);
    ot_close(app);
    app = open_app(path, &chat);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    library_has(l, "\"fileChanged\":false"); // A remounted, byte-identical file is reconciled.
    char held_path[512];
    snprintf(held_path,sizeof held_path,"%s/temporarily-unavailable.held",directory);
    assert(!rename(old_path,held_path));
    for (unsigned pass=0;pass<2;pass++) {
        ot_library_command(l,"{\"action\":\"discover\"}");settle_library(l);
        library_has(l,"\"available\":false"); // Scanning cannot recreate a missing file.
    }
    assert(!rename(held_path,old_path));
    ot_library_command(l,"{\"action\":\"discover\"}");settle_library(l);
    library_has(l,"\"available\":true");library_has(l,"\"fileChanged\":false");
    library_has(l, "\"requested\":1");
    library_has(
        l, "\"engineError\":\"memory_budget\""); // It retried after repair; this fixture has no RAM budget.
    assert(ot_set_preferences(app, "{\"startupModel\":-1,\"internet\":true}"));
    ot_library_command(l, "{\"action\":\"stop\"}");
    int fd = open(old_path, O_WRONLY);
    const unsigned char altered = 1;
    assert(fd >= 0 && pwrite(fd, &altered, 1, 67) == 1);
    struct timespec modified[2] = {{.tv_nsec = UTIME_NOW}, {.tv_sec = 100, .tv_nsec = 0}};
    assert(!futimens(fd, modified) && !close(fd));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"fileChanged\":true"); // An in-place edit is detected even with the same inode/size.
    ot_library_command(l, "{\"action\":\"select\",\"id\":1}");
    library_has(l, "file_changed");
    snprintf(command, sizeof command,
             "{\"action\":\"download\",\"url\":\"https://example.invalid/replacement.gguf\","
             "\"name\":\"Replacement\",\"checksum\":\"%s\",\"bytes\":68,\"replaceId\":1}",
             hash);
    ot_library_command(l, command);
    assert(ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, 20);
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":2}");
    assert(!access(old_path, F_OK) && access(new_path, F_OK));
    ot_library_command(l, command);
    assert(t.offset == 0 && ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    unsigned char damaged[68];
    memcpy(damaged, file, sizeof file);
    damaged[67] = 2;
    ot_library_bytes(l, t.id, damaged, sizeof damaged);
    ot_library_end(l, t.id, NULL);
    library_has(l, "checksum_mismatch");
    assert(!access(old_path, F_OK) && access(new_path, F_OK));
    ot_library_command(l, command);
    settle_library(l);
    assert(t.offset == 0 && ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, sizeof file);
    statement(path,
              "CREATE TRIGGER fail_replace BEFORE UPDATE ON models BEGIN SELECT RAISE(ABORT,'fault'); END;");
    ot_library_end(l, t.id, NULL);
    library_has(l, "storage_write");
    assert(!access(old_path, F_OK) && !access(new_path, F_OK));
    ot_library_close(l);
    ot_close(app);
    statement(path, "DROP TRIGGER fail_replace;");
    app = open_app(path, &chat);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_library_command(l, "{\"action\":\"resume\",\"id\":2}");
    for (unsigned i = 0; i < 1000 && !access(old_path, F_OK); i++) {
        ot_library_tick(l);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(access(old_path, F_OK) && !access(new_path, F_OK));
    library_has(l, "\"fileChanged\":false");
    sqlite3 *db = ot_store_connection(ot_app_store(app));
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT id,reference FROM models", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int64(q, 0) == 1 &&
           !strcmp((const char *)sqlite3_column_text(q, 1), new_path));
    assert(sqlite3_step(q) == SQLITE_DONE);
    sqlite3_finalize(q);
    ot_library_command(l, command);
    assert(ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, sizeof file);
    statement(path, "UPDATE models SET device=device+1 WHERE id=1;");
    ot_library_end(l, t.id, NULL);
    library_has(l, "replacement_changed"); // A stale confirmation cannot replace another registration.
    assert(!access(new_path, F_OK));
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":3}");
    statement(path, "UPDATE models SET device=device-1 WHERE id=1;");
    char external[512], installed[512];
    snprintf(external, sizeof external, "%s/external.gguf", directory);
    fd = open(external, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, file, sizeof file) == sizeof file && !close(fd));
    snprintf(command, sizeof command, "{\"action\":\"import\",\"path\":\"%s\"}", external);
    ot_library_command(l, command);
    snprintf(command, sizeof command,
             "UPDATE models SET metadata=json_set(metadata,'$.checksum','%s') WHERE id=2;", hash);
    statement(path, command);
    snprintf(command, sizeof command,
             "{\"action\":\"download\",\"url\":\"https://example.invalid/imported.gguf\","
             "\"name\":\"Imported\",\"checksum\":\"%s\",\"bytes\":68,\"replaceId\":2}",
             hash);
    ot_library_command(l, command);
    assert(ot_library_headers(l, t.id, 200, "\"v1\"", NULL, 68));
    ot_library_bytes(l, t.id, file, sizeof file);
    ot_library_end(l, t.id, NULL);
    snprintf(installed, sizeof installed, "%s/models/4.gguf", directory);
    assert(!access(external, F_OK) && !access(installed, F_OK)); // Imported originals remain external.
    ot_library_command(l, "{\"action\":\"remove\",\"id\":2,\"uninstall\":true}");
    assert(!access(external, F_OK) && access(installed, F_OK));
    assert(!unlink(external));
    ot_library_command(l, "{\"action\":\"remove\",\"id\":1,\"uninstall\":true}");
    assert(access(new_path, F_OK));
    for (unsigned mutation = 0; mutation < 2; mutation++) {
        char partial[512], changed_path[512];
        snprintf(partial, sizeof partial, "%s/models/99.part", directory);
        snprintf(changed_path, sizeof changed_path, "%s/models/.changed", directory);
        fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, file, sizeof file) == sizeof file && !close(fd));
        char *sql = sqlite3_mprintf("INSERT OR REPLACE INTO downloads(id,url,name,checksum,total,received,state) "
                                   "VALUES(99,'https://example.invalid/resume.gguf','Resume',%Q,68,68,'paused')", hash);
        assert(sql); statement(path, sql); sqlite3_free(sql);
        ot_library_command(l, "{\"action\":\"resume\",\"id\":99}");
        /* Keep the owner idle while the worker reads, then mutate the same-sized
         * partial before its completion is consumed. Both timings must reject it. */
        struct timespec delay = {.tv_nsec = 100000000}; nanosleep(&delay, NULL);
        fd = open(mutation ? changed_path : partial, O_WRONLY | (mutation ? O_CREAT | O_EXCL : 0), 0600);
        assert(fd >= 0);
        if (mutation) assert(write(fd, damaged, sizeof damaged) == sizeof damaged);
        else assert(pwrite(fd, &altered, 1, 67) == 1);
        assert(!close(fd));
        if (mutation) assert(!rename(changed_path, partial));
        for (unsigned i = 0; !db_integer(app, "SELECT state IN('paused','complete') FROM downloads WHERE id=99"); i++) {
            assert(i < 1000); ot_library_tick(l);
            delay.tv_nsec = 1000000; nanosleep(&delay, NULL);
        }
        assert(!db_integer(app, "SELECT count(*) FROM models WHERE managed=99"));
        assert(db_integer(app, "SELECT state='paused' AND error IN('partial_changed','checksum_mismatch') FROM downloads WHERE id=99"));
        assert(!unlink(partial));
    }
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    snprintf(command, sizeof command, "%s/models", directory);
    assert(!rmdir(command));
}
static void inventory_lifecycle(const char *path, const char *directory) {
    char folders[8][512], manifest[512], blob_path[512], cache[512];
    snprintf(cache, sizeof cache, "%s/cache/manifests/store", directory);
    const char *suffix[] = {"cache",
                            "cache/manifests",
                            "cache/manifests/store",
                            "cache/manifests/store/blobs",
                            "cache/manifests/store/manifests",
                            "cache/manifests/store/manifests/registry.ollama.ai",
                            "cache/manifests/store/manifests/registry.ollama.ai/library",
                            "cache/manifests/store/manifests/registry.ollama.ai/library/fixture"};
    for (unsigned i = 0; i < 8; i++) {
        snprintf(folders[i], sizeof folders[i], "%s/%s", directory, suffix[i]);
        assert(!mkdir(folders[i], 0700));
    }
    const char *digest = "0000000000000000000000000000000000000000000000000000000000000000";
    snprintf(blob_path, sizeof blob_path, "%s/blobs/sha256-%s", cache, digest);
    snprintf(manifest, sizeof manifest, "%s/manifests/registry.ollama.ai/library/fixture/latest", cache);
    FILE *file = fopen(blob_path, "wb");
    assert(file && fwrite("test", 1, 4, file) == 4 && !fclose(file));
    file = fopen(manifest, "wb");
    assert(file);
    fprintf(file,
            "{\"config\":{\"digest\":\"sha256:%s\",\"size\":4},"
            "\"layers\":[{\"mediaType\":\"application/vnd.ollama.image.model\","
            "\"digest\":\"sha256:%s\",\"size\":4}]}",
            digest, digest);
    assert(!fclose(file));
    assert(!setenv("OLLAMA_MODELS", cache, 1));
    transport chat = {0};
    acquisition t = {0};
    ot_app *app = open_app(path, &chat);
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    library_has(l, "fixture:latest");
    library_has(l, "\"available\":true");
    statement(path, "UPDATE models SET metadata=json_set(metadata,'$.family','fixture_moe',"
                    "'$.expertCount',8,'$.expertsUsed',2,'$.complexCache',json('true')) WHERE kind=2");
    assert(!unlink(blob_path));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"available\":false");
    file = fopen(blob_path, "wb");
    assert(file && fwrite("test", 1, 4, file) == 4 && !fclose(file));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    library_has(l, "\"available\":true");
    library_has(l, "\"family\":\"fixture_moe\"");
    library_has(l, "\"expertCount\":8");
    library_has(l, "\"complexCache\":true");
    ot_library_close(l);
    assert(!unlink(manifest));
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l); /* Startup scan detects an external manifest deletion too. */
    library_has(l, "\"available\":false");
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    assert(!unlink(blob_path));
    for (unsigned i = 8; i > 0; i--)
        assert(!rmdir(folders[i - 1]));
    snprintf(manifest, sizeof manifest, "%s/models", directory);
    assert(!rmdir(manifest));
    assert(!setenv("OLLAMA_MODELS", directory, 1));
}
static void discovery_filters(const char *path, const char *directory) {
    transport chat = {0};
    acquisition network = {0};
    ot_app *app = open_app(path, &chat);
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &network, .request = acquisition_request});
    assert(l);
    settle_library(l);
    assert(ot_set_preferences(app, "{\"internet\":true}"));
    const char *tasks[] = {"", "text-generation", "image-text-to-text", "feature-extraction"},
               *licenses[] = {"", "apache-2.0", "mit", "bsd-3-clause", "cc-by-4.0"},
               *orders[] = {"", "downloads", "recent", "trending"};
    unsigned cases = 0;
    for (unsigned task = 0; task < 4; task++)
        for (unsigned license = 0; license < 5; license++)
            for (unsigned order = 0; order < 4; order++)
                for (unsigned size = 0; size < 2; size++) {
                    char command[512];
                    snprintf(command, sizeof command,
                             "{\"action\":\"search\",\"query\":\"filters%u\",\"task\":\"%s\",\"license\":\"%"
                             "s\",\"order\":\"%s\",\"maxParameters\":%u}",
                             cases, tasks[task], licenses[license], orders[order], size ? 14u : 0u);
                    unsigned before = network.requests;
                    ot_library_command(l, command);
                    assert(network.requests == before + 1);
                    assert(strstr(network.url, "https://huggingface.co/api/models?filter=gguf&"));
                    assert(!size || strstr(network.url, "num_parameters=max%3A14B"));
                    assert(!license || strstr(network.url, "filter=license%3A"));
                    assert(order != 2 || strstr(network.url, "sort=lastModified"));
                    assert(order != 3 || strstr(network.url, "sort=trendingScore"));
                    ot_library_bytes(l, network.id, "[]", 2);
                    ot_library_end(l, network.id, NULL);
                    cases++;
                }
    const char *command = "{\"action\":\"search\",\"query\":\"x&escape\",\"task\":\"text-generation\","
                          "\"license\":\"mit\",\"maxParameters\":7}";
    ot_library_command(l, command);
    assert(strstr(network.url, "search=x%26escape"));
    const char *data =
        "[{\"id\":\"o/unknown-7B\",\"pipeline_tag\":\"text-generation\",\"cardData\":{\"license\":\"mit\"}},"
        "{\"id\":\"o/"
        "large\",\"pipeline_tag\":\"text-generation\",\"gguf\":{\"total\":70000000000},\"cardData\":{"
        "\"license\":\"mit\"}},"
        "{\"id\":\"o/"
        "wrong-license\",\"pipeline_tag\":\"text-generation\",\"gguf\":{\"total\":3800000000},\"cardData\":{"
        "\"license\":\"apache-2.0\"}},"
        "{\"id\":\"o/"
        "fit\",\"pipeline_tag\":\"text-generation\",\"gguf\":{\"total\":3800000000,\"context_length\":131072}"
        ",\"tags\":[\"license:mit\",\"reasoning\"]}]";
    ot_library_bytes(l, network.id, data, strlen(data));
    ot_library_end(l, network.id, NULL);
    char *raw = ot_library_view(l);
    yyjson_doc *d = yyjson_read(raw, strlen(raw), 0);
    assert(d);
    yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(d), "catalog");
    assert(yyjson_arr_size(items) == 1);
    yyjson_val *first = yyjson_arr_get(items, 0);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(first, "name")), "o/fit"));
    assert(yyjson_get_uint(yyjson_obj_get(first, "context")) == 131072 &&
           yyjson_is_true(yyjson_obj_get(first, "reasoning")));
    yyjson_doc_free(d);
    free(raw);
    /* Server-side limits count raw rows, not rows with sufficient metadata. */
    ot_library_command(l, "{\"action\":\"search\",\"query\":\"paging\",\"maxParameters\":7}");
    yyjson_mut_doc *page = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *page_items = yyjson_mut_arr(page);
    yyjson_mut_doc_set_root(page, page_items);
    for (unsigned i = 0; i < 20; i++) {
        yyjson_mut_val *row = yyjson_mut_obj(page);
        yyjson_mut_arr_append(page_items, row);
        yyjson_mut_obj_add_str(page, row, "id", "owner/unknown");
    }
    char *page_json = yyjson_mut_write(page, 0, NULL);
    assert(page_json);
    ot_library_bytes(l, network.id, page_json, strlen(page_json));
    ot_library_end(l, network.id, NULL);
    free(page_json);
    yyjson_mut_doc_free(page);
    library_has(l, "\"catalogHasMore\":true");
    library_has(l, "\"catalog\":[]");
    unsigned cached_requests = network.requests;
    ot_library_command(l, "{\"action\":\"search\",\"query\":\"paging\",\"maxParameters\":7}");
    assert(network.requests == cached_requests);
    library_has(l, "\"catalogHasMore\":true");
    ot_library_command(l, "{\"action\":\"search\",\"query\":\"paging\",\"limit\":40,\"maxParameters\":7}");
    assert(network.requests == cached_requests + 1);
    ot_library_bytes(l, network.id, "[]", 2);
    ot_library_end(l, network.id, NULL);
    library_has(l, "\"catalogHasMore\":false");
    ot_library_command(l, "{\"action\":\"variants\",\"repo\":\"owner/model\"}");
    const char *files =
        "{\"siblings\":[{\"rfilename\":\"mmproj-BF16.gguf\"},{\"rfilename\":\"lora-Q4.gguf\"},"
        "{\"rfilename\":\"model-Q4-00001-of-00002.gguf\"},{\"rfilename\":\"real-Q4.gguf\"}]}";
    ot_library_bytes(l, network.id, files, strlen(files));
    ot_library_end(l, network.id, NULL);
    raw = ot_library_view(l);
    d = yyjson_read(raw, strlen(raw), 0);
    assert(d);
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), "catalog")) == 1);
    yyjson_doc_free(d);
    free(raw);
    assert(ot_model_primary_file("adapters/real-adapterless-Q4.gguf"));
    assert(ot_model_primary_file("chain-of-thought-Q4.gguf"));
    assert(!ot_model_primary_file("model-00001-of-00002.gguf"));
    assert(!ot_model_primary_file("vision.mmproj-BF16.gguf"));
    const char *invalid[] = {"{\"action\":\"search\",\"task\":\"thinking&host=evil\"}",
                             "{\"action\":\"search\",\"license\":\"unknown\"}",
                             "{\"action\":\"search\",\"order\":\"invalid\"}",
                             "{\"action\":\"search\",\"maxParameters\":2049}",
                             "{\"action\":\"search\",\"maxParameters\":-1}",
                             "{\"action\":\"ollamaSearch\",\"license\":\"mit\"}"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        unsigned before = network.requests;
        ot_library_command(l, invalid[i]);
        assert(network.requests == before);
    }
    ot_library_command(l, "{\"action\":\"ollamaVariants\",\"repo\":\"owner/model\"}");
    assert(!strcmp(network.url, "https://ollama.com/owner/model/tags"));
    const char *html = "<a href='/owner/model:4b-Q4_K_M'><span>3GB</span> 32K context window</a>";
    ot_library_bytes(l, network.id, html, strlen(html));
    ot_library_end(l, network.id, NULL);
    library_has(l, "\"precision\":\"q4\"");
    library_has(l, "\"context\":32768");
    char *parsed = ot_ollama_catalog(
        "<a href='/owner/model'><h2>Model</h2><p>Coding e ricerca</p><span>thinking</span></a>", NULL);
    assert(parsed && strstr(parsed, "owner/model") && strstr(parsed, "reasoning\":true"));
    free(parsed);
    ot_model_candidate candidate = {0};
    strcpy(candidate.name, "Useful code");
    assert(ot_model_purposes(&candidate) & 2u);
    strcpy(candidate.name, "decode");
    assert(!(ot_model_purposes(&candidate) & 2u));
    char title[257], quant[33];
    ot_model_label("model-UD-Q4_K_XL.gguf", "Q4_K_XL", title, quant);
    assert(!strstr(title, "Q4") && !strcmp(quant, "Q4_K_XL"));
    assert(!strcmp(ot_model_precision("IQ4_XS"), "iq4") && !strcmp(ot_model_precision("MXFP4"), "mxfp4"));
    assert(!strcmp(ot_model_precision("Q4not-a-type"), "other") &&
           !strcmp(ot_model_precision(""), "unknown"));
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    char models[512];
    snprintf(models, sizeof models, "%s/models", directory);
    assert(!rmdir(models));
    puts("PASS: 160 upstream filter combinations, strict response constraints, unknown counts, encoding, "
         "namespaces, precision and shared usage semantics");
}
static void catalog_caching(const char *path, const char *directory) {
    transport chat = {0};
    ot_app *app = open_app(path, &chat);
    assert(ot_set_preferences(app, "{\"internet\":true}"));
    acquisition t = {0};
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    const char *command = "{\"action\":\"search\",\"query\":\"cache\"}";
    ot_library_command(l, command);
    uint64_t first = t.id;
    const char *data = "[{\"id\":\"owner/model\"}]";
    ot_library_bytes(l, t.id, data, strlen(data));
    ot_library_end(l, t.id, NULL);
    ot_library_command(l, command);
    assert(t.id == first);
    library_has(l, "owner/model");
    ot_library_command(l, "{\"action\":\"search\",\"query\":\"cache\",\"refresh\":true}");
    assert(t.id != first);
    first = t.id;
    ot_library_bytes(l, t.id, "{\"error\":\"unavailable\"}", 23);
    ot_library_end(l, t.id, NULL);
    library_has(l, "invalid_response");
    ot_library_command(l, command);
    assert(t.id != first); /* Explicit refresh invalidates the previous response, including on failure. */
    ot_library_bytes(l, t.id, "[]", 2);
    ot_library_end(l, t.id, NULL);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, items);
    char name[1000];
    memset(name, 'a', sizeof name);
    memcpy(name, "owner/", 6);
    for (unsigned i = 0; i < 100; i++) {
        snprintf(name + 980, 20, "%u", i);
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "id", name);
        yyjson_mut_arr_append(items, item);
    }
    char *large = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    assert(large);
    for (unsigned i = 0; i < 10; i++) {
        char request[128];
        snprintf(request, sizeof request, "{\"action\":\"search\",\"query\":\"bounded%u\"}", i);
        ot_library_command(l, request);
        ot_library_bytes(l, t.id, large, strlen(large));
        ot_library_end(l, t.id, NULL);
        char *health = ot_library_health(l);
        yyjson_doc *snapshot = yyjson_read(health, strlen(health), 0);
        assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(snapshot), "catalogCache"),
                                              "bytes")) <= 1024 * 1024);
        yyjson_doc_free(snapshot);
        free(health);
    }
    free(large);
    first = t.id;
    ot_library_command(l, command);
    assert(t.id != first); /* The least recently used response has been evicted. */
    ot_library_bytes(l, t.id, "[]", 2);
    ot_library_end(l, t.id, NULL);
    ot_preferences previous = *ot_get_preferences(app);
    assert(ot_set_preferences(app, "{\"internet\":false}"));
    ot_library_preferences_changed(l, &previous);
    char *health = ot_library_health(l);
    assert(strstr(health, "\"catalogCache\":{\"bytes\":0"));
    free(health);
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    char models[512];
    snprintf(models, sizeof models, "%s/models", directory);
    assert(!rmdir(models));
}
static void queued_downloads(const char *path, const char *directory) {
    transport chat = {0};
    ot_app *app = open_app(path, &chat);
    statement(path, "INSERT INTO models(id,name,kind,reference) VALUES(42,'Previous',1,'/missing/model');");
    ot_connection c = *ot_configuration(app);
    strcpy(c.model, "Previous");
    assert(ot_configure(app, &c, 42));
    assert(ot_set_preferences(app, "{\"internet\":true,\"startupModel\":-1}"));
    acquisition t = {0};
    ot_library *l =
        ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l && !*ot_configuration(app)->model);
    settle_library(l);
    assert(!t.id); /* None at startup retains the last selection without launching it. */
    char command[512];
    for (unsigned i = 0; i < 3; i++) {
        snprintf(command, sizeof command,
                 "{\"action\":\"download\",\"url\":\"https://example.invalid/%u.gguf\","
                 "\"name\":\"Queue %u\",\"checksum\":\"%064u\",\"bytes\":68}",
                 i, i, i + 1);
        uint64_t previous = t.id;
        ot_library_command(l, command);
        assert(i ? t.id == previous : t.id > 0);
    }
    library_has(l, "\"state\":\"queued\"");
    uint64_t first = t.id;
    ot_library_command(l, "{\"action\":\"pause\",\"id\":1}");
    ot_library_tick(l);
    assert(t.id != first && strstr(t.url, "/1.gguf"));
    uint64_t second = t.id;
    ot_library_command(l, "{\"action\":\"pause\",\"id\":3}");
    assert(t.id == second);
    ot_library_command(l, "{\"action\":\"discard\",\"id\":1}");
    library_has(l, "invalid_job");
    ot_library_command(l, "{\"action\":\"resume\",\"id\":1}");
    assert(t.id == second);
    ot_library_close(l);
    ot_close(app);
    app = open_app(path, &chat);
    l = ot_library_open(app, directory, (ot_library_io){.context = &t, .request = acquisition_request});
    assert(l);
    settle_library(l);
    assert(strstr(t.url, "/0.gguf")); /* The explicit queue survives restart; paused jobs stay paused. */
    for (unsigned id = 1; id <= 3; id++) {
        snprintf(command, sizeof command, "{\"action\":\"cancelJob\",\"id\":%u}", id);
        ot_library_command(l, command);
    }
    ot_library_tick(l);
    uint64_t cancelled_transfer = t.id;
    ot_library_command(l, "{\"action\":\"restart\",\"id\":1}");
    settle_library(l);
    assert(t.id > cancelled_transfer && strstr(t.url, "/0.gguf"));
    assert(t.offset == 0); /* Cancelled partial files cannot become a resume range. */
    uint64_t restarted = t.id;
    ot_library_command(l, "{\"action\":\"restart\",\"id\":2}");
    assert(t.id == restarted); /* Restart while another transfer runs joins the real queue. */
    library_has(l, "\"state\":\"queued\"");
    ot_library_command(l, "{\"action\":\"restart\",\"id\":9999}");
    library_has(l, "invalid_job");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":1}");
    ot_library_command(l, "{\"action\":\"cancelJob\",\"id\":2}");
    ot_library_tick(l);
    for (unsigned i = 0; i < 66; i++) {
        snprintf(command, sizeof command,
                 "{\"action\":\"download\",\"url\":\"https://example.invalid/bounded/%u.gguf\","
                 "\"name\":\"Bounded %u\",\"checksum\":\"%064u\",\"bytes\":68}",
                 i, i, i + 100);
        ot_library_command(l, command);
    }
    library_has(l, "queue_full"); /* One transfer plus at most 64 waiting jobs. */
    for (unsigned id = 4; id <= 68; id++) {
        snprintf(command, sizeof command, "{\"action\":\"cancelJob\",\"id\":%u}", id);
        ot_library_command(l, command);
    }
    ot_library_tick(l);
    ot_library_close(l);
    ot_close(app);
    free(chat.body);
    unlink(path);
    snprintf(command, sizeof command, "%s/models", directory);
    assert(!rmdir(command));
}
static void locations(const char *directory) {
    char source[512], target[512], foreign[512], db[1024], model[1024], filename[1024], sql[4096];
    snprintf(source, sizeof source, "%s/data-source", directory);
    snprintf(target, sizeof target, "%s/data-target", directory);
    snprintf(foreign, sizeof foreign, "%s/data-foreign", directory);
    assert(!mkdir(source, 0700) && !mkdir(target, 0700) && !mkdir(foreign, 0700));
    snprintf(db, sizeof db, "%s/workspace.sqlite", source);
    transport network = {0};
    ot_app *a = open_app(db, &network);
    assert(send_message(a, "Retained source conversation", "fixture", false));
    complete(a, &network, "Retained source answer");
    ot_close(a);
    free(network.body);
    network.body = NULL;
    snprintf(model, sizeof model, "%s/models", source);
    assert(!mkdir(model, 0700));
    snprintf(model, sizeof model, "%s/models/1.gguf", source);
    int fd = open(model, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "weights", 7) == 7 && !close(fd));
    struct stat st;
    assert(!stat(model, &st));
    snprintf(sql, sizeof sql,
             "INSERT INTO models(name,kind,reference,bytes,managed,device,inode,origin) "
             "VALUES('Owned',1,'%s',7,1,%lld,%lld,'overtone');",
             model, (long long)st.st_dev, (long long)st.st_ino);
    statement(db, sql);
    char *replacement_job = sqlite3_mprintf("INSERT INTO downloads(id,url,name,checksum,state,metadata) "
        "VALUES(3,'https://example.invalid/next.gguf','Next','%064u','paused',"
        "json_object('replaceId',1,'replacePath',%Q,'replaceDevice',%lld,'replaceInode',%lld))",
        0U, model, (long long)st.st_dev, (long long)st.st_ino);
    assert(replacement_job); statement(db, replacement_job); sqlite3_free(replacement_job);
    const char *pull_folders[] = {"models/pull-2", "models/pull-2/blobs", "models/pull-2/manifests"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(filename, sizeof filename, "%s/%s", source, pull_folders[i]);
        assert(!mkdir(filename, 0700));
    }
    snprintf(filename, sizeof filename, "%s/models/pull-2/blobs/partial", source);
    fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "partial", 7) == 7 && !close(fd));
    snprintf(filename, sizeof filename, "%s/foreign.txt", foreign);
    fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && !close(fd));
    ot_location l;
    assert(ot_location_open(&l, source));
    assert(!ot_location_plan(&l, foreign) && !strcmp(l.error, "location_not_empty"));
    assert(!ot_location_plan(&l, source));
    snprintf(filename, sizeof filename, "%s/models/pull-2/blobs/external", source);
    assert(!symlink(foreign, filename));
    assert(!ot_location_plan(&l, target) && !strcmp(l.error, "location_inventory"));
    assert(!unlink(filename));
    assert(ot_location_plan(&l, target) && l.files == 3);
    assert(ot_location_cancel(&l) && !*l.pending);
    assert(ot_location_open(&l, source) && !strcmp(l.active, source));
    assert(ot_location_plan(&l, target));
    atomic_bool cancel_copy = true;
    assert(!ot_location_apply(&l, &cancel_copy, NULL, NULL) && !strcmp(l.error, "cancelled"));
    assert(!access(db, F_OK) && !access(model, F_OK));
    atomic_store(&cancel_copy, false);
    assert(ot_location_apply(&l, &cancel_copy, NULL, NULL));
    assert(!strcmp(l.active, target) && !*l.pending);
    assert(ot_location_open(&l, source) && !strcmp(l.active, target));
    assert(!access(db, F_OK) && !access(model, F_OK));
    snprintf(filename, sizeof filename, "%s/workspace.sqlite", target);
    a = open_app(filename, &network);
    has(a, "Retained source answer");
    ot_close(a);
    free(network.body);
    network.body = NULL;
    sqlite3 *moved = NULL;
    assert(sqlite3_open(filename, &moved) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(moved, "SELECT reference,device,inode FROM models", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    snprintf(filename, sizeof filename, "%s/models/1.gguf", target);
    assert(!strcmp((const char *)sqlite3_column_text(q, 0), filename));
    assert(!stat(filename, &st));
    assert(sqlite3_column_int64(q, 1) == st.st_dev && sqlite3_column_int64(q, 2) == (sqlite3_int64)st.st_ino);
    sqlite3_finalize(q);
    assert(sqlite3_prepare_v2(moved, "SELECT json_extract(metadata,'$.replacePath'),"
        "json_extract(metadata,'$.replaceDevice'),json_extract(metadata,'$.replaceInode') FROM downloads WHERE id=3",
        -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(q, 0), filename) &&
        sqlite3_column_int64(q, 1) == st.st_dev && sqlite3_column_int64(q, 2) == (sqlite3_int64)st.st_ino);
    sqlite3_finalize(q);
    sqlite3_close(moved);
    snprintf(filename, sizeof filename, "%s/models/pull-2/blobs/partial", target);
    char preserved[7]; fd = open(filename, O_RDONLY);
    assert(fd >= 0 && read(fd, preserved, 7) == 7 && !memcmp(preserved, "partial", 7) && !close(fd));
    assert(ot_location_plan(&l, source));
    assert(ot_location_apply(&l, &cancel_copy, NULL, NULL));
    assert(!strcmp(l.active, source));
    const char *roots[] = {source, target};
    const char *files[] = {"workspace.sqlite", "workspace.sqlite-wal", "workspace.sqlite-shm",
                           "models/1.gguf",    "location.id",          "location.json",
                           "profile.lock"};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < sizeof files / sizeof *files; j++) {
            snprintf(filename, sizeof filename, "%s/%s", roots[i], files[j]);
            unlink(filename);
        }
        assert(ot_pull_discard(roots[i], 2));
        snprintf(filename, sizeof filename, "%s/models", roots[i]);
        assert(!rmdir(filename));
        assert(!rmdir(roots[i]));
    }
    snprintf(filename, sizeof filename, "%s/foreign.txt", foreign);
    assert(!unlink(filename) && !rmdir(foreign));
}
static void browser_invalidation(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    ot_store *store = ot_app_store(a);
    sqlite3 *db = ot_store_connection(store);
    assert(sqlite3_exec(db, "INSERT INTO chats(id,title) VALUES(1,'original'),(2,'second')", NULL,NULL,NULL)==SQLITE_OK);
    assert(value(a,"chatTotal")==2);
    assert(ot_draft(a,0,"independent draft"));
    assert(value(a,"chatTotal")==2);
    assert(sqlite3_exec(db,"BEGIN;UPDATE chats SET title='temporary' WHERE id=1",NULL,NULL,NULL)==SQLITE_OK);
    has(a,"temporary");
    assert(sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    has(a,"original");
    char *view=ot_view(a);assert(view && !strstr(view,"temporary"));free(view);
    statement(path,"INSERT INTO chats(id,title) VALUES(3,'external')");
    has(a,"external");assert(value(a,"chatTotal")==3);
    assert(ot_store_classify(store,1,3));assert(value(a,"chatTotal")==2);
    assert(ot_store_classify(store,2,1));assert(value(a,"chatTotal")==1);
    assert(ot_store_restore_archive(store));assert(value(a,"chatTotal")==2);
    assert(ot_store_classify(store,2,2) && ot_store_classify(store,3,2));
    assert(value(a,"chatTotal")==0 && value(a,"trashTotal")==2);
    assert(ot_store_empty_trash(store));assert(value(a,"trashTotal")==0);
    ot_close(a);free(t.body);unlink(path);
    puts("PASS: cached history, unrelated writes, transaction rollback, external commit, archive and empty trash");
}
static void restoration_invariants(const char *path) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(ot_set_preferences(
        a, "{\"setup\":3,\"setupTarget\":\"local:1\",\"setupTargetEngines\":3,"
           "\"setupPaused\":true,\"guideSeen\":7,\"adviceSeen\":\"seen\",\"theme\":\"dark\"}"));
    assert(ot_reset_preferences(a));
    const ot_preferences *p = ot_get_preferences(a);
    assert(p->setup == 3 && !strcmp(p->setup_target, "local:1") && p->setup_target_engines == 3 &&
           p->setup_paused && p->guide_seen == 7 && !strcmp(p->advice_seen, "seen") &&
           !strcmp(p->theme, "system"));
    ot_close(a);
    statement(path, "INSERT INTO chats(id,title) VALUES(1,'A'),(2,'B');"
                    "INSERT INTO turns(id,chat,prompt,state,input_id) VALUES"
                    "(1,1,'A','complete',1),(2,2,'B','complete',2),(3,1,'A','complete',1);"
                    "UPDATE chats SET head=id;");
    assert(ot_store_validate(path));
    const char *invalid[] = {"UPDATE chats SET head=2 WHERE id=1",
                             "UPDATE chats SET manual_title=9 WHERE id=1",
                             "UPDATE turns SET state='unknown' WHERE id=1",
                             "UPDATE turns SET summary_limited=8 WHERE id=2",
                             "UPDATE chats SET deleted=1793925146 WHERE id=1",
                             "UPDATE chats SET archived=-1 WHERE id=2",
                             "UPDATE chats SET archived=1,deleted=1 WHERE id=1",
                             "UPDATE turns SET parent=1 WHERE id=2",
                             "UPDATE turns SET next=2 WHERE id=1",
                             "UPDATE turns SET next=3 WHERE id=1", /* Same chat, but not a child. */
                             "UPDATE turns SET input_id=1 WHERE id=2",
                             "UPDATE turns SET prompt='other input' WHERE id=3",
                             "UPDATE advice_state SET catalogue='{}'",
                             "UPDATE advice_state SET dismissed='{}'",
                             "DELETE FROM advice_state",
                             "INSERT INTO advice_samples(turn,sample) VALUES(1,'[]')"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        statement(path, invalid[i]);
        assert(!ot_store_validate(path));
        statement(path, "UPDATE chats SET head=id,manual_title=0,archived=0,deleted=0;UPDATE turns SET "
                        "state='complete',summary_limited=0,parent=NULL,next=NULL;"
                        "UPDATE turns SET input_id=2 WHERE id=2;UPDATE turns SET prompt='A' WHERE id=3;"
                        "INSERT OR IGNORE INTO advice_state(id) VALUES(1);"
                        "UPDATE advice_state SET catalogue='[]',dismissed='[]';DELETE FROM advice_samples;");
        assert(ot_store_validate(path));
    }
    a = open_app(path, &t);
    assert(ot_get_preferences(a)->setup == 3 && ot_get_preferences(a)->setup_paused);
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
    puts("PASS: preference reset retains operation state; restore rejects cross-chat/invalid branch and "
         "advice records");
}
static void library_duplicates(const char *path, const char *directory) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    ot_library *l = ot_library_open(a, directory, (ot_library_io){0});
    assert(l);
    settle_library(l);
    statement(path,
              "INSERT INTO models(id,name,kind,reference,bytes,layers,embedding,device,inode,hidden) VALUES"
              "(1,'A',1,'a',1024,2,3,7,1,0),(2,'B',1,'b',1024,2,3,7,1,0),"
              "(3,'C',1,'c',1024,2,3,7,2,0),(4,'D',1,'d',1024,2,3,7,1,1),"
              "(5,'E',1,'e',1024,4,3,7,3,0),(6,'F',2,'f',1024,2,3,7,1,0),"
              "(7,'G',1,'g',0,0,0,0,0,0)");
    const unsigned same[] = {2, 2, 1, 2, 1, 0, 0}, possible[] = {3, 3, 3, 3, 1, 0, 0};
    char *view = ot_library_view(l);
    assert(view);
    yyjson_doc *d = yyjson_read(view, strlen(view), 0);
    assert(d);
    unsigned seen = 0;
    for (unsigned group = 0; group < 2; group++) {
        yyjson_val *rows = yyjson_obj_get(yyjson_doc_get_root(d), group ? "hiddenModels" : "models"), *row;
        size_t i, n;
        yyjson_arr_foreach(rows, i, n, row) {
            uint64_t id = yyjson_get_uint(yyjson_obj_get(row, "id"));
            assert(id >= 1 && id <= 7);
            assert(yyjson_get_uint(yyjson_obj_get(row, "sameFile")) == same[id - 1] &&
                   yyjson_get_uint(yyjson_obj_get(row, "possibleDuplicates")) == possible[id - 1]);
            seen++;
        }
    }
    assert(seen == 7);
    yyjson_doc_free(d);
    free(view);
    ot_library_close(l);
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
    char models[512];
    snprintf(models, sizeof models, "%s/models", directory);
    assert(!rmdir(models));
}
static void library_projection_bounds(const char *path, const char *directory) {
    transport t = {0};
    ot_app *a = open_app(path, &t);
    ot_library *l = ot_library_open(a, directory, (ot_library_io){0});
    assert(l);
    settle_library(l);
    statement(path,
              "WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1001) "
              "INSERT INTO models(id,name,kind,reference) SELECT x,CASE WHEN x=1001 THEN 'ZZ selected' "
              "ELSE 'A model' END,1,'/overtone-missing-fixture/'||x FROM n;"
              "WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1001) "
              "INSERT INTO downloads(id,url,name,checksum,state,updated) SELECT x,'https://example.com/'||x,"
              "'Fixture','',CASE WHEN x=1 THEN 'paused' ELSE 'failed' END,x FROM n");
    ot_connection route = {.endpoint = "http://127.0.0.1:11434", .model = "ZZ selected"};
    assert(ot_configure(a, &route, 1001));
    ot_library_command(l, "{\"action\":\"discover\"}");
    settle_library(l);
    char *view = ot_library_view(l);
    assert(view);
    yyjson_doc *d = yyjson_read(view, strlen(view), 0);
    assert(d);
    yyjson_val *root = yyjson_doc_get_root(d), *rows = yyjson_obj_get(root, "models"), *row;
    assert(yyjson_get_bool(yyjson_obj_get(root, "projectionLimited")) && yyjson_arr_size(rows) == 1000 &&
           yyjson_equals_str(yyjson_obj_get(root, "error"), "inventory_limited"));
    bool current = false;
    size_t i, n;
    yyjson_arr_foreach(rows, i, n, row) if (yyjson_get_sint(yyjson_obj_get(row, "id")) == 1001) {
        current = true;
        assert(!yyjson_get_bool(yyjson_obj_get(row, "available")));
    }
    assert(current &&
           yyjson_get_sint(yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(root, "jobs")), "id")) == 1);
    yyjson_doc_free(d);
    free(view);
    ot_library_close(l);
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
    char models[512];
    snprintf(models, sizeof models, "%s/models", directory);
    assert(!rmdir(models));
    puts("PASS: bounded inventory reports partial coverage and retains the current model and pending "
         "transfers");
}
static void advice_current_identity(const char *path, const char *directory) {
    transport t = {0};
    acquisition network = {0};
    ot_app *a = open_app(path, &t);
    assert(ot_set_preferences(a, "{\"setup\":2,\"internet\":true,\"network\":1,"
                                 "\"setupEngines\":2,\"startupModel\":-1}"));
    ot_resources resources = {.memory_total = 64ull << 30,
                              .memory_available = 56ull << 30,
                              .gpu = true,
                              .gpu_recommended = 48ull << 30,
                              .disk_available = 500ull << 30,
                              .cores = 8};
    ot_resources_update(a, &resources);
    ot_model_candidate model = {.kind = 2, .name = "fixture:8b", .bytes = 4ull << 30, .context = 32768};
    char *catalogue = ot_model_catalogue_write(&model, 1);
    assert(catalogue);
    char *sql =
        sqlite3_mprintf("UPDATE advice_state SET catalogue=%Q,updated=unixepoch(),checked=unixepoch();"
                        "INSERT INTO models(id,name,kind,reference,bytes,context) VALUES"
                        "(1,'Fixture',2,'fixture:8b',4294967296,32768);",
                        catalogue);
    assert(sql);
    statement(path, sql);
    sqlite3_free(sql);
    free(catalogue);
    ot_library *l =
        ot_library_open(a, directory, (ot_library_io){.context = &network, .request = acquisition_request});
    assert(l);
    settle_library(l);
    ot_connection route = {.ollama = true, .endpoint = "http://127.0.0.1:11434", .model = "fixture:8b"};
    assert(ot_configure(a, &route, 1));
    for (unsigned pass = 0; pass < 3; pass++) {
        if (pass == 1)
            statement(path,
                      "WITH RECURSIVE n(x) AS(VALUES(2) UNION ALL SELECT x+1 FROM n WHERE x<34) "
                      "INSERT INTO models(id,name,kind,reference,bytes,context,metadata) "
                      "SELECT x,'Other',2,'fixture:'||x,4294967296,32768,'{\"chatSupported\":false}' FROM n");
        if (pass == 2)
            assert(ot_configure(a, &route, 0)); /* Existing local server, not library-owned. */
        char *view = ot_library_view(l);
        assert(view);
        yyjson_doc *d = yyjson_read(view, strlen(view), 0);
        assert(d);
        assert(!yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), "adviceRecommendations")) &&
               !yyjson_obj_get(yyjson_doc_get_root(d), "recommendationNotice"));
        yyjson_doc_free(d);
        free(view);
    }
    ot_library_close(l);
    ot_close(a);
    free(t.body);
    assert(!unlink(path));
    char models[512];
    snprintf(models, sizeof models, "%s/models", directory);
    assert(!rmdir(models));
    puts("PASS: advice retains the current model beyond 32 newer registrations, including an existing local "
         "server");
}
static void resource_worker_fixture(const char *pid_path, bool close_output) {
    int ready[2];
    assert(!pipe(ready));
    pid_t descendant = fork();
    assert(descendant >= 0);
    if (!descendant) {
        close(ready[0]);
        signal(SIGTERM, SIG_IGN);
        if (close_output) {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
        assert(write(ready[1], "1", 1) == 1);
        close(ready[1]);
        for (;;)
            pause();
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    FILE *pid_file = fopen(pid_path, "w");
    assert(pid_file);
    fprintf(pid_file, "%d", descendant);
    assert(!fclose(pid_file));
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        const char *counter = getenv("OT_CHECK_HELP_COUNT");
        if (counter) {
            FILE *file = fopen(counter,"a");
            assert(file && fputs("x",file) >= 0 && !fclose(file));
        }
        if (getenv("OT_CHECK_COLD_HELP")) {
            struct timespec delay = {.tv_sec = 3};
            nanosleep(&delay, NULL);
        }
        const char *worker_path = getenv("OT_CHECK_HELP_WORKER");
        if (worker_path)
            resource_worker_fixture(worker_path, true);
        puts(getenv("OT_CHECK_HELP_LEGACY") ? "fixture --model FILE --ctx-size N" :
             "fixture --sleep-idle-seconds SECONDS --model FILE --ctx-size N");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--supervise"))
        return ot_engine_supervise(argv + 2);
    if (argc > 2 && !strcmp(argv[1], "-m") && !strcmp(argv[2], "fixture-orphan-exit")) {
        resource_worker_fixture("descendant.pid", false);
        return 0; /* The engine exits while a descendant still owns resources. */
    }
    if (argc > 2 && !strcmp(argv[1], "-m") && !strcmp(argv[2], "fixture")) {
        resource_worker_fixture("descendant.pid", false);
        fputs("load_tensors: offloaded 1/2 layers to GPU\nllama_context: n_ctx = 8192\n"
              "llama_context: n_batch = 2048\nllama_context: n_ubatch = 512\n",
              stderr);
        for (;;)
            pause();
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--owner-fixture")) {
        ot_engine e;
        ot_engine_init(&e);
        char executable[4096];
        assert(realpath(argv[0], executable));
        ot_preferences p = ot_preferences_default();
        ot_resources r = {.cores = 2};
        assert(ot_engine_start(&e, executable, argv[2], "fixture", false, false, NULL, &p, &r));
        for (unsigned i = 0; i < 500 && e.child <= 0; i++) {
            ot_engine_poll(&e);
            struct timespec t = {.tv_nsec = 10000000};
            nanosleep(&t, NULL);
        }
        assert(e.child > 0);
        for (unsigned i = 0; i < 500 && !e.runtime.micro_batch; i++) {
            ot_engine_poll(&e);
            struct timespec t = {.tv_nsec = 1000000};
            nanosleep(&t, NULL);
        }
        assert(e.runtime.kind == OT_RUNTIME_HYBRID && e.runtime.context == 8192 && e.runtime.batch == 2048 &&
               e.runtime.micro_batch == 512);
        uint64_t footprint = 0;
        assert(ot_engine_memory(&e, &footprint) && footprint > 0);
        char path[4096];
        snprintf(path, sizeof path, "%s/descendant.pid", argv[2]);
        FILE *worker = fopen(path, "r");
        int worker_pid = 0;
        assert(worker && fscanf(worker, "%d", &worker_pid) == 1 && worker_pid > 0);
        fclose(worker);
        snprintf(path, sizeof path, "%s/owned.pid", argv[2]);
        FILE *f = fopen(path, "w");
        assert(f);
        fprintf(f, "%d %d", e.child, worker_pid);
        assert(!fclose(f));
        if (argc == 4) {
            assert(!strcmp(argv[3], "clean"));
            ot_engine_close(&e);
            assert(e.pid <= 0 && e.child <= 0 && !e.stopping);
            return 0;
        }
        _exit(0); /* The supervisor must stop the engine without owner cleanup. */
    }
    if (argc == 3 && strcmp(argv[1], "--crash-fixture") == 0) {
        transport child_transport = {0};
        ot_app *child_app = open_app(argv[2], &child_transport);
        assert(send_message(child_app, "Accettato prima del crash", "model", false));
        _exit(0); /* No close: exercise WAL recovery, not clean shutdown. */
    }
    assert(argc == 1);
    rewrite_contract();
    char self[4096];
    assert(realpath(argv[0], self));
    assert(ot_engine_sleep_supported(self));
    assert(ot_engine_validate(self, false));
    assert(!setenv("OT_CHECK_COLD_HELP", "1", 1));
    assert(ot_engine_validate(self, false));
    assert(!unsetenv("OT_CHECK_COLD_HELP"));
    assert(!ot_engine_validate(self, true));
    assert(!ot_engine_validate("/usr/bin/true", false));
    assert(!ot_engine_sleep_supported("/usr/bin/true"));
    char directory[256], path[512], temporary_path[4096];
    const char *temporary = getenv("TMPDIR");
    /* macOS commonly provides /var/... through TMPDIR; SQLite deliberately rejects symlinks. */
    assert(realpath(temporary && *temporary ? temporary : "/private/tmp", temporary_path));
    int length = snprintf(directory, sizeof directory, "%s/overtone-checks-XXXXXX", temporary_path);
    assert(length > 0 && (size_t)length < sizeof directory);
    assert(mkdtemp(directory));
    char help_worker_path[300];
    snprintf(help_worker_path, sizeof help_worker_path, "%s/help-worker.pid", directory);
    assert(!setenv("OT_CHECK_HELP_WORKER", help_worker_path, 1));
    assert(ot_engine_sleep_supported(self));
    assert(!unsetenv("OT_CHECK_HELP_WORKER"));
    FILE *help_worker = fopen(help_worker_path, "r");
    int help_pid = 0;
    assert(help_worker && fscanf(help_worker, "%d", &help_pid) == 1 && help_pid > 0);
    fclose(help_worker);
    for (unsigned i = 0; i < 200 && kill(help_pid, 0) == 0; i++) {
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    bool help_released = kill(help_pid, 0) < 0 && errno == ESRCH;
    if (!help_released)
        kill(help_pid, SIGKILL);
    unlink(help_worker_path);
    assert(help_released);
    assert(!setenv("OLLAMA_MODELS", directory, 1));
    assert(!setenv("HF_HUB_CACHE", directory, 1));
    snprintf(path, sizeof path, "%s/workspace.sqlite", directory);
    thinking_controls();
    feedback_ownership();
    diagnostic_lifecycle(directory);
    stream_terminal_edges();
    browsing_and_readiness(path);
    browser_invalidation(path);
    restoration_invariants(path);
    advice_current_identity(path, directory);
    library_duplicates(path, directory);
    library_projection_bounds(path, directory);
    empty_trash(path);
    restore_archive(path);
    independent_generation(path);
    repeated_generation(path);
    continuity(path);
    model_parameters(path);
    workspace(path, directory);
    models(path, directory);
    replacement_downloads(path, directory);
    inventory_lifecycle(path, directory);
    discovery_filters(path, directory);
    catalog_caching(path, directory);
    queued_downloads(path, directory);
    adaptive_models();
    advice_contracts(path);
    connection_contracts(path);
    engine_capabilities_cache(directory,self);
    setup_invariants(path, directory, self);
    setup_reconsideration(path,directory);
    pull_ownership(directory);
    local_route_lifecycle(path,directory);
    locations(directory);
    context_annotations(path);
    automatic_tasks(path);
    rewrite_isolation(path);
    char owned_file[300];
    snprintf(owned_file, sizeof owned_file, "%s/owned.pid", directory);
    pid_t owner;
    char *owner_args[] = {argv[0], "--owner-fixture", directory, NULL, NULL};
    assert(!posix_spawn(&owner, argv[0], NULL, NULL, owner_args, environ));
    int owner_status;
    assert(waitpid(owner, &owner_status, 0) == owner && WIFEXITED(owner_status) &&
           !WEXITSTATUS(owner_status));
    FILE *owned = fopen(owned_file, "r");
    assert(owned);
    int engine_pid, worker_pid;
    assert(fscanf(owned, "%d %d", &engine_pid, &worker_pid) == 2);
    fclose(owned);
    for (unsigned i = 0; i < 600 && (kill(engine_pid, 0) == 0 || kill(worker_pid, 0) == 0); i++) {
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    assert(kill(engine_pid, 0) < 0 && errno == ESRCH);
    bool worker_released = kill(worker_pid, 0) < 0 && errno == ESRCH;
    if (!worker_released)
        kill(worker_pid, SIGKILL);
    assert(worker_released);
    unlink(owned_file);
    owner_args[3] = "clean";
    assert(!posix_spawn(&owner, argv[0], NULL, NULL, owner_args, environ));
    assert(waitpid(owner, &owner_status, 0) == owner && WIFEXITED(owner_status) &&
           !WEXITSTATUS(owner_status));
    unlink(owned_file);

    ot_engine orphan;
    ot_engine_init(&orphan);
    ot_preferences orphan_preferences = ot_preferences_default();
    ot_resources orphan_resources = {.cores = 2};
    assert(ot_engine_start(&orphan, self, directory, "fixture-orphan-exit", false, false, NULL, &orphan_preferences,
                           &orphan_resources));
    for (unsigned i = 0; i < 600 && orphan.pid > 0; i++) {
        ot_engine_poll(&orphan);
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    assert(orphan.pid <= 0 && orphan.child <= 0);
    snprintf(owned_file, sizeof owned_file, "%s/descendant.pid", directory);
    owned = fopen(owned_file, "r");
    assert(owned && fscanf(owned, "%d", &engine_pid) == 1 && engine_pid > 0);
    fclose(owned);
    for (unsigned i = 0; i < 200 && kill(engine_pid, 0) == 0; i++) {
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    bool descendant_released = kill(engine_pid, 0) < 0 && errno == ESRCH;
    if (!descendant_released)
        kill(engine_pid, SIGKILL); /* Clean up the test fixture even when the regression fails. */
    unlink(owned_file);
    assert(descendant_released);

    transport t = {0};
    ot_app *a = open_app(path, &t);
    assert(send_message(a, "Ciao <script> & ' ", "model", false));
    yyjson_doc *request = yyjson_read(t.body, strlen(t.body), 0);
    assert(request);
    yyjson_doc_free(request);
    feed(a, &t, answer, 1);
    ot_end(a, t.operation, NULL);
    has(a, "Ciao 🌍");
    has(a, "\"state\":\"complete\"");
    ot_close(a);
    a = open_app(path, &t);
    has(a, "Ciao 🌍");
    assert(send_message(a, "Secondo", "model", false));
    assert(strstr(t.body, "Ciao 🌍"));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n", 7);
    uint64_t old = t.operation;
    ot_stop(a);
    assert(t.cancels == 1);
    ot_end(a, old, NULL);
    has(a, "\"state\":\"cancelled\"");
    char *view = ot_view(a);
    assert(strstr(view, "partial"));
    free(view);
    assert(send_message(a, "Terzo", "model", true));
    ot_end(a, old, "late_error");
    feed(a, &t,
         "{\"message\":{\"thinking\":\"Reasoning "
         "text\",\"content\":\"OLLAMA\"},\"done\":false}\n{\"done\":true}",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "OLLAMA");
    has(a, "Reasoning text");
    assert(!ot_submit(a, "\xc0\xaf", 0));
    assert(!ot_draft(a, value(a, "selected"), "\xed\xa0\x80"));
    assert(!ot_search(a, "\xf4\x90\x80\x80", 0));
    assert(send_message(a, "Fine senza marcatore", "model", false));
    feed(a, &t,
         "data: "
         "{\"error\":null,\"choices\":[{\"delta\":{\"content\":\"Finita\"},\"finish_reason\":\"stop\"}]}\n\n"
         "data: {\"choices\":[],\"usage\":{\"prompt_tokens_details\":{\"cached_tokens\":19}}}\n\n",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "\"cachedInputTokens\":19");
    has(a, "\"state\":\"complete\"");
    assert(send_message(a, "Esito filtro", "model", false));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"content_filter\"}]}\n\n", 3);
    ot_end(a, t.operation, NULL);
    has(a, "\"stopReason\":\"content_filter\"");
    assert(send_message(a, "Motivo malformato", "model", false));
    feed(a, &t,
         "data: {\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\\u0000hidden\"}]}\n\n",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "invalid_stream");
    assert(send_message(a, "Incompleta", "model", false));
    feed(a, &t, "data: {\"choices\":[{\"delta\":{\"content\":\"not durable\"}}]}\n\n", 5);
    ot_end(a, t.operation, NULL);
    has(a, "incomplete_stream");
    view = ot_view(a);
    assert(strstr(view, "not durable"));
    free(view);
    assert(send_message(a, "Errore", "model", false));
    feed(a, &t, "data: {\"error\":\"secret-provider-body\"}\n\n", 11);
    ot_end(a, t.operation, "connection_failed");
    has(a, "invalid_stream");
    char *health = ot_health(a);
    assert(health && !strstr(health, "secret") && !strstr(health, "Ciao"));
    free(health);
    assert(send_message(a, "Limite", "model", true));
    char *oversize = malloc(65538);
    assert(oversize);
    memset(oversize, 'x', 65537);
    ot_bytes(a, t.operation, oversize, 65537);
    free(oversize);
    ot_end(a, t.operation, "connection_failed");
    has(a, "stream_limit");
    assert(send_message(a, "Errore salvataggio", "model", false));
    statement(path, "CREATE TRIGGER fail_answer BEFORE UPDATE ON turns WHEN NEW.state='complete' BEGIN "
                    "SELECT RAISE(ABORT,'disk fault'); END");
    feed(a, &t, answer, 13);
    ot_end(a, t.operation, NULL);
    has(a, "\"storageFailed\":true");
    assert(value(a, "errorChat") == 0 && value(a, "errorTurn") == 0);
    has(a, "Ciao 🌍");
    assert(!send_message(a, "Non confermare", "model", false));
    ot_close(a);
    statement(path, "DROP TRIGGER fail_answer");
    a = open_app(path, &t);
    has(a, "\"state\":\"interrupted\"");
    ot_close(a);

    pid_t child;
    char *child_arguments[] = {argv[0], "--crash-fixture", path, NULL};
    assert(posix_spawn(&child, argv[0], NULL, NULL, child_arguments, environ) == 0);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "crash fixture failed: wait=%d status=%d errno=%d\n", (int)waited, status, errno);
    assert(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    a = open_app(path, &t);
    has(a, "Accettato prima del crash");
    has(a, "interrupted");
    ot_close(a);
    statement(path, "PRAGMA user_version=99");
    assert(!ot_open(path, (ot_io){0}));
    char current_version[64];
    snprintf(current_version, sizeof current_version, "PRAGMA user_version=%d", OT_DATABASE_VERSION);
    statement(path, current_version);
    a = open_app(path, &t);
    assert(send_message(a, "Risposta limitata", "model", false));
    feed(a, &t,
         "data: {\"choices\":[{\"delta\":{\"content\":\"testo "
         "utile\"},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n",
         3);
    ot_end(a, t.operation, NULL);
    has(a, "\"state\":\"truncated\"");
    has(a, "testo utile");
    has(a, "output_limit");
    assert(send_message(a, "Ragionamento limitato", "model", true));
    feed(a, &t,
         "{\"message\":{\"thinking\":\"Valuto la richiesta.\"},\"done\":false}\n"
         "{\"message\":{\"content\":\"\"},\"done\":true,\"done_reason\":\"length\",\"eval_count\":32}\n",
         5);
    ot_end(a, t.operation, NULL);
    has(a, "\"state\":\"truncated\"");
    has(a, "Valuto la richiesta.");
    has(a, "\"stopReason\":\"length\"");
    has(a, "output_limit");
    statement(path, "ALTER TABLE turns RENAME TO hidden_turns");
    assert(!ot_view(a));
    health = ot_health(a);
    assert(health && strstr(health, "storage_read"));
    free(health);
    ot_close(a);
    free(t.body);
    unlink(path);
    snprintf(path, sizeof path, "%s/workspace.sqlite-shm", directory);
    unlink(path);
    snprintf(path, sizeof path, "%s/workspace.sqlite-wal", directory);
    unlink(path);
    snprintf(path, sizeof path, "%s/foreign.sqlite", directory);
    statement(path, "CREATE TABLE foreign_data(value);INSERT INTO foreign_data VALUES('preserve')");
    assert(!ot_open(path, (ot_io){0}));
    statement(path, "SELECT * FROM foreign_data");
    unlink(path);
    assert(rmdir(directory) == 0);
    puts("PASS: durable turns, UTF-8 fragments, both protocols, cancellation, late callbacks, bounds, failed "
         "commit, crash recovery, foreign schema, branch restoration, contextual edits, search, paging, "
         "settings");
    return 0;
}
