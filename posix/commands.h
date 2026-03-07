#ifndef COMMANDS_H
#define COMMANDS_H

#include "../shared/common_defs.h"
#include "peer.h"

typedef int (*command_handler_func)(app_state_t *state, const char *args);

typedef struct {
    const char *name;
    command_handler_func handler;
    const char *description;
} command_entry_t;

int handle_list_command(app_state_t *state, const char *args);
int handle_help_command(app_state_t *state, const char *args);
int handle_debug_command(app_state_t *state, const char *args);
int is_debug_enabled(void);
int handle_send_command(app_state_t *state, const char *args);
int handle_broadcast_command(app_state_t *state, const char *args);
int handle_quit_command(app_state_t *state, const char *args);
int handle_status_command(app_state_t *state, const char *args);
int handle_stats_command(app_state_t *state, const char *args);
int handle_history_command(app_state_t *state, const char *args);
int handle_version_command(app_state_t *state, const char *args);
int handle_peers_command(app_state_t *state, const char *args);
int handle_test_command(app_state_t *state, const char *args);

#endif /* COMMANDS_H */
