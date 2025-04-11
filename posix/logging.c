// FILE: ./posix/logging.c
// Include the header file for this module, which declares the log_message function.
#include "logging.h" // Changed from "logging.h"

// --- Standard Library Includes ---
// Provides standard input/output functions, specifically:
// - vprintf: Used to print formatted output using a va_list argument list, similar to printf.
// - printf: Used to print the timestamp prefix and the final newline.
// - stdout: Standard output stream, used with fflush.
// - fflush: Used to ensure the log message is immediately visible on the console.
#include <stdio.h>
// Provides support for handling variable argument lists (variadic functions), needed because
// log_message accepts a variable number of arguments like printf. Includes:
// - va_list: Type to hold information about the variable arguments.
// - va_start: Macro to initialize a va_list.
// - va_end: Macro to clean up a va_list.
#include <stdarg.h>
// Provides time-related functions and types, needed to generate the timestamp prefix. Includes:
// - time_t: Type representing calendar time.
// - time(): Function to get the current calendar time.
// - localtime(): Function to convert a time_t value to a broken-down time structure representing local time.
// - strftime(): Function to format a time structure into a string according to a specified format.
#include <time.h>

/**
 * @brief Logs a timestamped message to the standard output (console).
 * @details This function behaves similarly to `printf` but automatically prepends a
 *          timestamp in the format `[HH:MM:SS]` to the beginning of the message and
 *          appends a newline character (`\n`) at the end. It uses variadic arguments
 *          (`...`) to accept a format string and a variable number of arguments, just
 *          like `printf`. The output is immediately flushed to ensure visibility,
 *          which is useful for real-time logging in potentially multi-threaded applications.
 *
 * @param format A `printf`-style format string specifying how the subsequent arguments
 *               are interpreted and formatted.
 * @param ...    A variable number of arguments corresponding to the format specifiers
 *               in the `format` string.
 *
 * @note This function currently logs directly to `stdout`. In more complex applications,
 *       logging might be directed to files, network sockets, or managed by a dedicated
 *       logging library for more features (e.g., log levels, rotation).
 */
void log_message(const char *format, ...) {
    // --- Timestamp Generation ---
    // Get the current calendar time as a time_t value.
    time_t now = time(NULL);
    // Buffer to hold the formatted timestamp string (e.g., "14:35:02").
    // Size 20 is more than enough for "HH:MM:SS" plus null terminator and potential padding.
    char time_str[20];
    // Format the current local time into the time_str buffer.
    // "%H": Hour (24-hour clock) as a zero-padded decimal number.
    // "%M": Minute as a zero-padded decimal number.
    // "%S": Second as a zero-padded decimal number.
    // `localtime(&now)` converts the time_t `now` into a struct tm representing local time.
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    // --- Print Timestamp Prefix ---
    // Print the formatted timestamp enclosed in square brackets, followed by a space.
    printf("[%s] ", time_str);

    // --- Process Variadic Arguments ---
    // Declare a variable of type va_list to hold the argument list information.
    va_list args;
    // Initialize `args` to point to the first variable argument.
    // `format` is the last named argument before the ellipsis (...).
    va_start(args, format);
    // Print the variable arguments according to the format string.
    // `vprintf` is the variant of `printf` that takes a `va_list` instead of `...`.
    vprintf(format, args);
    // Clean up the va_list variable. This must be called after processing the arguments.
    va_end(args);

    // --- Finalize Output ---
    // Print a newline character to end the log message line.
    printf("\n");
    // Flush the standard output stream. This forces the operating system to write
    // any buffered output for stdout immediately to the console. It ensures that
    // log messages appear promptly, especially important if the program crashes
    // or in multi-threaded scenarios where output order might otherwise be unpredictable.
    fflush(stdout);
}
