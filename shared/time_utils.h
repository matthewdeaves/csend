#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <time.h>
#include <stddef.h>

/* Format current time into buffer using strftime format string */
void format_current_time(char *buffer, size_t size, const char *format);

/* Print timestamp with format to stdout (includes brackets) */
void print_timestamp(const char *format);

/* Get timestamp with fallback for errors */
void get_timestamp_with_fallback(char *buffer, size_t size, const char *format, const char *fallback);

#endif /* TIME_UTILS_H */