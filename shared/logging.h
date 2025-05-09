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
#include <stddef.h>
typedef struct {
    void (*get_timestamp)(char *buffer, size_t buffer_size);
    void (*display_debug_log)(const char *timestamp_and_prefix, const char *message_body);
} platform_logging_callbacks_t;
void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks);
void log_shutdown(void);
void log_debug(const char *format, ...);
void log_app_event(const char *format, ...);
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);
#endif
