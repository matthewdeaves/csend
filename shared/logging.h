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

/* Log levels */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARNING = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/* Log categories */
typedef enum {
    LOG_CAT_GENERAL = 0,
    LOG_CAT_NETWORKING,
    LOG_CAT_DISCOVERY,
    LOG_CAT_PEER_MGMT,
    LOG_CAT_UI,
    LOG_CAT_PROTOCOL,
    LOG_CAT_SYSTEM,
    LOG_CAT_MESSAGING,
    LOG_CAT_COUNT  /* Must be last */
} log_category_t;

typedef struct {
    void (*get_timestamp)(char *buffer, size_t buffer_size);
    void (*display_debug_log)(const char *timestamp_and_prefix, const char *message_body);
} platform_logging_callbacks_t;

void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks);
void log_shutdown(void);

/* Categorized logging functions - always specify a category */
void log_error_cat(log_category_t category, const char *format, ...);
void log_warning_cat(log_category_t category, const char *format, ...);
void log_info_cat(log_category_t category, const char *format, ...);
void log_debug_cat(log_category_t category, const char *format, ...);

/* User-facing app events (no level/category needed) */
void log_app_event(const char *format, ...);

/* Configuration */
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);
void set_log_level(log_level_t level);
log_level_t get_log_level(void);

#endif
