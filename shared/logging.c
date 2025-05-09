#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <time.h> // For fallback timestamp if platform doesn't provide one

#define MAX_LOG_LINE_LENGTH 1024
#define USER_MESSAGE_BUFFER_SIZE (MAX_LOG_LINE_LENGTH - 60) // Ample space for timestamp and prefix
#define TIMESTAMP_BUFFER_SIZE 30 // Generous buffer for timestamp

static FILE *g_log_file = NULL;
static platform_logging_callbacks_t g_platform_callbacks;
static Boolean g_callbacks_initialized = false;
static Boolean g_show_debug_output = false;
static char g_log_file_name[256];


// Fallback timestamp function if platform doesn't provide one (should not happen with new design)
static void fallback_get_timestamp(char *buffer, size_t buffer_size) {
    // This is a basic ISO 8601 like format.
    // Platform specific one is preferred.
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

void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks) {
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (callbacks != NULL && callbacks->get_timestamp != NULL) {
        g_platform_callbacks = *callbacks;
        g_callbacks_initialized = true;
    } else {
        // Critical: No timestamp function provided.
        // Logging to file will be impaired. UI logging might still work if display_debug_log is set.
        // For now, we'll use a fallback for file logging, but this indicates a setup error.
        g_platform_callbacks.get_timestamp = fallback_get_timestamp;
        g_platform_callbacks.display_debug_log = (callbacks != NULL) ? callbacks->display_debug_log : NULL;
        g_callbacks_initialized = true; // Partially initialized
        // Consider logging an error here if possible, though the logger itself is what's failing to init.
    }

    if (log_file_name_suggestion) {
        strncpy(g_log_file_name, log_file_name_suggestion, sizeof(g_log_file_name) - 1);
        g_log_file_name[sizeof(g_log_file_name) - 1] = '\0';
    } else {
        // Provide a generic default if suggestion is NULL
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
        // Cannot open log file. Platform might want to show an error.
        // For now, logging to file will silently fail.
        // UI logging might still work.
    }
}

void log_shutdown(void) {
    if (g_log_file != NULL) {
        char timestamp[TIMESTAMP_BUFFER_SIZE];
        if (g_callbacks_initialized && g_platform_callbacks.get_timestamp) {
            g_platform_callbacks.get_timestamp(timestamp, sizeof(timestamp));
        } else {
            fallback_get_timestamp(timestamp, sizeof(timestamp)); // Should not happen if init was ok
        }
        fprintf(g_log_file, "--- [%s] Log Session Ended ---\n\n", timestamp);
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_callbacks_initialized = false;
    memset(&g_platform_callbacks, 0, sizeof(platform_logging_callbacks_t)); // Clear callbacks
}

void log_debug(const char *format, ...) {
    char message_body[USER_MESSAGE_BUFFER_SIZE];
    char timestamp_str[TIMESTAMP_BUFFER_SIZE];
    char full_log_message[MAX_LOG_LINE_LENGTH];
    char timestamp_and_prefix_for_ui[TIMESTAMP_BUFFER_SIZE + 10]; // For "[DEBUG] "
    va_list args;

    va_start(args, format);
#ifdef __MACOS__
    // vsprintf is common on Classic Mac, vsnprintf might not be in all compilers/libs
    vsprintf(message_body, format, args);
#else
    vsnprintf(message_body, USER_MESSAGE_BUFFER_SIZE, format, args);
#endif
    va_end(args);
    message_body[USER_MESSAGE_BUFFER_SIZE - 1] = '\0'; // Ensure null termination

    if (g_callbacks_initialized && g_platform_callbacks.get_timestamp) {
        g_platform_callbacks.get_timestamp(timestamp_str, sizeof(timestamp_str));
    } else {
        fallback_get_timestamp(timestamp_str, sizeof(timestamp_str));
    }

    if (g_log_file != NULL) {
#ifdef __MACOS__
        sprintf(full_log_message, "%s [DEBUG] %s", timestamp_str, message_body);
#else
        snprintf(full_log_message, MAX_LOG_LINE_LENGTH, "%s [DEBUG] %s", timestamp_str, message_body);
#endif
        fprintf(g_log_file, "%s\n", full_log_message);
        fflush(g_log_file);
    }

    if (g_show_debug_output && g_callbacks_initialized && g_platform_callbacks.display_debug_log != NULL) {
#ifdef __MACOS__
        sprintf(timestamp_and_prefix_for_ui, "%s [DEBUG] ", timestamp_str);
#else
        snprintf(timestamp_and_prefix_for_ui, sizeof(timestamp_and_prefix_for_ui), "%s [DEBUG] ", timestamp_str);
#endif
        g_platform_callbacks.display_debug_log(timestamp_and_prefix_for_ui, message_body);
    }
}

void log_app_event(const char *format, ...) {
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
    message_body[USER_MESSAGE_BUFFER_SIZE - 1] = '\0'; // Ensure null termination

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
    // App events typically also go to a user-visible, non-debug part of the UI.
    // This is handled by explicit calls in the application logic (e.g., terminal_display_app_message or AppendToMessagesTE)
    // rather than through the g_platform_callbacks.display_debug_log.
}

void set_debug_output_enabled(Boolean enabled) {
    g_show_debug_output = enabled;
}

Boolean is_debug_output_enabled(void) {
    return g_show_debug_output;
}