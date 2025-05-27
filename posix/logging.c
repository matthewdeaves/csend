#include "logging.h"
#include "../shared/logging.h"
#include "../shared/time_utils.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
void posix_platform_get_timestamp(char *buffer, size_t buffer_size)
{
    format_current_time(buffer, buffer_size, "%Y-%m-%d %H:%M:%S");
}
void posix_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body)
{
    printf("%s%s\n", timestamp_and_prefix, message_body);
    fflush(stdout);
}
