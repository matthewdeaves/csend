#include "logging.h"
#include "dialog.h"
#include "../shared/logging_shared.h"
#include <stdarg.h>
#include <stdio.h>
#include <Sound.h>
#include <MacTypes.h>
FILE *gLogFile = NULL;
static Boolean gLoggingToTEInProgress = false;
static void _write_to_log_file(const char *format, va_list args) {
    char buffer[512];
    if (gLogFile == NULL) {
        gLogFile = fopen(kLogFileName, "a");
        if (gLogFile == NULL) {
            return;
        }
    }
    vsprintf(buffer, format, args);
    fprintf(gLogFile, "%s\n", buffer);
    fflush(gLogFile);
}
static void _write_to_messages_te(const char *text_to_append) {
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized && !gLoggingToTEInProgress) {
        gLoggingToTEInProgress = true;
        AppendToMessagesTE(text_to_append);
        AppendToMessagesTE("\r");
        gLoggingToTEInProgress = false;
    }
}
void InitLogFile(void) {
    if (gLogFile != NULL) {
        fclose(gLogFile);
        gLogFile = NULL;
    }
    gLogFile = fopen(kLogFileName, "a");
    if (gLogFile == NULL) {
        SysBeep(10);
    } else {
        fprintf(gLogFile, "\n--- Log Session Started ---\n");
        fflush(gLogFile);
    }
}
void CloseLogFile(void) {
    if (gLogFile != NULL) {
        fprintf(gLogFile, "--- Log Session Ended ---\n\n");
        fflush(gLogFile);
        fclose(gLogFile);
        gLogFile = NULL;
    }
}
void log_message(const char *format, ...) {
    va_list args_for_file, args_for_te;
    char buffer_for_te[512];
    va_start(args_for_file, format);
    _write_to_log_file(format, args_for_file);
    va_end(args_for_file);
    if (is_debug_output_enabled()) {
        va_start(args_for_te, format);
        vsprintf(buffer_for_te, format, args_for_te);
        va_end(args_for_te);
        _write_to_messages_te(buffer_for_te);
    }
}
void display_user_message(const char *format, ...) {
    va_list args_for_file, args_for_te;
    char buffer_for_te_and_file[512];
    va_start(args_for_te, format);
    vsprintf(buffer_for_te_and_file, format, args_for_te);
    va_end(args_for_te);
    va_start(args_for_file, format);
    _write_to_log_file(format, args_for_file);
    va_end(args_for_file);
    _write_to_messages_te(buffer_for_te_and_file);
}
void log_to_file_only(const char *format, ...) {
    va_list args;
    va_start(args, format);
    _write_to_log_file(format, args);
    va_end(args);
}
