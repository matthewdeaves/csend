#ifndef PEERTALK_BRIDGE_H
#define PEERTALK_BRIDGE_H

#include "peer.h"
#include "../shared/common_defs.h"

/* Command types for the thread-safe command queue */
enum {
    CMD_SEND,
    CMD_BROADCAST,
    CMD_QUIT
};

typedef struct {
    int type;
    int peer_index;
    char message[BUFFER_SIZE];
} pending_command_t;

/* Initialize peertalk callbacks on the context */
void bridge_init(app_state_t *state);

/* Thread-safe command queue (called from input thread) */
void bridge_queue_send(int peer_index, const char *msg);
void bridge_queue_broadcast(const char *msg);

/* Process pending commands (called from main thread) */
void bridge_process_queue(app_state_t *state);

/* Peer info helpers (wrapping PT_GetPeer*) */
int bridge_get_peer_count(app_state_t *state);

#endif
