#include "test.h"
#include "../shared/test.h"
#include "clog.h"
#include "peertalk.h"
#include "../shared/common_defs.h"
#include <string.h>

extern PT_Context *g_ctx;

static int test_send_broadcast(const char *message, void *context)
{
    (void)context;
    return (PT_Broadcast(g_ctx, MSG_CHAT, message, strlen(message)) == PT_OK) ? 0 : -1;
}

static int test_send_direct(int peer_index, const char *message, void *context)
{
    int i, total, connected;
    (void)context;

    connected = 0;
    total = PT_GetPeerCount(g_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(g_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            if (connected == peer_index) {
                return (PT_Send(g_ctx, p, MSG_CHAT, message, strlen(message)) == PT_OK) ? 0 : -1;
            }
            connected++;
        }
    }
    return -1;
}

static int test_get_peer_count(void *context)
{
    int i, total, count;
    (void)context;

    count = 0;
    total = PT_GetPeerCount(g_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(g_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) count++;
    }
    return count;
}

static int test_get_peer_info(int index, char *name_buf, size_t name_size,
                              char *addr_buf, size_t addr_size, void *context)
{
    int i, total, connected;
    (void)context;

    connected = 0;
    total = PT_GetPeerCount(g_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(g_ctx, i);
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

void PerformAutomatedTest(void)
{
    test_config_t config;
    static test_callbacks_t callbacks;

    CLOG_INFO("PerformAutomatedTest: starting async test...");

    config = get_default_test_config();

    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_info = test_get_peer_info;
    callbacks.context = NULL;

    start_automated_test(&config, &callbacks);
}
