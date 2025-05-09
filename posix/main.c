#include "peer.h"
#include "discovery.h"
#include "messaging.h"
#include "ui_terminal.h"
#include "logging.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
int main(int argc, char *argv[])
{
    app_state_t state;
    pthread_t listener_tid, discovery_tid, input_tid;
    char username[32] = "anonymous";
    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
    }
    init_app_state(&state, username);
    log_init("csend_posix.log", posix_platform_display_debug_log);
    log_app_event("Starting P2P messaging application as '%s'", state.username);
    terminal_display_app_message("Starting P2P messaging application as '%s'", state.username);
    if (init_listener(&state) < 0) {
        log_internal_message("Fatal: Failed to initialize TCP listener. Exiting.");
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    if (init_discovery(&state) < 0) {
        log_internal_message("Fatal: Failed to initialize UDP discovery. Exiting.");
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);
    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_internal_message("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0;
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    log_internal_message("Threads created successfully.");
    pthread_join(input_tid, NULL);
    log_internal_message("User input thread finished. Initiating shutdown...");
    state.running = 0;
    log_internal_message("Waiting for listener thread to finish...");
    pthread_join(listener_tid, NULL);
    log_internal_message("Listener thread joined.");
    log_internal_message("Waiting for discovery thread to finish...");
    pthread_join(discovery_tid, NULL);
    log_internal_message("Discovery thread joined.");
    cleanup_app_state(&state);
    log_app_event("Application terminated gracefully.");
    terminal_display_app_message("Application terminated gracefully.");
    log_shutdown();
    return EXIT_SUCCESS;
}
