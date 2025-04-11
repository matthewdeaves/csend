// FILE: ./posix/logging.h
// Include guard: Prevents the header file from being included multiple times
// within a single compilation unit (.c file). This avoids errors caused by
// redefining functions or types if the header were included via multiple paths.
#ifndef LOGGING_H // If the symbol LOGGING_H is not defined...
#define LOGGING_H // ...then define LOGGING_H.

// --- Includes ---
// No standard library headers (like <stdio.h>, <stdarg.h>, <time.h>) are included here.
// This header file only needs to declare the *signature* of the log_message function.
// The actual *implementation* in logging.c includes the necessary headers to define
// the function's body (using printf, va_list, time functions, etc.).
// Keeping includes out of header files where possible reduces compilation dependencies
// and build times, as files including logging.h won't unnecessarily pull in stdio.h, etc.

// --- Function Declaration ---

/**
 * @brief Logs a formatted, timestamped message to the standard output (console).
 * @details This function provides a convenient way to log messages throughout the application.
 *          It functions similarly to the standard `printf` function but adds two key features:
 *          1.  **Timestamp Prefix:** Automatically prepends the current time in `[HH:MM:SS]` format
 *              to the beginning of each message line.
 *          2.  **Newline Suffix:** Automatically appends a newline character (`\n`) to the end
 *              of each message, ensuring each log entry appears on its own line.
 *          3.  **Flushing:** Automatically flushes `stdout` after printing, ensuring the message
 *              appears immediately on the console.
 *
 *          It uses C's variable arguments mechanism (`...`) to accept a format string and
 *          any number of additional arguments, just like `printf`.
 *
 * @param format A constant character string specifying the format of the output. This string
 *               can contain literal text and format specifiers (e.g., `%s`, `%d`, `%f`) that
 *               correspond to the subsequent variable arguments.
 * @param ...    A variable number of arguments whose types correspond to the format specifiers
 *               used in the `format` string. These arguments will be formatted and inserted
 *               into the output string where the specifiers appear.
 *
 * @note The implementation resides in `logging.c`.
 */
void log_message(const char *format, ...);

#endif // End of the include guard LOGGING_H