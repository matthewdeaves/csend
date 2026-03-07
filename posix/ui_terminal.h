#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H

#include "peer.h"

void *user_input_thread(void *arg);
int handle_command(app_state_t *state, const char *input);

#endif
