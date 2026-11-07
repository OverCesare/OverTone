#ifndef OT_REWRITE_H
#define OT_REWRITE_H
#include <stdbool.h>
typedef struct yyjson_mut_doc yyjson_mut_doc;
typedef struct yyjson_mut_val yyjson_mut_val;
enum { OT_REWRITE_LIMIT = 16384 };
/* Validate one complete prompt proposal and preserve source literals. Output
 * must not alias inputs. Return -1 on invalid/incomplete data, 0 unchanged, 1 new.
 * This does not certify semantic equivalence; the caller requires user review. */
int ot_rewrite_apply(const char *source, const char *json, char output[OT_REWRITE_LIMIT + 1]);
const char *ot_rewrite_task(bool detailed);
yyjson_mut_val *ot_rewrite_schema(yyjson_mut_doc *doc);
#endif
