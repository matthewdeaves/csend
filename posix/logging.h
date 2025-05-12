#ifndef POSIX_LOGGING_H
#define POSIX_LOGGING_H 
#include <stddef.h>
void posix_platform_get_timestamp(char *buffer, size_t buffer_size);
void posix_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);
#endif
