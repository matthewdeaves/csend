//====================================
// FILE: ./classic_mac/logging.c
//====================================

#include "logging.h"
#include "dialog.h" // Needed for gMainWindow, gMessagesTE, gDialogTEInitialized
#include <stdarg.h>
#include <stdio.h>
#include <Sound.h>
#include <MacTypes.h> // For Boolean

FILE *gLogFile = NULL;

// Re-entrancy guard for logging to TE field
static Boolean gLoggingToTE = false;

void InitLogFile(void) {
    gLogFile = fopen(kLogFileName, "w");
    if (gLogFile == NULL) {
        SysBeep(10);
    } else {
        fprintf(gLogFile, "--- Log Started ---\n");
        fflush(gLogFile);
    }
}

void CloseLogFile(void) {
    if (gLogFile != NULL) {
        fprintf(gLogFile, "--- Log Ended ---\n");
        fclose(gLogFile);
        gLogFile = NULL;
    }
}

// Standard log: logs to file and potentially TE field
void log_message(const char *format, ...) {
    char buffer[512];
    va_list args;

    // Format the message first
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    // Log to file if available
    if (gLogFile != NULL) {
        fprintf(gLogFile, "%s\n", buffer);
        fflush(gLogFile); // Ensure it's written immediately
    }

    // Log to Dialog TE field *only if* initialized AND not already logging to TE
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized && !gLoggingToTE) {
        gLoggingToTE = true; // Set guard
        AppendToMessagesTE(buffer);
        AppendToMessagesTE("\r");
        gLoggingToTE = false; // Release guard
    } else if (gLogFile == NULL) {
        // Optional: Beep if logging is completely unavailable?
        // SysBeep(1);
    }
}


// File-only log: logs ONLY to file, never to TE field
void log_to_file_only(const char *format, ...) { // <-- ADD IMPLEMENTATION
    char buffer[512];
    va_list args;

    // Format the message
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    // Log ONLY to file if available
    if (gLogFile != NULL) {
        fprintf(gLogFile, "%s\n", buffer);
        fflush(gLogFile); // Ensure it's written immediately
    }
    // DO NOT append to TE field here
}