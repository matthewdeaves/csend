#include "logging.h"
#include "../shared/logging.h"
#include "dialog.h"
#include "dialog_messages.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#ifdef __MACOS__
#include <OSUtils.h>
#endif
#define MAX_LOG_LINE_LENGTH_MAC 1024
#define USER_MESSAGE_BUFFER_SIZE_MAC (MAX_LOG_LINE_LENGTH_MAC - 60)
#define TIMESTAMP_BUFFER_SIZE_MAC 20
static FILE *g_platform_log_file_mac = NULL;
static char g_log_file_name_mac[256];
static Boolean g_classic_mac_logging_to_te_in_progress = false;
static void get_platform_timestamp(char *buffer, size_t buffer_size)
{
#ifdef __MACOS__
    DateTimeRec dt_rec;
    GetTime(&dt_rec);
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            (int)dt_rec.year, (int)dt_rec.month, (int)dt_rec.day,
            (int)dt_rec.hour, (int)dt_rec.minute, (int)dt_rec.second);
#else
    strncpy(buffer, "0000-00-00 00:00:00", buffer_size);
    if (buffer_size > 0) buffer[buffer_size - 1] = '\0';
#endif
}
void log_init(const char *log_file_name_suggestion, platform_display_debug_log_func_t display_func)
{
    if (g_platform_log_file_mac != NULL) {
        fclose(g_platform_log_file_mac);
        g_platform_log_file_mac = NULL;
    }
    if (log_file_name_suggestion) {
        strncpy(g_log_file_name_mac, log_file_name_suggestion, sizeof(g_log_file_name_mac) - 1);
        g_log_file_name_mac[sizeof(g_log_file_name_mac) - 1] = '\0';
    } else {
        strcpy(g_log_file_name_mac, "csend_mac.log");
    }
    g_platform_log_file_mac = fopen(g_log_file_name_mac, "a");
    if (g_platform_log_file_mac != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE_MAC];
        get_platform_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_platform_log_file_mac, "\n--- [%s] Log Session Started ---\n", timestamp);
        fflush(g_platform_log_file_mac);
    }
    shared_logging_set_platform_callback(display_func);
}
void log_shutdown(void)
{
    if (g_platform_log_file_mac != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE_MAC];
        get_platform_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_platform_log_file_mac, "--- [%s] Log Session Ended ---\n\n", timestamp);
        fflush(g_platform_log_file_mac);
        fclose(g_platform_log_file_mac);
        g_platform_log_file_mac = NULL;
    }
    shared_logging_clear_platform_callback();
}
void log_internal_message(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE_MAC];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE_MAC];
    char full_log_message[MAX_LOG_LINE_LENGTH_MAC];
    char timestamp_and_prefix_for_ui[TIMESTAMP_BUFFER_SIZE_MAC + 10];
    va_list args;
    va_start(args, format);
    vsprintf(message_body, format, args);
    va_end(args);
    message_body[USER_MESSAGE_BUFFER_SIZE_MAC - 1] = '\0';
    get_platform_timestamp(timestamp_str, sizeof(timestamp_str));
    if (g_platform_log_file_mac != NULL) {
        sprintf(full_log_message, "%s [DEBUG] %s", timestamp_str, message_body);
        fprintf(g_platform_log_file_mac, "%s\n", full_log_message);
        fflush(g_platform_log_file_mac);
    }
    if (is_debug_output_enabled()) {
        platform_display_debug_log_func_t display_func = shared_logging_get_platform_callback();
        if (display_func != NULL) {
            sprintf(timestamp_and_prefix_for_ui, "%s [DEBUG] ", timestamp_str);
            display_func(timestamp_and_prefix_for_ui, message_body);
        }
    }
}
void log_app_event(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE_MAC];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE_MAC];
    char full_log_message[MAX_LOG_LINE_LENGTH_MAC];
    va_list args;
    va_start(args, format);
    vsprintf(message_body, format, args);
    va_end(args);
    message_body[USER_MESSAGE_BUFFER_SIZE_MAC - 1] = '\0';
    get_platform_timestamp(timestamp_str, sizeof(timestamp_str));
    if (g_platform_log_file_mac != NULL) {
        sprintf(full_log_message, "%s %s", timestamp_str, message_body);
        fprintf(g_platform_log_file_mac, "%s\n", full_log_message);
        fflush(g_platform_log_file_mac);
    }
}
void log_to_file_only(const char *format, ...)
{
    char message_body[USER_MESSAGE_BUFFER_SIZE_MAC];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE_MAC];
    char full_log_message[MAX_LOG_LINE_LENGTH_MAC];
    va_list args;
    va_start(args, format);
    vsprintf(message_body, format, args);
    va_end(args);
    message_body[USER_MESSAGE_BUFFER_SIZE_MAC - 1] = '\0';
    get_platform_timestamp(timestamp_str, sizeof(timestamp_str));
    if (g_platform_log_file_mac != NULL) {
        sprintf(full_log_message, "%s [DEBUG] %s", timestamp_str, message_body);
        fprintf(g_platform_log_file_mac, "%s\n", full_log_message);
        fflush(g_platform_log_file_mac);
    }
}
void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body)
{
    char full_ui_message[MAX_LOG_LINE_LENGTH_MAC];
    if (g_classic_mac_logging_to_te_in_progress) {
        return;
    }
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        g_classic_mac_logging_to_te_in_progress = true;
        sprintf(full_ui_message, "%s%s", timestamp_and_prefix, message_body);
        AppendToMessagesTE(full_ui_message);
        AppendToMessagesTE("\r");
        g_classic_mac_logging_to_te_in_progress = false;
    }
}
