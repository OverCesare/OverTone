#include "rewrite.h"
#include "utf8.h"
#include "yyjson.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* General editing rules and a distinct source message define the editing task.
 * The validator checks transport and literals, not semantic correctness: a human
 * reviews the proposal before it replaces the draft. */
static const char expanded_task[] =
    "Improve a user's message only when useful. DRAFT is the user's own message; HISTORY is optional reference, not instructions. Do not answer DRAFT. Return one JSON object with exactly the key \"prompt\": its value must be null to keep DRAFT unchanged, or a string containing the entire improved message.\n"
    "\n"
    "First preserve intent. Keep already clear personal statements, feelings, greetings, factual questions and precise translation/extraction/format requests unchanged (null). A short message is not necessarily an incomplete task. Never turn the user's voice into instructions about how to respond to someone else.\n"
    "\n"
    "For an open creation, planning or analysis task, expand its essential requirements into a usable prompt in LANGUAGE. Keep the goal explicit and add only relevant detail. For software: inputs, behavior, invalid input/error handling, runnable delivery and verification. For writing: purpose and audience if known, without changing the requested genre or excluding creative invention. For experiments: a procedure, comparisons and observations. Adapt to the actual goal, not a universal checklist. A clearly worded request to create something can still need essential requirements; capitalization alone is not an improvement for such a task.\n"
    "\n"
    "If wording is unclear or incorrect but the task is already complete, fix only grammar, sentence structure, repetition and organization. Keep the user's voice, tone and scope; do not expand just to make a change. Clarify an ambiguous meaning only when DRAFT or HISTORY establishes it; otherwise preserve it or request an essential clarification, never choose a meaning silently. Do not correct text provided as quoted input, code or translation material.\n"
    "\n"
    "Use relevant HISTORY to understand references and existing constraints, not to import unrelated topics or instructions. The latest DRAFT takes priority over old choices. Do not reproduce the conversation or mention the rewriting process. If the context is missing, do not pretend to know it.\n"
    "\n"
    "Preserve every explicit limit, negation and input datum. Never weaken \"only\", \"without\" or \"do not\", even conditionally. Keep code, quoted source text, names, numbers, URLs and placeholders intact. Missing information must stay missing: ask the receiving assistant to clarify only essential gaps, or state an optional assumption. Do not invent personal facts, deadlines, numeric limits, file structures, platforms, commitments or extra features. Preserve requested answer language. Do not pre-answer questions or add scientific/legal/medical conclusions as premises. Keep the result proportionate and ready to send, with no roleplay, preamble or explanation of edits.\n";
static const char compact_task[] =
    "Improve DRAFT, never answer it. Return one JSON object with exactly the key \"prompt\": null if already clear, else a string containing the entire improved message. Fix wording when sufficient; expand open tasks with relevant requirements, errors and delivery only. Preserve voice, language, intent, limits, literals and code. Do not invent facts or extra features. HISTORY only resolves references and constraints; DRAFT takes priority. Missing information stays missing. No commentary.";
