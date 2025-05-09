#include "dialog_peerlist.h"
#include "dialog.h"    // For kBroadcastCheckbox, gMainWindow
#include "logging.h"
#include "peer.h"      // For gPeerManager, PruneTimedOutPeers
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
Cell gLastSelectedCell = {0, -1}; // {h, v} - v = -1 means no selection

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
        SetPort(GetWindowPort(dialog)); // Use dialog's port for font info
        GetFontInfo(&fontInfo);
        SetPort(oldPort);

        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading;
        if (cellSize.v <= 0) {
            log_message("Warning: Calculated cell height is %d, using default 12.", cellSize.v);
            cellSize.v = 12; // A common default
        }
        cellSize.h = destRectList.right - destRectList.left; // Full width for now

        SetRect(&dataBounds, 0, 0, 1, 0); // 1 column, 0 rows initially

        log_message("Calling LNew for Peer List (Cell Size: H%d, V%d)...", cellSize.h, cellSize.v);
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)dialog,
                               true,  // drawIt
                               false, // hasGrow
                               false, // scrollHoriz
                               true); // scrollVert (though we don't provide a scrollbar DITL item for it yet)

        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            listOk = false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            (*gPeerListHandle)->selFlags = lOnlyOne; // Allow only single selection
            LActivate(true, gPeerListHandle); // Activate the list
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
        LActivate(false, gPeerListHandle); // Deactivate before disposing
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    gLastSelectedCell.v = -1; // Reset selection state
    log_message("Peer List Control cleanup finished.");
}

Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent)
{
    Boolean clickWasInContent = false;
    if (gPeerListHandle != NULL) {
        Point localClick = theEvent->where;
        GrafPtr oldPort;

        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog)); // Convert to dialog's local coordinates
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

            // LClick handles selection changes and updates gLastSelectedCell internally if lOnlyOne is set
            (void)LClick(localClick, theEvent->modifiers, gPeerListHandle);

            Cell clickedCell = LLastClick(gPeerListHandle); // Get the cell LClick determined was clicked
            Cell cellToVerify = clickedCell; // LGetSelect modifies the cell passed in, so use a copy

            // Verify that the clicked cell is actually selected
            if (clickedCell.v >= 0 && clickedCell.h >= 0) { // Valid cell coordinates
                if (LGetSelect(false, &cellToVerify, gPeerListHandle)) { // Check if this cell is selected
                    gLastSelectedCell = clickedCell; // Update our master selection state
                    log_to_file_only("HandlePeerListClick: LLastClick cell (%d,%d) IS selected. Unchecking broadcast.", gLastSelectedCell.h, gLastSelectedCell.v);

                    // If a peer is selected, uncheck the broadcast checkbox
                    DialogItemType itemType;
                    Handle itemHandle;
                    Rect itemRect;
                    ControlHandle broadcastCheckboxHandle;
                    GrafPtr clickOldPort; // Save port again for GetDialogItem which might change it
                    GetPort(&clickOldPort);
                    SetPort(GetWindowPort(gMainWindow)); // Ensure port is dialog for GetDialogItem

                    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
                        broadcastCheckboxHandle = (ControlHandle)itemHandle;
                        SetControlValue(broadcastCheckboxHandle, 0); // Uncheck
                    } else {
                        log_message("HandlePeerListClick: Could not find/set broadcast checkbox (item %d).", kBroadcastCheckbox);
                    }
                    SetPort(clickOldPort); // Restore port
                } else {
                    // LClick might have deselected it (e.g. clicking an already selected cell with certain modifiers)
                    log_to_file_only("HandlePeerListClick: LLastClick cell (%d,%d) is NOT selected by LGetSelect(false,...). Clearing selection.", clickedCell.h, clickedCell.v);
                    SetPt(&gLastSelectedCell, 0, -1); // No selection
                }
            } else {
                // Click was in the list but not on a specific cell (e.g. empty area)
                log_to_file_only("HandlePeerListClick: LLastClick returned invalid cell (%d,%d) after LClick. Clearing selection.", clickedCell.h, clickedCell.v);
                SetPt(&gLastSelectedCell, 0, -1); // No selection
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
    char peerStr[INET_ADDRSTRLEN + 32 + 2]; // username@ip + null
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

    // Try to preserve selection
    if (gLastSelectedCell.v >= 0) { // If there was a selection
        // Use the (now local) function to get data for the previously selected peer
        hadOldSelectionData = DialogPeerList_GetSelectedPeer(&oldSelectedPeerData);
        if (hadOldSelectionData) {
            log_to_file_only("UpdatePeerDisplayList: Attempting to preserve selection for peer %s@%s (was display row %d).", oldSelectedPeerData.username, oldSelectedPeerData.ip, gLastSelectedCell.v);
        } else {
            // This means gLastSelectedCell.v was >= 0, but DialogPeerList_GetSelectedPeer failed
            // (e.g., peer became inactive). DialogPeerList_GetSelectedPeer would have reset gLastSelectedCell.v.
            log_to_file_only("UpdatePeerDisplayList: gLastSelectedCell.v was %d, but DialogPeerList_GetSelectedPeer failed. No specific peer to preserve.", gLastSelectedCell.v /* this might be -1 now */);
        }
    } else {
         log_to_file_only("UpdatePeerDisplayList: No prior selection (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
    }

    PruneTimedOutPeers(); // Prune before redrawing

    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);

    if (*gPeerListHandle == NULL) {
        log_message("UpdatePeerDisplayList Error: gPeerListHandle deref failed after HLock!");
        HSetState((Handle)gPeerListHandle, listState);
        return;
    }

    currentListLengthInRows = (**gPeerListHandle).dataBounds.bottom;

    // Clear existing rows
    if (currentListLengthInRows > 0) {
        LDelRow(currentListLengthInRows, 0, gPeerListHandle); // Delete all rows starting from row 0
        log_to_file_only("UpdatePeerDisplayList: Deleted %d rows from List Manager.", currentListLengthInRows);
    }

    // Reset selection for now; will be restored if found
    Cell tempNewSelectedCell = {0, -1};
    // gLastSelectedCell.v = -1; // Done by DialogPeerList_GetSelectedPeer if it fails, or if no initial selection

    // Add active peers
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            const char *displayName = (gPeerManager.peers[i].username[0] != '\0') ? gPeerManager.peers[i].username : "???";
            sprintf(peerStr, "%s@%s", displayName, gPeerManager.peers[i].ip);

            LAddRow(1, activePeerCount, gPeerListHandle); // Add 1 row at the end (current activePeerCount index)
            SetPt(&theCell, 0, activePeerCount); // Column 0, current row
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);

            // Check if this is the previously selected peer
            if (hadOldSelectionData &&
                strcmp(gPeerManager.peers[i].ip, oldSelectedPeerData.ip) == 0 &&
                strcmp(gPeerManager.peers[i].username, oldSelectedPeerData.username) == 0) {
                tempNewSelectedCell = theCell; // This is the new cell for the old selection
                oldSelectionStillValidAndFound = true;
            }
            activePeerCount++;
        }
    }

    // Update dataBounds to reflect the new number of rows
    (**gPeerListHandle).dataBounds.bottom = activePeerCount;

    // Restore selection if the peer is still active and found
    if (oldSelectionStillValidAndFound) {
        LSetSelect(true, tempNewSelectedCell, gPeerListHandle);
        gLastSelectedCell = tempNewSelectedCell; // Update our master selection state
        log_to_file_only("UpdatePeerDisplayList: Reselected peer '%s@%s' at new display row %d.", oldSelectedPeerData.username, oldSelectedPeerData.ip, gLastSelectedCell.v);
    } else {
        if (hadOldSelectionData) {
             log_to_file_only("UpdatePeerDisplayList: Previous selection '%s@%s' not found/reselected or became inactive.", oldSelectedPeerData.username, oldSelectedPeerData.ip);
        }
        // If no selection was restored, gLastSelectedCell should be {-1, -1} or {0, -1}
        // LLastClick and LGetSelect handle this, or DialogPeerList_GetSelectedPeer if it failed.
        // Explicitly ensure no selection if nothing was restored.
        if (!oldSelectionStillValidAndFound) {
             SetPt(&gLastSelectedCell, 0, -1);
        }
    }

    HSetState((Handle)gPeerListHandle, listState);

    // Determine if a redraw is needed
    if (forceRedraw || activePeerCount != currentListLengthInRows || oldSelectionStillValidAndFound || (hadOldSelectionData && !oldSelectionStillValidAndFound) ) {
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPortForDrawing;
            GetPort(&oldPortForDrawing);
            SetPort(windowPort);

            listState = HGetState((Handle)gPeerListHandle); // Lock again for InvalRect context
            HLock((Handle)gPeerListHandle);
            if (*gPeerListHandle != NULL) {
                InvalRect(&(**gPeerListHandle).rView); // Invalidate the list's view rectangle
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
            SetPort(windowPort); // Set port to the dialog's window
            LUpdate(windowPort->visRgn, gPeerListHandle); // Update the list within the visible region
            SetPort(oldPort);
        } else {
            log_message("HandlePeerListUpdate Error: Cannot update list, window port is NULL.");
        }
    }
}

