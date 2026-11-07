#ifndef OVERTONE_DIAGNOSTICS_H
#define OVERTONE_DIAGNOSTICS_H
/* One bounded local request per connection. Callback returns an owned reply,
 * at most OT_DIAGNOSTICS_MAX_BYTES, and synchronizes with the application owner.
 * Inspection is read-only; the desktop adapter gates named controls on developer mode. */
#define OT_DIAGNOSTICS_MAX_BYTES 32768
#define OT_DIAGNOSTICS_REQUEST_BYTES 256
typedef struct ot_diagnostics ot_diagnostics;
ot_diagnostics *ot_diagnostics_open(const char *profile, char *(*request)(void *, const char *), void *context);
void ot_diagnostics_close(ot_diagnostics *server);
int ot_inspect(const char *profile);
int ot_control(const char *profile, const char *command);
#endif
