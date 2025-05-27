#include "ui_terminal.h"
#include "ui_terminal_commands.h"
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
#include <ctype.h>

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

/* Command table for dispatching */
static const command_entry_t command_table[] = {
    {"/list",      handle_list_command,      "List all active peers"},
    {"/help",      handle_help_command,      "Show help message"},
    {"/debug",     handle_debug_command,     "Toggle debug output"},
    {"/send",      handle_send_command,      "Send message to a peer"},
    {"/broadcast", handle_broadcast_command, "Send message to all peers"},
    {"/quit",      handle_quit_command,      "Quit the application"},
    {"/status",    handle_status_command,    "Show status information"},
    {"/stats",     handle_stats_command,     "Show statistics"},
    {"/history",   handle_history_command,   "Show message history"},
    {"/version",   handle_version_command,   "Show version information"},
    {"/peers",     handle_peers_command,     "List or filter peers"},
    {NULL, NULL, NULL} /* Sentinel */
};

/* Find command handler by name */
static const command_entry_t *find_command(const char *cmd_name)
{
    for (const command_entry_t *cmd = command_table; cmd->name != NULL; cmd++) {
        if (strcmp(cmd_name, cmd->name) == 0) {
            return cmd;
        }
    }
    return NULL;
}

/* Extract command name and arguments from input */
static int extract_command_and_args(const char *input, char *cmd_name, size_t cmd_size,
                                    char *args, size_t args_size)
{
    if (!input || !cmd_name || !args) return 0;

    /* Initialize outputs */
    cmd_name[0] = '\0';
    args[0] = '\0';

    /* Skip leading whitespace */
    while (*input && isspace(*input)) input++;

    /* Check if it's a command */
    if (*input != '/') return 0;

    /* Find the end of command name */
    const char *space = strchr(input, ' ');
    size_t cmd_len;

    if (space) {
        cmd_len = space - input;
        /* Copy arguments */
        strncpy(args, space + 1, args_size - 1);
        args[args_size - 1] = '\0';
    } else {
        cmd_len = strlen(input);
    }

    /* Copy command name */
    if (cmd_len >= cmd_size) cmd_len = cmd_size - 1;
    strncpy(cmd_name, input, cmd_len);
    cmd_name[cmd_len] = '\0';

    return 1;
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

    /* Strip --id parameter for command processing */
    char clean_input[BUFFER_SIZE];
    strncpy(clean_input, input, BUFFER_SIZE - 1);
    clean_input[BUFFER_SIZE - 1] = '\0';

    char *id_pos = strstr(clean_input, " --id=");
    if (id_pos) {
        *id_pos = '\0';  /* Truncate at --id parameter */
    }

    /* Extract command and arguments */
    char cmd_name[64];
    char args[BUFFER_SIZE];

    if (!extract_command_and_args(clean_input, cmd_name, sizeof(cmd_name),
                                  args, sizeof(args))) {
        log_app_event("Invalid command format: '%s'", input);
        if (state->ui) {
            UI_CALL(state->ui, notify_command_unknown, input);
        }
        goto command_complete;
    }

    /* Find and execute command */
    const command_entry_t *cmd = find_command(cmd_name);
    if (cmd) {
        result = cmd->handler(state, args);
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
                    log_info_cat(LOG_CAT_UI, "EOF detected on stdin. Exiting input loop.");
                    /* Notify about EOF via terminal display */
                    terminal_display_app_message("Input stream closed. Shutting down...");
                    if (g_state) g_state->running = 0;
                    else state->running = 0;
                } else if (errno == EINTR) {
                    /* Interrupted by signal, continue */
                    continue;
                } else {
                    log_error_cat(LOG_CAT_UI, "Error reading input from stdin: %s", strerror(errno));
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
    log_info_cat(LOG_CAT_UI, "User input thread stopped.");
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