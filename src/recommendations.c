#include "recommendations.h"
#include "utf8.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <yyjson.h>

bool ot_model_name(const char *s) {
    if (!s)
        return false;
    size_t n = strlen(s);
    if (!n || n > 256)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || strchr("._-/:@", s[i])))
            return false;
    return !strstr(s, "..");
}

/* Extract only numeric facts inside the same bounded link. Catalog sizes are rounded;
 * they must never become an integrity check for the actual transfer. */
static void ollama_link_facts(yyjson_mut_doc *d, yyjson_mut_val *item, const char *link) {
    const char *end = strstr(link, "</a>");
    if (!end || end - link > 8192)
        return;
    const char *mono = strstr(link, "class=\"font-mono");
    if (mono && mono < end) {
        const char *value = strchr(mono, '>');
        if (value && value < end) {
            value++;
            while (value < end && isspace((unsigned char)*value))
                value++;
            const char *after = value;
            while (after < end && isxdigit((unsigned char)*after))
                after++;
            size_t length = (size_t)(after - value);
            if (length == 12 || length == 64)
                yyjson_mut_obj_add_strncpy(d, item, "catalogueDigest", value, length);
        }
    }
    for (const char *p = link; p < end; p++) {
        if (!isdigit((unsigned char)*p) || (p > link && (isalnum((unsigned char)p[-1]) || p[-1] == '.')))
            continue;
        char *unit;
        double number = strtod(p, &unit);
        if (!(number > 0 && number <= 1000000) || unit >= end)
            continue;
        uint64_t factor = !strncmp(unit, "GB", 2)   ? 1000000000
                          : !strncmp(unit, "MB", 2) ? 1000000
                          : !strncmp(unit, "TB", 2) ? 1000000000000
                                                    : 0;
        if (factor) {
            uint64_t bytes = (uint64_t)(number * (double)factor);
            uint64_t previous = yyjson_mut_get_uint(yyjson_mut_obj_get(item, "bytes"));
            if (bytes > previous) {
                yyjson_mut_obj_remove_key(item, "bytes");
                yyjson_mut_obj_add_uint(d, item, "bytes", bytes);
            }
            yyjson_mut_obj_remove_key(item, "sizeApproximate");
            yyjson_mut_obj_add_bool(d, item, "sizeApproximate", true);
        }
        if ((*unit == 'K' || *unit == 'M') && !strncmp(unit + 1, " context window", 15)) {
            yyjson_mut_obj_remove_key(item, "context");
            yyjson_mut_obj_add_uint(d, item, "context", (uint64_t)(number * (*unit == 'K' ? 1024 : 1048576)));
        }
        p = unit;
    }
}
static void ollama_description(yyjson_mut_doc *d, yyjson_mut_val *item, const char *link) {
    const char *end = strstr(link, "</a>"), *p = strstr(link, "<p");
    if (!end || end - link > 8192)
        return;
    for (const char **cap = (const char *[]){"thinking", "vision", "tools", "embedding", NULL}; *cap; cap++) {
        char token[32];
        snprintf(token, sizeof token, ">%s<", *cap);
        const char *at = strstr(link, token);
        if (at && at < end)
            yyjson_mut_obj_add_bool(d, item, !strcmp(*cap, "thinking") ? "reasoning" : *cap, true);
    }
    if (!p || p >= end || !(p = strchr(p, '>')) || p >= end)
        return;
    const char *stop = strstr(++p, "</p>");
    if (!stop || stop > end)
        return;
    char description[513];
    size_t used = 0;
    bool tag = false;
    while (p < stop && used < sizeof description - 1) {
        if (*p == '<')
            tag = true;
        else if (*p == '>')
            tag = false;
        else if (!tag)
            description[used++] = *p;
        p++;
    }
    while (used && !ot_utf8_valid(description, used))
        used--;
    description[used] = 0;
    yyjson_mut_obj_add_strcpy(d, item, "description", description);
}
/* Catalog pages are untrusted data. Only bounded, validated facts reach the renderer. */
char *ot_ollama_catalog(const char *html, const char *repository) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *items = yyjson_mut_arr(d);
    if (!items) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_doc_set_root(d, items);
    const char *p = html ? html : "";
    bool variants = repository && *repository;
    while ((p = strstr(p, "href=")) && yyjson_mut_arr_size(items) < 100) {
        p += 5;
        char quote = *p;
        if (quote != '\'' && quote != '"')
            continue;
        p++;
        const char *end = strchr(p, quote);
        if (!end)
            break;
        size_t size = (size_t)(end - p);
        char name[257];
        size_t prefix = size > 9 && !memcmp(p, "/library/", 9) ? 9 : 1;
        const char *link_end = strstr(end + 1, "</a>"), *heading = strstr(end + 1, "<h2");
        bool custom = prefix == 1 && size > 2 && p[0] == '/' && p[1] != '/' && memchr(p + 1, '/', size - 1) &&
                      (variants ? memchr(p, ':', size) != NULL
                                : link_end && heading && heading < link_end && link_end - end <= 8192);
        if ((prefix == 9 || custom) && size > prefix && size - prefix < sizeof name) {
            memcpy(name, p + prefix, size - prefix);
            name[size - prefix] = 0;
            const char *tag = strchr(name, ':');
            bool valid =
                ot_model_name(name) && (variants ? tag && (size_t)(tag - name) == strlen(repository) &&
                                                       !memcmp(name, repository, strlen(repository)) && tag[1]
                                                 : !tag && (prefix != 9 || !strchr(name, '/')));
            bool duplicate = false;
            for (size_t i = 0; valid && i < yyjson_mut_arr_size(items); i++) {
                const char *existing =
                    yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(items, i), "name"));
                if (existing && !strcmp(existing, name))
                    duplicate = true;
            }
            if (valid && !duplicate) {
                yyjson_mut_val *item = yyjson_mut_obj(d);
                if (!item || !yyjson_mut_obj_add_strcpy(d, item, "name", name) ||
                    !yyjson_mut_arr_append(items, item)) {
                    yyjson_mut_doc_free(d);
                    return NULL;
                }
                ollama_description(d, item, end + 1);
                if (variants) {
                    ollama_link_facts(d, item, end + 1);
                    size_t length = strlen(name);
                    yyjson_mut_obj_add_bool(d, item, "cloud",
                                            length >= 6 && (!strcmp(name + length - 6, "-cloud") ||
                                                            !strcmp(name + length - 6, ":cloud")));
                } else {
                    const char *close = strstr(end + 1, "</a>"), *embedding = strstr(end + 1, ">embedding<");
                    if (close && close - end <= 8192 && embedding && embedding < close)
                        yyjson_mut_obj_add_bool(d, item, "chatSupported", false);
                }
            }
        }
        p = end + 1;
    }
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}

