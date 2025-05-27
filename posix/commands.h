#ifndef COMMANDS_H
#define COMMANDS_H

#include "../shared/common_defs.h"
#include "peer.h"

/* Command handler function type */
typedef int (*command_handler_func)(app_state_t *state, const char *args);

/* Command entry structure */
typedef struct {
    const char *name;
    command_handler_func handler;
    const char *description;
} command_entry_t;

/* Individual command handlers */
int handle_list_command(app_state_t *state, const char *args);
int handle_help_command(app_state_t *state, const char *args);
int handle_debug_command(app_state_t *state, const char *args);
int handle_send_command(app_state_t *state, const char *args);
int handle_broadcast_command(app_state_t *state, const char *args);
int handle_quit_command(app_state_t *state, const char *args);
int handle_status_command(app_state_t *state, const char *args);
int handle_stats_command(app_state_t *state, const char *args);
int handle_history_command(app_state_t *state, const char *args);
int handle_version_command(app_state_t *state, const char *args);
int handle_peers_command(app_state_t *state, const char *args);

/* Helper functions */
int parse_peer_number(const char *input, int *peer_num);
int find_peer_by_number(app_state_t *state, int peer_num, char *target_ip, size_t ip_size);
int send_to_peer(app_state_t *state, const char *target_ip, const char *message, int peer_num);
int broadcast_to_all_peers(app_state_t *state, const char *message);
void notify_peers_on_quit(app_state_t *state);

#endif /* COMMANDS_H */