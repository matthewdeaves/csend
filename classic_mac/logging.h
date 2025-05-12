#ifndef CLASSIC_MAC_LOGGING_H
#define CLASSIC_MAC_LOGGING_H 
#include <stddef.h>
void classic_mac_platform_get_timestamp(char *buffer, size_t buffer_size);
void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);
#endif
