#ifndef LOGGING_SHARED_H
#define LOGGING_SHARED_H
#ifdef __MACOS__
#include <MacTypes.h>
#else
#ifndef __cplusplus
#include <stdbool.h>
#define Boolean bool
#endif
#endif
#include <stdarg.h>
void log_message(const char *format, ...);
void display_user_message(const char *format, ...);
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);
#endif
