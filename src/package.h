#ifndef OVERTONE_PACKAGE_H
#define OVERTONE_PACKAGE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef struct {
    int kind;
    const char *name, *version, *url, *checksum, *entry;
    uint64_t bytes, unpacked_limit;
    unsigned strip_components;
} ot_package;
/* Pinned distributions, never commands or extraction rules supplied by a server. */
const ot_package *ot_package_for_kind(int kind);
const ot_package *ot_package_for_download(const char *url, const char *checksum);
bool ot_package_find(const ot_package *package, const char *profile, char path[4096]);
/* Bounded UTF-8 documents from a verified private package; caller frees the JSON. */
char *ot_package_notices(const ot_package *package, const char *profile);
/* Worker only. Verify the archive through the extraction descriptor and validate
 * the staged engine before publishing its private directory atomically. Valid installations are reused; only
 * an invalid private versioned directory can be replaced. External engines are untouched. */
bool ot_package_install(const ot_package *package, const char *profile, const char *archive_path,
                        const atomic_bool *cancel, char path[4096]);
#endif
