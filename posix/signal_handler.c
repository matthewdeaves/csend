#include "signal_handler.h"
#include "peer.h"
#include "clog.h"

void handle_signal(int sig)
{
    if (g_state != NULL) {
        g_state->running = 0;
    }
    CLOG_INFO("Received signal %d, initiating shutdown", sig);
}
