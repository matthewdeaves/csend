#ifndef PEER_H
#define PEER_H

#include <signal.h>
#include "peertalk.h"

typedef struct app_state_t {
    volatile sig_atomic_t running;
    char username[32];
    struct ui_context *ui;
    PT_Context *pt_ctx;
} app_state_t;

void init_app_state(app_state_t *state, const char *username);
void cleanup_app_state(app_state_t *state);

extern app_state_t *g_state;

#endif
