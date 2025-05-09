#ifndef DISCOVERY_H
#define DISCOVERY_H
#include <sys/socket.h>
#include <netinet/in.h>
#include "peer.h"
int init_discovery(app_state_t *state);
void *discovery_thread(void *arg);
int broadcast_discovery(app_state_t *state);
#endif
