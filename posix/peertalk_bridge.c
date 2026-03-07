#include "peertalk_bridge.h"
#include "ui_interface.h"
#include "clog.h"
#include "../shared/common_defs.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Command queue - SPSC ring buffer */
#define CMD_QUEUE_SIZE 32

static pending_command_t cmd_queue[CMD_QUEUE_SIZE];
static volatile int cmd_head = 0;
static volatile int cmd_tail = 0;
static pthread_mutex_t cmd_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Application state reference for callbacks */
static app_state_t *g_bridge_state = NULL;

/* Reconnection backoff — track last disconnect time per peer IP */
#define RECONNECT_COOLDOWN_SECS 5
#define MAX_TRACKED_PEERS 32

static struct {
    char ip[48];
    time_t disconnect_time;
} disconnect_tracker[MAX_TRACKED_PEERS];

static void track_disconnect(const char *ip)
{
    int i, oldest = 0;
    time_t oldest_time = 0;

    for (i = 0; i < MAX_TRACKED_PEERS; i++) {
        if (disconnect_tracker[i].ip[0] == '\0' ||
            strcmp(disconnect_tracker[i].ip, ip) == 0) {
            strncpy(disconnect_tracker[i].ip, ip, 47);
            disconnect_tracker[i].ip[47] = '\0';
            disconnect_tracker[i].disconnect_time = time(NULL);
            return;
        }
        if (oldest_time == 0 || disconnect_tracker[i].disconnect_time < oldest_time) {
            oldest_time = disconnect_tracker[i].disconnect_time;
            oldest = i;
        }
    }
    /* Evict oldest */
    strncpy(disconnect_tracker[oldest].ip, ip, 47);
    disconnect_tracker[oldest].ip[47] = '\0';
    disconnect_tracker[oldest].disconnect_time = time(NULL);
}

static int should_reconnect(const char *ip)
{
    int i;
    time_t now = time(NULL);
    for (i = 0; i < MAX_TRACKED_PEERS; i++) {
        if (strcmp(disconnect_tracker[i].ip, ip) == 0) {
            if (now - disconnect_tracker[i].disconnect_time < RECONNECT_COOLDOWN_SECS) {
                CLOG_INFO("Skipping reconnect to %s (cooldown)", ip);
                return 0;
            }
            return 1;
        }
    }
    return 1;
}

/* --- Peertalk callbacks --- */

