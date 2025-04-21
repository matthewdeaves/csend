// FILE: ./classic_mac/logging.c
#include "logging.h"
#include "dialog.h" // For AppendToMessagesTE and gDialogTEInitialized

#include <stdarg.h> // For va_list, va_start, va_end
#include <stdio.h>  // For file I/O (fopen, fprintf, fclose, fflush, vsprintf)
#include <Sound.h>  // For SysBeep

// --- Global Variable Definition ---
FILE *gLogFile = NULL; // File pointer for logging

/**
 * @brief Initializes the file logging system.
 */
void InitLogFile(void) {
    gLogFile = fopen(kLogFileName, "w");
    if (gLogFile == NULL) {
        SysBeep(10); // Beep if log file couldn't be opened
    } else {
        fprintf(gLogFile, "--- Log Started ---\n");
        fflush(gLogFile); // Ensure the header is written immediately
    }
}

/**
 * @brief Closes the file logging system.
 */
void CloseLogFile(void) {
    if (gLogFile != NULL) {
        fprintf(gLogFile, "--- Log Ended ---\n");
        fclose(gLogFile);
        gLogFile = NULL;
    }
}

/**
 * @brief Logs a formatted message to the log file and potentially the dialog window.
 */
void log_message(const char *format, ...) {
    char buffer[512]; // Buffer for the formatted message
    va_list args;

    // Format the message using variable arguments
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    // Always log to file if available
    if (gLogFile != NULL) {
        fprintf(gLogFile, "%s\n", buffer);
        fflush(gLogFile); // Ensure log entry is written immediately
    }

    // Append to dialog TE *only* if the dialog and its TEs are fully initialized
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        AppendToMessagesTE(buffer); // Append the main message
        AppendToMessagesTE("\r");    // Append a carriage return for new line in TE
    } else if (gLogFile == NULL) {
        // Optional: Beep only if file logging also failed AND dialog not ready
        // SysBeep(1); // Might be too noisy during initialization
    }
}