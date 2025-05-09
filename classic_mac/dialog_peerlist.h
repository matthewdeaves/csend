#ifndef DIALOG_PEERLIST_H
#define DIALOG_PEERLIST_H

#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Events.h>
#include "peer.h" // For peer_t, MAX_PEERS (via shared/common_defs.h) and gPeerManager extern

extern DialogPtr gMainWindow;
extern ListHandle gPeerListHandle;
extern Cell gLastSelectedCell;
// extern peer_t gPeerList[MAX_PEERS]; // REMOVED - Unused and incorrect

Boolean InitPeerListControl(DialogPtr dialog);
void CleanupPeerListControl(void);
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent);
void UpdatePeerDisplayList(Boolean forceRedraw);
void HandlePeerListUpdate(DialogPtr dialog);
Boolean DialogPeerList_GetSelectedPeer(peer_t *outPeer); // MOVED & RENAMED from classic_mac/peer.h
void DialogPeerList_DeselectAll(void);                 // NEW function
void ActivatePeerList(Boolean activating);

#endif