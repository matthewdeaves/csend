#ifndef UTILS_H
#define UTILS_H

// No standard includes are needed just for the function signature below.
// The implementation in utils.c will include necessary headers (like <stdio.h>, <stdarg.h>, <time.h>).

/**
 * @brief Logs a timestamped message to standard output.
 * Acts like printf but prepends [HH:MM:SS] and appends a newline.
 * Uses variable arguments (varargs).
 * @param format The format string (like printf).
 * @param ... Variable arguments corresponding to the format string.
 */
void log_message(const char *format, ...);

#endif // UTILS_H