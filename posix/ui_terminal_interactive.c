#include "ui_interface.h"
#include "ui_factory.h"
#include "logging.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <string.h>

/* Interactive terminal implementation */

static void interactive_init(void *context)
{
    (void)context;
    /* No special initialization needed for interactive mode */
}

static void interactive_cleanup(void *context)
{
    (void)context;
    /* No special cleanup needed */
}

static void interactive_display_message(void *context, const char *from_username, 
                                       const char *from_ip, const char *content)
{
    (void)context;
    time_t now = time(NULL);
    char time_str[20];
    struct tm *local_time_info = localtime(&now);
    
    if (local_time_info) {
        strftime(time_str, sizeof(time_str), "%H:%M:%S", local_time_info);
        printf("[%s] ", time_str);
    } else {
        printf("[--:--:--] ");
    }
    
    printf("%s@%s: %s\n", from_username, from_ip, content);
    fflush(stdout);
}

static void interactive_display_app_message(void *context, const char *format, va_list args)
{
    (void)context;
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

static void interactive_display_error(void *context, const char *format, va_list args)
{
    (void)context;
    printf("Error: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
}

static void interactive_display_peer_list(void *context, app_state_t *state)
{
    (void)context;
    pthread_mutex_lock(&state->peers_mutex);
    time_t now = time(NULL);
    int active_count = 0;
    
    printf("\n--- Active Peers ---\n");
    
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active) {
            if (difftime(now, state->peer_manager.peers[i].last_seen) > PEER_TIMEOUT) {
                log_debug("Peer %s@%s timed out (detected in print_peers).",
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

static void interactive_display_help(void *context)
{
    (void)context;
    printf("\nCommands:\n");
    printf("  /list                     - List all active peers\n");
    printf("  /send <peer_number> <msg> - Send <msg> to a specific peer from the list\n");
    printf("  /broadcast <message>      - Send <message> to all active peers\n");
    printf("  /debug                    - Toggle detailed debug message visibility\n");
    printf("  /quit                     - Send quit notification and exit the application\n");
    printf("  /help                     - Show this help message\n\n");
    fflush(stdout);
}

static void interactive_notify_send_result(void *context, int success, int peer_num, const char *peer_ip)
{
    ui_context_t *ui = (ui_context_t *)context;
    
    if (success) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Message sent to peer %d (%s)", peer_num, peer_ip);
        va_list dummy;
        interactive_display_app_message(ui, msg, dummy);
    } else {
        char msg[256];
        if (peer_num < 0) {
            snprintf(msg, sizeof(msg), "Invalid peer number. Use /list to see active peers.");
        } else {
            snprintf(msg, sizeof(msg), "Failed to send message to peer %d", peer_num);
        }
        va_list dummy;
        interactive_display_app_message(ui, msg, dummy);
    }
}

static void interactive_notify_broadcast_result(void *context, int sent_count)
{
    ui_context_t *ui = (ui_context_t *)context;
    char msg[256];
    snprintf(msg, sizeof(msg), "Broadcast message sent to %d active peer(s).", sent_count);
    va_list dummy;
    interactive_display_app_message(ui, msg, dummy);
}

static void interactive_notify_command_unknown(void *context, const char *command)
{
    ui_context_t *ui = (ui_context_t *)context;
    char msg[256];
    snprintf(msg, sizeof(msg), "Unknown command: '%s'. Type /help for available commands.", command);
    va_list dummy;
    interactive_display_app_message(ui, msg, dummy);
}

static void interactive_notify_peer_update(void *context)
{
    (void)context;
    /* No notification in interactive mode - users can /list when needed */
}

static void interactive_notify_debug_toggle(void *context, int enabled)
{
    ui_context_t *ui = (ui_context_t *)context;
    char msg[256];
    snprintf(msg, sizeof(msg), "Debug output %s.", enabled ? "ENABLED" : "DISABLED");
    va_list dummy;
    interactive_display_app_message(ui, msg, dummy);
}

static void interactive_show_prompt(void *context)
{
    (void)context;
    printf("> ");
    fflush(stdout);
}

static void interactive_handle_command_start(void *context, const char *command)
{
    (void)context;
    (void)command;
    /* No special handling needed in interactive mode */
}

static void interactive_handle_command_complete(void *context)
{
    (void)context;
    /* Prompt will be shown by show_prompt */
}

static void interactive_notify_startup(void *context, const char *username)
{
    ui_context_t *ui = (ui_context_t *)context;
    char msg[256];
    snprintf(msg, sizeof(msg), "Starting P2P messaging application as '%s'", username);
    va_list dummy;
    interactive_display_app_message(ui, msg, dummy);
}

static void interactive_notify_shutdown(void *context)
{
    ui_context_t *ui = (ui_context_t *)context;
    char msg[] = "Application terminated gracefully.";
    va_list dummy;
    interactive_display_app_message(ui, msg, dummy);
}

static void interactive_notify_ready(void *context)
{
    ui_context_t *ui = (ui_context_t *)context;
    interactive_display_help(ui);
}

/* Static operations table */
static ui_operations_t interactive_ops = {
    .init = interactive_init,
    .cleanup = interactive_cleanup,
    .display_message = interactive_display_message,
    .display_app_message = interactive_display_app_message,
    .display_error = interactive_display_error,
    .display_peer_list = interactive_display_peer_list,
    .display_help = interactive_display_help,
    .notify_send_result = interactive_notify_send_result,
    .notify_broadcast_result = interactive_notify_broadcast_result,
    .notify_command_unknown = interactive_notify_command_unknown,
    .notify_peer_update = interactive_notify_peer_update,
    .notify_debug_toggle = interactive_notify_debug_toggle,
    .show_prompt = interactive_show_prompt,
    .handle_command_start = interactive_handle_command_start,
    .handle_command_complete = interactive_handle_command_complete,
    .notify_startup = interactive_notify_startup,
    .notify_shutdown = interactive_notify_shutdown,
    .notify_ready = interactive_notify_ready
};

ui_operations_t *ui_terminal_interactive_ops(void)
{
    return &interactive_ops;
}