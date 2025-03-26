#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include "peer.h"

// Function declarations
void setup_signal_handlers(app_state_t *state);
void handle_signal(int sig);

#endif // SIGNAL_HANDLER_H