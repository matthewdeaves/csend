#include "logging_mac.h"
#include "../logging.h"
#include "dialog.h"
#include "ui/dialog_messages.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#ifdef __MACOS__
#include <OSUtils.h>
#endif
#define MAX_LOG_LINE_LENGTH_MAC_DISPLAY 256
static Boolean g_classic_mac_logging_to_te_in_progress = false;
void classic_mac_platform_get_timestamp(char *buffer, size_t buffer_size)
{
    (void)buffer_size; /* Unused parameter */
#ifdef __MACOS__
    DateTimeRec dt_rec;
    GetTime(&dt_rec);
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
            (int)dt_rec.year, (int)dt_rec.month, (int)dt_rec.day,
            (int)dt_rec.hour, (int)dt_rec.minute, (int)dt_rec.second);
#else
    strncpy(buffer, "0000-00-00 00:00:00", buffer_size);
    if (buffer_size > 0) buffer[buffer_size - 1] = '\0';
#endif
}
void classic_mac_platform_display_debug_log(const char *timestamp_and_prefix, const char *message_body)
{
    char full_ui_message[MAX_LOG_LINE_LENGTH_MAC_DISPLAY];
    if (g_classic_mac_logging_to_te_in_progress) {
        return;
    }
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        g_classic_mac_logging_to_te_in_progress = true;
        sprintf(full_ui_message, "%s%s", timestamp_and_prefix, message_body);
        full_ui_message[MAX_LOG_LINE_LENGTH_MAC_DISPLAY - 1] = '\0';
        AppendToMessagesTE(full_ui_message);
        AppendToMessagesTE("\r");
        g_classic_mac_logging_to_te_in_progress = false;
    }
}
