/* Reproducible boundary mutations: in-memory chat state and one temporary GGUF. */
#include "core.h"
#include "rewrite.h"
#include "memory.h"
#include "runtime.h"
#include "recommendations.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void exercise(const uint8_t *data, size_t size) {
    if (size < 2)
        return;
    ot_memory_plan memory;
    ot_memory_parse((const char *)data + 2, size - 2, &memory);
    ot_runtime_info runtime = {0};
    ot_runtime_ollama(&runtime, (const char *)data + 2, size - 2, "fixture");
    char line[1025];
    size_t length = size - 2 < 1024 ? size - 2 : 1024;
    memcpy(line, data + 2, length);
    line[length] = 0;
    char revised[OT_REWRITE_LIMIT + 1];
    (void)ot_rewrite_apply("Scrivi 2 righe in C17.", line, revised);
    ot_server_info server = {0};
    char *models = ot_server_models(line, (data[0] & 1) != 0);
    free(models);
    ot_server_metadata(line, (data[0] & 1) != 0, "fixture", &server);
    ot_server_residency(line, "fixture", &server);
    ot_memory_recall(line, "fixture", 8192, &memory);
    ot_model_candidate families[6], variants[8];
    ot_model_feed((const char *)data + 2, size - 2, families, 6);
    ot_model_hf_feed(line, families, 6);
    ot_model_candidate hf = {.kind = 1, .repository = "fixture/model"};
    ot_model_hf_variants(line, &hf, variants, 8);
    ot_advice_profile advice;
    (void)ot_advice_decode(&advice, line);
    size_t decoded = ot_model_catalogue_read(line, variants, 8);
    double scores[8] = {0};
    size_t order[8];
    ot_model_evidence(&hf, line, false);
    ot_model_evidence_scores(variants, scores, decoded);
    (void)ot_model_candidate_order(variants, scores, decoded, order);
    char *catalogue = ot_ollama_catalog(line, "fixture");
    if (catalogue) {
        ot_model_candidate family = {.kind = 2, .name = "fixture"};
        ot_model_variants(catalogue, &family, variants, 8);
        free(catalogue);
    }
    char *observation = ot_memory_observe(line, "fixture", &runtime);
    free(observation);
    ot_runtime_buffers buffers = {0};
    ot_runtime_buffer_line(&buffers, &runtime, line);
    yyjson_doc *metadata = yyjson_read((const char *)data + 2, size - 2, 0);
    ot_thinking modes = ot_thinking_detect(yyjson_doc_get_root(metadata), (data[0] & 1) != 0);
    ot_preferences preferences;
    ot_preferences_read(&preferences, yyjson_doc_get_root(metadata));
    if (modes.count > OT_THINKING_VALUES)
        abort();
    yyjson_doc_free(metadata);
    ot_app *app = ot_open(":memory:", (ot_io){0});
    if (!app)
        abort();
    ot_connection c = *ot_configuration(app);
    strcpy(c.model, "model");
    c.ollama = (data[0] & 1) != 0;
    if (!ot_configure(app, &c, 0) || !ot_submit(app, "input", 0))
        abort();
    size_t chunk = (size_t)data[1] + 1;
    for (size_t position = 2; position < size; position += chunk) {
        size_t count = size - position < chunk ? size - position : chunk;
        ot_bytes(app, 1, data + position, count);
    }
    ot_end(app, 1, NULL);
    char *view = ot_view(app);
    free(view);
    ot_close(app);
}

