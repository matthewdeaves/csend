#ifndef LOGGING_SHARED_H
#define LOGGING_SHARED_H

#ifdef __MACOS__
#include <MacTypes.h> // Provides Boolean
#else
#ifndef __cplusplus
#include <stdbool.h>
#define Boolean bool   // Defines Boolean for POSIX
#endif
#endif

#include <stdarg.h>
#include <stddef.h> // For size_t

// Platform-specific callbacks to be implemented by each platform
typedef struct {
    // Function to get a platform-specific timestamp string.
    void (*get_timestamp)(char *buffer, size_t buffer_size);
    // Function to display a debug log message on the platform's UI (e.g., console, dialog).
    // This can be NULL if no UI debug display is available or desired.
    void (*display_debug_log)(const char *timestamp_and_prefix, const char *message_body);
} platform_logging_callbacks_t;

/**
 * @brief Initializes the shared logging system.
 *
 * @param log_file_name_suggestion A suggested name for the log file. The implementation may alter it.
 * @param callbacks A pointer to a struct containing platform-specific callback functions.
 *                  The get_timestamp callback is mandatory. display_debug_log can be NULL.
 */
void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks);

/**
 * @brief Shuts down the shared logging system and closes the log file.
 */
void log_shutdown(void);

/**
 * @brief Logs a debug message.
 * Writes to the log file. If debug output is enabled and a display_debug_log callback is provided,
 * it also sends the message to the platform's debug UI.
 *
 * @param format The format string for the message (printf-style).
 * @param ... Variable arguments for the format string.
 */
void log_debug(const char *format, ...);

/**
 * @brief Logs a general application event.
 * Writes to the log file only. Does not go to the debug UI display.
 *
 * @param format The format string for the message (printf-style).
 * @param ... Variable arguments for the format string.
 */
void log_app_event(const char *format, ...);

/**
 * @brief Enables or disables debug message output to the platform's UI.
 * File logging is always active for log_debug messages.
 *
 * @param enabled true to enable UI debug output, false to disable.
 */
void set_debug_output_enabled(Boolean enabled);

/**
 * @brief Checks if debug message output to the platform's UI is enabled.
 *
 * @return true if UI debug output is enabled, false otherwise.
 */
Boolean is_debug_output_enabled(void);

#endif // LOGGING_SHARED_H