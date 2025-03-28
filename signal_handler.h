#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

// No includes are needed just for the function signature below.
// The implementation in signal_handler.c will include necessary headers (like peer.h for g_state).

/**
 * @brief Signal handler function registered for SIGINT and SIGTERM.
 * Sets the global running flag (g_state->running) to 0 to signal threads
 * to terminate gracefully.
 * @param sig The signal number received (e.g., SIGINT, SIGTERM).
 */
void handle_signal(int sig);

#endif // SIGNAL_HANDLER_H