static uint32_t random_number(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}
int main(void) {
    const char *seeds[] = {
        "{\"prompt\":\"Scrivi 2 righe in C17, senza librerie esterne.\"}",
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello 🌍\"}}]}\n\ndata: [DONE]\n\n",
        "{\"message\":{\"content\":\"hello 🌍\"},\"done\":false}\n{\"done\":true}\n",
        ("{\"type\":\"fit_memory_breakdown\",\"data\":{\"unit\":\"MiB\",\"rows\":["
         "{\"kind\":\"host\",\"name\":\"Host\",\"model\":512,\"context\":128,\"compute\":32}]}}"),
        "{\"models\":[{\"name\":\"fixture\",\"size\":1000,\"size_vram\":800,\"context_length\":8192}]}",
        "{\"thinking\":{\"values\":[false,\"low\",\"medium\",\"maximum\"]}}",
        "{\"version\":1,\"key\":\"fixture\",\"samples\":[{\"context\":8192,\"batch\":512,"
        "\"microBatch\":512,\"gpuLayers\":20,\"totalLayers\":20,\"windowCells\":0,"
        "\"allocation\":8,\"weights\":2,\"cache\":4,\"linearCache\":4,\"compute\":2}]}",
        "llama_kv_cache: MTL0 KV buffer size = 192.00 MiB",
        "{\"recommendations\":[{\"model\":\"fixture:4b\",\"vram_bytes\":6000000000}]}",
        "<a href=\"/library/fixture:4b\">3GB 32K context window</a>",
        "[{\"id\":\"fixture/model\",\"gated\":false,\"pipeline_tag\":\"text-generation\","
        "\"gguf\":{\"total\":4000000000,\"context_length\":32768}}]",
        "{\"data\":[{\"id\":\"fixture\",\"context_length\":8192}]}",
        "{\"capabilities\":[\"completion\",\"thinking\"],\"model_info\":{"
        "\"general.architecture\":\"fixture\",\"fixture.context_length\":32768}}",
        "{\"id\":\"fixture/model\",\"evalResults\":[{\"data\":{\"dataset\":{\"id\":\"fixture/quality\","
        "\"task_id\":\"accuracy\"},\"value\":80,\"source\":{\"url\":\"https://example.com/eval\"}}}]}",
        "[{\"kind\":2,\"name\":\"fixture:4b\",\"bytes\":4000000000,\"metrics\":[]}]",
        "{\"tasks\":[1,0,0,0,0,0],\"languages\":[1,0,0,0,0,0,0,0,0,0],\"input\":42,\"output\":10,"
        "\"first\":1,\"speed\":1,\"long\":0,\"limited\":0,\"reasoning\":0,\"failures\":0,\"samples\":1,"
        "\"identity\":\"\",\"host\":\"\",\"execution\":\"\",\"context\":0,\"runtime\":0}"};
    uint32_t random_state = 0x6f766572;
    uint8_t *data = malloc(65540);
    if (!data)
        abort();
    for (unsigned i = 0; i < 10000; i++) {
        const char *seed = seeds[i % (sizeof seeds / sizeof *seeds)];
        size_t size = strlen(seed) + 2;
        data[0] = i % 2;
        data[1] = (uint8_t)random_number(&random_state);
        memcpy(data + 2, seed, size - 2);
        if (i % 17 == 0) {
            memset(data + size, 'x', 65540 - size);
            size = 65540;
        } else if (i % 7 == 0)
            size = random_number(&random_state) % size;
        unsigned mutations = i % 9;
        for (unsigned j = 0; j < mutations && size; j++)
            data[random_number(&random_state) % size] = (uint8_t)random_number(&random_state);
        exercise(data, size);
    }
    free(data);
    char path[4096];
    const char *temporary = getenv("TMPDIR");
    int written = snprintf(path, sizeof path, "%s/overtone-fuzz-XXXXXX",
                           temporary && *temporary ? temporary : "/private/tmp");
    if (written < 0 || (size_t)written >= sizeof path)
        abort();
    int fd = mkstemp(path);
    if (fd < 0)
        abort();
    unsigned char seed[132] = {0x47, 0x47, 0x55, 0x46, 3}, sample[132];
    seed[8] = seed[16] = 1;
    seed[24] = 20;
    memcpy(seed + 32, "general.architecture", 20);
    seed[52] = 8;
    seed[56] = 5;
    memcpy(seed + 64, "qwen3", 5);
    seed[69] = 1;
    seed[77] = 'x';
    seed[78] = seed[82] = 1;
    for (unsigned i = 0; i < 1000; i++) {
        memcpy(sample, seed, sizeof sample);
        for (unsigned j = 0; j < i % 8; j++)
            sample[random_number(&random_state) % sizeof sample] = (uint8_t)random_number(&random_state);
        size_t size = i % 5 ? sizeof sample : random_number(&random_state) % sizeof sample;
        if (ftruncate(fd, (off_t)size) || pwrite(fd, sample, size, 0) != (ssize_t)size)
            abort();
        ot_model_info info;
        (void)ot_gguf(path, &info);
    }
    if (close(fd) || unlink(path))
        abort();
    puts("PASS: 10000 stream/planner/runtime/thinking/preferences mutations; 1000 GGUF mutations (seed "
         "0x6f766572)");
    return 0;
}