static void on_peer_discovered(PT_Peer *peer, void *user_data)
{
    app_state_t *state = (app_state_t *)user_data;
    CLOG_INFO("Discovered peer: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));

    /* Auto-connect to discovered peers with backoff */
    PT_PeerState ps = PT_GetPeerState(peer);
    if (ps == PT_PEER_DISCOVERED || ps == PT_PEER_DISCONNECTED) {
        if (should_reconnect(PT_PeerAddress(peer))) {
            PT_Connect(state->pt_ctx, peer);
        }
    }
}

static void on_peer_lost(PT_Peer *peer, void *user_data)
{
    app_state_t *state = (app_state_t *)user_data;
    CLOG_INFO("Lost peer: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));
    if (state->ui) {
        UI_CALL(state->ui, notify_peer_update);
    }
}

static void on_connected(PT_Peer *peer, void *user_data)
{
    app_state_t *state = (app_state_t *)user_data;
    CLOG_INFO("Connected to peer: %s (%s)", PT_PeerName(peer), PT_PeerAddress(peer));
    if (state->ui) {
        UI_CALL(state->ui, notify_peer_update);
    }
}

static void on_disconnected(PT_Peer *peer, PT_DisconnectReason reason, void *user_data)
{
    app_state_t *state = (app_state_t *)user_data;
    const char *reason_str = "unknown";
    switch (reason) {
    case PT_QUIT:
        reason_str = "quit";
        break;
    case PT_TIMEOUT:
        reason_str = "timeout";
        break;
    case PT_DISCONNECT_ERROR:
        reason_str = "error";
        break;
    }
    CLOG_INFO("Disconnected from peer: %s (%s) reason=%s",
              PT_PeerName(peer), PT_PeerAddress(peer), reason_str);
    track_disconnect(PT_PeerAddress(peer));
    if (state->ui) {
        UI_CALL(state->ui, notify_peer_update);
    }
}

static void on_chat_message(PT_Peer *peer, const void *data, size_t len, void *user_data)
{
    app_state_t *state = (app_state_t *)user_data;
    char msg_buf[BUFFER_SIZE];
    size_t copy_len = len < sizeof(msg_buf) - 1 ? len : sizeof(msg_buf) - 1;
    memcpy(msg_buf, data, copy_len);
    msg_buf[copy_len] = '\0';

    CLOG_INFO("Message from %s: %s", PT_PeerName(peer), msg_buf);

    if (state->ui) {
        UI_CALL(state->ui, display_message,
                PT_PeerName(peer), PT_PeerAddress(peer), msg_buf);
    }
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

/* --- Public API --- */

void bridge_init(app_state_t *state)
{
    g_bridge_state = state;

    PT_RegisterMessage(state->pt_ctx, MSG_CHAT, PT_RELIABLE);

    PT_OnPeerDiscovered(state->pt_ctx, on_peer_discovered, state);
    PT_OnPeerLost(state->pt_ctx, on_peer_lost, state);
    PT_OnConnected(state->pt_ctx, on_connected, state);
    PT_OnDisconnected(state->pt_ctx, on_disconnected, state);
    PT_OnMessage(state->pt_ctx, MSG_CHAT, on_chat_message, state);
    PT_OnError(state->pt_ctx, on_error, state);
}

void bridge_queue_send(int peer_index, const char *msg)
{
    pthread_mutex_lock(&cmd_mutex);
    int next = (cmd_head + 1) % CMD_QUEUE_SIZE;
    if (next != cmd_tail) {
        cmd_queue[cmd_head].type = CMD_SEND;
        cmd_queue[cmd_head].peer_index = peer_index;
        strncpy(cmd_queue[cmd_head].message, msg, BUFFER_SIZE - 1);
        cmd_queue[cmd_head].message[BUFFER_SIZE - 1] = '\0';
        cmd_head = next;
    } else {
        CLOG_WARN("Command queue full, dropping send");
    }
    pthread_mutex_unlock(&cmd_mutex);
}

void bridge_queue_broadcast(const char *msg)
{
    pthread_mutex_lock(&cmd_mutex);
    int next = (cmd_head + 1) % CMD_QUEUE_SIZE;
    if (next != cmd_tail) {
        cmd_queue[cmd_head].type = CMD_BROADCAST;
        cmd_queue[cmd_head].peer_index = -1;
        strncpy(cmd_queue[cmd_head].message, msg, BUFFER_SIZE - 1);
        cmd_queue[cmd_head].message[BUFFER_SIZE - 1] = '\0';
        cmd_head = next;
    } else {
        CLOG_WARN("Command queue full, dropping broadcast");
    }
    pthread_mutex_unlock(&cmd_mutex);
}

void bridge_process_queue(app_state_t *state)
{
    while (cmd_tail != cmd_head) {
        pending_command_t cmd;
        pthread_mutex_lock(&cmd_mutex);
        if (cmd_tail == cmd_head) {
            pthread_mutex_unlock(&cmd_mutex);
            break;
        }
        cmd = cmd_queue[cmd_tail];
        cmd_tail = (cmd_tail + 1) % CMD_QUEUE_SIZE;
        pthread_mutex_unlock(&cmd_mutex);

        switch (cmd.type) {
        case CMD_SEND: {
            PT_Peer *peer = PT_GetPeer(state->pt_ctx, cmd.peer_index);
            if (peer && PT_GetPeerState(peer) == PT_PEER_CONNECTED) {
                PT_Status st = PT_Send(state->pt_ctx, peer, MSG_CHAT,
                                       cmd.message, strlen(cmd.message));
                if (state->ui) {
                    UI_CALL(state->ui, notify_send_result,
                            st == PT_OK ? 1 : 0,
                            cmd.peer_index + 1,
                            PT_PeerAddress(peer));
                }
            } else {
                if (state->ui) {
                    UI_CALL(state->ui, notify_send_result, 0, cmd.peer_index + 1, "");
                }
            }
            break;
        }
        case CMD_BROADCAST: {
            PT_Status st = PT_Broadcast(state->pt_ctx, MSG_CHAT,
                                        cmd.message, strlen(cmd.message));
            int count = 0;
            if (st == PT_OK) {
                int i, total = PT_GetPeerCount(state->pt_ctx);
                for (i = 0; i < total; i++) {
                    PT_Peer *p = PT_GetPeer(state->pt_ctx, i);
                    if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
                        count++;
                    }
                }
            }
            if (state->ui) {
                UI_CALL(state->ui, notify_broadcast_result, count);
            }
            break;
        }
        case CMD_QUIT:
            state->running = 0;
            break;
        }
    }
}

int bridge_get_peer_count(app_state_t *state)
{
    int count = 0;
    int i, total = PT_GetPeerCount(state->pt_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(state->pt_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            count++;
        }
    }
    return count;
}
