#ifndef MESSAGING_H
#define MESSAGING_H
#include "peer.h"
int init_listener(app_state_t *state);
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username);
void *listener_thread(void *arg);
#endif
