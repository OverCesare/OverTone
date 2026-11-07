#ifndef OVERTONE_INVENTORY_H
#define OVERTONE_INVENTORY_H
#include "gguf.h"
#include <stdatomic.h>
#include <stddef.h>
typedef struct {
    char name[257], reference[4096], manifest[4096], manifest_digest[65];
    int kind;
    size_t manifest_root_length;
    ot_model_info info;
} ot_inventory_item;
typedef struct {
    ot_inventory_item *items;
    size_t count;
    bool limited;
} ot_inventory;
/* Worker-only, fixed known cache roots, bounded traversal and metadata. No network or writes. */
ot_inventory ot_inventory_read(const atomic_bool *cancel);
/* Validate a previously observed local manifest and all its blobs. */
bool ot_inventory_manifest(const char *path, size_t root_length);
#endif
