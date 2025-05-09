#include "logging.h"           // For local declarations
#include "../shared/logging.h" // For shared function calls and type definitions
#include "dialog.h"            // For gMainWindow, gMessagesTE, gDialogTEInitialized
#include "dialog_messages.h"   // For AppendToMessagesTE

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifdef __MACOS__
#include <OSUtils.h> // For GetTime, DateTimeRec
#endif

// Buffer sizes are now primarily managed in shared/logging.c
#define MAX_LOG_LINE_LENGTH_MAC_DISPLAY 256 // For constructing message for AppendToMessagesTE

// Guard to prevent re-entrancy if AppendToMessagesTE itself calls log_debug
static Boolean g_classic_mac_logging_to_te_in_progress = false;

void classic_mac_platform_get_timestamp(char *buffer, size_t buffer_size) {
#ifdef __MACOS__
    DateTimeRec dt_rec;
    GetTime(&dt_rec);
    // Ensure we don't overflow buffer; snprintf would be better if available
    // For simplicity with classic Mac, assuming buffer_size is adequate.
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            (int)dt_rec.year, (int)dt_rec.month, (int)dt_rec.day,
            (int)dt_rec.hour, (int)dt_rec.minute, (int)dt_rec.second);
#else
    // Fallback for non-Mac builds, though this file is __MACOS__ specific
    strncpy(buffer, "0000-00-00 00:00:00", buffer_size);
    if (buffer_size > 0) buffer[buffer_size - 1] = '\0';
#endif
}

void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body) {
    char full_ui_message[MAX_LOG_LINE_LENGTH_MAC_DISPLAY];

    if (g_classic_mac_logging_to_te_in_progress) {
        // Avoid recursion if AppendToMessagesTE itself logs something
        return;
    }

    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        g_classic_mac_logging_to_te_in_progress = true;

        // Construct the full message for the TE
        // Again, careful with buffer sizes. sprintf is used for simplicity here.
        sprintf(full_ui_message, "%s%s", timestamp_and_prefix, message_body);
        full_ui_message[MAX_LOG_LINE_LENGTH_MAC_DISPLAY -1] = '\0'; // Ensure null termination

        AppendToMessagesTE(full_ui_message);
        AppendToMessagesTE("\r"); // Add a carriage return for TE display

        g_classic_mac_logging_to_te_in_progress = false;
    }
    // If dialog isn't ready, debug messages to UI are silently dropped.
}

// The log_init, log_shutdown, log_debug (formerly log_internal_message),
// and log_app_event functions are now primarily in shared/logging.c.
// The platform-specific main.c will call the shared log_init with
// pointers to the above callback functions.

// log_to_file_only is removed. If specific messages should *never* go to UI
// even when debug is enabled, that requires a different approach (e.g. log levels,
// or direct calls to a file-writing utility not part of the public log_debug API).
// For now, all log_debug calls are subject to the UI display toggle.
// Calls to the old log_to_file_only should be changed to log_debug.