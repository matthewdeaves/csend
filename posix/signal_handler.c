#include "signal_handler.h"
#include "peer.h"
#include "../shared/logging.h"
void handle_signal(int sig)
{
    if (g_state != NULL) {
        g_state->running = 0;
    } else {
        log_warning_cat(LOG_CAT_SYSTEM, "Warning: Received signal %d before application state was fully initialized.", sig);
    }
    log_info_cat(LOG_CAT_SYSTEM, "Received signal %d. Initiating graceful shutdown...", sig);
}
