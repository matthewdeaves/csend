#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H
#include "peer.h"
void print_help_message(void);
void *user_input_thread(void *arg);
void print_peers(app_state_t *state);
int handle_command(app_state_t *state, const char *input);
#endif
