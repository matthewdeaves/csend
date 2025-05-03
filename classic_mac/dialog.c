//====================================
// FILE: ./classic_mac/dialog.c
//====================================

#include "dialog.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "common_defs.h"
#include "peer_mac.h"

#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Memory.h>
#include <Sound.h>
#include <Resources.h>
#include <Lists.h> // Ensure List Manager is included
#include <Fonts.h> // For GetFontInfo
#include <Events.h> // For modifier key constants like shiftKey
#include <Windows.h> // For WindowRecord/GrafPort access
#include <string.h>
#include <stdio.h> // For sprintf
#include <stdlib.h>

DialogPtr gMainWindow = NULL;
TEHandle gMessagesTE = NULL;
TEHandle gInputTE = NULL;
ListHandle gPeerListHandle = NULL; // Initialize ListHandle to NULL
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false; // Initialize List state
char gMyUsername[32] = "MacUser";
Cell gLastSelectedCell = {0, 0}; // Initialize selected cell

// Forward declaration if needed, or move UpdatePeerDisplayList above InitDialog
void UpdatePeerDisplayList(Boolean forceRedraw);

Boolean InitDialog(void) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect destRectInput, viewRectInput;
    Rect destRectList, dataBounds; // Rects for List Manager
    Point cellSize; // Cell size for List Manager
    FontInfo fontInfo; // For calculating list cell height
    Boolean messagesOk = false;
    Boolean inputOk = false;
    Boolean listOk = false;

    log_message("Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
    if (gMainWindow == NULL) {
        log_message("Fatal: GetNewDialog failed (Error: %d).", ResError());
        return false;
    }
    log_message("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);

    log_message("Showing window...");
    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);

    log_message("Setting port...");
    SetPort((GrafPtr)gMainWindow);
    log_message("Port set.");

    // --- Initialize TextEdit Fields ---
    log_message("Getting item %d info (Messages UserItem)...", kMessagesTextEdit);
    GetDialogItem(gMainWindow, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    if (itemType == userItem) {
        viewRectMessages = destRectMessages;
        InsetRect(&viewRectMessages, 1, 1); // Inset view rect slightly for border
        log_message("Calling TENew for Messages TE...");
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(false, gMessagesTE); // Keep TEAutoView disabled for messages
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        messagesOk = false;
    }

    log_message("Getting item %d info (Input UserItem)...", kInputTextEdit);
    GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        viewRectInput = destRectInput;
        InsetRect(&viewRectInput, 1, 1); // Inset view rect slightly
        log_message("Calling TENew for Input TE...");
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            inputOk = false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE); // Keep AutoView for input field
            inputOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        inputOk = false;
    }
    gDialogTEInitialized = (messagesOk && inputOk);
    log_message("Dialog TE fields initialization complete (Success: %s).", gDialogTEInitialized ? "YES" : "NO");

    // --- Initialize List Manager Field ---
    log_message("Getting item %d info (Peer List UserItem)...", kPeerListUserItem);
    GetDialogItem(gMainWindow, kPeerListUserItem, &itemType, &itemHandle, &destRectList);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kPeerListUserItem,
                    destRectList.top, destRectList.left, destRectList.bottom, destRectList.right);

        // Calculate cell size
        GetFontInfo(&fontInfo); // Get info for current port's font
        SetPt(&cellSize, destRectList.right - destRectList.left, fontInfo.ascent + fontInfo.descent + fontInfo.leading);

        // Define data bounds (1 column, initially 0 rows, max MAX_PEERS)
        SetRect(&dataBounds, 0, 0, 1, 0); // Start with 0 rows, 1 column

        // Create the list
        log_message("Calling LNew for Peer List...");
        // Set drawIt parameter to TRUE for initial drawing
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)gMainWindow, true, false, false, true);
        // LNew(rView, dataBounds, cSize, procID, theWindow, drawIt, hasGrow, scrollHoriz, scrollVert)

        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            listOk = false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            (*gPeerListHandle)->selFlags = lOnlyOne; // Allow only single selection
            listOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for LNew.", kPeerListUserItem, itemType);
        gPeerListHandle = NULL;
        listOk = false;
    }
    gDialogListInitialized = listOk;
    log_message("Dialog List Manager initialization complete (Success: %s).", gDialogListInitialized ? "YES" : "NO");

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components failed to initialize.");
        // Cleanup what might have been allocated before returning false
        CleanupDialog();
        return false;
    }

    // Initial population of the list
    UpdatePeerDisplayList(true); // Force initial population and redraw if needed

    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    if (gInputTE) TEActivate(gInputTE);
    log_message("Input TE activated.");

    return true;
}

