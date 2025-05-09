#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#define MAX_LOG_LINE_LENGTH_POSIX 1024
#define USER_MESSAGE_BUFFER_SIZE_POSIX (MAX_LOG_LINE_LENGTH_POSIX - 60)
#define TIMESTAMP_BUFFER_SIZE_POSIX 20
static FILE *g_platform_log_file_posix = NULL;
static char g_log_file_name_posix[256];
static void get_platform_timestamp(char *buffer, size_t buffer_size)
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
void posix_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body)
{
    printf("%s%s\n", timestamp_and_prefix, message_body);
    fflush(stdout);
}
void log_init(const char *log_file_name_suggestion, platform_display_debug_log_func_t display_func)
{
    if (g_platform_log_file_posix != NULL) {
        fclose(g_platform_log_file_posix);
        g_platform_log_file_posix = NULL;
    }
    if (log_file_name_suggestion) {
        strncpy(g_log_file_name_posix, log_file_name_suggestion, sizeof(g_log_file_name_posix) - 1);
        g_log_file_name_posix[sizeof(g_log_file_name_posix) - 1] = '\0';
    } else {
        strcpy(g_log_file_name_posix, "csend_posix.log");
    }
    g_platform_log_file_posix = fopen(g_log_file_name_posix, "a");
    if (g_platform_log_file_posix == NULL) {
        perror("Error opening log file");
    } else {
        char timestamp[TIMESTAMP_BUFFER_SIZE_POSIX];
        get_platform_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_platform_log_file_posix, "\n--- [%s] Log Session Started ---\n", timestamp);
        fflush(g_platform_log_file_posix);
    }
    shared_logging_set_platform_callback(display_func);
}
void log_shutdown(void)
{
    if (g_platform_log_file_posix != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE_POSIX];
        get_platform_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_platform_log_file_posix, "--- [%s] Log Session Ended ---\n\n", timestamp);
        fflush(g_platform_log_file_posix);
        fclose(g_platform_log_file_posix);
        g_platform_log_file_posix = NULL;
    }
    shared_logging_clear_platform_callback();
}
void log_internal_message(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE_POSIX];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE_POSIX];
    char full_log_message[MAX_LOG_LINE_LENGTH_POSIX];
    char timestamp_and_prefix_for_ui[TIMESTAMP_BUFFER_SIZE_POSIX + 10];
    va_list args;
    va_start(args, format);
    vsnprintf(message_body, USER_MESSAGE_BUFFER_SIZE_POSIX, format, args);
    va_end(args);
    get_platform_timestamp(timestamp_str, sizeof(timestamp_str));
    if (g_platform_log_file_posix != NULL) {
        snprintf(full_log_message, MAX_LOG_LINE_LENGTH_POSIX, "%s [DEBUG] %s", timestamp_str, message_body);
        fprintf(g_platform_log_file_posix, "%s\n", full_log_message);
        fflush(g_platform_log_file_posix);
    }
    if (is_debug_output_enabled()) {
        platform_display_debug_log_func_t display_func = shared_logging_get_platform_callback();
        if (display_func != NULL) {
            snprintf(timestamp_and_prefix_for_ui, sizeof(timestamp_and_prefix_for_ui), "%s [DEBUG] ", timestamp_str);
            display_func(timestamp_and_prefix_for_ui, message_body);
        }
    }
}
void log_app_event(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE_POSIX];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE_POSIX];
    char full_log_message[MAX_LOG_LINE_LENGTH_POSIX];
    va_list args;
    va_start(args, format);
    vsnprintf(message_body, USER_MESSAGE_BUFFER_SIZE_POSIX, format, args);
    va_end(args);
    get_platform_timestamp(timestamp_str, sizeof(timestamp_str));
    if (g_platform_log_file_posix != NULL) {
        snprintf(full_log_message, MAX_LOG_LINE_LENGTH_POSIX, "%s %s", timestamp_str, message_body);
        fprintf(g_platform_log_file_posix, "%s\n", full_log_message);
        fflush(g_platform_log_file_posix);
    }
}
