#ifndef CLASSIC_MAC_LOGGING_H_WRAPPER
#define CLASSIC_MAC_LOGGING_H_WRAPPER
#include "../shared/logging.h"
void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);
void log_to_file_only(const char *format, ...);
#endif
