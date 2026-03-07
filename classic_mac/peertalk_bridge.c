#include "peertalk_bridge.h"
#include "dialog.h"
#include "clog.h"
#include "../shared/common_defs.h"
#include <Sound.h>
#include <OSUtils.h>
#include <stdio.h>
#include <string.h>

Boolean gPeerListNeedsRefresh = false;

/* Reconnection backoff — track last disconnect time per peer IP */
#define RECONNECT_COOLDOWN_TICKS (5 * 60)  /* 5 seconds at 60 ticks/sec */
#define MAX_TRACKED_PEERS 32

static struct {
    char ip[48];
    unsigned long disconnect_ticks;
} disconnect_tracker[MAX_TRACKED_PEERS];

static void track_disconnect(const char *ip)
{
    int i, oldest = 0;
    unsigned long oldest_time = 0xFFFFFFFF;

    for (i = 0; i < MAX_TRACKED_PEERS; i++) {
        if (disconnect_tracker[i].ip[0] == '\0' ||
            strcmp(disconnect_tracker[i].ip, ip) == 0) {
            strncpy(disconnect_tracker[i].ip, ip, 47);
            disconnect_tracker[i].ip[47] = '\0';
            disconnect_tracker[i].disconnect_ticks = TickCount();
            return;
        }
        if (disconnect_tracker[i].disconnect_ticks < oldest_time) {
            oldest_time = disconnect_tracker[i].disconnect_ticks;
            oldest = i;
        }
    }
    strncpy(disconnect_tracker[oldest].ip, ip, 47);
    disconnect_tracker[oldest].ip[47] = '\0';
    disconnect_tracker[oldest].disconnect_ticks = TickCount();
}

static int should_reconnect(const char *ip)
{
    int i;
    unsigned long now = TickCount();
    for (i = 0; i < MAX_TRACKED_PEERS; i++) {
        if (strcmp(disconnect_tracker[i].ip, ip) == 0) {
            if (now - disconnect_tracker[i].disconnect_ticks < RECONNECT_COOLDOWN_TICKS) {
                CLOG_INFO("Skipping reconnect to %s (cooldown)", ip);
                return 0;
            }
            return 1;
        }
    }
    return 1;
}

static void on_peer_discovered(PT_Peer *peer, void *user_data)
{
    PT_Context *ctx = (PT_Context *)user_data;
    CLOG_INFO("Discovered: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));

    /* Auto-connect with backoff */
    PT_PeerState ps = PT_GetPeerState(peer);
    if (ps == PT_PEER_DISCOVERED || ps == PT_PEER_DISCONNECTED) {
        if (should_reconnect(PT_PeerAddress(peer))) {
            PT_Connect(ctx, peer);
        }
    }
}

static void on_peer_lost(PT_Peer *peer, void *user_data)
{
    (void)user_data;
    CLOG_INFO("Lost: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));
    gPeerListNeedsRefresh = true;
}

static void on_connected(PT_Peer *peer, void *user_data)
{
    (void)user_data;
    CLOG_INFO("Connected: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));
    if (IsDebugEnabled()) {
        char msg[128];
        sprintf(msg, "%s connected.\r", PT_PeerName(peer));
        AppendToMessagesTE(msg);
    }
    gPeerListNeedsRefresh = true;
}

static void on_disconnected(PT_Peer *peer, PT_DisconnectReason reason, void *user_data)
{
    (void)user_data;
    CLOG_INFO("Disconnected: %s (reason=%d)", PT_PeerName(peer), (int)reason);
    track_disconnect(PT_PeerAddress(peer));
    if (gMainWindow != NULL && IsDebugEnabled()) {
        char msg[128];
        sprintf(msg, "%s disconnected.\r", PT_PeerName(peer));
        AppendToMessagesTE(msg);
    }
    gPeerListNeedsRefresh = true;
}

static void on_chat_message(PT_Peer *peer, const void *data, size_t len, void *user_data)
{
    char msg_buf[BUFFER_SIZE];
    char display[BUFFER_SIZE + 64];
    size_t copy_len;
    (void)user_data;

    copy_len = len < sizeof(msg_buf) - 1 ? len : sizeof(msg_buf) - 1;
    memcpy(msg_buf, data, copy_len);
    msg_buf[copy_len] = '\0';

    sprintf(display, "%s: %s\r", PT_PeerName(peer), msg_buf);
    AppendToMessagesTE(display);
    SysBeep(1);
}

static void on_error(PT_Peer *peer, PT_Status error, const char *description, void *user_data)
{
    (void)user_data;
    if (peer) {
        CLOG_ERR("PeerTalk error %d for %s: %s", (int)error, PT_PeerName(peer), description);
    } else {
        CLOG_ERR("PeerTalk error %d: %s", (int)error, description);
    }
}

void bridge_mac_init(PT_Context *ctx)
{
    PT_RegisterMessage(ctx, MSG_CHAT, PT_RELIABLE);

    PT_OnPeerDiscovered(ctx, on_peer_discovered, ctx);
    PT_OnPeerLost(ctx, on_peer_lost, ctx);
    PT_OnConnected(ctx, on_connected, ctx);
    PT_OnDisconnected(ctx, on_disconnected, ctx);
    PT_OnMessage(ctx, MSG_CHAT, on_chat_message, ctx);
    PT_OnError(ctx, on_error, ctx);
}
