#ifndef LOGGING_SHARED_H
#define LOGGING_SHARED_H

#ifdef __MACOS__
#include <MacTypes.h> // For Boolean
#else
#ifndef __cplusplus
#include <stdbool.h> // For bool
#define Boolean bool // Define Boolean for POSIX if not C++
#endif
#endif

// Include stdarg.h for functions using "..." as a good practice,
// though only implementations strictly need va_list etc.
#include <stdarg.h>

// Shared logging functions, to be implemented by each platform
void log_message(const char *format, ...);
void display_user_message(const char *format, ...);

// Functions implemented in shared/logging.c
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);

#endif /* LOGGING_SHARED_H */