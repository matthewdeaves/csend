/*
 * Classic Mac Test Implementation for P2P Messenger
 *
 * This module implements the automated test feature for the Classic Mac
 * platform, providing a bridge between the cross-platform test logic
 * and Classic Mac-specific messaging and timing functions.
 */

#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "../shared/protocol.h"
#include "messaging.h"

/*
 * TEST BROADCAST MESSAGE CALLBACK
 *
 * This function implements broadcast messaging for the test system by
 * deliberately reusing the same messaging code paths that the UI uses.
 */
static int test_send_broadcast(const char *message, void *context)
{
    int i;                     /* Loop counter for peer iteration */
    int failed_count = 0;      /* Failed message attempts */
    int total_active_peers;    /* Current number of discovered peers */
    OSErr sendErr;             /* MacTCP operation result */

    (void)context;             /* Context not used in Classic Mac implementation */

    total_active_peers = pw_get_active_peer_count();

    if (total_active_peers == 0) {
        log_app_event("Test: No active peers to broadcast to");
        return 0;  /* Success - no peers to message is valid state */
    }

    for (i = 0; i < total_active_peers; i++) {
        peer_t peer;  /* Peer information structure */
        pw_get_peer_by_index(i, &peer);

        /* CRITICAL: Use SAME messaging function as UI */
        sendErr = MacTCP_QueueMessage(peer.ip, message, MSG_TEXT);

        if (sendErr != noErr) {
            failed_count++;
            log_app_event("Test broadcast failed for %s@%s: %d",
                         peer.username, peer.ip, sendErr);
        }
    }

    return (failed_count == 0) ? 0 : -1;
}

/*
 * TEST DIRECT MESSAGE CALLBACK
 *
 * Implements direct (peer-to-peer) messaging for the test system using
 * the same messaging infrastructure as the UI.
 */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */

    /* CRITICAL: Use identical messaging function as UI */
    return (MacTCP_QueueMessage(peer_ip, message, MSG_TEXT) == noErr) ? 0 : -1;
}

/*
 * TEST PEER COUNT CALLBACK
 *
 * Returns the current number of active peers for test planning.
 */
static int test_get_peer_count(void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */
    return pw_get_active_peer_count();
}

/*
 * TEST PEER ENUMERATION CALLBACK
 *
 * Retrieves peer information by index for test targeting.
 */
static int test_get_peer_by_index(int index, peer_t *out_peer, void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */
    pw_get_peer_by_index(index, out_peer);
    return 0;
}

/*
 * MAIN TEST ENTRY POINT FOR CLASSIC MAC
 *
 * This function is called from the UI (File menu "Perform Test") to
 * kick off the asynchronous automated test sequence.
 */
void PerformAutomatedTest(void)
{
    test_config_t config;
    static test_callbacks_t callbacks; /* Use static to ensure lifetime */

    log_app_event("PerformAutomatedTest: Kicking off asynchronous test...");

    config = get_default_test_config();

    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_by_index = test_get_peer_by_index;
    callbacks.context = NULL;

    /*
     * EXECUTE SHARED TEST LOGIC ASYNCHRONOUSLY
     *
     * This function returns immediately. The test is processed in the background
     * via calls to process_automated_test() from the main event loop.
     */
    start_automated_test(&config, &callbacks);
}