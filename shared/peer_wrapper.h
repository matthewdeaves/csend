#ifndef PEER_WRAPPER_H
#define PEER_WRAPPER_H

#include "peer.h"

void pw_init(void);
void pw_shutdown(void);
int pw_add_or_update(const char *ip, const char *username);
int pw_prune_timed_out(void);
int pw_mark_inactive(const char *ip);
void pw_get_peer_by_index(int index, peer_t *peer);
int pw_get_active_peer_count(void);

/* Classic Mac-style function name aliases for backward compatibility */
#ifdef __MACOS__
#include <MacTypes.h>
#define InitPeerList() pw_init()
#define AddOrUpdatePeer(ip, username) pw_add_or_update(ip, username)
#define MarkPeerInactive(ip) ((void)pw_mark_inactive(ip))
#define PruneTimedOutPeers() pw_prune_timed_out()
Boolean GetPeerByIndex(int active_index, peer_t *out_peer);
#endif

#endif // PEER_WRAPPER_H
