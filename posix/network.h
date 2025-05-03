#ifndef NETWORK_H
#define NETWORK_H 
#include <stddef.h>
#include "peer.h"
int get_local_ip(char *buffer, size_t size);
void set_socket_timeout(int socket, int seconds);
#endif
