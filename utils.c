#include <stdio.h>    /* For printf, fflush, stdout */
#include <stdarg.h>   /* For va_list, va_start, va_end */
#include <time.h>     /* For time_t, time, localtime, strftime */
#include <stddef.h>   /* For NULL */

/*
 * This function creates a timestamped log entry and outputs it to the console.
 * It works similar to printf() but automatically adds a timestamp prefix in the
 * format [HH:MM:SS] and a newline character at the end of each message.
 * The function also ensures immediate output by flushing stdout.
*/
void log_message(const char *format, ...) {
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    
    printf("[%s] ", time_str);
    
    va_list args;
    va_start(args, format);
    // print out the args using the format string, like printf
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}