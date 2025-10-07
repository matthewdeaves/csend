#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "../shared/protocol.h"
#include "messaging.h"
#include <Timer.h>

/* External function from messaging.c - already exists! */
extern OSErr SendMessageToPeer(const char *targetIP, const char *message, const char *msg_type);

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
    (void)context;
    /* Calls the SAME function the UI uses for broadcasts */
    return (BroadcastMessage(message) == noErr) ? 0 : -1;
}

/* Direct message callback - REUSES EXISTING APPLICATION CODE */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    (void)context;
    /* Calls the SAME function used for direct messages - SendMessageToPeer with MSG_TEXT type */
    return (SendMessageToPeer(peer_ip, message, MSG_TEXT) == noErr) ? 0 : -1;
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
