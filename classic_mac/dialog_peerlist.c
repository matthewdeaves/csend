#include "dialog_peerlist.h"
#include "dialog.h"
#include "logging.h"
#include "peer.h"
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Fonts.h>
#include <Memory.h>
#include <Resources.h>
#include <Controls.h>
#include <stdio.h>
#include <string.h>
ListHandle gPeerListHandle = NULL;
Cell gLastSelectedCell = {0, -1};
Boolean InitPeerListControl(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectList, dataBounds;
    Point cellSize;
    FontInfo fontInfo;
    Boolean listOk = false;
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
                               true,
                               false,
                               false,
                               true);
        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            listOk = false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            (*gPeerListHandle)->selFlags = lOnlyOne;
            LActivate(true, gPeerListHandle);
            listOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for LNew.", kPeerListUserItem, itemType);
        gPeerListHandle = NULL;
        listOk = false;
    }
    return listOk;
}
void CleanupPeerListControl(void)
{
    log_message("Cleaning up Peer List Control...");
    if (gPeerListHandle != NULL) {
        LActivate(false, gPeerListHandle);
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    log_message("Peer List Control cleanup finished.");
}
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent)
{
    Boolean clickWasInContent = false;
    if (gPeerListHandle != NULL) {
        Point localClick = theEvent->where;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GlobalToLocal(&localClick);
        SetPort(oldPort);
        SignedByte listState = HGetState((Handle)gPeerListHandle);
        HLock((Handle)gPeerListHandle);
        if (*gPeerListHandle == NULL) {
            log_message("HandlePeerListClick Error: gPeerListHandle deref failed after HLock!");
            HSetState((Handle)gPeerListHandle, listState);
            return false;
        }
        if (PtInRect(localClick, &(**gPeerListHandle).rView)) {
            clickWasInContent = true;
            log_to_file_only("HandlePeerListClick: Click inside Peer List view rect. Calling LClick.");
            (void)LClick(localClick, theEvent->modifiers, gPeerListHandle);
            Cell clickedCell = LLastClick(gPeerListHandle);
            Cell cellToVerify = clickedCell;
            if (clickedCell.v >= 0 && clickedCell.h >= 0) {
                if (LGetSelect(false, &cellToVerify, gPeerListHandle)) {
                    gLastSelectedCell = clickedCell;
                    log_to_file_only("HandlePeerListClick: LLastClick cell (%d,%d) IS selected. Unchecking broadcast.", gLastSelectedCell.h, gLastSelectedCell.v);
                    DialogItemType itemType;
                    Handle itemHandle;
                    Rect itemRect;
                    ControlHandle broadcastCheckboxHandle;
                    GrafPtr clickOldPort;
                    GetPort(&clickOldPort);
                    SetPort(GetWindowPort(gMainWindow));
                    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
                        broadcastCheckboxHandle = (ControlHandle)itemHandle;
                        SetControlValue(broadcastCheckboxHandle, 0);
                    } else {
                        log_message("HandlePeerListClick: Could not find/set broadcast checkbox (item %d).", kBroadcastCheckbox);
                    }
                    SetPort(clickOldPort);
                } else {
                    log_to_file_only("HandlePeerListClick: LLastClick cell (%d,%d) is NOT selected by LGetSelect(false,...). Clearing selection.", clickedCell.h, clickedCell.v);
                    SetPt(&gLastSelectedCell, 0, -1);
                }
            } else {
                log_to_file_only("HandlePeerListClick: LLastClick returned invalid cell (%d,%d) after LClick. Clearing selection.", clickedCell.h, clickedCell.v);
                SetPt(&gLastSelectedCell, 0, -1);
            }
        } else {
            log_to_file_only("HandlePeerListClick: Click was outside Peer List view rect.");
        }
        HSetState((Handle)gPeerListHandle, listState);
    }
    return clickWasInContent;
}
void UpdatePeerDisplayList(Boolean forceRedraw)
{
    Cell theCell;
    char peerStr[INET_ADDRSTRLEN + 32 + 2];
    int currentListLengthInRows;
    int activePeerCount = 0;
    SignedByte listState;
    Boolean oldSelectionStillValidAndFound = false;
    peer_t oldSelectedPeerData;
    Boolean hadOldSelectionData = false;
    if (gPeerListHandle == NULL) {
        log_message("Skipping UpdatePeerDisplayList: List not initialized.");
        return;
    }
    if (gLastSelectedCell.v >= 0) {
        hadOldSelectionData = GetSelectedPeerInfo(&oldSelectedPeerData);
        if (hadOldSelectionData) {
            log_to_file_only("UpdatePeerDisplayList: Attempting to preserve selection for peer %s@%s (was display row %d).", oldSelectedPeerData.username, oldSelectedPeerData.ip, gLastSelectedCell.v);
        } else {
            log_to_file_only("UpdatePeerDisplayList: gLastSelectedCell.v was %d, but GetSelectedPeerInfo failed. No specific peer to preserve.", gLastSelectedCell.v);
        }
    } else {
         log_to_file_only("UpdatePeerDisplayList: No prior selection (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
    }
    PruneTimedOutPeers();
    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);
    if (*gPeerListHandle == NULL) {
        log_message("UpdatePeerDisplayList Error: gPeerListHandle deref failed after HLock!");
        HSetState((Handle)gPeerListHandle, listState);
        return;
    }
    currentListLengthInRows = (**gPeerListHandle).dataBounds.bottom;
    if (currentListLengthInRows > 0) {
        LDelRow(currentListLengthInRows, 0, gPeerListHandle);
        log_to_file_only("UpdatePeerDisplayList: Deleted %d rows from List Manager.", currentListLengthInRows);
    }
    Cell tempNewSelectedCell = {0, -1};
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            const char *displayName = (gPeerManager.peers[i].username[0] != '\0') ? gPeerManager.peers[i].username : "???";
            sprintf(peerStr, "%s@%s", displayName, gPeerManager.peers[i].ip);
            LAddRow(1, activePeerCount, gPeerListHandle);
            SetPt(&theCell, 0, activePeerCount);
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);
            if (hadOldSelectionData && strcmp(gPeerManager.peers[i].ip, oldSelectedPeerData.ip) == 0 &&
                strcmp(gPeerManager.peers[i].username, oldSelectedPeerData.username) == 0) {
                tempNewSelectedCell = theCell;
                oldSelectionStillValidAndFound = true;
            }
            activePeerCount++;
        }
    }
    (**gPeerListHandle).dataBounds.bottom = activePeerCount;
    if (oldSelectionStillValidAndFound) {
        LSetSelect(true, tempNewSelectedCell, gPeerListHandle);
        gLastSelectedCell = tempNewSelectedCell;
        log_to_file_only("UpdatePeerDisplayList: Reselected peer '%s@%s' at new display row %d.", oldSelectedPeerData.username, oldSelectedPeerData.ip, gLastSelectedCell.v);
    } else {
        if (hadOldSelectionData) {
             log_to_file_only("UpdatePeerDisplayList: Previous selection '%s@%s' not found/reselected or became inactive.", oldSelectedPeerData.username, oldSelectedPeerData.ip);
        }
        SetPt(&gLastSelectedCell, 0, -1);
    }
    HSetState((Handle)gPeerListHandle, listState);
    if (forceRedraw || activePeerCount != currentListLengthInRows || oldSelectionStillValidAndFound || (hadOldSelectionData && !oldSelectionStillValidAndFound) ) {
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPortForDrawing;
            GetPort(&oldPortForDrawing);
            SetPort(windowPort);
            listState = HGetState((Handle)gPeerListHandle);
            HLock((Handle)gPeerListHandle);
            if (*gPeerListHandle != NULL) {
                InvalRect(&(**gPeerListHandle).rView);
            }
            HSetState((Handle)gPeerListHandle, listState);
            SetPort(oldPortForDrawing);
            log_message("Peer list updated. Active peers: %d. Invalidating list rect.", activePeerCount);
        } else {
            log_message("Peer list updated, but cannot invalidate rect (window port is NULL).");
        }
    } else {
        log_to_file_only("UpdatePeerDisplayList: No significant change detected for redraw. Active: %d, OldRows: %d.", activePeerCount, currentListLengthInRows);
    }
}
void HandlePeerListUpdate(DialogPtr dialog)
{
    if (gPeerListHandle != NULL) {
        GrafPtr windowPort = GetWindowPort(dialog);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort);
            LUpdate(windowPort->visRgn, gPeerListHandle);
            SetPort(oldPort);
        } else {
            log_message("HandlePeerListUpdate Error: Cannot update list, window port is NULL.");
        }
    }
}
void ActivatePeerList(Boolean activating)
{
    if (gPeerListHandle != NULL) {
        LActivate(activating, gPeerListHandle);
        log_to_file_only("ActivatePeerList: List 0x%lX %s.",
                         (unsigned long)gPeerListHandle, activating ? "activated" : "deactivated");
    }
}
