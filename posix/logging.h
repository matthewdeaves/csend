#ifndef POSIX_LOGGING_H
#define POSIX_LOGGING_H

#include <stddef.h> // For size_t

// Platform-specific implementation for getting a timestamp.
void posix_platform_get_timestamp(char *buffer, size_t buffer_size);

// Platform-specific implementation for displaying a debug log message on the terminal.
void posix_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);

// No need for posix_log_init or posix_log_shutdown declarations here,
// as main.c will call the shared log_init/log_shutdown directly.
// These functions are the callbacks themselves.

#endif // POSIX_LOGGING_H