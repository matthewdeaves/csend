#include "peer.h"
#include "discovery.h"
#include "messaging.h"
#include "ui_terminal.h"
#include "ui_interface.h"
#include "ui_factory.h"
#include "logging.h"
#include "signal_handler.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
int main(int argc, char *argv[])
{
    app_state_t state;
    pthread_t listener_tid = 0, discovery_tid = 0, input_tid = 0;
    char username[32] = "anonymous";
    int machine_mode = 0;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--machine-mode") == 0) {
            machine_mode = 1;
        } else if (i == 1 || (i == 2 && machine_mode)) {
            /* Username is first non-flag argument */
            strncpy(username, argv[i], sizeof(username) - 1);
            username[sizeof(username) - 1] = '\0';
        }
    }
    platform_logging_callbacks_t posix_log_callbacks = {
        .get_timestamp = posix_platform_get_timestamp,
        .display_debug_log = posix_platform_display_debug_log
    };
    log_init("csend_posix.log", &posix_log_callbacks);
    init_app_state(&state, username);

    /* Set up signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);  /* Ignore SIGPIPE to prevent crashes on broken pipes */

    /* Create UI based on mode */
    state.ui = ui_factory_create(machine_mode ? UI_MODE_MACHINE : UI_MODE_INTERACTIVE);
    if (!state.ui) {
        fprintf(stderr, "Fatal: Failed to create UI interface\n");
        return EXIT_FAILURE;
    }

    /* Notify UI of startup */
    UI_CALL(state.ui, notify_startup, state.username);
    log_app_event("Starting P2P messaging application as '%s'", state.username);
    if (init_listener(&state) < 0) {
        log_app_event("Fatal: Failed to initialize TCP listener. Exiting.");
        ui_factory_destroy(state.ui);
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    if (init_discovery(&state) < 0) {
        log_app_event("Fatal: Failed to initialize UDP discovery. Exiting.");
        ui_factory_destroy(state.ui);
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);
    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_app_event("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0;
        if (input_err == 0 && input_tid != 0) {
            log_error_cat(LOG_CAT_SYSTEM, "Cancelling input thread due to other thread creation failure.");
            pthread_cancel(input_tid);
        }
        if (discovery_err == 0 && discovery_tid != 0) {
            log_error_cat(LOG_CAT_SYSTEM, "Cancelling discovery thread due to other thread creation failure.");
            pthread_cancel(discovery_tid);
        }
        if (listener_err == 0 && listener_tid != 0) {
            log_error_cat(LOG_CAT_SYSTEM, "Cancelling listener thread due to other thread creation failure.");
            pthread_cancel(listener_tid);
        }
        if (input_err == 0 && input_tid != 0) pthread_join(input_tid, NULL);
        if (discovery_err == 0 && discovery_tid != 0) pthread_join(discovery_tid, NULL);
        if (listener_err == 0 && listener_tid != 0) pthread_join(listener_tid, NULL);
        cleanup_app_state(&state);
        log_shutdown();
        return EXIT_FAILURE;
    }
    log_info_cat(LOG_CAT_SYSTEM, "All application threads created successfully.");
    pthread_join(input_tid, NULL);
    log_info_cat(LOG_CAT_SYSTEM, "User input thread finished. Main thread initiating shutdown...");
    if (state.running) {
        log_debug_cat(LOG_CAT_SYSTEM, "Main thread explicitly setting running=0.");
        state.running = 0;
    }
    log_debug_cat(LOG_CAT_SYSTEM, "Main thread: Waiting for listener thread to join...");
    if (listener_tid != 0) pthread_join(listener_tid, NULL);
    log_debug_cat(LOG_CAT_SYSTEM, "Main thread: Listener thread joined.");
    log_debug_cat(LOG_CAT_SYSTEM, "Main thread: Waiting for discovery thread to join...");
    if (discovery_tid != 0) pthread_join(discovery_tid, NULL);
    log_debug_cat(LOG_CAT_SYSTEM, "Main thread: Discovery thread joined.");
    cleanup_app_state(&state);

    /* Notify UI of shutdown */
    if (state.ui) {
        UI_CALL(state.ui, notify_shutdown);
        ui_factory_destroy(state.ui);
    }

    log_app_event("Application terminated gracefully.");
    log_shutdown();
    return EXIT_SUCCESS;
}