void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

    if (gPeerListHandle != NULL) {
        log_message("Disposing Peer List...");
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    if (gMessagesTE != NULL) {
        log_message("Disposing Messages TE...");
        TEDispose(gMessagesTE);
        gMessagesTE = NULL;
    }
    if (gInputTE != NULL) {
        log_message("Disposing Input TE...");
        TEDispose(gInputTE);
        gInputTE = NULL;
    }
    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
    gDialogTEInitialized = false;
    gDialogListInitialized = false;
    log_message("Dialog cleanup complete.");
}

// Modified to accept click location and event record for modifiers
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent) { // Pass the whole event
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    short currentValue;
    Boolean listClicked = false;
    Point clickLoc = theEvent->where; // Get global click location from event

    if (dialog != gMainWindow) return;

    // Check if the click was in the List Manager item *before* the switch
    if (gPeerListHandle != NULL && itemHit == kPeerListUserItem) {
         Point localClick = clickLoc; // Copy the point
         SetPort((GrafPtr)dialog); // Ensure port is correct for GlobalToLocal
         GlobalToLocal(&localClick);

         // Check if the local click is within the list's view rect
         if (PtInRect(localClick, &(**gPeerListHandle).rView)) {
             log_message("Click is inside Peer List view rect. Calling LClick.");
             // Pass modifiers directly from the event record
             listClicked = LClick(localClick, theEvent->modifiers, gPeerListHandle);
             // LClick handles selection highlighting.
             // listClicked is true if selection changed, false otherwise.
             // We might want to store the selection:
             if (!LGetSelect(true, &gLastSelectedCell, gPeerListHandle)) {
                 // No selection or error
                 SetPt((Point*)&gLastSelectedCell, 0, 0); // Reset selection
             } else {
                 log_message("Peer list item selected: Row %d, Col %d", gLastSelectedCell.v, gLastSelectedCell.h);
             }
         } else {
              log_message("Click in Peer List item rect, but outside LClick view rect.");
         }
    }

    // Handle other items only if the list wasn't clicked (or handle list click side effects here)
    if (!listClicked) {
        switch (itemHit) {
            case kSendButton:
                log_message("Send button clicked.");
                DoSendAction(dialog);
                break;
            case kBroadcastCheckbox:
                GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                if (itemType == (ctrlItem + chkCtrl)) {
                    checkboxHandle = (ControlHandle)itemHandle;
                    currentValue = GetControlValue(checkboxHandle);
                    SetControlValue(checkboxHandle, !currentValue);
                    log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
                    // Force redraw of message TE after checkbox toggle (as it logs)
                    AppendToMessagesTE(""); // Append empty string to trigger redraw logic
                } else {
                    log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
                }
                break;
            // Add cases for kMessagesTextEdit and kInputTextEdit if needed,
            // although TEActivate/TEDeactivate usually handle focus clicks.
            // LClick handles clicks within the list itself.
            default:
                 // Click might be outside any active item, e.g., to activate TE field
                 // Need local coordinates for TEClick
                 { // Create a new scope for localPt
                     Point localPt = clickLoc;
                     SetPort((GrafPtr)dialog);
                     GlobalToLocal(&localPt);

                     if (gInputTE && PtInRect(localPt, &(**gInputTE).viewRect)) {
                         // Pass shift key state from event modifiers for extend parameter
                         TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
                     } else if (gMessagesTE && PtInRect(localPt, &(**gMessagesTE).viewRect)) {
                         // Usually don't handle clicks in read-only TE fields
                     }
                 }
                break;
        }
    }
}

