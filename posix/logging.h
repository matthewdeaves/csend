#ifndef POSIX_LOGGING_H_WRAPPER
#define POSIX_LOGGING_H_WRAPPER
#include "../shared/logging.h"
void posix_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body);
#endif
