#include "peer.h"
#include "logging.h"
#include <string.h>
#include <stdio.h>
#ifdef __MACOS__
#include <MacTypes.h>
#include <Events.h>
#else
#include <time.h>
#endif
void peer_shared_init_list(peer_manager_t *manager)
{
    if (!manager) return;
    memset(manager->peers, 0, sizeof(peer_t) * MAX_PEERS);
}
int peer_shared_find_by_ip(peer_manager_t *manager, const char *ip)
{
    if (!manager || !ip) return -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (manager->peers[i].active && strcmp(manager->peers[i].ip, ip) == 0) {
            return i;
        }
    }
    return -1;
}
int peer_shared_find_empty_slot(peer_manager_t *manager)
{
    if (!manager) return -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!manager->peers[i].active) {
            return i;
        }
    }
    return -1;
}
void peer_shared_update_entry(peer_t *peer, const char *username)
{
    if (!peer) return;
#ifdef __MACOS__
    peer->last_seen = TickCount();
#else
    peer->last_seen = (unsigned long)time(NULL);
#endif
    if (username && username[0] != '\0') {
        strncpy(peer->username, username, sizeof(peer->username) - 1);
        peer->username[sizeof(peer->username) - 1] = '\0';
    }
}
int peer_shared_add_or_update(peer_manager_t *manager, const char *ip, const char *username)
{
    if (!manager || !ip) return -1;
    int existing_index = peer_shared_find_by_ip(manager, ip);
    if (existing_index != -1) {
        peer_shared_update_entry(&manager->peers[existing_index], username);
        return 0;
    }
    int empty_slot = peer_shared_find_empty_slot(manager);
    if (empty_slot != -1) {
        peer_t *new_peer = &manager->peers[empty_slot];
        strncpy(new_peer->ip, ip, INET_ADDRSTRLEN - 1);
        new_peer->ip[INET_ADDRSTRLEN - 1] = '\0';
        new_peer->active = 1;
        new_peer->username[0] = '\0';
        peer_shared_update_entry(new_peer, username);
        return 1;
    }
    log_message("Peer list is full. Cannot add peer %s@%s.", username ? username : "??", ip);
    return -1;
}
int peer_shared_prune_timed_out(peer_manager_t *manager)
{
    if (!manager) return 0;
    int pruned_count = 0;
    unsigned long current_time;
    unsigned long timeout_duration;
#ifdef __MACOS__
    current_time = TickCount();
    timeout_duration = (unsigned long)PEER_TIMEOUT * 60;
#else
    current_time = (unsigned long)time(NULL);
    timeout_duration = (unsigned long)PEER_TIMEOUT;
#endif
    for (int i = 0; i < MAX_PEERS; i++) {
        if (manager->peers[i].active) {
            unsigned long last_seen = manager->peers[i].last_seen;
            unsigned long time_diff;
            if (current_time >= last_seen) {
                time_diff = current_time - last_seen;
            } else {
#ifdef __MACOS__
                time_diff = (0xFFFFFFFFUL - last_seen) + current_time + 1;
#else
                time_diff = timeout_duration + 1;
#endif
            }
            if (time_diff > timeout_duration) {
                log_message("Peer %s@%s timed out.", manager->peers[i].username, manager->peers[i].ip);
                manager->peers[i].active = 0;
                pruned_count++;
            }
        }
    }
    return pruned_count;
}
