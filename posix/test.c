#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "commands.h"
#include <unistd.h>
#include <string.h>

/* Broadcast callback - REUSES EXISTING APPLICATION CODE */
static int test_send_broadcast(const char *message, void *context)
{
    app_state_t *state = (app_state_t *)context;

    /* Calls the SAME function the UI uses for broadcasts */
    int sent_count = broadcast_to_all_peers(state, message);

    if (sent_count == 0) {
        return -1;
    }

    return 0;
}

/* Direct message callback - REUSES EXISTING APPLICATION CODE */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    app_state_t *state = (app_state_t *)context;
    int peer_count = pw_get_active_peer_count();
    int peer_num = -1;

    /* Find the peer number (1-based) for proper logging */
    for (int i = 0; i < peer_count; i++) {
        peer_t peer;
        pw_get_peer_by_index(i, &peer);
        if (strcmp(peer.ip, peer_ip) == 0) {
            peer_num = i + 1;  /* Convert 0-based index to 1-based peer number */
            break;
        }
    }

    /* Calls the SAME function used for direct messages - send_to_peer */
    /* send_to_peer returns 1 on success, 0 on failure */
    /* Test expects 0 on success, non-zero on failure, so invert the result */
    int result = send_to_peer(state, peer_ip, message, peer_num);
    return result == 1 ? 0 : -1;
}

/* Get peer count callback */
static int test_get_peer_count(void *context)
{
    (void)context;
    return pw_get_active_peer_count();
}

/* Get peer by index callback */
static int test_get_peer_by_index(int index, peer_t *out_peer, void *context)
{
    (void)context;
    pw_get_peer_by_index(index, out_peer);
    return 0;
}

void run_posix_automated_test(app_state_t *state)
{
    test_config_t config;
    test_callbacks_t callbacks;

    log_app_event("========================================");
    log_app_event("Starting automated test...");
    log_app_event("This will send test messages to all peers");
    log_app_event("========================================");

    /* Get default config */
    config = get_default_test_config();

    /* Set up callbacks */
    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_by_index = test_get_peer_by_index;
    callbacks.context = state;

    /* Start the asynchronous test */
    if (start_automated_test(&config, &callbacks) != 0) {
        return; /* Test already running */
    }

    /* Block and process the test until it's done */
    while (is_automated_test_running()) {
        process_automated_test();
        usleep(10000); /* Sleep for 10ms to prevent busy-waiting */
    }

    log_app_event("========================================");
    log_app_event("Automated test completed!");
    log_app_event("Check the log file for detailed results");
    log_app_event("========================================");
}