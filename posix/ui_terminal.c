#include "ui_terminal.h"
#include "commands.h"
#include "ui_interface.h"
#include "clog.h"
#include "../shared/common_defs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

/* Command table */
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
    {"/test",      handle_test_command,      "Run automated test"},
    {NULL, NULL, NULL}
};

static const command_entry_t *find_command(const char *cmd_name)
{
    const command_entry_t *cmd;
    for (cmd = command_table; cmd->name != NULL; cmd++) {
        if (strcmp(cmd_name, cmd->name) == 0) {
            return cmd;
        }
    }
    return NULL;
}

static int extract_command_and_args(const char *input, char *cmd_name, size_t cmd_size,
                                    char *args, size_t args_size)
{
    const char *space;
    size_t cmd_len;

    if (!input || !cmd_name || !args) return 0;

    cmd_name[0] = '\0';
    args[0] = '\0';

    while (*input && isspace((unsigned char)*input)) input++;
    if (*input != '/') return 0;

    space = strchr(input, ' ');

    if (space) {
        cmd_len = (size_t)(space - input);
        strncpy(args, space + 1, args_size - 1);
        args[args_size - 1] = '\0';
    } else {
        cmd_len = strlen(input);
    }

    if (cmd_len >= cmd_size) cmd_len = cmd_size - 1;
    strncpy(cmd_name, input, cmd_len);
    cmd_name[cmd_len] = '\0';

    return 1;
}

int handle_command(app_state_t *state, const char *input)
{
    char clean_input[BUFFER_SIZE];
    char cmd_name[64];
    char args[BUFFER_SIZE];
    char *id_pos;
    const command_entry_t *cmd;
    int result = 0;

    if (!state || !input) return 0;

    if (state->ui) {
        UI_CALL(state->ui, handle_command_start, input);
    }

    strncpy(clean_input, input, BUFFER_SIZE - 1);
    clean_input[BUFFER_SIZE - 1] = '\0';

    id_pos = strstr(clean_input, " --id=");
    if (id_pos) {
        *id_pos = '\0';
    }

    if (!extract_command_and_args(clean_input, cmd_name, sizeof(cmd_name),
                                  args, sizeof(args))) {
        if (state->ui) {
            UI_CALL(state->ui, notify_command_unknown, input);
        }
        goto command_complete;
    }

    cmd = find_command(cmd_name);
    if (cmd) {
        result = cmd->handler(state, args);
    } else {
        if (state->ui) {
            UI_CALL(state->ui, notify_command_unknown, input);
        }
    }

command_complete:
    if (state->ui) {
        UI_CALL(state->ui, handle_command_complete);
    }

    return result;
}

void *user_input_thread(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    char input[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;

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
            if (errno == EINTR) continue;
            break;
        }
        if (!state->running) break;
        if (activity == 0) continue;

        if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
            if (state->running) {
                if (feof(stdin)) {
                    CLOG_INFO("EOF on stdin, shutting down");
                    if (g_state) g_state->running = 0;
                    else state->running = 0;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    clearerr(stdin);
                    usleep(100000);
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

    CLOG_INFO("Input thread stopped");
    return NULL;
}
