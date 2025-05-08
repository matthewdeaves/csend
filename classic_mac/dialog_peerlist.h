#ifndef DIALOG_PEERLIST_H
#define DIALOG_PEERLIST_H 
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Events.h>
#include "peer.h"
extern DialogPtr gMainWindow;
extern ListHandle gPeerListHandle;
extern Cell gLastSelectedCell;
extern peer_t gPeerList[MAX_PEERS];
Boolean InitPeerListControl(DialogPtr dialog);
void CleanupPeerListControl(void);
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent);
void UpdatePeerDisplayList(Boolean forceRedraw);
void HandlePeerListUpdate(DialogPtr dialog);
Boolean GetSelectedPeerInfo(peer_t *outPeer);
void ActivatePeerList(Boolean activating);
#endif
