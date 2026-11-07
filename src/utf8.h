#ifndef OT_UTF8_H
#define OT_UTF8_H
#include <stdbool.h>
#include <stddef.h>
/* Valid Unicode text without embedded NUL; empty text is valid. */
bool ot_utf8_valid(const char *value, size_t size);
#endif
