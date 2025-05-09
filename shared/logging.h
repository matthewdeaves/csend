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
typedef void (*platform_display_debug_log_func_t)(const char *timestamp_and_prefix, const char *message_body);
void log_init(const char *log_file_name_suggestion, platform_display_debug_log_func_t display_func);
void log_shutdown(void);
void log_internal_message(const char *format, ...);
void log_app_event(const char *format, ...);
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);
void shared_logging_set_platform_callback(platform_display_debug_log_func_t display_func);
void shared_logging_clear_platform_callback(void);
platform_display_debug_log_func_t shared_logging_get_platform_callback(void);
#endif
