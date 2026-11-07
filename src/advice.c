#include "recommendations.h"
#include "yyjson.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

unsigned ot_advice_language(const char *language) {
    static const char *const codes[] = {"it", "en", "es", "fr", "de", "pt", "ru", "zh", "ja", "ko"};
    for (unsigned i = 0; language && i < OT_ADVICE_LANGUAGES; i++)
        if (!strncasecmp(language, codes[i], 2) && (!language[2] || language[2] == '-'))
            return 1u << i;
    return 0;
}
static bool term(const char *text, const char *word) {
    size_t n = strlen(word);
    for (const char *p = text; *p; p++)
        if ((p == text || !isalnum((unsigned char)p[-1])) && !strncasecmp(p, word, n) &&
            !isalnum((unsigned char)p[n]))
            return true;
    return false;
}
void ot_advice_request(ot_advice_profile *out, const char *prompt, const char *language) {
    *out = (ot_advice_profile){.samples = 1};
    unsigned lang = ot_advice_language(language);
    for (unsigned i = 0; i < OT_ADVICE_LANGUAGES; i++)
        out->languages[i] = (lang & (1u << i)) ? 1 : 0;
    /* Intent signals are deliberately broad and weak. They describe a task, never
     * satisfaction, profession, or the correctness of an answer. No text is retained. */
    char text[4097];
    size_t size = prompt ? strlen(prompt) : 0, n = size < sizeof text - 1 ? size : sizeof text - 1;
    memcpy(text, prompt ? prompt : "", n);
    text[n] = 0;
    out->input_bytes = (double)size;
    out->long_inputs = size > 4096;
    static const char *const words[OT_ADVICE_TASKS][24] = {
        {NULL},
        {"code",   "codice", "program", "programma",    "debug",    "function", "funzione", "sql",
         "python", "rust",   "compile", "código",       "programm", "fonction", "код",      "программ",
         "代码",   "编程",   "コード",  "コードを書く", "코드",     "함수",     NULL},
        {"source",     "fonti",     "research",     "ricerca",  "compare",   "confronta",
         "document",   "documento", "cite",         "analyse",  "recherche", "forschen",
         "investigar", "pesquisa",  "исследование", "источник", "研究",      "比较",
         "調査",       "比較",      "연구",         "비교",     NULL},
        {"rewrite",  "riscrivi",   "translate", "traduci",  "writing", "scrivi",   "summarize", "riassumi",
         "traduire", "übersetzen", "traducir",  "traduzir", "resumir", "переведи", "напиши",    "翻译",
         "总结",     "翻訳",       "要約",      "번역",     "요약",    NULL},
        {"calculate", "calcola",       "math",        "equation", "equazione", "invoice",
         "fattura",   "accounting",    "contabilità", "prozent",  "berechnen", "calculer",
         "calcular",  "contabilidade", "счёт",        "вычисли",  "计算",      "会计",
         "計算",      "会計",          "계산",        "회계",     NULL},
        {"legal",    "legale", "law",     "legge",    "contratto", "contract", "juridique",
         "jurídico", "recht",  "vertrag", "contrato", "contrat",   "договор",  "закон",
         "法律",     "合同",   "契約",    "법률",     "계약",      NULL}};
    double sum = 0;
    for (unsigned task = 1; task < OT_ADVICE_TASKS; task++) {
        unsigned matches = 0;
        for (unsigned i = 0; words[task][i]; i++)
            if (term(text, words[task][i]))
                matches++;
        if (task == 1 && (strstr(text, "```") || strstr(text, "#include") || strstr(text, "def ")))
            matches += 2;
        out->tasks[task] = matches ? fmin(2, (double)matches) : 0;
        sum += out->tasks[task];
    }
    if (sum <= 0)
        out->tasks[0] = 1;
    else
        for (unsigned i = 1; i < OT_ADVICE_TASKS; i++)
            out->tasks[i] /= sum;
}
void ot_advice_environment(ot_advice_profile *out, const ot_preferences *p, const ot_resources *r) {
    snprintf(out->host, sizeof out->host, "%s", r->cpu_name);
    out->context = p->context;
    int n = snprintf(out->execution, sizeof out->execution, "%s/%llu/%llu/%u/%u/%d/%u/%u/%s/%s",
                     r->system_build, (unsigned long long)r->memory_total,
                     (unsigned long long)r->gpu_recommended, r->cores, ot_thread_budget(r, p), p->gpu_layers,
                     p->memory_percent, r->unified, p->thinking_mode, p->keep_alive);
    if (n < 0 || (size_t)n >= sizeof out->execution)
        out->execution[0] = 0;
}
void ot_advice_merge(ot_advice_profile *to, const ot_advice_profile *from, double weight) {
    if (!isfinite(weight) || weight <= 0 || weight > 1)
        return;
    for (unsigned i = 0; i < OT_ADVICE_TASKS; i++)
        to->tasks[i] += from->tasks[i] * weight;
    for (unsigned i = 0; i < OT_ADVICE_LANGUAGES; i++)
        to->languages[i] += from->languages[i] * weight;
    to->input_bytes += from->input_bytes * weight;
    to->output_tokens += from->output_tokens * weight;
    to->first_token_ms += from->first_token_ms * weight;
    to->tokens_per_second += from->tokens_per_second * weight;
    to->samples += from->samples;
    to->long_inputs += from->long_inputs * weight;
    to->limited += from->limited * weight;
    to->reasoning += from->reasoning * weight;
    to->failures += from->failures * weight;
}
char *ot_advice_encode(const ot_advice_profile *p) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_obj(d), *tasks = yyjson_mut_arr(d), *langs = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, v);
    for (unsigned i = 0; i < OT_ADVICE_TASKS; i++)
        yyjson_mut_arr_add_real(d, tasks, p->tasks[i]);
    for (unsigned i = 0; i < OT_ADVICE_LANGUAGES; i++)
        yyjson_mut_arr_add_real(d, langs, p->languages[i]);
    yyjson_mut_obj_add_val(d, v, "tasks", tasks);
    yyjson_mut_obj_add_val(d, v, "languages", langs);
    yyjson_mut_obj_add_real(d, v, "input", p->input_bytes);
    yyjson_mut_obj_add_real(d, v, "output", p->output_tokens);
    yyjson_mut_obj_add_real(d, v, "first", p->first_token_ms);
    yyjson_mut_obj_add_real(d, v, "speed", p->tokens_per_second);
    yyjson_mut_obj_add_uint(d, v, "samples", p->samples);
    yyjson_mut_obj_add_real(d, v, "long", p->long_inputs);
    yyjson_mut_obj_add_real(d, v, "limited", p->limited);
    yyjson_mut_obj_add_real(d, v, "reasoning", p->reasoning);
    yyjson_mut_obj_add_real(d, v, "failures", p->failures);
    yyjson_mut_obj_add_strcpy(d, v, "identity", p->identity);
    yyjson_mut_obj_add_strcpy(d, v, "host", p->host);
    yyjson_mut_obj_add_strcpy(d, v, "execution", p->execution);
    yyjson_mut_obj_add_uint(d, v, "context", p->context);
    yyjson_mut_obj_add_uint(d, v, "runtime", p->runtime);
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}
bool ot_advice_decode(ot_advice_profile *out, const char *json) {
    if (!out || !json || strlen(json) > 4096)
        return false;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *v = yyjson_doc_get_root(d), *tasks = yyjson_obj_get(v, "tasks"),
               *languages = yyjson_obj_get(v, "languages");
    ot_advice_profile p = {0};
    bool ok = yyjson_is_obj(v) && yyjson_arr_size(tasks) == OT_ADVICE_TASKS &&
              yyjson_arr_size(languages) == OT_ADVICE_LANGUAGES;
    for (unsigned i = 0; ok && i < OT_ADVICE_TASKS + OT_ADVICE_LANGUAGES; i++) {
        yyjson_val *x = yyjson_arr_get(i < OT_ADVICE_TASKS ? tasks : languages,
                                       i < OT_ADVICE_TASKS ? i : i - OT_ADVICE_TASKS);
        double n = yyjson_get_num(x);
        ok = yyjson_is_num(x) && isfinite(n) && n >= 0 && n <= 1;
        if (i < OT_ADVICE_TASKS)
            p.tasks[i] = n;
        else
            p.languages[i - OT_ADVICE_TASKS] = n;
    }
    const char *const fields[] = {"input", "output", "first", "speed"};
    double *values[] = {&p.input_bytes, &p.output_tokens, &p.first_token_ms, &p.tokens_per_second};
    const double maximum[] = {16384, 1000000, 86400000, 1000000};
    for (unsigned i = 0; ok && i < 4; i++) {
        yyjson_val *x = yyjson_obj_get(v, fields[i]);
        *values[i] = yyjson_get_num(x);
        ok = yyjson_is_num(x) && isfinite(*values[i]) && *values[i] >= 0 && *values[i] <= maximum[i];
    }
    const char *const counts[] = {"long", "limited", "reasoning", "failures"};
    double *numbers[] = {&p.long_inputs, &p.limited, &p.reasoning, &p.failures};
    for (unsigned i = 0; ok && i < 4; i++) {
        yyjson_val *x = yyjson_obj_get(v, counts[i]);
        *numbers[i] = yyjson_get_num(x);
        ok = yyjson_is_num(x) && isfinite(*numbers[i]) && *numbers[i] >= 0 && *numbers[i] <= 1;
    }
    p.samples = (unsigned)yyjson_get_uint(yyjson_obj_get(v, "samples"));
    double task_total = 0, language_total = 0;
    for (unsigned i = 0; i < OT_ADVICE_TASKS; i++)
        task_total += p.tasks[i];
    for (unsigned i = 0; i < OT_ADVICE_LANGUAGES; i++)
        language_total += p.languages[i];
    ok = ok && p.samples == 1 && fabs(task_total - 1) < 1e-6 && language_total <= 1.000001;
    const char *id = yyjson_get_str(yyjson_obj_get(v, "identity")),
               *host = yyjson_get_str(yyjson_obj_get(v, "host")),
               *execution = yyjson_get_str(yyjson_obj_get(v, "execution"));
    uint64_t context = yyjson_get_uint(yyjson_obj_get(v, "context")),
             runtime = yyjson_get_uint(yyjson_obj_get(v, "runtime"));
    ok = ok && id && host && execution && strlen(execution) < sizeof p.execution &&
         strlen(execution) == yyjson_get_len(yyjson_obj_get(v, "execution")) &&
         strlen(id) == yyjson_get_len(yyjson_obj_get(v, "identity")) &&
         strlen(host) == yyjson_get_len(yyjson_obj_get(v, "host")) &&
         (!*id || (strlen(id) == 64 && strspn(id, "0123456789abcdefABCDEF") == 64)) &&
         strlen(host) < sizeof p.host && context <= 262144 && runtime <= 2;
    if (ok) {
        strcpy(p.identity, id);
        strcpy(p.host, host);
        strcpy(p.execution, execution);
        p.context = (unsigned)context;
        p.runtime = (unsigned)runtime;
    }
    if (ok)
        *out = p;
    yyjson_doc_free(d);
    return ok;
}

