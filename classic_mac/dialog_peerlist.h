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
Boolean InitPeerListControl(DialogPtr dialog);
void CleanupPeerListControl(void);
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent);
void UpdatePeerDisplayList(Boolean forceRedraw);
void HandlePeerListUpdate(DialogPtr dialog);
Boolean DialogPeerList_GetSelectedPeer(peer_t *outPeer);
void DialogPeerList_DeselectAll(void);
void ActivatePeerList(Boolean activating);
#endif
