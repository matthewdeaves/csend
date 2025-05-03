#include "logging.h"
#include "dialog.h"
#include <stdarg.h>
#include <stdio.h>
#include <Sound.h>
FILE *gLogFile = NULL;
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
void log_message(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);
    if (gLogFile != NULL) {
        fprintf(gLogFile, "%s\n", buffer);
        fflush(gLogFile);
    }
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        AppendToMessagesTE(buffer);
        AppendToMessagesTE("\r");
    } else if (gLogFile == NULL) {
    }
}
