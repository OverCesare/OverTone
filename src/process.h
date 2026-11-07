#ifndef OVERTONE_PROCESS_H
#define OVERTONE_PROCESS_H
#include <stdbool.h>
#include <stdatomic.h>
bool ot_process_executable(const char *path);
/* Worker-only finite command, no shell. Owns/reaps the process group; captures at
 * most 256 KiB. input_fd is borrowed, or -1 for /dev/null. NULL means failure. */
char *ot_process_capture(const char *file, char *const arguments[], double seconds, const atomic_bool *cancel,
                         bool clean_environment, int input_fd);
#endif
