#ifndef CLASSIC_MAC_LOGGING_H
#define CLASSIC_MAC_LOGGING_H

#include <stddef.h> // For size_t

// Platform-specific implementation for getting a timestamp.
void classic_mac_platform_get_timestamp(char *buffer, size_t buffer_size);

// Platform-specific implementation for displaying a debug log message in the dialog.
void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);

// This function is no longer needed as its purpose is covered by log_debug
// and the debug UI display toggle. If truly file-only debug messages are needed,
// that's a more advanced logging feature (e.g., log levels).
// void log_debug(const char *format, ...);

#endif // CLASSIC_MAC_LOGGING_H
