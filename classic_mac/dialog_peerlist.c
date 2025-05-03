#include "dialog_peerlist.h"
#include "dialog.h" // Needed for kPeerListUserItem define
#include "logging.h"
#include "peer_mac.h" // For PruneTimedOutPeers

#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Fonts.h>    // For GetFontInfo
#include <Memory.h>   // For HLock, HSetState
#include <Resources.h> // For ResError
#include <stdio.h>    // For sprintf
#include <string.h>   // For strlen

/*----------------------------------------------------------*/
/* Implementation                                           */
/*----------------------------------------------------------*/

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

        // Calculate cell size based on current font in the dialog's port
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog)); // Use dialog's port for font info
        GetFontInfo(&fontInfo);
        SetPort(oldPort);

        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading;
        if (cellSize.v <= 0) { // Basic sanity check
            log_message("Warning: Calculated cell height is %d, using default 12.", cellSize.v);
            cellSize.v = 12;
        }
        cellSize.h = destRectList.right - destRectList.left; // Cell width is list width

        // Initial data bounds: 1 column, 0 rows
        SetRect(&dataBounds, 0, 0, 1, 0);

        log_message("Calling LNew for Peer List (Cell Size: H%d, V%d)...", cellSize.h, cellSize.v);
        // LNew parameters: viewRect, dataBounds, cellSize, resID (0), window,
        // drawIt, hasGrow, scrollHoriz, scrollVert
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)dialog,
                               true, false, false, true); // No horiz scroll, yes vert scroll

        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            return false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            // Set list properties - allow only single selection
            (*gPeerListHandle)->selFlags = lOnlyOne;
            // Could set other flags via (*gPeerListHandle)->lFlags if needed
            LActivate(true, gPeerListHandle); // Activate the list
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
        // Deactivate before disposing might be good practice, though LDispose should handle it
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
         SetPort(GetWindowPort(dialog)); // Use dialog's port for coordinate conversion
         GlobalToLocal(&localClick);
         SetPort(oldPort);

         // Check if the click was within the list's view rectangle
         // Need to lock handle to access rView safely
         SignedByte listState = HGetState((Handle)gPeerListHandle);
         HLock((Handle)gPeerListHandle);
         Boolean clickedInside = false;
         if (*gPeerListHandle != NULL) {
             clickedInside = PtInRect(localClick, &(**gPeerListHandle).rView);
         } else {
             log_message("HandlePeerListClick Error: gPeerListHandle deref failed!");
         }
         HSetState((Handle)gPeerListHandle, listState); // Unlock handle

         if (clickedInside) {
             log_message("HandlePeerListClick: Click inside Peer List view rect. Calling LClick.");
             // LClick returns true if it handled the click (selection change, etc.)
             listClicked = LClick(localClick, theEvent->modifiers, gPeerListHandle);

             // Update our tracked selection cell
             // LGetSelect(true, ...) gets the *first* selected cell if any are selected
             if (!LGetSelect(true, &gLastSelectedCell, gPeerListHandle)) {
                 // No selection after click (or LClick returned false), reset our tracker
                 SetPt(&gLastSelectedCell, 0, 0); // Or use {-1, -1} if preferred
                 log_message("HandlePeerListClick: No selection after LClick.");
             } else {
                 log_message("HandlePeerListClick: Peer list item selected: Row %d, Col %d", gLastSelectedCell.v, gLastSelectedCell.h);
             }
         } else {
              log_message("HandlePeerListClick: Click was outside Peer List view rect.");
         }
    }
    return listClicked; // Return whether LClick handled it
}

