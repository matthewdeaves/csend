#include "ui_terminal.h"
#include "network.h"
#include "logging.h"
#include "../shared/protocol.h"
#include "../shared/logging.h"
#include "messaging.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
void print_help_message(void)
{
    printf("\nCommands:\n");
    printf("  /list                       - List all active peers\n");
    printf("  /send <peer_number> <msg> - Send <msg> to a specific peer from the list\n");
    printf("  /broadcast <message>        - Send <message> to all active peers\n");
    printf("  /debug                      - Toggle detailed debug message visibility\n");
    printf("  /quit                       - Send quit notification and exit the application\n");
    printf("  /help                       - Show this help message\n\n");
    fflush(stdout);
}
void print_peers(app_state_t *state)
{
    pthread_mutex_lock(&state->peers_mutex);
    time_t now = time(NULL);
    int active_count = 0;
    printf("\n--- Active Peers ---\n");
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active) {
            if (difftime(now, state->peer_manager.peers[i].last_seen) > PEER_TIMEOUT) {
                log_internal_message("Peer %s@%s timed out (detected in print_peers).",
                                     state->peer_manager.peers[i].username,
                                     state->peer_manager.peers[i].ip);
                state->peer_manager.peers[i].active = 0;
                continue;
            }
            printf("%d. %s@%s (last seen %ld seconds ago)\n",
                   ++active_count,
                   state->peer_manager.peers[i].username,
                   state->peer_manager.peers[i].ip,
                   (long)(now - state->peer_manager.peers[i].last_seen));
        }
    }
    if (active_count == 0) {
        printf("No active peers found.\n");
    }
    printf("--------------------\n\n");
    pthread_mutex_unlock(&state->peers_mutex);
    fflush(stdout);
}
int handle_command(app_state_t *state, const char *input)
{
    if (strcmp(input, "/list") == 0) {
        print_peers(state);
        return 0;
    } else if (strcmp(input, "/help") == 0) {
        print_help_message();
        return 0;
    } else if (strcmp(input, "/debug") == 0) {
        Boolean current_debug_state = is_debug_output_enabled();
        set_debug_output_enabled(!current_debug_state);
        log_app_event("Debug output %s.", is_debug_output_enabled() ? "ENABLED" : "DISABLED");
        terminal_display_app_message("Debug output %s.", is_debug_output_enabled() ? "ENABLED" : "DISABLED");
        return 0;
    } else if (strncmp(input, "/send ", 6) == 0) {
        int peer_num_input;
        char *msg_start;
        char input_copy[BUFFER_SIZE];
        strncpy(input_copy, input, BUFFER_SIZE - 1);
        input_copy[BUFFER_SIZE - 1] = '\0';
        msg_start = strchr(input_copy + 6, ' ');
        if (msg_start == NULL) {
            log_app_event("Usage: /send <peer_number> <message>");
            terminal_display_app_message("Usage: /send <peer_number> <message>");
            return 0;
        }
        *msg_start = '\0';
        peer_num_input = atoi(input_copy + 6);
        msg_start++;
        if (peer_num_input <= 0) {
            log_app_event("Invalid peer number. Use /list to see active peers.");
            terminal_display_app_message("Invalid peer number. Use /list to see active peers.");
            return 0;
        }
        pthread_mutex_lock(&state->peers_mutex);
        int current_peer_index = 0;
        int found = 0;
        char target_ip[INET_ADDRSTRLEN];
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peer_manager.peers[i].active &&
                    (difftime(time(NULL), state->peer_manager.peers[i].last_seen) <= PEER_TIMEOUT)) {
                current_peer_index++;
                if (current_peer_index == peer_num_input) {
                    strncpy(target_ip, state->peer_manager.peers[i].ip, INET_ADDRSTRLEN - 1);
                    target_ip[INET_ADDRSTRLEN - 1] = '\0';
                    found = 1;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&state->peers_mutex);
        if (found) {
            if (send_message(target_ip, msg_start, MSG_TEXT, state->username) < 0) {
                log_internal_message("Failed to send message to %s", target_ip);
            } else {
                log_app_event("Message sent to peer %d (%s)", peer_num_input, target_ip);
                terminal_display_app_message("Message sent to peer %d (%s)", peer_num_input, target_ip);
            }
        } else {
            log_app_event("Invalid peer number '%d'. Use /list to see active peers.", peer_num_input);
            terminal_display_app_message("Invalid peer number '%d'. Use /list to see active peers.", peer_num_input);
        }
        return 0;
    } else if (strncmp(input, "/broadcast ", 11) == 0) {
        const char *message_content = input + 11;
        log_app_event("Broadcasting message: %s", message_content);
        terminal_display_app_message("Broadcasting message: %s", message_content);
        pthread_mutex_lock(&state->peers_mutex);
        int sent_count = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peer_manager.peers[i].active &&
                    (difftime(time(NULL), state->peer_manager.peers[i].last_seen) <= PEER_TIMEOUT)) {
                if (send_message(state->peer_manager.peers[i].ip, message_content, MSG_TEXT, state->username) < 0) {
                    log_internal_message("Failed to send broadcast message to %s", state->peer_manager.peers[i].ip);
                } else {
                    sent_count++;
                }
            }
        }
        pthread_mutex_unlock(&state->peers_mutex);
        log_app_event("Broadcast message sent to %d active peer(s).", sent_count);
        terminal_display_app_message("Broadcast message sent to %d active peer(s).", sent_count);
        return 0;
    } else if (strcmp(input, "/quit") == 0) {
        log_internal_message("Initiating quit sequence...");
        pthread_mutex_lock(&state->peers_mutex);
        log_internal_message("Sending QUIT notifications to peers...");
        int notify_count = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peer_manager.peers[i].active) {
                if (send_message(state->peer_manager.peers[i].ip, "", MSG_QUIT, state->username) < 0) {
                    log_internal_message("Failed to send quit notification to %s", state->peer_manager.peers[i].ip);
                } else {
                    notify_count++;
                }
            }
        }
        pthread_mutex_unlock(&state->peers_mutex);
        log_internal_message("Quit notifications sent to %d peer(s).", notify_count);
        if (g_state) {
            g_state->running = 0;
        } else {
            state->running = 0;
        }
        log_internal_message("Exiting application via /quit command...");
        return 1;
    } else {
        log_app_event("Unknown command: '%s'. Type /help for available commands.", input);
        terminal_display_app_message("Unknown command: '%s'. Type /help for available commands.", input);
        return 0;
    }
}
void *user_input_thread(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    char input[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;
    print_help_message();
    printf("> ");
    fflush(stdout);
    while (state->running) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int activity = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                perror("select error in user input thread");
                break;
            }
        }
        if (!state->running) {
            break;
        }
        if (activity == 0) {
            continue;
        }
        if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
            if (state->running) {
                if (feof(stdin)) {
                    log_internal_message("EOF detected on stdin. Exiting input loop.");
                    if (g_state) g_state->running = 0;
                    else state->running = 0;
                } else {
                    perror("Error reading input from stdin");
                }
            }
            break;
        }
        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }
        if (handle_command(state, input) == 1) {
            break;
        }
        printf("> ");
        fflush(stdout);
    }
    log_internal_message("User input thread stopped.");
    return NULL;
}
void terminal_display_app_message(const char *format, ...)
{
    va_list args;
    time_t now = time(NULL);
    char time_str[20];
    struct tm *local_time_info = localtime(&now);
    if (local_time_info) {
        strftime(time_str, sizeof(time_str), "%H:%M:%S", local_time_info);
        printf("[%s] ", time_str);
    } else {
        printf("[--:--:--] ");
    }
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}
