#include "ui_terminal.h"
#include "ui_interface.h"
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

/* Legacy function for compatibility - redirects to UI interface */
void print_help_message(void)
{
    /* This is called from places that don't have UI context */
    /* For now, just print basic help */
    printf("\nCommands:\n");
    printf("  /list                     - List all active peers\n");
    printf("  /send <peer_number> <msg> - Send <msg> to a specific peer from the list\n");
    printf("  /broadcast <message>      - Send <message> to all active peers\n");
    printf("  /debug                    - Toggle detailed debug message visibility\n");
    printf("  /quit                     - Send quit notification and exit the application\n");
    printf("  /help                     - Show this help message\n\n");
    fflush(stdout);
}

/* Legacy function for compatibility */
void print_peers(app_state_t *state)
{
    if (state->ui) {
        UI_CALL(state->ui, display_peer_list, state);
    }
}

/* Handle command - returns 1 if quit command, 0 otherwise */
int handle_command(app_state_t *state, const char *input)
{
    if (!state || !input) return 0;
    
    /* Notify UI of command start */
    if (state->ui) {
        UI_CALL(state->ui, handle_command_start, input);
    }
    
    int result = 0;
    
    if (strcmp(input, "/list") == 0) {
        if (state->ui) {
            UI_CALL(state->ui, display_peer_list, state);
        }
    } else if (strcmp(input, "/help") == 0) {
        if (state->ui) {
            UI_CALL(state->ui, display_help);
        }
    } else if (strcmp(input, "/debug") == 0) {
        Boolean current_debug_state = is_debug_output_enabled();
        set_debug_output_enabled(!current_debug_state);
        log_app_event("Debug output %s.", is_debug_output_enabled() ? "ENABLED" : "DISABLED");
        if (state->ui) {
            UI_CALL(state->ui, notify_debug_toggle, is_debug_output_enabled());
        }
    } else if (strncmp(input, "/send ", 6) == 0) {
        int peer_num_input;
        char *msg_start;
        char input_copy[BUFFER_SIZE];
        strncpy(input_copy, input, BUFFER_SIZE - 1);
        input_copy[BUFFER_SIZE - 1] = '\0';
        msg_start = strchr(input_copy + 6, ' ');
        if (msg_start == NULL) {
            log_app_event("Usage: /send <peer_number> <message>");
            if (state->ui) {
                UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
            }
            goto command_complete;
        }
        *msg_start = '\0';
        peer_num_input = atoi(input_copy + 6);
        msg_start++;
        if (peer_num_input <= 0) {
            log_app_event("Invalid peer number. Use /list to see active peers.");
            if (state->ui) {
                UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
            }
            goto command_complete;
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
                log_debug("Failed to send message to %s", target_ip);
                if (state->ui) {
                    UI_CALL(state->ui, notify_send_result, 0, peer_num_input, target_ip);
                }
            } else {
                log_app_event("Message sent to peer %d (%s)", peer_num_input, target_ip);
                if (state->ui) {
                    UI_CALL(state->ui, notify_send_result, 1, peer_num_input, target_ip);
                }
            }
        } else {
            log_app_event("Invalid peer number '%d'. Use /list to see active peers.", peer_num_input);
            if (state->ui) {
                UI_CALL(state->ui, notify_send_result, 0, -1, NULL);
            }
        }
    } else if (strncmp(input, "/broadcast ", 11) == 0) {
        const char *message_content = input + 11;
        log_app_event("Broadcasting message: %s", message_content);
        
        pthread_mutex_lock(&state->peers_mutex);
        int sent_count = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peer_manager.peers[i].active &&
                    (difftime(time(NULL), state->peer_manager.peers[i].last_seen) <= PEER_TIMEOUT)) {
                if (send_message(state->peer_manager.peers[i].ip, message_content, MSG_TEXT, state->username) < 0) {
                    log_debug("Failed to send broadcast message to %s", state->peer_manager.peers[i].ip);
                } else {
                    sent_count++;
                }
            }
        }
        pthread_mutex_unlock(&state->peers_mutex);
        
        log_app_event("Broadcast message sent to %d active peer(s).", sent_count);
        if (state->ui) {
            UI_CALL(state->ui, notify_broadcast_result, sent_count);
        }
    } else if (strcmp(input, "/quit") == 0) {
        log_debug("Initiating quit sequence...");
        pthread_mutex_lock(&state->peers_mutex);
        log_debug("Sending QUIT notifications to peers...");
        int notify_count = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (state->peer_manager.peers[i].active) {
                if (send_message(state->peer_manager.peers[i].ip, "", MSG_QUIT, state->username) < 0) {
                    log_debug("Failed to send quit notification to %s", state->peer_manager.peers[i].ip);
                } else {
                    notify_count++;
                }
            }
        }
        pthread_mutex_unlock(&state->peers_mutex);
        log_debug("Quit notifications sent to %d peer(s).", notify_count);
        if (g_state) {
            g_state->running = 0;
        } else {
            state->running = 0;
        }
        log_debug("Exiting application via /quit command...");
        result = 1;  /* Signal quit */
    } else {
        log_app_event("Unknown command: '%s'. Type /help for available commands.", input);
        if (state->ui) {
            UI_CALL(state->ui, notify_command_unknown, input);
        }
    }
    
command_complete:
    /* Notify UI of command completion */
    if (state->ui) {
        UI_CALL(state->ui, handle_command_complete);
    }
    
    return result;
}

/* User input thread */
void *user_input_thread(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    char input[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;
    
    /* Notify UI we're ready */
    if (state->ui) {
        UI_CALL(state->ui, notify_ready);
        UI_CALL(state->ui, show_prompt);
    }
    
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
                    log_debug("EOF detected on stdin. Exiting input loop.");
                    /* Notify about EOF via terminal display */
                    terminal_display_app_message("Input stream closed. Shutting down...");
                    if (g_state) g_state->running = 0;
                    else state->running = 0;
                } else if (errno == EINTR) {
                    /* Interrupted by signal, continue */
                    continue;
                } else {
                    log_debug("Error reading input from stdin: %s", strerror(errno));
                    /* For errors, we'll just log them and try to recover */
                    /* Try to recover from transient errors */
                    clearerr(stdin);
                    usleep(100000); /* 100ms delay before retry */
                    continue;
                }
            }
            break;
        }
        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0) {
            if (state->ui) {
                UI_CALL(state->ui, show_prompt);
            }
            continue;
        }
        
        if (handle_command(state, input) == 1) {
            break;
        }
        
        if (state->ui) {
            UI_CALL(state->ui, show_prompt);
        }
    }
    log_debug("User input thread stopped.");
    return NULL;
}

/* Legacy function for displaying app messages */
void terminal_display_app_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    
    /* If we have a global state with UI, use it */
    if (g_state && g_state->ui) {
        UI_CALL_VA(g_state->ui, display_app_message, format, args);
    } else {
        /* Fallback to direct output */
        time_t now = time(NULL);
        char time_str[20];
        struct tm *local_time_info = localtime(&now);
        if (local_time_info) {
            strftime(time_str, sizeof(time_str), "%H:%M:%S", local_time_info);
            printf("[%s] ", time_str);
        } else {
            printf("[--:--:--] ");
        }
        vprintf(format, args);
        printf("\n");
        fflush(stdout);
    }
    
    va_end(args);
}