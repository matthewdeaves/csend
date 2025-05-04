#include "logging.h"
#include "dialog.h"
#include <stdarg.h>
#include <stdio.h>
#include <Sound.h>
#include <MacTypes.h>
FILE *gLogFile = NULL;
static Boolean gLoggingToTE = false;
static void _write_log_entry(const char *format, va_list args) {
    char buffer[512];
    if (gLogFile == NULL) {
        return;
    }
    vsprintf(buffer, format, args);
    fprintf(gLogFile, "%s\n", buffer);
    fflush(gLogFile);
}
void InitLogFile(void) {
    if (gLogFile != NULL) {
        fclose(gLogFile);
        gLogFile = NULL;
    }
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
void log_message(const char *format, ...) {
    va_list args;
    char buffer[512];
    va_start(args, format);
    _write_log_entry(format, args);
    va_end(args);
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized && !gLoggingToTE) {
        gLoggingToTE = true;
        AppendToMessagesTE(buffer);
        AppendToMessagesTE("\r");
        gLoggingToTE = false;
    } else if (gLogFile == NULL) {
    }
}
void log_to_file_only(const char *format, ...) {
    va_list args;
    va_start(args, format);
    _write_log_entry(format, args);
    va_end(args);
}
