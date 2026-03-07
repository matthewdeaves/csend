#include "commands.h"
#include "ui_interface.h"
#include "peertalk_bridge.h"
#include "test.h"
#include "clog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Debug state (simple toggle, no old logging system) */
static int g_debug_enabled = 0;

int handle_list_command(app_state_t *state, const char *args)
{
    (void)args;
    if (state->ui) {
        UI_CALL(state->ui, display_peer_list, state);
    }
    return 0;
}

int handle_help_command(app_state_t *state, const char *args)
{
    (void)args;
    if (state->ui) {
        UI_CALL(state->ui, display_help);
    }
    return 0;
}

int is_debug_enabled(void)
{
    return g_debug_enabled;
}

int handle_debug_command(app_state_t *state, const char *args)
{
    (void)args;
    g_debug_enabled = !g_debug_enabled;
    clog_set_level(g_debug_enabled ? CLOG_LVL_DBG : CLOG_LVL_INFO);
    if (state->ui) {
        UI_CALL(state->ui, notify_debug_toggle, g_debug_enabled);
    }
    return 0;
}

int handle_send_command(app_state_t *state, const char *args)
{
    if (!args || strlen(args) == 0) {
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    int peer_num;
    const char *msg_start = strchr(args, ' ');

    if (!msg_start) {
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    char peer_str[32];
    size_t peer_len = (size_t)(msg_start - args);
    if (peer_len >= sizeof(peer_str)) {
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    strncpy(peer_str, args, peer_len);
    peer_str[peer_len] = '\0';
    peer_num = atoi(peer_str);

    if (peer_num <= 0) {
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    msg_start++;

    /* Convert 1-based peer_num to 0-based connected peer index */
    int connected_idx = 0;
    int found = 0;
    int total = PT_GetPeerCount(state->pt_ctx);
    int i;
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(state->pt_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            connected_idx++;
            if (connected_idx == peer_num) {
                bridge_queue_send(i, msg_start);
                found = 1;
                break;
            }
        }
    }

    if (!found) {
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, peer_num, NULL);
        }
    }

    return 0;
}

int handle_broadcast_command(app_state_t *state, const char *args)
{
    if (!args || strlen(args) == 0) {
        if (state->ui) {
            UI_CALL(state->ui, notify_broadcast_result, 0);
        }
        return 0;
    }

    bridge_queue_broadcast(args);
    return 0;
}

int handle_quit_command(app_state_t *state, const char *args)
{
    (void)args;
    CLOG_INFO("Quit command received");

    if (g_state) {
        g_state->running = 0;
    } else {
        state->running = 0;
    }

    return 1;
}

int handle_status_command(app_state_t *state, const char *args)
{
    (void)args;
    if (state->ui && state->ui->ops->notify_status) {
        UI_CALL(state->ui, notify_status, state);
    }
    return 0;
}

int handle_stats_command(app_state_t *state, const char *args)
{
    (void)args;
    if (state->ui && state->ui->ops->notify_stats) {
        UI_CALL(state->ui, notify_stats, state);
    }
    return 0;
}

int handle_history_command(app_state_t *state, const char *args)
{
    int count = 10;
    if (args && strlen(args) > 0) {
        count = atoi(args);
        if (count <= 0) count = 10;
        if (count > 100) count = 100;
    }
    if (state->ui && state->ui->ops->notify_history) {
        UI_CALL(state->ui, notify_history, count);
    }
    return 0;
}

int handle_version_command(app_state_t *state, const char *args)
{
    (void)args;
    if (state->ui && state->ui->ops->notify_version) {
        UI_CALL(state->ui, notify_version);
    }
    return 0;
}

int handle_peers_command(app_state_t *state, const char *args)
{
    return handle_list_command(state, args);
}

int handle_test_command(app_state_t *state, const char *args)
{
    (void)args;
    CLOG_INFO("Starting automated test...");
    run_posix_automated_test(state);
    return 0;
}
