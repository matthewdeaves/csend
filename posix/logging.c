#include "logging.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
void posix_platform_get_timestamp(char *buffer, size_t buffer_size)
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
