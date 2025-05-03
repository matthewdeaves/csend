//====================================
// FILE: ./classic_mac/dialog.c
//====================================

#include "dialog.h"
#include "logging.h" // Includes log_message AND log_to_file_only
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
ControlHandle gMessagesScrollBar = NULL; // Define global
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false; // Initialize List state
char gMyUsername[32] = "MacUser";
Cell gLastSelectedCell = {0, 0}; // Initialize selected cell

// Forward declaration if needed, or move UpdatePeerDisplayList above InitDialog
void UpdatePeerDisplayList(Boolean forceRedraw);

// Action procedure for the scroll bar
pascal void MyScrollAction(ControlHandle theControl, short partCode) {
    GrafPtr oldPort; // To save/restore port
    // *** Use file-only logging within the action procedure ***
    log_to_file_only("MyScrollAction called: Control=0x%lX, PartCode=%d", (unsigned long)theControl, partCode);

    if (theControl == gMessagesScrollBar && gMessagesTE != NULL && partCode != 0) {
        short linesToScroll = 0;
        short currentScroll, maxScroll;
        short lineHeight = 0; // Initialize
        short pageScroll = 1; // Default page scroll
        short scrollDeltaPixels = 0; // Pixel delta for TEScroll
        Rect viewRectToInvalidate; // To store viewRect for InvalRect

        // Ensure correct port is set
        GetPort(&oldPort);
        SetPort(GetWindowPort(gMainWindow)); // Use GetWindowPort for safety

        // Get TE info safely
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            lineHeight = (**gMessagesTE).lineHeight;
            viewRectToInvalidate = (**gMessagesTE).viewRect; // Copy viewRect while locked
            if (lineHeight <= 0) {
                log_to_file_only("MyScrollAction Warning: lineHeight is %d!", lineHeight);
                HSetState((Handle)gMessagesTE, teState);
                SetPort(oldPort); // Restore port before returning
                return;
            }
             pageScroll = (viewRectToInvalidate.bottom - viewRectToInvalidate.top) / lineHeight - 1;
             if (pageScroll < 1) pageScroll = 1;
        } else {
             log_to_file_only("MyScrollAction Error: gMessagesTE dereference failed!");
             HSetState((Handle)gMessagesTE, teState);
             SetPort(oldPort); // Restore port before returning
             return;
        }
        HSetState((Handle)gMessagesTE, teState); // Unlock handle after getting info

        currentScroll = GetControlValue(theControl);
        maxScroll = GetControlMaximum(theControl);

        log_to_file_only("MyScrollAction: currentScroll=%d, maxScroll=%d, lineHeight=%d, pageScroll=%d",
                    currentScroll, maxScroll, lineHeight, pageScroll);

        // Use literal part codes from Inside Macintosh: Volume IV, page 6-41
        switch (partCode) {
            case 20:    linesToScroll = -1; break; // inUpButton
            case 21:    linesToScroll = 1; break;  // inDownButton
            case 22:    linesToScroll = -pageScroll; break; // inPageUp
            case 23:    linesToScroll = pageScroll; break;  // inPageDown
            default:
                log_to_file_only("MyScrollAction: Ignoring partCode %d", partCode);
                SetPort(oldPort); // Restore port before returning
                return; // Ignore clicks in indicator/inactive parts
        }

        short newScroll = currentScroll + linesToScroll;

        // Clamp newScroll value
        if (newScroll < 0) newScroll = 0;
        if (newScroll > maxScroll) newScroll = maxScroll;

        log_to_file_only("MyScrollAction: linesToScroll=%d, newScroll=%d (clamped)", linesToScroll, newScroll);

        if (newScroll != currentScroll) {
            SetControlValue(theControl, newScroll);

            // TEScroll scrolls by pixel difference. Positive scrolls DOWN, negative UP.
            scrollDeltaPixels = (currentScroll - newScroll) * lineHeight;
            log_to_file_only("MyScrollAction: Scrolling TE by %d pixels.", scrollDeltaPixels);

            // Lock handle again for TEScroll
            teState = HGetState((Handle)gMessagesTE);
            HLock((Handle)gMessagesTE);
            if (*gMessagesTE != NULL) {
                TEScroll(0, scrollDeltaPixels, gMessagesTE);
                // *** ADD INVALIDATION AFTER SCROLL ***
                InvalRect(&viewRectToInvalidate); // Use the copied rect
                log_to_file_only("MyScrollAction: Invalidated TE viewRect.");
            } else {
                 log_to_file_only("MyScrollAction Error: gMessagesTE dereference failed before TEScroll!");
            }
            HSetState((Handle)gMessagesTE, teState);

        } else {
             log_to_file_only("MyScrollAction: newScroll == currentScroll, no action needed.");
        }
        SetPort(oldPort); // Restore original port
    } else {
         log_to_file_only("MyScrollAction: Control mismatch (0x%lX vs 0x%lX) or TE NULL (0x%lX) or partCode 0.",
                     (unsigned long)theControl, (unsigned long)gMessagesScrollBar, (unsigned long)gMessagesTE);
    }
}


