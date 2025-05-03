//====================================
// FILE: ./classic_mac/logging.h
//====================================

#ifndef LOGGING_H
#define LOGGING_H
#include <stdio.h>

#define kLogFileName "csend_log.txt"

extern FILE *gLogFile;

void InitLogFile(void);
void CloseLogFile(void);

// Standard log: logs to file and potentially TE field
void log_message(const char *format, ...);

// File-only log: logs ONLY to file, never to TE field
void log_to_file_only(const char *format, ...); // <-- ADD DECLARATION

#endif // LOGGING_H