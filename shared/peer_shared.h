#ifndef PEER_SHARED_H
#define PEER_SHARED_H 
#include "common_defs.h"
void peer_shared_init_list(peer_t *peers, int max_peers);
int peer_shared_find_by_ip(peer_t *peers, int max_peers, const char *ip);
int peer_shared_find_empty_slot(peer_t *peers, int max_peers);
void peer_shared_update_entry(peer_t *peer, const char *username);
int peer_shared_add_or_update(peer_t *peers, int max_peers, const char *ip, const char *username);
int peer_shared_prune_timed_out(peer_t *peers, int max_peers);
#endif
