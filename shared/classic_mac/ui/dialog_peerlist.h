#ifndef DIALOG_PEERLIST_H
#define DIALOG_PEERLIST_H
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Events.h>
#include "peertalk.h"

extern ListHandle gPeerListHandle;
extern Cell gLastSelectedCell;

Boolean InitPeerListControl(DialogPtr dialog);
void CleanupPeerListControl(void);
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent);
void UpdatePeerDisplayList(Boolean forceRedraw);
void HandlePeerListUpdate(DialogPtr dialog);
PT_Peer *DialogPeerList_GetSelectedPeer(void);
void DialogPeerList_DeselectAll(void);
void ActivatePeerList(Boolean activating);

#endif
