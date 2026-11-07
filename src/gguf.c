#include "gguf.h"
#include "utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* GGUF general.file_type values, defined by llama.cpp's public llama_ftype enum. */
static const char *const quantizations[] = {
    "F32",   "F16",     "Q4_0",   "Q4_1",   "",       "",        "",       "Q8_0",   "Q5_0",
    "Q5_1",  "Q2_K",    "Q3_K_S", "Q3_K_M", "Q3_K_L", "Q4_K_S",  "Q4_K_M", "Q5_K_S", "Q5_K_M",
    "Q6_K",  "IQ2_XXS", "IQ2_XS", "Q2_K_S", "IQ3_XS", "IQ3_XXS", "IQ1_S",  "IQ4_NL", "IQ3_S",
    "IQ3_M", "IQ2_S",   "IQ2_M",  "IQ4_XS", "IQ1_M",  "BF16",    "",       "",       "",
    "TQ1_0", "TQ2_0",   "MXFP4",  "NVFP4",  "Q1_0",   "Q2_0"};
const char *ot_model_precision(const char *quantization) {
    static const char *const groups[] = {"iq1", "iq2", "iq3",  "iq4", "q1",  "q2",    "q3",    "q4",  "q5",
                                         "q6",  "q8",  "bf16", "f16", "f32", "mxfp4", "nvfp4", "tq1", "tq2"};
    if (!quantization || !*quantization)
        return "unknown";
    for (size_t i = 0; i < sizeof groups / sizeof *groups; i++) {
        size_t length = strlen(groups[i]);
        if (!strncasecmp(quantization, groups[i], length) &&
            (!quantization[length] || quantization[length] == '_'))
            return groups[i];
    }
    return "other";
}
static bool label_separator(char c) {
    return !c || strchr(" .-_", c) != NULL;
}
void ot_model_label(const char *name, const char *quantization, char title[257], char quant[33]) {
    if (!name)
        name = "";
    size_t length = strlen(name);
    bool file = length >= 5 && !strcasecmp(name + length - 5, ".gguf");
    const char *base = file ? strrchr(name, '/') : NULL;
    name = base ? base + 1 : name;
    length = strlen(name) - (file ? 5u : 0u);
    if (length > 256) {
        length = 256;
        while (length && ((unsigned char)name[length] & 0xc0) == 0x80)
            length--;
    }
    char raw[257];
    memcpy(raw, name, length);
    raw[length] = 0;
    char provided[33] = "";
    if (quantization && strlen(quantization) < 33)
        snprintf(provided, sizeof provided, "%s", quantization);
    memcpy(quant, provided, sizeof provided);
    size_t start = length, end = length;
    for (size_t i = 0; i < length && start == length; i++) {
        if (i && !label_separator(raw[i - 1]))
            continue;
        size_t supplied = strlen(provided);
        if (supplied && i + supplied <= length && !strncasecmp(raw + i, provided, supplied) &&
            label_separator(raw[i + supplied])) {
            start = i;
            end = i + supplied;
            break;
        }
        for (size_t q = 0; q < sizeof quantizations / sizeof *quantizations; q++) {
            size_t n = strlen(quantizations[q]);
            if (n && i + n <= length && !strncasecmp(raw + i, quantizations[q], n) &&
                label_separator(raw[i + n])) {
                start = i;
                end = i + n;
                if (!*quant)
                    snprintf(quant, 33, "%s", quantizations[q]);
                break;
            }
        }
    }
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        if (i >= start && i < end)
            continue;
        char c = raw[i];
        if (file && (c == '-' || c == '_'))
            c = ' ';
        if (c == ' ' && (!written || title[written - 1] == ' '))
            continue;
        title[written++] = c;
    }
    while (written && label_separator(title[written - 1]))
        written--;
    title[written] = 0;
    if (!written)
        snprintf(title, 257, "%s", raw);
}

