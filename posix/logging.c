#include "logging.h"
#include "../shared/logging_shared.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
static void _print_log_entry(const char *prefix, const char *format, va_list args) {
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    printf("[%s]%s", time_str, prefix ? prefix : "");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
}
void log_message(const char *format, ...) {
    if (is_debug_output_enabled()) {
        va_list args;
        va_start(args, format);
        _print_log_entry(" [DEBUG] ", format, args);
        va_end(args);
    }
}
void display_user_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    _print_log_entry(" ", format, args);
    va_end(args);
}
