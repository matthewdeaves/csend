// FILE: ./classic_mac/logging.h
#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h> // For FILE type

// --- Constants ---
#define kLogFileName "csend_log.txt" // Name for the log file

// --- Global Variables ---
extern FILE *gLogFile; // File pointer for logging

// --- Function Prototypes ---

/**
 * @brief Initializes the file logging system.
 * @details Opens the log file specified by kLogFileName in write mode.
 *          If opening fails, it may trigger a system beep.
 */
void InitLogFile(void);

/**
 * @brief Closes the file logging system.
 * @details Writes a final message to the log file and closes it.
 */
void CloseLogFile(void);

/**
 * @brief Logs a formatted message to the log file and potentially the dialog window.
 * @details This function formats the message like printf. It always attempts to write
 *          to the log file (if open). If the main dialog and its message TextEdit field
 *          are initialized (checked via gDialogTEInitialized), it also appends the
 *          message to the dialog's message area using AppendToMessagesTE().
 * @param format A printf-style format string.
 * @param ... Variable arguments corresponding to the format string.
 */
void log_message(const char *format, ...);

#endif // LOGGING_H