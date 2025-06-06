#include "time_utils.h"
#include <stdio.h>
#include <string.h>

void format_current_time(char *buffer, size_t size, const char *format)
{
    time_t now;
    struct tm *local_time_info;

    if (!buffer || size == 0) return;

    time(&now);
    local_time_info = localtime(&now);

    if (local_time_info != NULL) {
        strftime(buffer, size, format, local_time_info);
    } else {
        /* Fallback for error cases */
        if (strcmp(format, "%H:%M:%S") == 0) {
            strncpy(buffer, "--:--:--", size);
        } else {
            strncpy(buffer, "0000-00-00 00:00:00", size);
        }
        if (size > 0) buffer[size - 1] = '\0';
    }
}

void print_timestamp(const char *format)
{
    char time_str[64];
    format_current_time(time_str, sizeof(time_str), format);
    printf("[%s] ", time_str);
}

void get_timestamp_with_fallback(char *buffer, size_t size, const char *format, const char *fallback)
{
    time_t now;
    struct tm *local_time_info;

    if (!buffer || size == 0) return;

    time(&now);
    local_time_info = localtime(&now);

    if (local_time_info != NULL) {
        strftime(buffer, size, format, local_time_info);
    } else {
        strncpy(buffer, fallback, size);
        if (size > 0) buffer[size - 1] = '\0';
    }
}