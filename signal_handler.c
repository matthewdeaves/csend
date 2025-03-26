#include "peer.h"    // For g_state declaration
#include "utils.h"   // For log_message function

// Used to catch SIGINT and SIGTERM signals and perform graceful shutdown via g_state used in a loop
void handle_signal(int sig) {
    if (g_state) {
        g_state->running = 0;
    }
    log_message("Received signal %d. Shutting down...", sig);
}