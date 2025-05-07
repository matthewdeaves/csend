#ifndef LOGGING_H
#define LOGGING_H 
#include <stdio.h>
#define kLogFileName "csend_log.txt"
extern FILE *gLogFile;
void InitLogFile(void);
void CloseLogFile(void);
void log_message(const char *format, ...);
void display_user_message(const char *format, ...);
void log_to_file_only(const char *format, ...);
#endif