void DoSendAction(DialogPtr dialog) {
    char inputCStr[256];
    char formattedMsg[BUFFER_SIZE];
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100];
    SignedByte teState;

    if (gInputTE == NULL) {
        log_message("Error: Input TextEdit not initialized. Cannot send.");
        SysBeep(10);
        return;
    }

    // Get text from input TE field
    teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);
    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        Size textLen = (**gInputTE).teLength;
        if (textLen > sizeof(inputCStr) - 1) {
             textLen = sizeof(inputCStr) - 1;
             log_message("Warning: Input text truncated to %d bytes.", textLen);
        }
        BlockMoveData(*((**gInputTE).hText), inputCStr, textLen);
        inputCStr[textLen] = '\0';
    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
        HSetState((Handle)gInputTE, teState);
        SysBeep(10);
        return;
    }
    HSetState((Handle)gInputTE, teState);

    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        // SysBeep(5); // Maybe don't beep for empty send attempt
        return;
    }

    // Check broadcast checkbox state
    GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false;
    }

    // Format the message (using shared protocol function)
    int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                      gMyUsername, gMyLocalIPStr, inputCStr);

    if (formatResult > 0) { // format_message returns length on success, 0 on error
        if (isBroadcast) {
            log_message("Broadcasting: %s (Network send not implemented yet)", inputCStr);
            // TODO: Implement broadcast send loop here
            sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r");

        } else {
            // Send to selected peer
            int selectedRow = gLastSelectedCell.v; // Row index is 0-based
            int activePeerIndex = -1;
            int current_active_count = 0;

            // Need to map selected list row back to an active peer index in gPeerList
            PruneTimedOutPeers(); // Ensure list is current before mapping index
            for (int i = 0; i < MAX_PEERS; i++) {
                if (gPeerList[i].active) {
                    if (current_active_count == selectedRow) {
                        activePeerIndex = i;
                        break;
                    }
                    current_active_count++;
                }
            }

            if (activePeerIndex != -1) {
                peer_t targetPeer = gPeerList[activePeerIndex];
                log_message("Sending to selected peer %d (%s@%s): %s (Network send not implemented yet)",
                             selectedRow + 1, targetPeer.username, targetPeer.ip, inputCStr);
                // TODO: Implement single peer send here using targetPeer.ip
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
            } else {
                 log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
                 SysBeep(10);
            }
        }

        // Clear input field only if send was attempted (formatted ok)
        if (gInputTE != NULL) {
            teState = HGetState((Handle)gInputTE);
            HLock((Handle)gInputTE);
            if (*gInputTE != NULL) {
                TESetText((Ptr)"", 0, gInputTE);
                TECalText(gInputTE);
            }
            HSetState((Handle)gInputTE, teState);
            log_message("Input field cleared.");
        }

    } else {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
    }

    // Reactivate input field
    if (gInputTE != NULL) {
       TEActivate(gInputTE);
       // log_message("Input field activated."); // Maybe too verbose
    }
}

void AppendToMessagesTE(const char *text) {
    Boolean scroll = false; // Flag to indicate if scrolling is needed
    GrafPtr oldPort; // To restore port

    if (gMessagesTE == NULL || !gDialogTEInitialized) {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "Skipping AppendToMessagesTE: gMessagesTE is NULL or dialog not initialized. Msg: %s\n", text);
            fflush(gLogFile);
        }
        return;
    }

    // Ensure the dialog's port is current before TE operations
    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow)); // Use GetWindowPort for safety

    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);

    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        long selStart = (**gMessagesTE).selStart;
        long selEnd = (**gMessagesTE).selEnd;

        // Check if selection is already at the end (insertion point at end)
        if (selStart == currentLength && selEnd == currentLength) {
            scroll = true;
        }

        // Check if adding text exceeds TE limit (32K) - simple check
        if (currentLength + strlen(text) < 32000) {
            TESetSelect(currentLength, currentLength, gMessagesTE); // Select end
            TEInsert((Ptr)text, strlen(text), gMessagesTE);

            // Scroll to show the new text ONLY if insertion point was at the end
            if (scroll) {
                TEScroll(0, (**gMessagesTE).nLines * (**gMessagesTE).lineHeight, gMessagesTE);
            }

            // Recalculate line breaks AFTER inserting and scrolling
            TECalText(gMessagesTE); // <-- Add TECalText

            // Invalidate the view rectangle AFTER all modifications
            InvalRect(&(**gMessagesTE).viewRect); // <-- Use InvalRect

            // Remove the forced TEUpdate call
            // TEUpdate(&(**gMessagesTE).viewRect, gMessagesTE);

        } else {
            log_message("Warning: Messages TE field is full. Cannot append.");
            // Optional: Implement text clearing logic here
        }
    } else {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!\n");
            fflush(gLogFile);
        }
    }

    HSetState((Handle)gMessagesTE, teState);
    SetPort(oldPort); // Restore original port
}


void ActivateDialogTE(Boolean activating) {
    // Note: List Manager doesn't have an activate/deactivate call like TE.
    // Selection highlighting is handled by LClick and LUpdate.
    if (gMessagesTE != NULL) {
        if (activating) {
            // TEActivate(gMessagesTE); // Messages field likely read-only, no activation needed
        } else {
            TEDeactivate(gMessagesTE);
        }
    }
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
        } else {
            TEDeactivate(gInputTE);
        }
    }
}