typedef struct {
    int fd;
    uint64_t at, limit, start;
    size_t count;
    unsigned char buffer[65536];
    bool ok;
} reader;
static void advance(reader *r, uint64_t count) {
    if (!r->ok || count > r->limit - r->at)
        r->ok = false;
    else
        r->at += count;
}
static void take(reader *r, unsigned char *out, size_t count) {
    if (!r->ok || count > r->limit - r->at) {
        r->ok = false;
        return;
    }
    while (count) {
        if (r->at < r->start || r->at - r->start >= r->count) {
            size_t wanted =
                r->limit - r->at < sizeof r->buffer ? (size_t)(r->limit - r->at) : sizeof r->buffer;
            ssize_t n;
            do
                n = pread(r->fd, r->buffer, wanted, (off_t)r->at);
            while (n < 0 && errno == EINTR);
            if (n <= 0) {
                r->ok = false;
                return;
            }
            r->start = r->at;
            r->count = (size_t)n;
        }
        size_t offset = (size_t)(r->at - r->start), available = r->count - offset;
        size_t n = count < available ? count : available;
        memcpy(out, r->buffer + offset, n);
        r->at += n;
        out += n;
        count -= n;
    }
}
static uint64_t number(reader *r, unsigned size) {
    unsigned char bytes[8] = {0};
    take(r, bytes, size);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)bytes[i] << (i * 8);
    return v;
}
static bool string(reader *r, char *out, size_t capacity) {
    uint64_t n = number(r, 8);
    if (!out || n >= capacity) {
        advance(r, n);
        return !out && r->ok;
    }
    take(r, (unsigned char *)out, (size_t)n);
    if (!r->ok || !ot_utf8_valid(out, (size_t)n))
        return false;
    out[n] = 0;
    return true;
}
static void skip(reader *r, unsigned type, unsigned depth) {
    const unsigned widths[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    if (type >= 13 || depth > 2 || (type == 9 && depth == 2)) {
        r->ok = false;
        return;
    }
    if (widths[type]) {
        advance(r, widths[type]);
        return;
    }
    if (type == 8) {
        string(r, NULL, 0);
        return;
    }
    unsigned child = (unsigned)number(r, 4);
    uint64_t n = number(r, 8);
    if (n > 1000000 || child >= 13) {
        r->ok = false;
        return;
    }
    if (widths[child]) {
        advance(r, n * widths[child]);
        return;
    }
    for (uint64_t i = 0; r->ok && i < n; i++)
        skip(r, child, depth + 1);
}
static void dimension(ot_model_info *info, const char *field, unsigned value) {
    if (!strcmp(field, "context_length"))
        info->context = value;
    else if (!strcmp(field, "embedding_length"))
        info->embedding = value;
    else if (!strcmp(field, "block_count"))
        info->layers = value;
    else if (!strcmp(field, "attention.head_count"))
        info->heads = value;
    else if (!strcmp(field, "attention.head_count_kv"))
        info->kv_heads = value;
    else if (!strcmp(field, "attention.key_length"))
        info->key_length = value;
    else if (!strcmp(field, "attention.value_length"))
        info->value_length = value;
    else if (!strcmp(field, "expert_count"))
        info->experts = value;
    else if (!strcmp(field, "expert_used_count"))
        info->experts_used = value;
    if (value && (!strncmp(field, "ssm.", 4) || !strcmp(field, "attention.sliding_window") ||
                  !strcmp(field, "attention.kv_lora_rank")))
        info->complex_cache = true;
}
bool ot_gguf(const char *path, ot_model_info *out) {
    if (!path || !out)
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 24) {
        close(fd);
        return false;
    }
    reader r = {.fd = fd,
                .ok = true,
                .limit = (uint64_t)st.st_size < 32u * 1024 * 1024 ? (uint64_t)st.st_size : 32u * 1024 * 1024};
    ot_model_info info = {.bytes = (uint64_t)st.st_size};
    if (number(&r, 4) != 0x46554747)
        r.ok = false;
    uint64_t version = number(&r, 4), tensors = number(&r, 8), keys = number(&r, 8), alignment = 32;
    if ((version != 2 && version != 3) || !tensors || tensors > 1000000 || keys > 1000000)
        r.ok = false;
    // Identity may follow dimensions. Repeat metadata only in that case, tying
    // geometry to the main architecture rather than a vision/auxiliary encoder.
    bool rescan = false, architecture_seen = false;
    for (unsigned pass = 0; r.ok && pass < 2; pass++) {
        r.at = 24;
        for (uint64_t i = 0; r.ok && i < keys; i++) {
            char name[513];
            if (!string(&r, name, sizeof name)) {
                r.ok = false;
                break;
            }
            unsigned type = (unsigned)number(&r, 4);
            if (!pass && type == 8 &&
                (!strcmp(name, "general.name") || !strcmp(name, "general.architecture"))) {
                bool label = !strcmp(name, "general.name");
                if (!label && architecture_seen) {
                    r.ok = false;
                    break;
                }
                architecture_seen |= !label;
                char *to = label ? info.name : info.architecture;
                if (!string(&r, to, label ? sizeof info.name : sizeof info.architecture))
                    to[0] = 0;
            } else if (type == 4 || type == 10) {
                uint64_t raw = number(&r, type == 4 ? 4 : 8);
                unsigned value = raw <= UINT32_MAX ? (unsigned)raw : 0;
                size_t prefix = strlen(info.architecture);
                if (!strcmp(name, "general.alignment"))
                    alignment = value;
                else if (!strcmp(name, "general.file_type") &&
                         raw < sizeof quantizations / sizeof *quantizations)
                    snprintf(info.quantization, sizeof info.quantization, "%s", quantizations[raw]);
                else if (!prefix)
                    rescan |= strncmp(name, "general.", 8) != 0;
                else if (!strncmp(name, info.architecture, prefix) && name[prefix] == '.')
                    dimension(&info, name + prefix + 1, value);
            } else
                skip(&r, type, 0);
        }
        if (!info.architecture[0] || !rescan)
            break;
    }
    if (!alignment || alignment > 65536 || (alignment & (alignment - 1)))
        r.ok = false;
    uint64_t required = 0;
    for (uint64_t i = 0; r.ok && i < tensors; i++) {
        string(&r, NULL, 0);
        unsigned dimensions = (unsigned)number(&r, 4);
        uint64_t elements = 1;
        if (!dimensions || dimensions > 4) {
            r.ok = false;
            break;
        }
        for (unsigned j = 0; j < dimensions; j++) {
            uint64_t value = number(&r, 8);
            if (!value || value > UINT64_MAX / elements) {
                r.ok = false;
                break;
            }
            elements *= value;
        }
        unsigned type = (unsigned)number(&r, 4);
        uint64_t offset = number(&r, 8), length = 1;
        static const unsigned block[] = {1, 1, 32, 32, 0, 0, 32, 32, 32, 32, 256, 256, 256, 256, 256, 256};
        static const unsigned width[] = {4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292};
        if (type < 16 && block[type]) {
            if (elements % block[type] || elements / block[type] > UINT64_MAX / width[type])
                r.ok = false;
            else
                length = elements / block[type] * width[type];
        }
        if (!r.ok || offset % alignment || offset > UINT64_MAX - length) {
            r.ok = false;
            break;
        }
        if (offset + length > required)
            required = offset + length;
    }
    uint64_t start = alignment ? (r.at + alignment - 1) / alignment * alignment : 0;
    bool ok = r.ok && start <= info.bytes && required <= info.bytes - start;
    if (ok)
        info.tensor_bytes = info.bytes - start;
    struct stat after;
    ok = ok && !fstat(fd, &after) && after.st_size == st.st_size &&
         after.st_mtimespec.tv_sec == st.st_mtimespec.tv_sec &&
         after.st_mtimespec.tv_nsec == st.st_mtimespec.tv_nsec;
    close(fd);
    if (ok)
        *out = info;
    return ok;
}
