#include "peer.h"
#include "peertalk_bridge.h"
#include "ui_terminal.h"
#include "ui_factory.h"
#include "ui_interface.h"
#include "signal_handler.h"
#include "../shared/test.h"
#include "../shared/common_defs.h"
#include "peertalk.h"
#include "clog.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

/* Global application state */
app_state_t *g_state = NULL;

void init_app_state(app_state_t *state, const char *username)
{
    memset(state, 0, sizeof(app_state_t));
    state->running = 1;
    strncpy(state->username, username, sizeof(state->username) - 1);
    state->username[sizeof(state->username) - 1] = '\0';
}

void cleanup_app_state(app_state_t *state)
{
    (void)state;
}

int main(int argc, char *argv[])
{
    app_state_t state;
    int machine_mode = 0;
    const char *username = "User";
    int i;
    pthread_t input_tid;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--machine-mode") == 0) {
            machine_mode = 1;
        } else {
            username = argv[i];
        }
    }

    /* Initialize logging — write to file, not stderr (which mixes with UI) */
    clog_set_file("csend_posix.log");
    clog_init("csend", CLOG_LVL_INFO);

    /* Initialize app state */
    init_app_state(&state, username);
    g_state = &state;

    /* Set up signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Create UI */
    state.ui = ui_factory_create(machine_mode ? UI_MODE_MACHINE : UI_MODE_INTERACTIVE);
    if (!state.ui) {
        fprintf(stderr, "Failed to create UI\n");
        clog_shutdown();
        return 1;
    }

    UI_CALL(state.ui, notify_startup, state.username);

    /* Initialize PeerTalk */
    PT_Status pt_err = PT_Init(&state.pt_ctx, state.username);
    if (pt_err != PT_OK) {
        fprintf(stderr, "PeerTalk initialization failed: %d\n", (int)pt_err);
        ui_factory_destroy(state.ui);
        clog_shutdown();
        return 1;
    }

    /* Set up peertalk bridge (callbacks, message registration) */
    bridge_init(&state);

    /* Start discovery */
    PT_StartDiscovery(state.pt_ctx);
    CLOG_INFO("Discovery started for '%s'", state.username);

    /* Start input thread */
    if (pthread_create(&input_tid, NULL, user_input_thread, &state) != 0) {
        fprintf(stderr, "Failed to create input thread\n");
        PT_Shutdown(state.pt_ctx);
        ui_factory_destroy(state.ui);
        clog_shutdown();
        return 1;
    }

    /* Main loop: poll peertalk + process command queue + test */
    while (state.running) {
        PT_Poll(state.pt_ctx);
        bridge_process_queue(&state);
        process_automated_test();
        usleep(10000);  /* 10ms = 100Hz */
    }

    /* Clean shutdown */
    CLOG_INFO("Shutting down...");

    pthread_join(input_tid, NULL);

    PT_Shutdown(state.pt_ctx);

    UI_CALL(state.ui, notify_shutdown);
    ui_factory_destroy(state.ui);

    cleanup_app_state(&state);
    g_state = NULL;

    clog_shutdown();
    return 0;
}
