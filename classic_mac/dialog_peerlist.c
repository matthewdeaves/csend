#include "dialog_peerlist.h"
#include "dialog.h"
#include "logging.h"
#include "peer_mac.h"
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Fonts.h>
#include <Memory.h>
#include <Resources.h>
#include <stdio.h>
#include <string.h>
Boolean InitPeerListControl(DialogPtr dialog) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectList, dataBounds;
    Point cellSize;
    FontInfo fontInfo;
    log_message("Initializing Peer List Control...");
    GetDialogItem(dialog, kPeerListUserItem, &itemType, &itemHandle, &destRectList);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kPeerListUserItem,
                    destRectList.top, destRectList.left, destRectList.bottom, destRectList.right);
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GetFontInfo(&fontInfo);
        SetPort(oldPort);
        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading;
        if (cellSize.v <= 0) {
            log_message("Warning: Calculated cell height is %d, using default 12.", cellSize.v);
            cellSize.v = 12;
        }
        cellSize.h = destRectList.right - destRectList.left;
        SetRect(&dataBounds, 0, 0, 1, 0);
        log_message("Calling LNew for Peer List (Cell Size: H%d, V%d)...", cellSize.h, cellSize.v);
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)dialog,
                               true, false, false, true);
        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            return false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            (*gPeerListHandle)->selFlags = lOnlyOne;
            LActivate(true, gPeerListHandle);
            return true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for LNew.", kPeerListUserItem, itemType);
        gPeerListHandle = NULL;
        return false;
    }
}
void CleanupPeerListControl(void) {
    log_message("Cleaning up Peer List Control...");
    if (gPeerListHandle != NULL) {
        LActivate(false, gPeerListHandle);
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    log_message("Peer List Control cleanup finished.");
}
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent) {
    Boolean listClicked = false;
    if (gPeerListHandle != NULL) {
         Point localClick = theEvent->where;
         GrafPtr oldPort;
         GetPort(&oldPort);
         SetPort(GetWindowPort(dialog));
         GlobalToLocal(&localClick);
         SetPort(oldPort);
         SignedByte listState = HGetState((Handle)gPeerListHandle);
         HLock((Handle)gPeerListHandle);
         Boolean clickedInside = false;
         if (*gPeerListHandle != NULL) {
             clickedInside = PtInRect(localClick, &(**gPeerListHandle).rView);
         } else {
             log_message("HandlePeerListClick Error: gPeerListHandle deref failed!");
         }
         HSetState((Handle)gPeerListHandle, listState);
         if (clickedInside) {
             log_message("HandlePeerListClick: Click inside Peer List view rect. Calling LClick.");
             listClicked = LClick(localClick, theEvent->modifiers, gPeerListHandle);
             if (!LGetSelect(true, &gLastSelectedCell, gPeerListHandle)) {
                 SetPt(&gLastSelectedCell, 0, 0);
                 log_message("HandlePeerListClick: No selection after LClick.");
             } else {
                 log_message("HandlePeerListClick: Peer list item selected: Row %d, Col %d", gLastSelectedCell.v, gLastSelectedCell.h);
             }
         } else {
              log_message("HandlePeerListClick: Click was outside Peer List view rect.");
         }
    }
    return listClicked;
}
void UpdatePeerDisplayList(Boolean forceRedraw) {
    Cell theCell;
    char peerStr[INET_ADDRSTRLEN + 32 + 2];
    int currentListLength;
    int activePeerCount = 0;
    SignedByte listState;
    Boolean selectionStillValid = false;
    Cell oldSelection = gLastSelectedCell;
    if (gPeerListHandle == NULL) {
        log_message("Skipping UpdatePeerDisplayList: List not initialized.");
        return;
    }
    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);
    if (*gPeerListHandle == NULL) {
        log_message("UpdatePeerDisplayList Error: gPeerListHandle deref failed after HLock!");
        HSetState((Handle)gPeerListHandle, listState);
        return;
    }
    currentListLength = (**gPeerListHandle).dataBounds.bottom;
    if (currentListLength > 0) {
        LDelRow(0, 0, gPeerListHandle);
    }
    PruneTimedOutPeers();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            const char *displayName = (gPeerList[i].username[0] != '\0') ? gPeerList[i].username : "???";
            sprintf(peerStr, "%s@%s", displayName, gPeerList[i].ip);
            LAddRow(1, activePeerCount, gPeerListHandle);
            SetPt(&theCell, 0, activePeerCount);
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);
            if (oldSelection.v == activePeerCount) {
                 selectionStillValid = true;
            }
            activePeerCount++;
        }
    }
     (**gPeerListHandle).dataBounds.bottom = activePeerCount;
     if (selectionStillValid && oldSelection.v < activePeerCount) {
         LSetSelect(true, oldSelection, gPeerListHandle);
         gLastSelectedCell = oldSelection;
     } else {
         if (oldSelection.v >= 0 && oldSelection.v < currentListLength) {
            LSetSelect(false, oldSelection, gPeerListHandle);
         }
         SetPt(&gLastSelectedCell, 0, 0);
     }
    HSetState((Handle)gPeerListHandle, listState);
    if (forceRedraw || activePeerCount != currentListLength) {
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort);
            listState = HGetState((Handle)gPeerListHandle);
            HLock((Handle)gPeerListHandle);
            if (*gPeerListHandle != NULL) {
                InvalRect(&(**gPeerListHandle).rView);
            }
            HSetState((Handle)gPeerListHandle, listState);
            SetPort(oldPort);
            log_message("Peer list updated. Active peers: %d. Invalidating list rect.", activePeerCount);
        } else {
            log_message("Peer list updated, but cannot invalidate rect (window port is NULL).");
        }
    } else {
        log_to_file_only("UpdatePeerDisplayList: No change in active peer count (%d), redraw not forced.", activePeerCount);
    }
}
void HandlePeerListUpdate(DialogPtr dialog) {
    if (gPeerListHandle != NULL) {
        GrafPtr windowPort = GetWindowPort(dialog);
        if (windowPort != NULL) {
            LUpdate(windowPort->visRgn, gPeerListHandle);
        } else {
             log_message("HandlePeerListUpdate Error: Cannot update list, window port is NULL.");
        }
    }
}
Boolean GetSelectedPeerInfo(peer_t *outPeer) {
    if (gPeerListHandle == NULL || outPeer == NULL) {
        return false;
    }
    if (gLastSelectedCell.v < 0) {
         log_to_file_only("GetSelectedPeerInfo: No peer selected (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
        return false;
    }
    int selectedDisplayRow = gLastSelectedCell.v;
    int current_active_count = 0;
    PruneTimedOutPeers();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            if (current_active_count == selectedDisplayRow) {
                *outPeer = gPeerList[i];
                log_to_file_only("GetSelectedPeerInfo: Found selected peer '%s'@'%s' at display row %d (data index %d).",
                                 outPeer->username, outPeer->ip, selectedDisplayRow, i);
                return true;
            }
            current_active_count++;
        }
    }
    log_message("GetSelectedPeerInfo Warning: Selected row %d is out of bounds for current active peers (%d).",
                selectedDisplayRow, current_active_count);
    return false;
}
