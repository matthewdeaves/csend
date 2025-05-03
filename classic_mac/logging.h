#ifndef LOGGING_H
#define LOGGING_H 
#include <stdio.h>
#define kLogFileName "csend_log.txt"
extern FILE *gLogFile;
void InitLogFile(void);
void CloseLogFile(void);
void log_message(const char *format, ...);
#endif