// Renamed from UpdateDialogTE to reflect it updates more controls now
void UpdateDialogControls(void) {
    Rect itemRect;
    DialogItemType itemTypeIgnored;
    Handle itemHandleIgnored;
    GrafPtr oldPort;
    GrafPtr windowPort; // Use GrafPtr for GetWindowPort result

    GetPort(&oldPort);
    windowPort = GetWindowPort(gMainWindow); // Get the window's port
    SetPort(windowPort); // Ensure port is correct

    // Update TextEdit fields
    if (gMessagesTE != NULL) {
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // TEUpdate is still needed here for general window updates (expose, etc.)
        TEUpdate(&itemRect, gMessagesTE);
    }
    if (gInputTE != NULL) {
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gInputTE);
    }

    // Update List Manager field
    if (gPeerListHandle != NULL) {
        // Use LUpdate with the window's visRgn.
        LUpdate(windowPort->visRgn, gPeerListHandle);
    }

    SetPort(oldPort); // Restore original port
}


// New function to update the List Manager list from gPeerList
void UpdatePeerDisplayList(Boolean forceRedraw) {
    Cell theCell;
    char peerStr[INET_ADDRSTRLEN + 32 + 2]; // IP + @ + User + safety
    int currentListLength;
    int activePeerCount = 0;
    SignedByte listState;
    Boolean selectionStillValid = false;
    Cell oldSelection = gLastSelectedCell; // Keep track of old selection

    if (gPeerListHandle == NULL || !gDialogListInitialized) {
        log_message("Skipping UpdatePeerDisplayList: List not initialized.");
        return;
    }

    // Lock the ListHandle while modifying
    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);

    // Get current number of rows
    currentListLength = (**gPeerListHandle).dataBounds.bottom;

    // Clear existing rows (LDelRow(0, startRow, listHandle) deletes all rows from startRow)
    if (currentListLength > 0) {
        LDelRow(0, 0, gPeerListHandle);
    }
    // Don't reset gLastSelectedCell here, try to preserve it below

    // Iterate through gPeerList and add active peers
    PruneTimedOutPeers(); // Ensure peer list is up-to-date before display
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            // Format the string: "Username@IP"
            sprintf(peerStr, "%s@%s", gPeerList[i].username, gPeerList[i].ip);

            // Add a row for this peer (at the end, index activePeerCount)
            LAddRow(1, activePeerCount, gPeerListHandle);

            // Set the cell data
            SetPt((Point*)&theCell, 0, activePeerCount); // Column 0, Row activePeerCount
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);

            // Check if this newly added row corresponds to the previously selected peer
            if (oldSelection.v == activePeerCount) { // Check if the row index matches
                 // We could add a check here to see if the peer data (IP/User) actually matches
                 // the peer that *was* at oldSelection.v, but for now, just matching row index
                 // might be sufficient if the list order is relatively stable.
                 selectionStillValid = true;
            }

            activePeerCount++;
        }
    }

     // Update dataBounds to reflect the new number of rows
     (**gPeerListHandle).dataBounds.bottom = activePeerCount;

     // Try to restore selection if it's still valid
     if (selectionStillValid && oldSelection.v < activePeerCount) {
         LSetSelect(true, oldSelection, gPeerListHandle);
         gLastSelectedCell = oldSelection; // Ensure global reflects restored selection
     } else {
         // Selection is no longer valid (peer disappeared or list changed too much)
         SetPt((Point*)&gLastSelectedCell, 0, 0); // Reset global selection state
         // Ensure nothing is selected in the list itself
         if (currentListLength > 0) { // Only deselect if there was something to deselect
            LSetSelect(false, oldSelection, gPeerListHandle); // Deselect the old cell
         }
     }


    // Restore ListHandle state
    HSetState((Handle)gPeerListHandle, listState);

    // Redraw the list if requested or if content changed significantly
    // For simplicity, always invalidate if the count changed or forced.
    // A more complex check could compare old vs new content.
    if (forceRedraw || activePeerCount != currentListLength) {
        // Check if the port is valid before invalidating
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort); // Set port before invalidating
            InvalRect(&(**gPeerListHandle).rView); // Invalidate the list area for update event
            SetPort(oldPort); // Restore port
            log_message("Peer list updated. Active peers: %d. Invalidating list rect.", activePeerCount);
        } else {
            log_message("Peer list updated, but cannot invalidate rect (window port is NULL).");
        }
    } else {
         // log_message("Peer list checked, no changes needed."); // Can be verbose
    }
}