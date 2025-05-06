#ifndef PEER_H
#define PEER_H
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include "../shared/common_defs.h"
#include "../shared/peer_shared.h"
typedef struct app_state_t {
    volatile sig_atomic_t running;
    peer_manager_t peer_manager;
    int tcp_socket;
    int udp_socket;
    char username[32];
    pthread_mutex_t peers_mutex;
} app_state_t;
void init_app_state(app_state_t *state, const char *username);
void cleanup_app_state(app_state_t *state);
int add_peer(app_state_t *state, const char *ip, const char *username);
int prune_peers(app_state_t *state);
extern app_state_t *g_state;
#endif