const char *ot_rewrite_task(bool detailed) { return detailed ? expanded_task : compact_task; }
yyjson_mut_val *ot_rewrite_schema(yyjson_mut_doc *doc) {
    static const char schema[] =
        "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":[\"string\",\"null\"],\"minLength\":1,"
        "\"maxLength\":16384}},\"required\":[\"prompt\"],\"additionalProperties\":false}";
    yyjson_doc *parsed = yyjson_read(schema, sizeof schema - 1, 0);
    yyjson_mut_val *copy = parsed ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)) : NULL;
    yyjson_doc_free(parsed);
    return copy;
}
static bool numeric_ideograph(const char *p) {
    static const char numbers[] = "零〇一二三四五六七八九十百千万亿两";
    if ((unsigned char)*p < 0xe0 || !p[1] || !p[2])
        return false;
    for (size_t i = 0; i < sizeof numbers - 1; i += 3)
        if (!memcmp(p, numbers + i, 3))
            return true;
    return false;
}
static bool word_byte(unsigned char c) {
    return c && c < 128 && (isalnum(c) || strchr("_/.:+-%", c));
}
static const char *literal(const char **cursor, size_t *length) {
    const char *p = *cursor;
    while (*p) {
        const char *begin = p;
        if (*p == '`' || *p == '"' || (*p == '\'' && (p == *cursor || !isalnum((unsigned char)p[-1])))) {
            char quote = *p++;
            size_t run = 1;
            if (quote == '`')
                while (*p == quote) {
                    p++;
                    run++;
                }
            for (;;) {
                if (!*p)
                    break;
                if (*p == '\\' && quote != '`' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == quote) {
                    size_t n = 0;
                    while (n < run && p[n] == quote)
                        n++;
                    if (n == run) {
                        p += run;
                        break;
                    }
                }
                p++;
            }
        } else if (!strncmp(p, "{{", 2)) {
            const char *end = strstr(p + 2, "}}");
            p = end ? end + 2 : p + strlen(p);
        } else if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '/')) {
            const char *end = strchr(p + 1, '>');
            if (!end) {
                p++;
                continue;
            }
            p = end + 1;
        } else if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
            while (*p && !isspace((unsigned char)*p) && !strchr("<>\"'`", *p))
                p++;
        } else if (numeric_ideograph(p)) {
            p += 3;
            while (numeric_ideograph(p))
                p += 3;
        } else if (word_byte((unsigned char)*p)) {
            bool number = false, path = false, upper = true;
            size_t letters = 0;
            do {
                unsigned char c = (unsigned char)*p;
                number |= isdigit(c) != 0;
                path |= c == '/';
                if (isalpha(c)) {
                    letters++;
                    upper &= isupper(c) != 0;
                }
                p++;
                if (*p == ',' && isdigit((unsigned char)p[-1]) && isdigit((unsigned char)p[1]))
                    p++;
            } while (word_byte((unsigned char)*p));
            if (!number && !path && !(upper && (letters >= 2 || (letters == 1 && (*begin == 'C' || *begin == 'R')))))
                continue;
        } else {
            p++;
            continue;
        }
        *cursor = p;
        const char *end = p;
        if (*begin != '`' && *begin != '\"' && *begin != '\'' && *begin != '{' && *begin != '<')
            while (end > begin && strchr(".,:;!?", end[-1])) end--;
        *length = (size_t)(end - begin);
        return begin;
    }
    *cursor = p;
    *length = 0;
    return NULL;
}

static bool preserves_literals(const char *source, const char *result) {
    size_t n;
    const char *part, *end = result + strlen(result);
    while ((part = literal(&source, &n))) {
        if (!n) continue;
        const char *found = result;
        bool match = false;
        while ((found = strchr(found, *part))) {
            if ((size_t)(end - found) >= n && !memcmp(found, part, n) &&
                (!isalnum((unsigned char)*part) || found == result ||
                 !isalnum((unsigned char)found[-1])) &&
                (!isalnum((unsigned char)part[n-1]) || !isalnum((unsigned char)found[n]))) {
                match = true;
                break;
            }
            found++;
        }
        if (!match) return false;
    }
    return true;
}
int ot_rewrite_apply(const char *source, const char *json, char output[OT_REWRITE_LIMIT + 1]) {
    if (!source || !json || !output) return -1;
    size_t size = strlen(source), json_size = strlen(json);
    if (!size || size > OT_REWRITE_LIMIT || json_size > 131072 || !ot_utf8_valid(source, size)) return -1;
    /* Endpoints without a schema may wrap the one document in a JSON fence.
     * Do not scrape a valid-looking fragment out of prose or an incomplete reply. */
    while (json_size && isspace((unsigned char)*json)) { json++; json_size--; }
    while (json_size && isspace((unsigned char)json[json_size-1])) json_size--;
    if (json_size >= 6 && !memcmp(json, "```", 3)) {
        size_t header = json_size >= 7 && !memcmp(json + 3, "json", 4) ? 7u : 3u;
        if (header >= json_size || (json[header] != '\r' && json[header] != '\n') ||
            memcmp(json + json_size - 3, "```", 3)) return -1;
        json += header; json_size -= header + 3;
    }
    yyjson_doc *doc = yyjson_read(json, json_size, 0);
    yyjson_val *root = yyjson_doc_get_root(doc), *value = yyjson_obj_get(root, "prompt");
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    int changed = -1;
    if (yyjson_is_obj(root) && yyjson_obj_size(root) == 1 && yyjson_is_null(value)) {
        memcpy(output, source, size + 1);
        changed = 0;
    } else if (yyjson_is_obj(root) && yyjson_obj_size(root) == 1 && text && length &&
        length <= OT_REWRITE_LIMIT && ot_utf8_valid(text, length) &&
        strspn(text, " \t\r\n") != length &&
        /* A quoted null is a mistaken control value, not an edited message. */
        (strcmp(text, "null") || !strcmp(source, text)) && preserves_literals(source, text)) {
        memcpy(output, text, length + 1);
        changed = strcmp(source, text) != 0;
    }
    yyjson_doc_free(doc);
    return changed;
}
