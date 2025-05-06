#include "logging.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
void log_message(const char *format, ...)
{
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    printf("[%s] ", time_str);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}
