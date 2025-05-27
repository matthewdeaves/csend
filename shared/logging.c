#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#define MAX_LOG_LINE_LENGTH 1024
#define USER_MESSAGE_BUFFER_SIZE (MAX_LOG_LINE_LENGTH - 60)
#define TIMESTAMP_BUFFER_SIZE 30
#define CATEGORY_NAME_SIZE 12

static FILE *g_log_file = NULL;
static platform_logging_callbacks_t g_platform_callbacks;
static Boolean g_callbacks_initialized = false;
static Boolean g_show_debug_output = false;
static char g_log_file_name[256];
static log_level_t g_current_log_level = LOG_LEVEL_DEBUG;

/* Category names for output */
static const char* g_category_names[LOG_CAT_COUNT] = {
    "GENERAL",
    "NETWORKING",
    "DISCOVERY",
    "PEER_MGMT",
    "UI",
    "PROTOCOL",
    "SYSTEM",
    "MESSAGING"
};

/* Level names for output */
static const char* g_level_names[] = {
    "ERROR",
    "WARNING",
    "INFO",
    "DEBUG"
};
static void fallback_get_timestamp(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm *local_time_info;
    time(&now);
    local_time_info = localtime(&now);
    if (local_time_info != NULL) {
        strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", local_time_info);
    } else {
        strncpy(buffer, "0000-00-00 00:00:00", buffer_size);
        if (buffer_size > 0) buffer[buffer_size - 1] = '\0';
    }
}
void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (callbacks != NULL && callbacks->get_timestamp != NULL) {
        g_platform_callbacks = *callbacks;
        g_callbacks_initialized = true;
    } else {
        g_platform_callbacks.get_timestamp = fallback_get_timestamp;
        g_platform_callbacks.display_debug_log = (callbacks != NULL) ? callbacks->display_debug_log : NULL;
        g_callbacks_initialized = true;
    }
    if (log_file_name_suggestion) {
        strncpy(g_log_file_name, log_file_name_suggestion, sizeof(g_log_file_name) - 1);
        g_log_file_name[sizeof(g_log_file_name) - 1] = '\0';
    } else {
#ifdef __MACOS__
        strcpy(g_log_file_name, "app_classic_mac.log");
#else
        strcpy(g_log_file_name, "app_posix.log");
#endif
    }
    g_log_file = fopen(g_log_file_name, "a");
    if (g_log_file != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE];
        g_platform_callbacks.get_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_log_file, "\n--- [%s] Log Session Started ---\n", timestamp);
        fflush(g_log_file);
    } else {
    }
}
void log_shutdown(void)
{
    if (g_log_file != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE];
        if (g_callbacks_initialized && g_platform_callbacks.get_timestamp) {
            g_platform_callbacks.get_timestamp(timestamp, sizeof(timestamp));
        } else {
            fallback_get_timestamp(timestamp, sizeof(timestamp));
        }
        fprintf(g_log_file, "--- [%s] Log Session Ended ---\n\n", timestamp);
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_callbacks_initialized = false;
    memset(&g_platform_callbacks, 0, sizeof(platform_logging_callbacks_t));
}
static void log_message_internal(log_level_t level, log_category_t category, const char *format, va_list args)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE];
    char full_log_message[MAX_LOG_LINE_LENGTH];
    char timestamp_and_prefix_for_ui[TIMESTAMP_BUFFER_SIZE + 30];
    
    /* Check if we should log this message based on level */
    if (level > g_current_log_level) {
        return;
    }
    
#ifdef __MACOS__
    vsprintf(message_body, format, args);
#else
    vsnprintf(message_body, USER_MESSAGE_BUFFER_SIZE, format, args);
#endif
    message_body[USER_MESSAGE_BUFFER_SIZE - 1] = '\0';
    
    if (g_callbacks_initialized && g_platform_callbacks.get_timestamp) {
        g_platform_callbacks.get_timestamp(timestamp_str, sizeof(timestamp_str));
    } else {
        fallback_get_timestamp(timestamp_str, sizeof(timestamp_str));
    }
    
    if (g_log_file != NULL) {
#ifdef __MACOS__
        sprintf(full_log_message, "%s [%s][%s] %s", 
                timestamp_str, g_level_names[level], g_category_names[category], message_body);
#else
        snprintf(full_log_message, MAX_LOG_LINE_LENGTH, "%s [%s][%s] %s", 
                 timestamp_str, g_level_names[level], g_category_names[category], message_body);
#endif
        fprintf(g_log_file, "%s\n", full_log_message);
        fflush(g_log_file);
    }
    
    if (g_show_debug_output && g_callbacks_initialized && g_platform_callbacks.display_debug_log != NULL) {
#ifdef __MACOS__
        sprintf(timestamp_and_prefix_for_ui, "%s [%s][%s] ", 
                timestamp_str, g_level_names[level], g_category_names[category]);
#else
        snprintf(timestamp_and_prefix_for_ui, sizeof(timestamp_and_prefix_for_ui), "%s [%s][%s] ", 
                 timestamp_str, g_level_names[level], g_category_names[category]);
#endif
        g_platform_callbacks.display_debug_log(timestamp_and_prefix_for_ui, message_body);
    }
}

void log_error_cat(log_category_t category, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_message_internal(LOG_LEVEL_ERROR, category, format, args);
    va_end(args);
}

void log_warning_cat(log_category_t category, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_message_internal(LOG_LEVEL_WARNING, category, format, args);
    va_end(args);
}

void log_info_cat(log_category_t category, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_message_internal(LOG_LEVEL_INFO, category, format, args);
    va_end(args);
}

void log_debug_cat(log_category_t category, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_message_internal(LOG_LEVEL_DEBUG, category, format, args);
    va_end(args);
}

void log_app_event(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE];
    char full_log_message[MAX_LOG_LINE_LENGTH];
    va_list args;
    va_start(args, format);
#ifdef __MACOS__
    vsprintf(message_body, format, args);
#else
    vsnprintf(message_body, USER_MESSAGE_BUFFER_SIZE, format, args);
#endif
    va_end(args);
    message_body[USER_MESSAGE_BUFFER_SIZE - 1] = '\0';
    if (g_callbacks_initialized && g_platform_callbacks.get_timestamp) {
        g_platform_callbacks.get_timestamp(timestamp_str, sizeof(timestamp_str));
    } else {
        fallback_get_timestamp(timestamp_str, sizeof(timestamp_str));
    }
    if (g_log_file != NULL) {
#ifdef __MACOS__
        sprintf(full_log_message, "%s %s", timestamp_str, message_body);
#else
        snprintf(full_log_message, MAX_LOG_LINE_LENGTH, "%s %s", timestamp_str, message_body);
#endif
        fprintf(g_log_file, "%s\n", full_log_message);
        fflush(g_log_file);
    }
}
void set_debug_output_enabled(Boolean enabled)
{
    g_show_debug_output = enabled;
}
Boolean is_debug_output_enabled(void)
{
    return g_show_debug_output;
}

void set_log_level(log_level_t level)
{
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_DEBUG) {
        g_current_log_level = level;
    }
}

log_level_t get_log_level(void)
{
    return g_current_log_level;
}
