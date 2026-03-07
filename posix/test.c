#include "test.h"
#include "../shared/test.h"
#include "peertalk_bridge.h"
#include "clog.h"
#include <unistd.h>
#include <string.h>

/* Broadcast callback - queues broadcast via bridge */
static int test_send_broadcast(const char *message, void *context)
{
    (void)context;
    bridge_queue_broadcast(message);
    return 0;
}

/* Direct message callback - queues send via bridge using peer index */
static int test_send_direct(int peer_index, const char *message, void *context)
{
    (void)context;
    bridge_queue_send(peer_index, message);
    return 0;
}

/* Get connected peer count */
static int test_get_peer_count(void *context)
{
    app_state_t *state = (app_state_t *)context;
    return bridge_get_peer_count(state);
}

/* Get peer info by connected index */
static int test_get_peer_info(int index, char *name_buf, size_t name_size,
                              char *addr_buf, size_t addr_size, void *context)
{
    app_state_t *state = (app_state_t *)context;
    int i, total, connected = 0;

    total = PT_GetPeerCount(state->pt_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(state->pt_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            if (connected == index) {
                strncpy(name_buf, PT_PeerName(p), name_size - 1);
                name_buf[name_size - 1] = '\0';
                strncpy(addr_buf, PT_PeerAddress(p), addr_size - 1);
                addr_buf[addr_size - 1] = '\0';
                return 0;
            }
            connected++;
        }
    }
    return -1;
}

void run_posix_automated_test(app_state_t *state)
{
    test_config_t config;
    test_callbacks_t callbacks;

    CLOG_INFO("========================================");
    CLOG_INFO("Starting automated test...");
    CLOG_INFO("========================================");

    config = get_default_test_config();

    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_info = test_get_peer_info;
    callbacks.context = state;

    if (start_automated_test(&config, &callbacks) != 0) {
        return;
    }

    while (is_automated_test_running()) {
        process_automated_test();
        usleep(10000);
    }

    CLOG_INFO("========================================");
    CLOG_INFO("Automated test completed!");
    CLOG_INFO("========================================");
}
