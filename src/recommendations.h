#ifndef OVERTONE_RECOMMENDATIONS_H
#define OVERTONE_RECOMMENDATIONS_H
#include "resources.h"
#include <stddef.h>
enum {
    OT_ADVICE_FAMILIES = 8,
    OT_ADVICE_MODELS = 192,
    OT_ADVICE_METRICS = 8,
    OT_ADVICE_TASKS = 6,
    OT_ADVICE_LANGUAGES = 10
};
typedef struct {
    char benchmark[129], task[65], source[129], configuration[129], revision[41];
    double value;
    bool verified, exact, higher_better;
} ot_model_metric;
typedef struct {
    char name[257], description[513], catalogue_digest[65];
    char repository[257], file[257], checksum[65], revision[41], license[129];
    uint64_t bytes, memory;
    unsigned context;
    int64_t installed, update_of;
    int kind;
    bool reasoning, approximate, verified;
    char base[257], quantization[33], library[33];
    unsigned languages, metric_count;
    unsigned usage_count;
    double parameters, bits, tokens_per_second, first_token_ms;
    ot_model_metric metrics[OT_ADVICE_METRICS];
    bool base_evidence;
} ot_model_candidate;
/* Aggregate needs, not an engagement/quality reward. No text or user identifiers. */
typedef struct {
    double tasks[OT_ADVICE_TASKS], languages[OT_ADVICE_LANGUAGES];
    double input_bytes, output_tokens, first_token_ms, tokens_per_second;
    unsigned samples;
    double long_inputs, limited, reasoning, failures;
    char identity[65], host[96], execution[193];
    unsigned context, runtime;
} ot_advice_profile;
void ot_advice_request(ot_advice_profile *out, const char *prompt, const char *language);
void ot_advice_environment(ot_advice_profile *out, const ot_preferences *preferences,
                           const ot_resources *resources);
void ot_advice_merge(ot_advice_profile *target, const ot_advice_profile *sample, double weight);
char *ot_advice_encode(const ot_advice_profile *sample);
bool ot_advice_decode(ot_advice_profile *out, const char *json);
unsigned ot_advice_language(const char *language);
void ot_model_evidence(ot_model_candidate *model, const char *json, bool base);
char *ot_model_catalogue_write(const ot_model_candidate *models, size_t count);
size_t ot_model_catalogue_read(const char *json, ot_model_candidate *models, size_t capacity);
bool ot_model_candidate_same_family(const ot_model_candidate *a, const ot_model_candidate *b);
bool ot_model_candidate_dominates(const ot_model_candidate *a, const ot_model_candidate *b);
/* A materially better next step; NULL current means no usable local reference. */
bool ot_model_candidate_upgrade(const ot_model_candidate *candidate, double score,
                                const ot_model_candidate *current, double current_score);
void ot_model_evidence_scores(const ot_model_candidate *models, double *scores, size_t count);
double ot_model_candidate_rank(const ot_model_candidate *model, const ot_resources *resources,
                               const ot_preferences *preferences, const ot_advice_profile *profile);
typedef struct yyjson_val yyjson_val;
void ot_model_hf_facts(ot_model_candidate *model, yyjson_val *metadata);
void ot_model_variant_facts(ot_model_candidate *model, const char *name);
/* File-role screening only; runtime metadata still decides actual compatibility. */
bool ot_model_primary_file(const char *name);
unsigned ot_model_purposes(const ot_model_candidate *model);
/* Pure bounded parsing/ranking. Providers supply candidates; qualification is independent. */
bool ot_model_name(const char *name);
char *ot_ollama_catalog(const char *html, const char *repository);
size_t ot_model_feed(const char *json, size_t bytes, ot_model_candidate *out, size_t capacity);
size_t ot_model_variants(const char *json, const ot_model_candidate *family, ot_model_candidate *out,
                         size_t capacity);
size_t ot_model_hf_feed(const char *json, ot_model_candidate *out, size_t capacity);
size_t ot_model_hf_variants(const char *json, const ot_model_candidate *family, ot_model_candidate *out,
                            size_t capacity);
bool ot_model_candidate_fits(const ot_model_candidate *model, const ot_resources *resources,
                             const ot_preferences *preferences);
double ot_model_candidate_score(const ot_model_candidate *model, const ot_resources *resources,
                                const ot_preferences *preferences);
bool ot_model_verified(const char *name, const char *digest);
bool ot_model_file_verified(const char *digest);
bool ot_model_verification_candidate(const char *name);
size_t ot_model_candidate_best(const ot_model_candidate *models, const double *scores, size_t count);
size_t ot_model_candidate_order(const ot_model_candidate *models, const double *scores, size_t count,
                                size_t *order);
#endif
