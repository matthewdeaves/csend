#include "peer.h"
#include "discovery.h"
#include "messaging.h"
#include "ui_terminal.h"
#include "logging.h"             // For POSIX platform-specific callback declarations
#include "../shared/logging.h"   // For shared log_init, log_shutdown, log_debug, log_app_event
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h> // For sigaction in init_app_state, though it's in peer.c

// app_state_t is defined in peer.h, g_state is also there.

int main(int argc, char *argv[]) {
    app_state_t state; // g_state will be set in init_app_state
    pthread_t listener_tid = 0, discovery_tid = 0, input_tid = 0;
    char username[32] = "anonymous";

    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
    }

    // 1. Initialize platform-specific logging callbacks structure
    platform_logging_callbacks_t posix_log_callbacks = {
        .get_timestamp = posix_platform_get_timestamp,
        .display_debug_log = posix_platform_display_debug_log
    };

    // 2. Call shared log_init
    log_init("csend_posix.log", &posix_log_callbacks);
    // set_debug_output_enabled(true); // Uncomment to have debug messages in UI by default

    // 3. Initialize application state (includes signal handlers via g_state)
    // init_app_state itself might call log_debug, so log_init should be called before it.
    init_app_state(&state, username);

    // 4. Log application start
    log_app_event("Starting P2P messaging application as '%s'", state.username);
    terminal_display_app_message("Starting P2P messaging application as '%s'", state.username);

    // 5. Initialize network listeners
    if (init_listener(&state) < 0) {
        // log_debug is used inside init_listener for detailed errors
        log_app_event("Fatal: Failed to initialize TCP listener. Exiting."); // More user-facing summary
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    if (init_discovery(&state) < 0) {
        // log_debug is used inside init_discovery for detailed errors
        log_app_event("Fatal: Failed to initialize UDP discovery. Exiting."); // More user-facing summary
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }

    // 6. Create threads
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);

    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_app_event("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0; // Signal all threads to stop

        // Attempt to join threads that might have started to ensure clean exit
        // It's a bit tricky if some created and others failed.
        // pthread_cancel could be used, but join is generally preferred if threads check 'running'.
        if (input_err == 0 && input_tid != 0) {
             // listener_thread and discovery_thread use select() with timeout,
             // and check state.running. input_thread also uses select().
             // Setting state.running = 0 should make them exit.
             // If they block indefinitely, pthread_cancel might be needed, but that's more complex.
            log_debug("Cancelling input thread due to other thread creation failure.");
            pthread_cancel(input_tid); // Or signal it in a more graceful way if possible
        }
        if (discovery_err == 0 && discovery_tid != 0) {
            log_debug("Cancelling discovery thread due to other thread creation failure.");
            pthread_cancel(discovery_tid);
        }
        if (listener_err == 0 && listener_tid != 0) {
            log_debug("Cancelling listener thread due to other thread creation failure.");
            pthread_cancel(listener_tid);
        }
        
        // Wait for them to actually exit
        if (input_err == 0 && input_tid != 0) pthread_join(input_tid, NULL);
        if (discovery_err == 0 && discovery_tid != 0) pthread_join(discovery_tid, NULL);
        if (listener_err == 0 && listener_tid != 0) pthread_join(listener_tid, NULL);

        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }

    log_debug("All application threads created successfully.");

    // 7. Wait for user input thread to terminate (e.g., on /quit command)
    pthread_join(input_tid, NULL);
    log_debug("User input thread finished. Main thread initiating shutdown...");

    // 8. Signal other threads to stop and wait for them
    // state.running should have been set to 0 by the input_thread or a signal handler
    if (state.running) { // If not already set by quit or signal
        log_debug("Main thread explicitly setting running=0.");
        state.running = 0;
    }

    // The select calls in listener and discovery threads have timeouts,
    // so they will check state.running periodically.
    // Closing sockets can also help unblock them if they are in a blocking recv/accept.
    // This is handled in cleanup_app_state.

    log_debug("Main thread: Waiting for listener thread to join...");
    if (listener_tid != 0) pthread_join(listener_tid, NULL);
    log_debug("Main thread: Listener thread joined.");

    log_debug("Main thread: Waiting for discovery thread to join...");
    if (discovery_tid != 0) pthread_join(discovery_tid, NULL);
    log_debug("Main thread: Discovery thread joined.");

    // 9. Clean up resources
    cleanup_app_state(&state); // This closes sockets, destroys mutex
    log_app_event("Application terminated gracefully.");
    terminal_display_app_message("Application terminated gracefully.");
    log_shutdown(); // Finalize and close the log file

    return EXIT_SUCCESS;
}