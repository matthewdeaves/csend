#include "signal_handler.h"
#include "peer.h"
#include "logging.h"
void handle_signal(int sig)
{
    if (g_state != NULL) {
        g_state->running = 0;
    } else {
        log_internal_message("Warning: Received signal %d before application state was fully initialized.", sig);
    }
    log_internal_message("Received signal %d. Initiating graceful shutdown...", sig);
}
