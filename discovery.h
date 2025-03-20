#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "peer.h"

// Function declarations
int init_discovery(app_state_t *state);
void *discovery_thread(void *arg);
int broadcast_discovery(app_state_t *state);
int handle_discovery_message(app_state_t *state, const char *buffer, 
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr);

#endif // DISCOVERY_H