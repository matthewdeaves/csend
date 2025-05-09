#ifndef PEER_SHARED_H
#define PEER_SHARED_H
#include "common_defs.h"
typedef struct {
    peer_t peers[MAX_PEERS];
} peer_manager_t;
void peer_shared_init_list(peer_manager_t *manager);
int peer_shared_find_by_ip(peer_manager_t *manager, const char *ip);
int peer_shared_find_empty_slot(peer_manager_t *manager);
void peer_shared_update_entry(peer_t *peer, const char *username);
int peer_shared_add_or_update(peer_manager_t *manager, const char *ip, const char *username);
int peer_shared_prune_timed_out(peer_manager_t *manager);
#endif