Boolean InitDialog(void) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect destRectInput, viewRectInput;
    Rect destRectList, dataBounds; // Rects for List Manager
    Rect scrollBarRect; // For getting scroll bar handle
    Point cellSize; // Cell size for List Manager
    FontInfo fontInfo; // For calculating list cell height
    Boolean messagesOk = false;
    Boolean inputOk = false;
    Boolean listOk = false;
    Boolean scrollBarOk = false; // Flag for scroll bar init

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
        // Shrink viewRect width to make space for scroll bar (standard width 16)
        viewRectMessages.right -= 16;
        // Inset view rect slightly for border (optional, applied to adjusted rect)
        // InsetRect(&viewRectMessages, 1, 1);

        log_message("Calling TENew for Messages TE (Adjusted Rect)...");
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(true, gMessagesTE); // Keep TEAutoView enabled
            // Make messages TE read-only (optional, but typical)
            // TESetSelect(0, 0, gMessagesTE); // Ensure no selection initially
            // (**gMessagesTE).txFlags |= teFReadOnly; // Example if needed
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        messagesOk = false;
    }
    // --- Initialize Scroll Bar ---
    if (messagesOk) { // Only init scrollbar if TE field is okay
        log_message("Getting item %d info (Messages Scrollbar)...", kMessagesScrollbar);
        GetDialogItem(gMainWindow, kMessagesScrollbar, &itemType, &itemHandle, &scrollBarRect);
        log_message("DEBUG: GetDialogItem for item %d returned type %d, handle 0x%lX.", kMessagesScrollbar, itemType, (unsigned long)itemHandle); // Log type AND handle

        // *** MODIFIED CHECK ***
        // Trust the handle if it's non-NULL, regardless of the unexpected itemType.
        // We know item 6 *should* be the scrollbar control from the DITL.
        if (itemHandle != NULL) {
             gMessagesScrollBar = (ControlHandle)itemHandle;
             log_message("Scrollbar handle obtained: 0x%lX (Ignoring unexpected itemType %d).", (unsigned long)gMessagesScrollBar, itemType);
             AdjustMessagesScrollbar(); // Set initial state
             scrollBarOk = true; // Set to true since we got a handle
        } else {
             log_message("ERROR: Item %d (Scrollbar) handle is NULL! Type was %d.", kMessagesScrollbar, itemType);
             gMessagesScrollBar = NULL;
             scrollBarOk = false;
        }
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

    // *** CRITICAL: Update gDialogTEInitialized based on ALL necessary components ***
    gDialogTEInitialized = (messagesOk && inputOk && scrollBarOk); // Ensure scrollBarOk is included
    log_message("Dialog TE fields & Scrollbar initialization complete (Success: %s).", gDialogTEInitialized ? "YES" : "NO");

    // --- Initialize List Manager Field ---
    log_message("Getting item %d info (Peer List UserItem)...", kPeerListUserItem);
    GetDialogItem(gMainWindow, kPeerListUserItem, &itemType, &itemHandle, &destRectList);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kPeerListUserItem,
                    destRectList.top, destRectList.left, destRectList.bottom, destRectList.right);

        // Calculate cell size
        GetFontInfo(&fontInfo); // Get info for current port's font
        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading; // Height
        cellSize.h = destRectList.right - destRectList.left; // Width

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

    // *** FINAL CHECK before returning true ***
    if (!gDialogTEInitialized || !gDialogListInitialized) { // Check combined status
        log_message("Error: One or more dialog components failed to initialize. Cleaning up.");
        // Cleanup what might have been allocated before returning false
        CleanupDialog();
        return false;
    }

    // Initial population of the list
    UpdatePeerDisplayList(true); // Force initial population and redraw if needed

    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    if (gInputTE) TEActivate(gInputTE);
    log_message("Input TE activated.");

    log_message("InitDialog finished successfully."); // Add success message
    return true;
}