static const char *text(yyjson_val *root, const char *key) {
    yyjson_val *value = yyjson_obj_get(root, key);
    const char *s = yyjson_get_str(value);
    return s && strlen(s) == yyjson_get_len(value) ? s : "";
}
static bool hex_digest(const char *s, size_t size) {
    if (strlen(s) != size)
        return false;
    for (size_t i = 0; i < size; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}
static bool hf_repository(const char *s) {
    const char *slash = strchr(s, '/');
    return ot_model_name(s) && slash && slash != s && slash[1] && !strchr(slash + 1, '/') &&
           !strchr(s, ':') && !strchr(s, '@');
}
void ot_model_variant_facts(ot_model_candidate *m, const char *name) {
    const char *const quants[] = {"IQ1", "IQ2",  "IQ3", "IQ4", "Q2",    "Q3",    "Q4",  "Q5",  "Q6",
                                  "Q8",  "BF16", "F16", "F32", "MXFP4", "NVFP4", "TQ1", "TQ2", "Q1"};
    for (unsigned q = 0; q < sizeof quants / sizeof *quants; q++) {
        const char *p = strcasestr(name, quants[q]);
        if (!p || (p != name && isalnum((unsigned char)p[-1])))
            continue;
        size_t n = strcspn(p, ".-/");
        if (n && n < sizeof m->quantization) {
            memcpy(m->quantization, p, n);
            m->quantization[n] = 0;
        }
        break;
    }
    if (m->parameters <= 0) {
        for (const char *p = name; *p; p++) {
            if (!isdigit((unsigned char)*p) || (p != name && isdigit((unsigned char)p[-1])))
                continue;
            char *end;
            double value = strtod(p, &end);
            if (value > 0 && value < 100000 && (*end == 'b' || *end == 'B')) {
                m->parameters = value * 1e9;
                break;
            }
        }
    }
    if (m->parameters > 0 && m->bytes)
        m->bits = (double)m->bytes * 8 / m->parameters;
}
static bool standard_runtime(const char *library) {
    return !*library || !strcmp(library, "transformers") || !strcmp(library, "llama.cpp") ||
           !strcmp(library, "gguf");
}
bool ot_model_primary_file(const char *name) {
    const char *file = strrchr(name, '/');
    file = file ? file + 1 : name;
    for (const char **role = (const char *[]){"mmproj", "projector", "adapter", "lora", "tokenizer", NULL};
         *role; role++) {
        for (const char *p = file; (p = strcasestr(p, *role)); p++) {
            size_t length = strlen(*role);
            if ((p == file || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[length]))
                return false;
        }
    }
    /* GGUF split naming: filename-00001-of-00002.gguf. A lone shard cannot load. */
    const char *split = strstr(file, "-of-");
    if (split && split - file >= 6 && split[-6] == '-' && strlen(split + 4) >= 5) {
        bool numbered = true;
        for (unsigned i = 0; i < 5; i++)
            numbered &= isdigit((unsigned char)split[-5 + (int)i]) && isdigit((unsigned char)split[4 + i]);
        if (numbered && !isalnum((unsigned char)split[9]))
            return false;
    }
    return true;
}
static bool standalone_variant(const ot_model_candidate *m) {
    const char *file = strrchr(m->file, '/');
    file = file ? file + 1 : m->file;
    return standard_runtime(m->library) && *m->quantization && strcasestr(file, m->quantization) &&
           ot_model_primary_file(file);
}
void ot_model_hf_facts(ot_model_candidate *m, yyjson_val *row) {
    const char *library = text(row, "library_name");
    if (*library)
        snprintf(m->library, sizeof m->library, "%s",
                 strlen(library) < sizeof m->library ? library : "unsupported");
    yyjson_val *gguf = yyjson_obj_get(row, "gguf"), *tags = yyjson_obj_get(row, "tags"), *tag;
    uint64_t context = yyjson_get_uint(yyjson_obj_get(gguf, "context_length"));
    if (context && context <= UINT32_MAX)
        m->context = (unsigned)context;
    uint64_t parameters = yyjson_get_uint(yyjson_obj_get(gguf, "total"));
    if (!parameters)
        parameters = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(row, "safetensors"), "total"));
    if (parameters && parameters < UINT64_C(100000000000000))
        m->parameters = (double)parameters;
    yyjson_val *card = yyjson_obj_get(row, "cardData"), *base = yyjson_obj_get(card, "base_model");
    const char *parent = yyjson_is_str(base) ? yyjson_get_str(base) : yyjson_get_str(yyjson_arr_get(base, 0));
    if (!*m->base && parent && hf_repository(parent))
        snprintf(m->base, sizeof m->base, "%s", parent);
    yyjson_val *langs = yyjson_obj_get(card, "language");
    if (yyjson_is_str(langs))
        m->languages |= ot_advice_language(yyjson_get_str(langs));
    for (size_t j = 0; j < yyjson_arr_size(langs) && j < 32; j++)
        m->languages |= ot_advice_language(yyjson_get_str(yyjson_arr_get(langs, j)));
    size_t used = strlen(m->description), i, n;
    yyjson_arr_foreach(tags, i, n, tag) {
        const char *s = yyjson_get_str(tag);
        if (!s || strlen(s) != yyjson_get_len(tag) || strlen(s) > 64)
            continue;
        if (!strcmp(s, "reasoning"))
            m->reasoning = true;
        if (used + strlen(s) + 2 < sizeof m->description)
            used += (size_t)snprintf(m->description + used, sizeof m->description - used, " %s", s);
    }
}
size_t ot_model_hf_feed(const char *json, ot_model_candidate *out, size_t capacity) {
    if (!json || strlen(json) > 1024 * 1024)
        return 0;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_doc_get_root(doc), *row;
    size_t count = 0, i, n;
    yyjson_arr_foreach(rows, i, n, row) {
        if (i >= 32 || count == capacity)
            break;
        const char *id = text(row, "id");
        uint64_t total = yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(row, "gguf"), "total"));
        if (!hf_repository(id) || !yyjson_is_false(yyjson_obj_get(row, "gated")) ||
            yyjson_is_true(yyjson_obj_get(row, "private")) ||
            (strcmp(text(row, "pipeline_tag"), "text-generation") &&
             strcmp(text(row, "pipeline_tag"), "image-text-to-text")) ||
            !standard_runtime(text(row, "library_name")) || !total || total > INT64_MAX / 2)
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < count; j++)
            if (!strcmp(out[j].repository, id))
                duplicate = true;
        if (duplicate)
            continue;
        ot_model_candidate *m = &out[count++];
        *m = (ot_model_candidate){.kind = 1, .approximate = true, .bytes = total * 3 / 5};
        snprintf(m->repository, sizeof m->repository, "%s", id);
        snprintf(m->name, sizeof m->name, "%s", id);
        snprintf(m->description, sizeof m->description, "%s", id);
        ot_model_hf_facts(m, row);
    }
    yyjson_doc_free(doc);
    return count;
}
size_t ot_model_hf_variants(const char *json, const ot_model_candidate *family, ot_model_candidate *out,
                            size_t capacity) {
    if (!json || !family || !capacity || strlen(json) > 1024 * 1024 || !hf_repository(family->repository))
        return 0;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc), *rows = yyjson_obj_get(root, "siblings"), *row;
    const char *revision = text(root, "sha"), *license = text(yyjson_obj_get(root, "cardData"), "license");
    size_t count = 0, i, n;
    bool usable = !strcmp(text(root, "id"), family->repository) && hex_digest(revision, 40) &&
                  yyjson_is_false(yyjson_obj_get(root, "gated")) &&
                  !yyjson_is_true(yyjson_obj_get(root, "private")) &&
                  *text(yyjson_obj_get(root, "gguf"), "chat_template");
    /* Every independent variant participates. The file list is never a quality
     * ranking, and quantization labels do not stand in for measured accuracy. */
    if (usable) {
        yyjson_arr_foreach(rows, i, n, row) {
            if (i >= 1000 || count == capacity)
                break;
            const char *file = text(row, "rfilename");
            size_t length = strlen(file);
            yyjson_val *lfs = yyjson_obj_get(row, "lfs");
            const char *hash = text(lfs, "sha256");
            if (!*hash)
                hash = text(lfs, "oid");
            uint64_t bytes = yyjson_get_uint(yyjson_obj_get(lfs, "size"));
            if (!ot_model_name(file) || length <= 5 || strcmp(file + length - 5, ".gguf") ||
                strstr(file, "-of-") || strcasestr(file, "mmproj") || strcasestr(file, "projector") ||
                !hex_digest(hash, 64) || !bytes || bytes > INT64_MAX)
                continue;
            ot_model_candidate *m = &out[count++];
            *m = *family;
            snprintf(m->file, sizeof m->file, "%s", file);
            const char *name = strrchr(file, '/');
            snprintf(m->name, sizeof m->name, "%s", name ? name + 1 : file);
            snprintf(m->revision, sizeof m->revision, "%s", revision);
            snprintf(m->checksum, sizeof m->checksum, "%s", hash);
            if (strlen(license) < sizeof m->license)
                snprintf(m->license, sizeof m->license, "%s", license);
            m->bytes = bytes;
            m->approximate = false;
            ot_model_hf_facts(m, root);
            ot_model_variant_facts(m, file);
            if (!standalone_variant(m))
                count--;
        }
    }
    yyjson_doc_free(doc);
    return count;
}
static bool cloud(const char *name) {
    size_t n = strlen(name);
    return n >= 6 && (!strcmp(name + n - 6, ":cloud") || !strcmp(name + n - 6, "-cloud"));
}
size_t ot_model_feed(const char *json, size_t bytes, ot_model_candidate *out, size_t capacity) {
    if (!json || bytes > 1024 * 1024)
        return 0;
    yyjson_doc *doc = yyjson_read(json, bytes, 0);
    yyjson_val *rows = yyjson_obj_get(yyjson_doc_get_root(doc), "recommendations");
    size_t count = 0, i, n;
    yyjson_val *row;
    yyjson_arr_foreach(rows, i, n, row) {
        if (i >= 128 || count == capacity)
            break;
        const char *name = text(row, "model"), *description = text(row, "description");
        if (!ot_model_name(name) || cloud(name) || strchr(name, '/') || *text(row, "required_plan") ||
            yyjson_get_uint(yyjson_obj_get(row, "vram_bytes")) > INT64_MAX)
            continue;
        const char *colon = strchr(name, ':');
        size_t family_length = colon ? (size_t)(colon - name) : strlen(name);
        bool duplicate = false;
        for (size_t j = 0; j < count; j++)
            if (!strncmp(out[j].name, name, family_length) &&
                (out[j].name[family_length] == ':' || !out[j].name[family_length]))
                duplicate = true;
        if (duplicate)
            continue;
        ot_model_candidate *m = &out[count++];
        *m = (ot_model_candidate){.kind = 2};
        snprintf(m->name, sizeof m->name, "%s", name);
        if (strlen(description) < sizeof m->description)
            snprintf(m->description, sizeof m->description, "%s", description);
        m->memory = yyjson_get_uint(yyjson_obj_get(row, "vram_bytes"));
        uint64_t context = yyjson_get_uint(yyjson_obj_get(row, "context_length"));
        m->context = context <= UINT32_MAX ? (unsigned)context : 0;
        yyjson_val *values = yyjson_obj_get(yyjson_obj_get(row, "thinking"), "values");
        m->reasoning = yyjson_is_arr(values) && yyjson_arr_size(values) > 1;
    }
    yyjson_doc_free(doc);
    return count;
}
static bool automatic_tag(const char *tag) {
    if (*tag == 'e')
        tag++;
    if (!isdigit((unsigned char)*tag))
        return false;
    char *end;
    double number = strtod(tag, &end);
    if (!isfinite(number) || number <= 0 || number > 1000000 || (*end != 'b' && *end != 'm'))
        return false;
    end++;
    if (!*end)
        return true;
    /* Numeric size tags may carry instruct, MoE and quantization variants. */
    return *end == '-' && end[1] && !strcasestr(end, "cloud") && !strcasestr(end, "embedding") &&
           !strcasestr(end, "-base") &&
           strspn(end, "-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.") == strlen(end);
}
size_t ot_model_variants(const char *json, const ot_model_candidate *family, ot_model_candidate *out,
                         size_t capacity) {
    if (!json || !family || !ot_model_name(family->name) || strlen(json) > 1024 * 1024)
        return 0;
    const char *family_tag = strchr(family->name, ':');
    size_t family_length = family_tag ? (size_t)(family_tag - family->name) : strlen(family->name);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_doc_get_root(doc), *row;
    size_t count = 0, i, n;
    for (unsigned pass = 0; pass < 2 && !count; pass++) {
        yyjson_arr_foreach(rows, i, n, row) {
            if (i >= 100 || count == capacity)
                break;
            const char *name = text(row, "name"), *tag = strchr(name, ':');
            uint64_t bytes = yyjson_get_uint(yyjson_obj_get(row, "bytes"));
            if (!ot_model_name(name) || !tag || (size_t)(tag - name) != family_length ||
                memcmp(name, family->name, family_length) || cloud(name) || !bytes || bytes > INT64_MAX ||
                yyjson_is_true(yyjson_obj_get(row, "cloud")) ||
                (pass == 0 ? !automatic_tag(tag + 1) : strcmp(tag + 1, "latest") != 0))
                continue;
            /* A catalogue hash prefix only collapses duplicate suggestion aliases.
             * It is never accepted as integrity or developer qualification. */
            const char *digest = text(row, "catalogueDigest");
            bool duplicate = false;
            for (size_t j = 0; *digest && j < count; j++)
                if (!strcmp(out[j].catalogue_digest, digest))
                    duplicate = true;
            if (duplicate)
                continue;
            ot_model_candidate *m = &out[count++];
            *m = *family;
            snprintf(m->name, sizeof m->name, "%s", name);
            m->bytes = bytes;
            ot_model_variant_facts(m, tag + 1);
            if (strlen(digest) < sizeof m->catalogue_digest)
                snprintf(m->catalogue_digest, sizeof m->catalogue_digest, "%s", digest);
            m->memory = !strcmp(name, family->name) ? family->memory : 0;
            m->approximate = true;
            uint64_t context = yyjson_get_uint(yyjson_obj_get(row, "context"));
            if (context && context <= UINT32_MAX)
                m->context = (unsigned)context;
        }
    }
    yyjson_doc_free(doc);
    return count;
}
bool ot_model_candidate_same_family(const ot_model_candidate *a, const ot_model_candidate *b) {
    if ((*a->base && !strcmp(a->base, b->base)) || (*a->repository && !strcmp(a->repository, b->repository)))
        return true;
    if (a->kind != 2 || b->kind != 2)
        return false;
    size_t length = strcspn(a->name, ":");
    return length && length == strcspn(b->name, ":") && !memcmp(a->name, b->name, length);
}
static uint64_t admission(const ot_model_candidate *m) {
    /* Unmeasured downloads need room beyond the weights. This is a placement
     * policy reserve, not a fabricated prediction of graph/KV allocation. */
    uint64_t reserve = UINT64_C(2) << 30;
    uint64_t bytes = m->bytes > UINT64_MAX - reserve ? UINT64_MAX : m->bytes + reserve;
    return m->memory > bytes ? m->memory : bytes;
}
bool ot_model_candidate_fits(const ot_model_candidate *m, const ot_resources *r, const ot_preferences *p) {
    if (!m || !r || !p)
        return false;
    uint64_t budget = ot_memory_budget(r, p);
    if (r->gpu && r->gpu_recommended && r->gpu_recommended < budget)
        budget = r->gpu_recommended;
    return m->bytes && (m->kind == 1 || m->kind == 2) && (p->setup_engines & (unsigned)m->kind) && r->gpu &&
           r->pressure < 2 && admission(m) <= budget &&
           (m->installed || !r->disk_available ||
            (m->bytes < r->disk_available && (UINT64_C(2) << 30) < r->disk_available - m->bytes));
}
static bool contains(const char *text_value, const char *word) {
    size_t length = strlen(word);
    for (const char *s = text_value; *s; s++)
        if ((s == text_value || !isalnum((unsigned char)s[-1])) && !strncasecmp(s, word, length) &&
            !isalnum((unsigned char)s[length]))
            return true;
    return false;
}
unsigned ot_model_purposes(const ot_model_candidate *m) {
    const char *const terms[][5] = {
        {"general", "instruct", "multilingual", NULL}, {"coding", "code", "coder", "programming", NULL},
        {"research", "reasoning", "knowledge", NULL},  {"writing", "translation", "creative", NULL},
        {"math", "finance", "accounting", NULL},       {"legal", "law", "research", NULL}};
    unsigned mask = 0;
    for (unsigned purpose = 0; purpose < OT_ADVICE_TASKS; purpose++)
        for (unsigned i = 0; terms[purpose][i]; i++)
            if (contains(m->description, terms[purpose][i]) || contains(m->name, terms[purpose][i])) {
                mask |= 1u << purpose;
                break;
            }
    return mask;
}
double ot_model_candidate_score(const ot_model_candidate *m, const ot_resources *r, const ot_preferences *p) {
    if (!ot_model_candidate_fits(m, r, p))
        return -1;
    uint64_t budget = ot_memory_budget(r, p);
    if (r->gpu_recommended && r->gpu_recommended < budget)
        budget = r->gpu_recommended;
    double target = (double)budget / (p->purpose == 3 ? 4 : p->purpose == 0 ? 3 : 2);
    if (p->advice_goal == 1)
        target *= .5;
    if (p->advice_size_gb)
        target = (double)p->advice_size_gb * 1000000000;
    double size = (double)m->bytes;
    double fit = target > size ? size / target : target / size;
    if (p->advice_goal == 3 && !p->advice_size_gb)
        fit = fmax(0, 1 - size / (double)budget);
    unsigned purpose = p->purpose < OT_ADVICE_TASKS ? p->purpose : 0;
    bool relevant = (ot_model_purposes(m) & (1u << purpose)) != 0;
    /* This ranks capacity/relevance/latency tradeoffs, not professional accuracy.
     * It never consults the developer qualification table below. */
    double language = 0;
    unsigned wanted = ot_advice_language(strcmp(p->input_language, "auto") ? p->input_language : p->language);
    if (m->languages && wanted)
        language = (m->languages & wanted) ? .06 : -.06;
    /* Missing optional descriptions/geometry do not disqualify a runnable model.
     * Compression uncertainty is a small prior, never a quality measurement. */
    double compression = m->bits > 0 && m->bits < 3 ? .08 : 0;
    for (unsigned i = 0; i < m->metric_count && i < OT_ADVICE_METRICS; i++)
        if (m->metrics[i].verified && m->metrics[i].exact)
            compression = 0;
    return fmax(
        .001, (p->advice_size_gb     ? .65
               : p->advice_goal == 2 ? .2
                                     : .55) *
                      fit +
                  (relevant ? .25 : 0) + language - compression +
                  ((purpose == 1 || purpose == 2 || purpose == 4 || purpose == 5) && m->reasoning ? .08 : 0) +
                  (m->installed ? .06 : 0));
}

