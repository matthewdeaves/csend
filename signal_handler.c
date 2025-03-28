// Include the header file for this module, which declares the handle_signal function.
#include "signal_handler.h"

// Include peer.h because this module needs access to:
// 1. The definition of the `app_state_t` structure (specifically the `running` member).
// 2. The declaration of the global pointer `g_state` (`extern app_state_t *g_state;`).
#include "peer.h"
// Include utils.h for the `log_message` function to print a message when a signal is caught.
#include "utils.h"

/**
 * @brief Signal handler function registered to catch termination signals (SIGINT, SIGTERM).
 * @details This function is designed to be called asynchronously by the operating system
 *          when the process receives specific signals (like Ctrl+C generating SIGINT, or
 *          the `kill` command generating SIGTERM). Its primary role is to initiate a
 *          graceful shutdown of the application.
 *
 *          It achieves this by setting the globally accessible `running` flag within the
 *          application state (`g_state->running`) to 0. The main loops in the application's
 *          threads (listener, discovery, user input) periodically check this flag. When
 *          they see it has become 0, they exit their loops, allowing the threads to terminate
 *          cleanly.
 *
 *          Using a `volatile sig_atomic_t` type for the `running` flag (as defined in peer.h)
 *          is crucial here:
 *          - `volatile`: Ensures the compiler doesn't optimize away reads of the flag, always
 *                      fetching the current value from memory.
 *          - `sig_atomic_t`: Guarantees that writing 0 to the flag is an atomic operation,
 *                          meaning it cannot be interrupted halfway through by another signal
 *                          or thread access, preventing potential data corruption.
 *
 *          The function also logs a message indicating which signal was received.
 *
 * @param sig The integer identifier of the signal that triggered this handler (e.g., SIGINT, SIGTERM).
 *            This parameter is required by the standard signal handler function signature.
 *
 * @note Signal handlers should be very careful about the functions they call, as only a specific
 *       subset of standard library functions are guaranteed to be "async-signal-safe". While
 *       `log_message` (which uses `printf`, `time`, etc.) might not be strictly async-signal-safe
 *       in all implementations, it's often used in simple examples for informational purposes.
 *       In robust applications, logging from signal handlers might use safer, more direct methods
 *       (like writing directly to a file descriptor). The core safe action here is the assignment
 *       to the `sig_atomic_t` variable.
 */
void handle_signal(int sig) {
    // Check if the global state pointer has been initialized.
    // This prevents a potential NULL pointer dereference if a signal arrives
    // very early during startup before init_app_state() has run.
    if (g_state != NULL) {
        // Set the application's running flag to 0.
        // This assignment is atomic because g_state->running is of type sig_atomic_t.
        // Worker threads checking this flag will detect the change and start shutting down.
        g_state->running = 0;
    } else {
        // Log a warning if the state wasn't ready - shutdown might not be graceful.
        log_message("Warning: Received signal %d before application state was fully initialized.", sig);
        // Consider calling _exit(1) here for immediate termination if state isn't ready?
    }

    // Log a message indicating that a signal was caught and shutdown is commencing.
    // Note the potential async-signal-safety concerns mentioned above regarding log_message.
    log_message("Received signal %d. Initiating graceful shutdown...", sig);

    // It's generally recommended *not* to do extensive work or cleanup directly
    // within a signal handler. The primary job is to set a flag or perform minimal
    // safe actions, letting the main application loops handle the actual shutdown process.
}