void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");
    // Scroll bar is owned by the dialog, no need to dispose explicitly
    // unless it was created manually (which it isn't here).
    gMessagesScrollBar = NULL;

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
                 SetPt(&gLastSelectedCell, 0, 0); // Reset selection
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
                    // AppendToMessagesTE(""); // Append empty string to trigger redraw logic
                } else {
                    log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
                }
                break;
            // Add cases for kMessagesTextEdit and kInputTextEdit if needed,
            // although TEActivate/TEDeactivate usually handle focus clicks.
            // LClick handles clicks within the list itself.
            // Scrollbar clicks are handled in main event loop via FindControl/TrackControl
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
                         // If TE field should be activatable on click:
                         // TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gMessagesTE);
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
                             selectedRow, targetPeer.username, targetPeer.ip, inputCStr); // Log 0-based index
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

// Simplified version - relying on TEAutoView
void AppendToMessagesTE(const char *text) {
    GrafPtr oldPort; // To restore port

    if (gMessagesTE == NULL || !gDialogTEInitialized) {
        if (gLogFile != NULL) {
            // Use fprintf for logging to avoid recursion if AppendToMessagesTE is called from log_message
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

        // Check if adding text exceeds TE limit (32K) - simple check
        if (currentLength + strlen(text) < 32000) {
            TESetSelect(currentLength, currentLength, gMessagesTE); // Select end
            TEInsert((Ptr)text, strlen(text), gMessagesTE);

            // *** REMOVED InvalRect(&(**gMessagesTE).viewRect); ***
            // Let TEAutoView handle scrolling the view when inserting at the end.

            // Adjust scrollbar after text changes
            AdjustMessagesScrollbar(); // <-- Adjust scrollbar state

        } else {
            // Use fprintf for logging to avoid recursion
             if (gLogFile != NULL) {
                 fprintf(gLogFile, "Warning: Messages TE field is full. Cannot append.\n");
                 fflush(gLogFile);
             }
            // Optional: Implement text clearing logic here
        }
    } else {
        // Use fprintf for logging to avoid recursion
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
    // Activate/Deactivate scroll bar along with the window/TE field
    if (gMessagesScrollBar != NULL) {
        // According to IM:IV Controls chapter, HiliteControl with 0 makes it active, 255 inactive.
        HiliteControl(gMessagesScrollBar, activating ? 0 : 255);
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
    if (windowPort == NULL) { // Safety check
        log_message("UpdateDialogControls Error: Window port is NULL!");
        SetPort(oldPort);
        return;
    }
    SetPort(windowPort); // Ensure port is correct

    // Update TextEdit fields
    if (gMessagesTE != NULL) {
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // TEUpdate is still needed here for general window updates (expose, etc.)
        TEUpdate(&itemRect, gMessagesTE);
        // *** REMOVED AdjustMessagesScrollbar(); from here ***
    }
    if (gInputTE != NULL) {
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gInputTE);
    }

    // Update List Manager field
    if (gPeerListHandle != NULL) {
        // Use LUpdate with the window's visRgn.
        LUpdate(GetWindowPort(gMainWindow)->visRgn, gPeerListHandle); // Use GetWindowPort again for safety
    }

    // Draw all controls (including scroll bar)
    DrawControls(gMainWindow); // <-- Draw controls

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
            SetPt(&theCell, 0, activePeerCount); // Column 0, Row activePeerCount
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
         SetPt(&gLastSelectedCell, 0, 0); // Reset global selection state
         // Ensure nothing is selected in the list itself
         if (currentListLength > 0 && oldSelection.v < currentListLength) { // Only deselect if old selection was valid
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

// New function to adjust the scroll bar based on TE state
void AdjustMessagesScrollbar(void) {
    // *** REMOVED static Boolean adjustingScrollBar guard ***
    // The guard is now in log_message

    if (gMessagesTE == NULL || gMessagesScrollBar == NULL) {
        return;
    }

    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);

    if (*gMessagesTE != NULL) {
        short lineHeight = (**gMessagesTE).lineHeight;
        short linesInView = 0;
        short firstVisibleLine = 0;
        short totalLines = (**gMessagesTE).nLines;
        short maxScroll = 0;
        short currentVal = GetControlValue(gMessagesScrollBar); // Get current value before changing max

        if (lineHeight > 0) {
            linesInView = ((**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top) / lineHeight;
            // Calculate the first visible line based on destRect's top edge relative to viewRect's top edge
            // destRect.top is negative when scrolled down.
            firstVisibleLine = -(**gMessagesTE).destRect.top / lineHeight;
        } else {
            // Handle case where lineHeight might be 0 (e.g., TE handle invalid)
            linesInView = 0;
            firstVisibleLine = 0;
            totalLines = 0; // Cannot determine lines if height is 0
            log_message("AdjustMessagesScrollbar Warning: lineHeight is %d!", lineHeight);
        }

        if (linesInView < 1) linesInView = 1; // Avoid division by zero or negative

        maxScroll = totalLines - linesInView;
        if (maxScroll < 0) maxScroll = 0;

        // Use file-only logging inside AdjustMessagesScrollbar to prevent recursion via log_message
        log_to_file_only("AdjustMessagesScrollbar: totalLines=%d, linesInView=%d, maxScroll=%d, firstVisibleLine=%d",
                    totalLines, linesInView, maxScroll, firstVisibleLine);

        // Check if scrollbar state needs changing
        // IMPORTANT: Set max *before* setting value, especially if max is decreasing
        if (GetControlMaximum(gMessagesScrollBar) != maxScroll) {
             log_to_file_only("AdjustMessagesScrollbar: Setting Max to %d (was %d)", maxScroll, GetControlMaximum(gMessagesScrollBar));
             SetControlMaximum(gMessagesScrollBar, maxScroll);
             // Re-clamp current value if max changed
             if (currentVal > maxScroll) {
                 currentVal = maxScroll;
                 // If the value had to be clamped, we might need to scroll TE to match
                 // This situation happens if text is deleted, reducing totalLines
                 if (firstVisibleLine > maxScroll) {
                     short scrollDeltaPixels = (firstVisibleLine - maxScroll) * lineHeight;
                     log_to_file_only("AdjustScrollbar: Clamping scroll value, scrolling TE by %d pixels.", scrollDeltaPixels);
                     // *** Log before TEScroll ***
                     log_to_file_only("AdjustScrollbar: Calling TEScroll(0, %d, ...)", scrollDeltaPixels);
                     TEScroll(0, scrollDeltaPixels, gMessagesTE); // Scroll content down
                     log_to_file_only("AdjustScrollbar: TEScroll finished.");
                     // *** Log after TEScroll ***
                     firstVisibleLine = maxScroll; // Update our idea of the visible line
                 }
             }
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (after max change)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine); // Set value *after* max
        } else if (GetControlValue(gMessagesScrollBar) != firstVisibleLine) {
            // Max is the same, just update the value if needed
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (max unchanged)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        }


        // Show/Hide scrollbar based on whether scrolling is possible
        Boolean shouldBeVisible = (maxScroll > 0);
        Boolean isVisible = (**gMessagesScrollBar).contrlVis; // Check visibility flag

        if (shouldBeVisible && !isVisible) {
            log_to_file_only("AdjustMessagesScrollbar: Showing scrollbar.");
            ShowControl(gMessagesScrollBar);
        } else if (!shouldBeVisible && isVisible) {
            log_to_file_only("AdjustMessagesScrollbar: Hiding scrollbar.");
            HideControl(gMessagesScrollBar);
        }
    } else {
        log_message("AdjustMessagesScrollbar Error: gMessagesTE deref failed!"); // Use normal log here, unlikely to recurse
    }

    HSetState((Handle)gMessagesTE, teState);
}