#include "ui_interface.h"
#include "ui_factory.h"
#include "peertalk_bridge.h"
#include "clog.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

static void print_timestamp(void)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    printf("[%s] ", buf);
}

static void interactive_init(void *context)
{
    (void)context;
}

static void interactive_cleanup(void *context)
{
    (void)context;
}

static void interactive_display_message(void *context, const char *from_username,
                                        const char *from_ip, const char *content)
{
    (void)context;
    print_timestamp();
    printf("%s@%s: %s\n", from_username, from_ip, content);
    fflush(stdout);
}

static void interactive_display_app_message(void *context, const char *format, va_list args)
{
    (void)context;
    print_timestamp();
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
    int i, total, connected_num;
    (void)context;

    total = PT_GetPeerCount(state->pt_ctx);
    connected_num = 0;

    printf("\n--- Active Peers ---\n");

    for (i = 0; i < total; i++) {
        PT_Peer *peer = PT_GetPeer(state->pt_ctx, i);
        if (peer && PT_GetPeerState(peer) == PT_PEER_CONNECTED) {
            connected_num++;
            printf("%d. %s@%s\n", connected_num,
                   PT_PeerName(peer), PT_PeerAddress(peer));
        }
    }

    if (connected_num == 0) {
        printf("No active peers found.\n");
    }
    printf("--------------------\n\n");
    fflush(stdout);
}

static void interactive_display_help(void *context)
{
    (void)context;
    printf("\nCommands:\n");
    printf("  /list                     - List all active peers\n");
    printf("  /send <peer_number> <msg> - Send <msg> to a specific peer\n");
    printf("  /broadcast <message>      - Send <message> to all active peers\n");
    printf("  /debug                    - Toggle debug output\n");
    printf("  /test                     - Run automated test sequence\n");
    printf("  /quit                     - Exit the application\n");
    printf("  /help                     - Show this help message\n\n");
    fflush(stdout);
}

static void interactive_notify_send_result(void *context, int success, int peer_num, const char *peer_ip)
{
    (void)context;
    if (success) {
        print_timestamp();
        printf("Message sent to peer %d (%s)\n", peer_num, peer_ip ? peer_ip : "");
    } else {
        print_timestamp();
        if (peer_num < 0) {
            printf("Invalid peer number. Use /list to see active peers.\n");
        } else {
            printf("Failed to send message to peer %d\n", peer_num);
        }
    }
    fflush(stdout);
}

static void interactive_notify_broadcast_result(void *context, int sent_count)
{
    (void)context;
    print_timestamp();
    printf("Broadcast message sent to %d active peer(s).\n", sent_count);
    fflush(stdout);
}

static void interactive_notify_command_unknown(void *context, const char *command)
{
    (void)context;
    print_timestamp();
    printf("Unknown command: '%s'. Type /help for available commands.\n", command);
    fflush(stdout);
}

static void interactive_notify_peer_update(void *context)
{
    (void)context;
}

static void interactive_notify_debug_toggle(void *context, int enabled)
{
    (void)context;
    print_timestamp();
    printf("Debug output %s.\n", enabled ? "ENABLED" : "DISABLED");
    fflush(stdout);
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
}

static void interactive_handle_command_complete(void *context)
{
    (void)context;
}

static void interactive_notify_startup(void *context, const char *username)
{
    (void)context;
    print_timestamp();
    printf("Starting P2P messaging application as '%s'\n", username);
    fflush(stdout);
}

static void interactive_notify_shutdown(void *context)
{
    (void)context;
    print_timestamp();
    printf("Application terminated gracefully.\n");
    fflush(stdout);
}

static void interactive_notify_ready(void *context)
{
    ui_context_t *ui = (ui_context_t *)context;
    interactive_display_help(ui);
}

static ui_operations_t interactive_ops = {
    interactive_init,
    interactive_cleanup,
    interactive_display_message,
    interactive_display_app_message,
    interactive_display_error,
    interactive_display_peer_list,
    interactive_display_help,
    interactive_notify_send_result,
    interactive_notify_broadcast_result,
    interactive_notify_command_unknown,
    interactive_notify_peer_update,
    interactive_notify_debug_toggle,
    NULL,  /* notify_status */
    NULL,  /* notify_stats */
    NULL,  /* notify_history */
    NULL,  /* notify_version */
    interactive_show_prompt,
    interactive_handle_command_start,
    interactive_handle_command_complete,
    interactive_notify_startup,
    interactive_notify_shutdown,
    interactive_notify_ready
};

ui_operations_t *ui_terminal_interactive_ops(void)
{
    return &interactive_ops;
}