static bool same_metric(const ot_model_metric *a, const ot_model_metric *b) {
    return a->higher_better && b->higher_better && *a->source && *a->task &&
           !strcmp(a->benchmark, b->benchmark) && !strcmp(a->task, b->task) &&
           !strcmp(a->source, b->source) && !strcmp(a->configuration, b->configuration) &&
           !strcmp(a->revision, b->revision);
}
bool ot_model_candidate_dominates(const ot_model_candidate *a, const ot_model_candidate *b) {
    if (!a || !b || a->metric_count > OT_ADVICE_METRICS || b->metric_count > OT_ADVICE_METRICS || !a->bytes ||
        !b->bytes || a->bytes > b->bytes || a->context < b->context || (b->reasoning && !a->reasoning) ||
        (b->languages && (a->languages & b->languages) != b->languages) || a->tokens_per_second <= 0 ||
        b->tokens_per_second <= 0 || a->tokens_per_second < b->tokens_per_second || a->first_token_ms <= 0 ||
        b->first_token_ms <= 0 || a->first_token_ms > b->first_token_ms)
        return false;
    bool compared = false, better = a->bytes < b->bytes;
    for (unsigned i = 0; i < b->metric_count; i++) {
        const ot_model_metric *x = &b->metrics[i];
        const ot_model_metric *match = NULL;
        for (unsigned j = 0; j < a->metric_count; j++)
            if (same_metric(x, &a->metrics[j]) && x->verified && a->metrics[j].verified && x->exact &&
                a->metrics[j].exact)
                match = &a->metrics[j];
        if (!match)
            return false;
        if (match->value + 1e-6 < x->value)
            return false;
        compared = true;
        better |= match->value > x->value + 1e-6;
    }
    /* Unknown runtime speed cannot establish performance dominance. */
    if (!compared)
        return false;
    return better || a->tokens_per_second > b->tokens_per_second || a->first_token_ms < b->first_token_ms;
}
typedef struct {
    const ot_model_metric *metric;
    size_t model;
} metric_entry;
static int metric_order(const void *left, const void *right) {
    const ot_model_metric *a = ((const metric_entry *)left)->metric,
                          *b = ((const metric_entry *)right)->metric;
    int c = strcmp(a->benchmark, b->benchmark);
    if (!c)
        c = strcmp(a->task, b->task);
    if (!c)
        c = strcmp(a->source, b->source);
    if (!c)
        c = strcmp(a->configuration, b->configuration);
    if (!c)
        c = strcmp(a->revision, b->revision);
    return c;
}
void ot_model_evidence_scores(const ot_model_candidate *models, double *scores, size_t count) {
    if (!models || !scores || count > OT_ADVICE_MODELS + 32)
        return;
    double changes[OT_ADVICE_MODELS + 32] = {0}, comparisons[OT_ADVICE_MODELS + 32] = {0};
    metric_entry entries[(OT_ADVICE_MODELS + 32) * OT_ADVICE_METRICS];
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
        for (unsigned j = 0; scores[i] >= 0 && j < models[i].metric_count && j < OT_ADVICE_METRICS; j++) {
            const ot_model_metric *m = &models[i].metrics[j];
            if (m->higher_better && *m->source && *m->task && isfinite(m->value))
                entries[used++] = (metric_entry){m, i};
        }
    qsort(entries, used, sizeof *entries, metric_order);
    /* Compare only like-for-like benchmark, task and source. A base result may
     * inform a cautious prior, but cannot establish measured variant dominance. */
    for (size_t start = 0; start < used;) {
        size_t end = start + 1;
        while (end < used && !metric_order(&entries[start], &entries[end]))
            end++;
        for (size_t a = start; a < end; a++)
            for (size_t b = a + 1; b < end; b++) {
                size_t i = entries[a].model, j = entries[b].model;
                if (i == j)
                    continue;
                const ot_model_metric *x = entries[a].metric, *y = entries[b].metric;
                double trust = x->verified && y->verified ? 1 : .2;
                if (!x->exact || !y->exact)
                    trust *= .35;
                double scale = fmax(1e-9, fabs(x->value) + fabs(y->value));
                double margin = 8 * (x->value - y->value) / scale;
                double change = tanh(margin) * trust;
                changes[i] += change;
                changes[j] -= change;
                comparisons[i]++;
                comparisons[j]++;
            }
        start = end;
    }
    for (size_t i = 0; i < count; i++)
        if (scores[i] >= 0 && comparisons[i] > 0)
            scores[i] = fmax(.001, scores[i] + .22 * changes[i] / comparisons[i]);
}
double ot_model_candidate_rank(const ot_model_candidate *m, const ot_resources *r, const ot_preferences *p,
                               const ot_advice_profile *profile) {
    double base = ot_model_candidate_score(m, r, p);
    if (base < 0)
        return base;
    if (!profile || !profile->samples)
        return base;
    /* A declared purpose remains an anchor. Sparse or mixed implicit evidence
     * cannot abruptly replace it, and throughput is not treated as answer quality. */
    double learned = 0, total = 0;
    ot_preferences task = *p;
    for (unsigned i = 0; i < OT_ADVICE_TASKS; i++) {
        task.purpose = i;
        learned += ot_model_candidate_score(m, r, &task) * profile->tasks[i];
        total += profile->tasks[i];
    }
    double trust = fmin(.45, total / (total + 24));
    if (total > 0)
        base = base * (1 - trust) + learned / total * trust;
    double languages = 0, covered = 0;
    for (unsigned i = 0; i < OT_ADVICE_LANGUAGES; i++) {
        languages += profile->languages[i];
        if (m->languages & (1u << i))
            covered += profile->languages[i];
    }
    if (languages > 0 && m->languages)
        base += .08 * covered / languages * fmin(1, total / 8);
    if (m->context && profile->long_inputs > 2 && m->context < 16384)
        base -= .12;
    if (m->context && profile->limited > 2 && m->context < p->context)
        base -= .12;
    if (total > 3 && m->context) {
        double needed = profile->input_bytes / total / 2 + profile->output_tokens / total * 2 + 2048;
        if ((double)m->context < needed)
            base -= .12;
    }
    if (total > 3 && profile->reasoning > total / 2 && m->reasoning)
        base += .03;
    if (p->advice_goal == 1 && m->tokens_per_second > 0)
        base += .12 * m->tokens_per_second / (m->tokens_per_second + 40);
    base += .03 * (double)m->usage_count / ((double)m->usage_count + 8);
    return fmax(.001, base);
}

/* This is a recommendation margin, not a claim of measured answer quality. */
bool ot_model_candidate_upgrade(const ot_model_candidate *candidate, double score,
                                const ot_model_candidate *current, double current_score) {
    if (!candidate || !isfinite(score) || score < 0)
        return false;
    if (!current)
        return true;
    if (candidate == current ||
        (candidate->installed && candidate->installed == current->installed) ||
        (candidate->kind == current->kind && !strcmp(candidate->name, current->name) &&
         !candidate->update_of) || ot_model_candidate_dominates(current, candidate))
        return false;
    return isfinite(current_score) && (current_score < 0 || score >= current_score + .12);
}
