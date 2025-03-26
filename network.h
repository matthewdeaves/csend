#ifndef NETWORK_H
#define NETWORK_H

#include "peer.h"

// Function declarations
int get_local_ip(char *buffer, size_t size);
void set_socket_timeout(int socket, int seconds);
int init_listener(app_state_t *state);
int send_message(const char *ip, const char *message, const char *msg_type);
void *listener_thread(void *arg);

#endif // NETWORK_H