#ifndef PEER_H
#define PEER_H
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include "../shared/common_defs.h"
#include "../shared/peer.h"
typedef struct app_state_t {
    volatile sig_atomic_t running;
    int tcp_socket;
    int udp_socket;
    char username[32];
    struct ui_context *ui;  /* UI interface pointer */
} app_state_t;
void init_app_state(app_state_t *state, const char *username);
void cleanup_app_state(app_state_t *state);
extern app_state_t *g_state;
#endif
