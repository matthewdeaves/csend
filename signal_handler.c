#include "signal_handler.h" // Declaration for handle_signal

#include "peer.h"           // Needed for the definition of app_state_t and the declaration of g_state
#include "utils.h"          // Needed for log_message

// Used to catch SIGINT and SIGTERM signals and perform graceful shutdown via g_state used in a loop
void handle_signal(int sig) {
    if (g_state) {
        g_state->running = 0;
    }
    log_message("Received signal %d. Shutting down...", sig);
}