#include "peer_wrapper.h"
#include "peer.h"
#include "platform_sync.h"
#include <string.h>

static peer_manager_t g_peer_manager;
static platform_mutex_t g_peer_mutex;

void pw_init(void) {
    platform_mutex_init(&g_peer_mutex);
    platform_mutex_lock(&g_peer_mutex);
    peer_shared_init_list(&g_peer_manager);
    platform_mutex_unlock(&g_peer_mutex);
}

void pw_shutdown(void) {
    platform_mutex_destroy(&g_peer_mutex);
}

int pw_add_or_update(const char *ip, const char *username) {
    int result;
    platform_mutex_lock(&g_peer_mutex);
    result = peer_shared_add_or_update(&g_peer_manager, ip, username);
    platform_mutex_unlock(&g_peer_mutex);
    return result;
}

int pw_prune_timed_out(void) {
    int result;
    platform_mutex_lock(&g_peer_mutex);
    result = peer_shared_prune_timed_out(&g_peer_manager);
    platform_mutex_unlock(&g_peer_mutex);
    return result;
}

int pw_mark_inactive(const char *ip) {
    int result;
    platform_mutex_lock(&g_peer_mutex);
    result = peer_shared_mark_inactive(&g_peer_manager, ip);
    platform_mutex_unlock(&g_peer_mutex);
    return result;
}

void pw_get_peer_by_index(int index, peer_t *peer) {
    if (!peer) return;
    platform_mutex_lock(&g_peer_mutex);
    int current_active_count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peer_manager.peers[i].active) {
            if (current_active_count == index) {
                *peer = g_peer_manager.peers[i];
                break;
            }
            current_active_count++;
        }
    }
    platform_mutex_unlock(&g_peer_mutex);
}

int pw_get_active_peer_count(void) {
    int count = 0;
    platform_mutex_lock(&g_peer_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peer_manager.peers[i].active) {
            count++;
        }
    }
    platform_mutex_unlock(&g_peer_mutex);
    return count;
}

#ifdef __MACOS__
#include <MacTypes.h>
Boolean GetPeerByIndex(int active_index, peer_t *out_peer) {
    if (active_index < 0 || out_peer == NULL) {
        return false;
    }
    int count = pw_get_active_peer_count();
    if (active_index >= count) {
        return false;
    }
    pw_get_peer_by_index(active_index, out_peer);
    return true;
}
#endif