void ot_model_evidence(ot_model_candidate *m, const char *json, bool base) {
    if (!m || !json || strlen(json) > 1048576)
        return;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(d);
    const char *id = text(root, "id");
    const char *expected = base ? m->base : m->repository;
    if (!*expected || strcmp(id, expected)) {
        yyjson_doc_free(d);
        return;
    }
    unsigned context = m->context;
    double parameters = m->parameters;
    ot_model_hf_facts(m, root);
    if (base && context)
        m->context = context;
    if (base && parameters > 0)
        m->parameters = parameters;
    m->base_evidence = base || m->kind == 1;
    yyjson_val *rows = yyjson_obj_get(root, "evalResults"), *row;
    size_t i, n;
    yyjson_arr_foreach(rows, i, n, row) {
        if (i >= 64 || m->metric_count == OT_ADVICE_METRICS)
            break;
        yyjson_val *data = yyjson_obj_get(row, "data"), *dataset = yyjson_obj_get(data, "dataset"),
                   *value = yyjson_obj_get(data, "value");
        const char *benchmark = text(dataset, "id"), *task = text(dataset, "task_id"),
                   *source = text(yyjson_obj_get(data, "source"), "url"),
                   *configuration = text(data, "notes"), *revision = text(dataset, "revision");
        double score = yyjson_get_num(value);
        if (!hf_repository(benchmark) || strlen(benchmark) >= sizeof m->metrics[0].benchmark ||
            strlen(task) >= sizeof m->metrics[0].task || !yyjson_is_num(value) || !isfinite(score) ||
            score < 0 || score > 100 || (*source && strncmp(source, "https://", 8)) ||
            strlen(source) >= sizeof m->metrics[0].source ||
            strlen(configuration) >= sizeof m->metrics[0].configuration ||
            (*revision && !hex_digest(revision, 40)) || contains(benchmark, "latency") ||
            contains(task, "latency") || contains(task, "perplexity") || contains(task, "loss") ||
            contains(task, "error") || contains(task, "tokens") || contains(task, "memory"))
            continue;
        bool duplicate = false;
        for (unsigned j = 0; j < m->metric_count; j++)
            if (!strcmp(m->metrics[j].benchmark, benchmark) && !strcmp(m->metrics[j].task, task))
                duplicate = true;
        if (duplicate)
            continue;
        ot_model_metric *metric = &m->metrics[m->metric_count++];
        snprintf(metric->benchmark, sizeof metric->benchmark, "%s", benchmark);
        snprintf(metric->task, sizeof metric->task, "%s", task);
        snprintf(metric->source, sizeof metric->source, "%s", source);
        snprintf(metric->configuration, sizeof metric->configuration, "%s", configuration);
        snprintf(metric->revision, sizeof metric->revision, "%s", revision);
        metric->value = score;
        /* Unknown score direction is useful to display, not safe to optimize. */
        metric->higher_better = contains(task, "accuracy") || contains(task, "pass") ||
                                contains(task, "score") || contains(task, "f1") ||
                                contains(benchmark, "mmlu") || contains(benchmark, "gpqa") ||
                                contains(benchmark, "gsm8k") || contains(benchmark, "aime");
        metric->verified = yyjson_is_true(yyjson_obj_get(row, "verified"));
        /* Exact file evidence needs an explicit content identity. A result for a
         * base repository never certifies all its downloadable quantizations. */
        metric->exact =
            !base && *m->checksum && !strcmp(text(yyjson_obj_get(data, "model"), "sha256"), m->checksum);
        if (metric->exact)
            m->base_evidence = false;
    }
    yyjson_doc_free(d);
}
char *ot_model_catalogue_write(const ot_model_candidate *models, size_t count) {
    if (!models || count > OT_ADVICE_MODELS)
        return NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *rows = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, rows);
    for (size_t i = 0; i < count; i++) {
        const ot_model_candidate *m = &models[i];
        yyjson_mut_val *v = yyjson_mut_obj(d), *metrics = yyjson_mut_arr(d);
        yyjson_mut_arr_append(rows, v);
#define STR(field) yyjson_mut_obj_add_strcpy(d, v, #field, m->field)
        STR(name);
        STR(description);
        STR(catalogue_digest);
        STR(repository);
        STR(file);
        STR(checksum);
        STR(revision);
        STR(license);
        STR(base);
        STR(quantization);
        STR(library);
#undef STR
        yyjson_mut_obj_add_int(d, v, "kind", m->kind);
        yyjson_mut_obj_add_uint(d, v, "bytes", m->bytes);
        yyjson_mut_obj_add_uint(d, v, "memory", m->memory);
        yyjson_mut_obj_add_uint(d, v, "context", m->context);
        yyjson_mut_obj_add_uint(d, v, "languages", m->languages);
        yyjson_mut_obj_add_real(d, v, "parameters", m->parameters);
        yyjson_mut_obj_add_real(d, v, "bits", m->bits);
        yyjson_mut_obj_add_bool(d, v, "reasoning", m->reasoning);
        yyjson_mut_obj_add_bool(d, v, "approximate", m->approximate);
        yyjson_mut_obj_add_bool(d, v, "baseEvidence", m->base_evidence);
        yyjson_mut_obj_add_val(d, v, "metrics", metrics);
        for (unsigned j = 0; j < m->metric_count && j < OT_ADVICE_METRICS; j++) {
            const ot_model_metric *x = &m->metrics[j];
            yyjson_mut_val *metric = yyjson_mut_obj(d);
            yyjson_mut_arr_append(metrics, metric);
            yyjson_mut_obj_add_strcpy(d, metric, "benchmark", x->benchmark);
            yyjson_mut_obj_add_strcpy(d, metric, "task", x->task);
            yyjson_mut_obj_add_strcpy(d, metric, "source", x->source);
            yyjson_mut_obj_add_strcpy(d, metric, "configuration", x->configuration);
            yyjson_mut_obj_add_strcpy(d, metric, "revision", x->revision);
            yyjson_mut_obj_add_real(d, metric, "value", x->value);
            yyjson_mut_obj_add_bool(d, metric, "verified", x->verified);
            yyjson_mut_obj_add_bool(d, metric, "exact", x->exact);
            yyjson_mut_obj_add_bool(d, metric, "higherBetter", x->higher_better);
        }
    }
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}
size_t ot_model_catalogue_read(const char *json, ot_model_candidate *models, size_t capacity) {
    if (!json || !models || strlen(json) > 2097152)
        return 0;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    yyjson_val *rows = yyjson_doc_get_root(d), *v;
    size_t i, n, count = 0;
    if (yyjson_arr_size(rows) > OT_ADVICE_MODELS) {
        yyjson_doc_free(d);
        return 0;
    }
    yyjson_arr_foreach(rows, i, n, v) {
        if (count == capacity)
            break;
        ot_model_candidate m = {0};
        bool ok = true;
#define STR(field)                                                                                           \
    do {                                                                                                     \
        const char *s = text(v, #field);                                                                     \
        if (strlen(s) >= sizeof m.field)                                                                     \
            ok = false;                                                                                      \
        else                                                                                                 \
            memcpy(m.field, s, strlen(s) + 1);                                                               \
    } while (0)
        STR(name);
        STR(description);
        STR(catalogue_digest);
        STR(repository);
        STR(file);
        STR(checksum);
        STR(revision);
        STR(license);
        STR(base);
        STR(quantization);
        STR(library);
#undef STR
        m.kind = yyjson_get_int(yyjson_obj_get(v, "kind"));
        m.bytes = yyjson_get_uint(yyjson_obj_get(v, "bytes"));
        m.memory = yyjson_get_uint(yyjson_obj_get(v, "memory"));
        uint64_t context = yyjson_get_uint(yyjson_obj_get(v, "context"));
        m.context = context <= UINT32_MAX ? (unsigned)context : 0;
        m.languages = (unsigned)(yyjson_get_uint(yyjson_obj_get(v, "languages")) & 1023);
        m.parameters = yyjson_get_num(yyjson_obj_get(v, "parameters"));
        m.bits = yyjson_get_num(yyjson_obj_get(v, "bits"));
        m.reasoning = yyjson_is_true(yyjson_obj_get(v, "reasoning"));
        m.approximate = yyjson_is_true(yyjson_obj_get(v, "approximate"));
        m.base_evidence = yyjson_is_true(yyjson_obj_get(v, "baseEvidence"));
        ok = ok && ot_model_name(m.name) && m.bytes && m.bytes <= INT64_MAX && m.memory <= INT64_MAX &&
             (m.kind == 1 || m.kind == 2) && isfinite(m.parameters) && m.parameters >= 0 &&
             m.parameters < 1e14 && isfinite(m.bits) && m.bits >= 0 && m.bits < 1024;
        if (m.kind == 1)
            ok = ok && hf_repository(m.repository) && ot_model_name(m.file) && hex_digest(m.checksum, 64) &&
                 hex_digest(m.revision, 40) && standalone_variant(&m);
        if (!ok)
            continue;
        yyjson_val *metrics = yyjson_obj_get(v, "metrics");
        for (size_t j = 0; j < yyjson_arr_size(metrics) && j < OT_ADVICE_METRICS; j++) {
            yyjson_val *x = yyjson_arr_get(metrics, j);
            const char *benchmark = text(x, "benchmark"), *task = text(x, "task"),
                       *source = text(x, "source"), *configuration = text(x, "configuration"),
                       *revision = text(x, "revision");
            double value = yyjson_get_num(yyjson_obj_get(x, "value"));
            if (!hf_repository(benchmark) || strlen(benchmark) >= sizeof m.metrics[0].benchmark ||
                strlen(task) >= sizeof m.metrics[0].task || strlen(source) >= sizeof m.metrics[0].source ||
                strlen(configuration) >= sizeof m.metrics[0].configuration ||
                (*revision && !hex_digest(revision, 40)) || (*source && strncmp(source, "https://", 8)) ||
                !yyjson_is_num(yyjson_obj_get(x, "value")) || !isfinite(value) || value < 0 || value > 100)
                continue;
            ot_model_metric *metric = &m.metrics[m.metric_count++];
            snprintf(metric->benchmark, sizeof metric->benchmark, "%s", benchmark);
            snprintf(metric->task, sizeof metric->task, "%s", task);
            snprintf(metric->source, sizeof metric->source, "%s", source);
            snprintf(metric->configuration, sizeof metric->configuration, "%s", configuration);
            snprintf(metric->revision, sizeof metric->revision, "%s", revision);
            metric->value = value;
            metric->verified = yyjson_is_true(yyjson_obj_get(x, "verified"));
            metric->exact = yyjson_is_true(yyjson_obj_get(x, "exact"));
            metric->higher_better = yyjson_is_true(yyjson_obj_get(x, "higherBetter"));
        }
        m.verified =
            m.kind == 1 ? ot_model_file_verified(m.checksum) : ot_model_verified(m.name, m.catalogue_digest);
        models[count++] = m;
    }
    yyjson_doc_free(d);
    return count;
}

/* Test identities never supply catalogue entries. They identify the badge and
 * break close choices only after the hardware/activity score has been computed. */
static const struct {
    int kind;
    const char *name, *digest;
} qualified[] = {{2, "qwen3.5:2b", "dfaf2605162e689c8cac330dc4a190c4c50ad30aa47814a8fe9b79fa5127e97e"},
                 {2, "qwen3.5:4b", "7f3251fa6878a78a606bcb3c074283306e5ad2ea411a7774a9dd98a4bc7f2d13"},
                 {2, "qwen3.5:9b", "d63f830f28d515fefea546f213fca7a75e9dd5176ef51a6deabc952d1c99a911"},
                 {2, "qwen3.5:27b", "2d2e4b8fc7c0479b70f8cba9fc8bdb49a3c9a43a8a6da3873b724c9fca4672ef"},
                 {2, "qwen3.5:0.8b", "d9c89605f39abfd9e8cb4fd3c099c203d7d17e82d7765b4266a0612402d88f1c"},
                 {1, "Qwen3 8B Q4_K_M", "d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785"}};
static bool verified(int kind, const char *name, const char *digest) {
    if (!digest || (kind == 2 && !name))
        return false;
    if (!strncmp(digest, "sha256:", 7))
        digest += 7;
    for (size_t i = 0; i < sizeof qualified / sizeof *qualified; i++)
        if (qualified[i].kind == kind && (kind == 1 || !strcmp(name, qualified[i].name)) &&
            !strcmp(digest, qualified[i].digest))
            return true;
    return false;
}
bool ot_model_verified(const char *name, const char *digest) {
    return verified(2, name, digest);
}
bool ot_model_file_verified(const char *digest) {
    return verified(1, NULL, digest);
}
bool ot_model_verification_candidate(const char *name) {
    for (size_t i = 0; name && i < sizeof qualified / sizeof *qualified; i++)
        if (qualified[i].kind == 2 && !strcmp(name, qualified[i].name))
            return true;
    return false;
}
size_t ot_model_candidate_order(const ot_model_candidate *models, const double *scores, size_t count,
                                size_t *order) {
    if (!models || !scores || !order || count > OT_ADVICE_MODELS + 32)
        return 0;
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(scores[i]))
            continue;
        bool dominated = false;
        for (size_t j = 0; scores[i] >= 0 && !dominated && j < count; j++)
            dominated = i != j && scores[j] >= 0 && ot_model_candidate_dominates(&models[j], &models[i]);
        if (dominated)
            continue;
        size_t at = used++;
        while (at && (scores[i] > scores[order[at - 1]] || (fabs(scores[i] - scores[order[at - 1]]) < 1e-9 &&
                                                            models[i].bytes < models[order[at - 1]].bytes))) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    if (!used || scores[order[0]] < 0 || models[order[0]].verified)
        return used;
    size_t best = order[0], preferred = 0;
    for (size_t rank = 1; rank < used; rank++) {
        size_t i = order[rank];
        const ot_model_candidate *a = &models[i], *b = &models[best];
        uint64_t small = a->bytes < b->bytes ? a->bytes : b->bytes,
                 large = a->bytes > b->bytes ? a->bytes : b->bytes;
        unsigned context = b->context < 65536 ? b->context : 65536;
        if (a->verified && large && (double)small / (double)large >= .8 && a->context >= context &&
            a->reasoning == b->reasoning && scores[i] >= scores[best] * .97 &&
            (!preferred || scores[i] > scores[order[preferred]]))
            preferred = rank;
    }
    if (preferred) {
        size_t swap = order[0];
        order[0] = order[preferred];
        order[preferred] = swap;
    }
    return used;
}
size_t ot_model_candidate_best(const ot_model_candidate *models, const double *scores, size_t count) {
    size_t order[OT_ADVICE_MODELS + 32];
    return ot_model_candidate_order(models, scores, count, order) ? order[0] : 0;
}