void UpdatePeerDisplayList(Boolean forceRedraw) {
    Cell theCell;
    char peerStr[INET_ADDRSTRLEN + 32 + 2]; // username@ip + null
    int currentListLength; // Number of rows currently in the List Manager list
    int activePeerCount = 0; // Number of active peers found in gPeerList
    SignedByte listState;
    Boolean selectionStillValid = false;
    Cell oldSelection = gLastSelectedCell; // Remember the selection before update

    if (gPeerListHandle == NULL) {
        log_message("Skipping UpdatePeerDisplayList: List not initialized.");
        return;
    }

    // Lock the list handle for the duration of the update
    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);

    if (*gPeerListHandle == NULL) {
        log_message("UpdatePeerDisplayList Error: gPeerListHandle deref failed after HLock!");
        HSetState((Handle)gPeerListHandle, listState);
        return;
    }

    // Get the number of rows currently displayed
    currentListLength = (**gPeerListHandle).dataBounds.bottom;

    // Delete all existing rows efficiently (deleting 0 rows is safe)
    // LDelRow(count, startRow, listHandle)
    if (currentListLength > 0) {
        LDelRow(0, 0, gPeerListHandle); // 0 count means delete all
    }

    // Prune timed-out peers from the underlying data source (gPeerList)
    PruneTimedOutPeers(); // Assumes this modifies gPeerList directly

    // Iterate through the peer data and add rows for active peers
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            // Format the string to display: "username@ip"
            // Ensure username is not empty for display purposes
            const char *displayName = (gPeerList[i].username[0] != '\0') ? gPeerList[i].username : "???";
            sprintf(peerStr, "%s@%s", displayName, gPeerList[i].ip);

            // Add a new row at the end of the list (index activePeerCount)
            // LAddRow(count, rowNum, listHandle)
            LAddRow(1, activePeerCount, gPeerListHandle);

            // Set the content of the cell in the new row
            SetPt(&theCell, 0, activePeerCount); // Column 0, new row index
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);

            // Check if the previously selected row index matches the current active peer index
            if (oldSelection.v == activePeerCount) {
                 selectionStillValid = true;
            }

            activePeerCount++; // Increment count of displayed peers
        }
    }

     // Update the list's internal dataBounds to reflect the new number of rows
     (**gPeerListHandle).dataBounds.bottom = activePeerCount;

     // Restore selection if it's still valid
     if (selectionStillValid && oldSelection.v < activePeerCount) {
         LSetSelect(true, oldSelection, gPeerListHandle);
         gLastSelectedCell = oldSelection; // Keep tracking the valid selection
     } else {
         // If selection is no longer valid (peer timed out or list empty), clear it
         if (oldSelection.v >= 0 && oldSelection.v < currentListLength) { // Check if old selection was valid before clearing
            LSetSelect(false, oldSelection, gPeerListHandle);
         }
         SetPt(&gLastSelectedCell, 0, 0); // Reset tracked selection
     }

    HSetState((Handle)gPeerListHandle, listState); // Unlock the list handle

    // Invalidate the list's view rectangle if the number of rows changed
    // or if explicitly requested, to force a redraw.
    if (forceRedraw || activePeerCount != currentListLength) {
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort);

            // Lock handle again briefly to access rView
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
            // LUpdate redraws the visible portion of the list
            // It takes the update region of the window as a parameter
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

    // Check if gLastSelectedCell indicates a valid selection row
    if (gLastSelectedCell.v < 0) { // Use < 0 check if {0,0} could be a valid selection
         log_to_file_only("GetSelectedPeerInfo: No peer selected (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
        return false;
    }

    int selectedDisplayRow = gLastSelectedCell.v;
    int current_active_count = 0;

    // Prune first to ensure the list display matches the underlying data state
    PruneTimedOutPeers();

    // Iterate through the peer data, counting only active peers
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            // If the current active peer count matches the selected display row index...
            if (current_active_count == selectedDisplayRow) {
                // ...we found the corresponding peer in the data array.
                *outPeer = gPeerList[i]; // Copy the peer data
                log_to_file_only("GetSelectedPeerInfo: Found selected peer '%s'@'%s' at display row %d (data index %d).",
                                 outPeer->username, outPeer->ip, selectedDisplayRow, i);
                return true;
            }
            current_active_count++; // Increment the count for the next active peer
        }
    }

    // If we finish the loop without finding a match, the selection is invalid
    // (e.g., list changed since last click)
    log_message("GetSelectedPeerInfo Warning: Selected row %d is out of bounds for current active peers (%d).",
                selectedDisplayRow, current_active_count);
    return false;
}