// MOVED & RENAMED from classic_mac/peer.c
Boolean DialogPeerList_GetSelectedPeer(peer_t *outPeer)
{
    // gPeerListHandle is a global in this file
    // gLastSelectedCell is a global in this file
    // gPeerManager is an extern from "peer.h"

    if (gPeerListHandle == NULL || outPeer == NULL) {
        return false;
    }

    if (gLastSelectedCell.v < 0) { // v is the row index; -1 means no selection
        log_to_file_only("DialogPeerList_GetSelectedPeer: No peer selected (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
        return false;
    }

    int selectedDisplayRow = gLastSelectedCell.v;
    int current_active_peer_index = 0; // This will be the 0-based index into the *displayed* list

    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            if (current_active_peer_index == selectedDisplayRow) {
                *outPeer = gPeerManager.peers[i];
                log_to_file_only("DialogPeerList_GetSelectedPeer: Found selected peer '%s'@'%s' at display row %d (data index %d).",
                                 (outPeer->username[0] ? outPeer->username : "???"), outPeer->ip, selectedDisplayRow, i);
                return true;
            }
            current_active_peer_index++;
        }
    }

    // If we reach here, the selectedDisplayRow was out of bounds for the current active peers
    // (e.g., peer became inactive since last selection)
    log_message("DialogPeerList_GetSelectedPeer Warning: Selected row %d is out of bounds or peer became inactive (current active peers: %d).",
                selectedDisplayRow, current_active_peer_index);
    SetPt(&gLastSelectedCell, 0, -1); // Clear the invalid selection
    return false;
}

// NEW function
void DialogPeerList_DeselectAll(void) {
    if (gPeerListHandle != NULL && gLastSelectedCell.v >= 0) {
        GrafPtr oldPortForList;
        GetPort(&oldPortForList);
        SetPort(GetWindowPort(gMainWindow)); // Assumes gMainWindow is valid and is the list's window

        LSetSelect(false, gLastSelectedCell, gPeerListHandle); // Deselect the current cell
        SetPt(&gLastSelectedCell, 0, -1); // Clear our record of the selection

        // Invalidate the list's view to ensure it redraws correctly
        SignedByte listState = HGetState((Handle)gPeerListHandle);
        HLock((Handle)gPeerListHandle);
        if (*gPeerListHandle != NULL) {
            InvalRect(&(**gPeerListHandle).rView);
        }
        HSetState((Handle)gPeerListHandle, listState);

        SetPort(oldPortForList);
        log_to_file_only("DialogPeerList_DeselectAll: Cleared selection and invalidated view.");
    } else {
        log_to_file_only("DialogPeerList_DeselectAll: No selection to clear or list not initialized.");
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