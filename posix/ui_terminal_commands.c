#include "ui_terminal_commands.h"
#include "ui_interface.h"
#include "network.h"
#include "logging.h"
#include "messaging.h"
#include "../shared/protocol.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>

/* Command handlers implementation */

int handle_list_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    if (state->ui) {
        UI_CALL(state->ui, display_peer_list, state);
    }
    return 0;
}

int handle_help_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    if (state->ui) {
        UI_CALL(state->ui, display_help);
    }
    return 0;
}

int handle_debug_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    Boolean current_debug_state = is_debug_output_enabled();
    set_debug_output_enabled(!current_debug_state);
    log_app_event("Debug output %s.", is_debug_output_enabled() ? "ENABLED" : "DISABLED");
    if (state->ui) {
        UI_CALL(state->ui, notify_debug_toggle, is_debug_output_enabled());
    }
    return 0;
}

int handle_send_command(app_state_t *state, const char *args)
{
    if (!args || strlen(args) == 0) {
        log_app_event("Usage: /send <peer_number> <message>");
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    /* Parse peer number and message */
    int peer_num;
    const char *msg_start = strchr(args, ' ');

    if (!msg_start) {
        log_app_event("Usage: /send <peer_number> <message>");
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    /* Extract peer number */
    char peer_str[32];
    size_t peer_len = msg_start - args;
    if (peer_len >= sizeof(peer_str)) {
        log_app_event("Invalid peer number format.");
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    strncpy(peer_str, args, peer_len);
    peer_str[peer_len] = '\0';

    if (!parse_peer_number(peer_str, &peer_num)) {
        log_app_event("Invalid peer number. Use /list to see active peers.");
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
        return 0;
    }

    /* Skip the space to get to the message */
    msg_start++;

    /* Find the peer and send message */
    char target_ip[INET_ADDRSTRLEN];
    if (find_peer_by_number(state, peer_num, target_ip, sizeof(target_ip))) {
        send_to_peer(state, target_ip, msg_start, peer_num);
    } else {
        log_app_event("Invalid peer number '%d'. Use /list to see active peers.", peer_num);
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
        }
    }

    return 0;
}

int handle_broadcast_command(app_state_t *state, const char *args)
{
    if (!args || strlen(args) == 0) {
        log_app_event("Usage: /broadcast <message>");
        if (state->ui) {
            UI_CALL(state->ui, notify_broadcast_result, 0);
        }
        return 0;
    }

    log_app_event("Broadcasting message: %s", args);
    int sent_count = broadcast_to_all_peers(state, args);

    log_app_event("Broadcast message sent to %d active peer(s).", sent_count);
    if (state->ui) {
        UI_CALL(state->ui, notify_broadcast_result, sent_count);
    }

    return 0;
}

int handle_quit_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */

    log_info_cat(LOG_CAT_SYSTEM, "Initiating quit sequence...");
    notify_peers_on_quit(state);

    if (g_state) {
        g_state->running = 0;
    } else {
        state->running = 0;
    }

    log_info_cat(LOG_CAT_SYSTEM, "Exiting application via /quit command...");
    return 1; /* Signal quit */
}

int handle_status_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    if (state->ui && state->ui->ops->notify_status) {
        UI_CALL(state->ui, notify_status, state);
    }
    return 0;
}

int handle_stats_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    if (state->ui && state->ui->ops->notify_stats) {
        UI_CALL(state->ui, notify_stats, state);
    }
    return 0;
}

int handle_history_command(app_state_t *state, const char *args)
{
    int count = 10; /* Default to 10 messages */

    if (args && strlen(args) > 0) {
        count = atoi(args);
        if (count <= 0) count = 10;
        if (count > 100) count = 100; /* Cap at 100 */
    }

    if (state->ui && state->ui->ops->notify_history) {
        UI_CALL(state->ui, notify_history, count);
    }
    return 0;
}

int handle_version_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused */
    if (state->ui && state->ui->ops->notify_version) {
        UI_CALL(state->ui, notify_version);
    }
    return 0;
}

int handle_peers_command(app_state_t *state, const char *args)
{
    /* Check if it's the filter variant */
    if (args && strncmp(args, "--filter ", 9) == 0) {
        /* TODO: Implement peer filtering */
        log_app_event("Peer filtering not yet implemented.");
        if (state->ui) {
            UI_CALL(state->ui, notify_command_unknown, "/peers --filter");
        }
    } else {
        /* Just list peers normally */
        return handle_list_command(state, args);
    }
    return 0;
}

/* Helper functions implementation */

int parse_peer_number(const char *input, int *peer_num)
{
    if (!input || !peer_num) return 0;

    *peer_num = atoi(input);
    return (*peer_num > 0);
}

int find_peer_by_number(app_state_t *state, int peer_num, char *target_ip, size_t ip_size)
{
    if (!state || !target_ip || ip_size < INET_ADDRSTRLEN) return 0;

    pthread_mutex_lock(&state->peers_mutex);

    int current_peer_index = 0;
    int found = 0;

    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active &&
                (difftime(time(NULL), state->peer_manager.peers[i].last_seen) <= PEER_TIMEOUT)) {
            current_peer_index++;
            if (current_peer_index == peer_num) {
                strncpy(target_ip, state->peer_manager.peers[i].ip, ip_size - 1);
                target_ip[ip_size - 1] = '\0';
                found = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&state->peers_mutex);
    return found;
}

int send_to_peer(app_state_t *state, const char *target_ip, const char *message, int peer_num)
{
    if (send_message(target_ip, message, MSG_TEXT, state->username) < 0) {
        log_error_cat(LOG_CAT_MESSAGING, "Failed to send message to %s", target_ip);
        if (state->ui) {
            UI_CALL(state->ui, notify_send_result, 0, peer_num, target_ip);
        }
        return 0;
    }

    log_app_event("Message sent to peer %d (%s)", peer_num, target_ip);
    if (state->ui) {
        UI_CALL(state->ui, notify_send_result, 1, peer_num, target_ip);
    }
    return 1;
}

int broadcast_to_all_peers(app_state_t *state, const char *message)
{
    pthread_mutex_lock(&state->peers_mutex);

    int sent_count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active &&
                (difftime(time(NULL), state->peer_manager.peers[i].last_seen) <= PEER_TIMEOUT)) {
            if (send_message(state->peer_manager.peers[i].ip, message, MSG_TEXT, state->username) >= 0) {
                sent_count++;
            } else {
                log_error_cat(LOG_CAT_MESSAGING, "Failed to send broadcast message to %s",
                              state->peer_manager.peers[i].ip);
            }
        }
    }

    pthread_mutex_unlock(&state->peers_mutex);
    return sent_count;
}

void notify_peers_on_quit(app_state_t *state)
{
    pthread_mutex_lock(&state->peers_mutex);

    log_info_cat(LOG_CAT_MESSAGING, "Sending QUIT notifications to peers...");
    int notify_count = 0;

    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active) {
            if (send_message(state->peer_manager.peers[i].ip, "", MSG_QUIT, state->username) >= 0) {
                notify_count++;
            } else {
                log_error_cat(LOG_CAT_MESSAGING, "Failed to send quit notification to %s",
                              state->peer_manager.peers[i].ip);
            }
        }
    }

    pthread_mutex_unlock(&state->peers_mutex);
    log_info_cat(LOG_CAT_MESSAGING, "Quit notifications sent to %d peer(s).", notify_count);
}