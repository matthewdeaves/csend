#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "../shared/protocol.h"
#include "messaging.h"
#include <Timer.h>

/* External function from dialog.c via messaging path - already exists! */
/* MacTCP uses MacTCP_QueueMessage for ALL message sending */
extern OSErr MacTCP_QueueMessage(const char *peerIPStr, const char *message_content, const char *msg_type);

/* Delay function for Classic Mac */
static void mac_delay_ms(int milliseconds, void *context)
{
    unsigned long finalTicks;
    (void)context;

    /* Convert milliseconds to ticks (1 tick = ~16.67ms) */
    Delay((milliseconds * 60) / 1000, &finalTicks);
}

/* Broadcast callback - REUSES EXISTING APPLICATION CODE */
static int test_send_broadcast(const char *message, void *context)
{
    int i;
    int sent_count = 0;
    int failed_count = 0;
    int total_active_peers;
    OSErr sendErr;

    (void)context;

    total_active_peers = pw_get_active_peer_count();

    if (total_active_peers == 0) {
        log_app_event("Test: No active peers to broadcast to");
        return 0;  /* Not an error, just no peers */
    }

    /* Send to each peer using the SAME queue mechanism the UI uses */
    for (i = 0; i < total_active_peers; i++) {
        peer_t peer;
        pw_get_peer_by_index(i, &peer);

        /* Calls the SAME function the UI uses for sending messages */
        sendErr = MacTCP_QueueMessage(peer.ip, message, MSG_TEXT);

        if (sendErr == noErr) {
            sent_count++;
        } else {
            failed_count++;
            log_app_event("Test broadcast failed for %s@%s: %d",
                         peer.username, peer.ip, sendErr);
        }
    }

    return (failed_count == 0) ? 0 : -1;
}

/* Direct message callback - REUSES EXISTING APPLICATION CODE */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    (void)context;
    /* Calls the SAME function used for direct messages - MacTCP_QueueMessage with MSG_TEXT type */
    return (MacTCP_QueueMessage(peer_ip, message, MSG_TEXT) == noErr) ? 0 : -1;
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

void PerformAutomatedTest(void)
{
    test_config_t config;
    test_callbacks_t callbacks;

    log_app_event("PerformAutomatedTest: Starting automated test");

    /* Get default config */
    config = get_default_test_config();

    /* Set up callbacks */
    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_by_index = test_get_peer_by_index;
    callbacks.delay_func = mac_delay_ms;
    callbacks.context = NULL;

    /* Run test */
    run_automated_test(&config, &callbacks);

    log_app_event("PerformAutomatedTest: Test completed");
}
