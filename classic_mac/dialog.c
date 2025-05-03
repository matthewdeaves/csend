#include "dialog.h" // Includes component headers automatically
#include "logging.h"
#include "network.h" // For gMyLocalIPStr
#include "protocol.h"
#include "peer_mac.h" // For peer_t, MAX_PEERS
#include "common_defs.h" // For BUFFER_SIZE etc.

#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h>
#include <Events.h>
#include <Windows.h>
#include <Memory.h>
#include <Sound.h>
#include <Resources.h>
#include <Fonts.h>
#include <stdio.h>   // For sprintf
#include <string.h>  // For strcmp, strlen
#include <stdlib.h>

/*----------------------------------------------------------*/
/* Global Variable Definitions                              */
/*----------------------------------------------------------*/

DialogPtr gMainWindow = NULL;
TEHandle gMessagesTE = NULL;
TEHandle gInputTE = NULL;
ListHandle gPeerListHandle = NULL;
ControlHandle gMessagesScrollBar = NULL;
Boolean gDialogTEInitialized = false;   // Tracks success of TE components
Boolean gDialogListInitialized = false; // Tracks success of List component
char gMyUsername[32] = "MacUser";       // Default username
Cell gLastSelectedCell = {0, 0};        // Tracks list selection {v, h}

/*----------------------------------------------------------*/
/* Main Dialog Functions Implementation                     */
/*----------------------------------------------------------*/

Boolean InitDialog(void) {
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

    // Prepare the window
    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);
    SetPort((GrafPtr)gMainWindow); // Set port for component initializations
    log_message("Window shown, selected, port set.");

    // Initialize components by calling their init functions
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);

    // Determine overall initialization success
    // Note: Scrollbar init is now part of messagesOk
    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components failed to initialize. Cleaning up.");
        CleanupDialog(); // This will call component cleanup functions
        return false;
    }

    // Perform initial population/update of controls
    UpdatePeerDisplayList(true); // Populate the peer list initially

    // Set initial focus to the input field
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true); // Activate the input component

    log_message("InitDialog finished successfully.");
    return true;
}

void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

    // Call component cleanup functions first
    CleanupMessagesTEAndScrollbar();
    CleanupInputTE();
    CleanupPeerListControl();

    // Now dispose the main dialog window itself
    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }

    // Reset flags
    gDialogTEInitialized = false;
    gDialogListInitialized = false;

    log_message("Dialog cleanup complete.");
}

void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent) {
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    short currentValue;
    Boolean clickHandled = false; // Flag to see if a component handled the click

    if (dialog != gMainWindow) return; // Should not happen if called via DialogSelect

    // 1. Check if the click was in the Peer List user item boundary first
    // GetDialogItem is relatively cheap, check this before PtInRect checks
    GetDialogItem(dialog, kPeerListUserItem, &itemType, &itemHandle, &itemRect);
    if (PtInRect(theEvent->where, &itemRect)) { // Check against screen coords first
        // Convert to local for LClick/component handler
        Point localClick = theEvent->where;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GlobalToLocal(&localClick);
        SetPort(oldPort);

        // Check specifically if it's within the list's view rect (safer)
        SignedByte listState = HGetState((Handle)gPeerListHandle);
        HLock((Handle)gPeerListHandle);
        Boolean inView = false;
        if (*gPeerListHandle != NULL) {
            inView = PtInRect(localClick, &(**gPeerListHandle).rView);
        }
        HSetState((Handle)gPeerListHandle, listState);

        if (inView) {
            clickHandled = HandlePeerListClick(dialog, theEvent);
        } else {
            log_to_file_only("Click in Peer List item rect, but outside LClick view rect.");
        }
    }

    // 2. If not handled by the list, check standard dialog items
    if (!clickHandled) {
        switch (itemHit) {
            case kSendButton:
                log_message("Send button clicked.");
                DoSendAction(dialog);
                clickHandled = true; // Assume button click is handled
                break;

            case kBroadcastCheckbox:
                GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                if (itemType == (ctrlItem + chkCtrl)) { // Verify it's a checkbox
                    checkboxHandle = (ControlHandle)itemHandle;
                    currentValue = GetControlValue(checkboxHandle);
                    SetControlValue(checkboxHandle, !currentValue); // Toggle value
                    log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
                } else {
                    log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
                }
                clickHandled = true; // Assume checkbox click is handled
                break;

            // Explicitly ignore clicks on the TE user items here,
            // they are handled by the PtInRect check below if needed.
            case kMessagesTextEdit:
            case kInputTextEdit:
            case kPeerListUserItem: // Already checked above more carefully
            case kMessagesScrollbar: // Scrollbar clicks handled by FindControl/TrackControl in main loop
                log_to_file_only("Ignoring click on userItem/scrollbar item %d in HandleDialogClick.", itemHit);
                break;

            default:
                // If DialogSelect didn't identify a specific known item,
                // or it was an item we ignore here, check if it was in the Input TE.
                // We don't need to handle clicks in the Messages TE directly.
                 {
                     Point localPt = theEvent->where;
                     GrafPtr oldPort;
                     GetPort(&oldPort);
                     SetPort(GetWindowPort(dialog));
                     GlobalToLocal(&localPt);
                     SetPort(oldPort);

                     // Check Input TE view rect
                     SignedByte teState = HGetState((Handle)gInputTE);
                     HLock((Handle)gInputTE);
                     Boolean inInputView = false;
                     if (*gInputTE != NULL) {
                         inInputView = PtInRect(localPt, &(**gInputTE).viewRect);
                     }
                     HSetState((Handle)gInputTE, teState);

                     if (inInputView) {
                         HandleInputTEClick(dialog, theEvent);
                         clickHandled = true;
                     } else {
                         // Click was not in list, known controls, or input TE.
                         log_to_file_only("HandleDialogClick: Click in content not handled (itemHit %d).", itemHit);
                     }
                 }
                break;
        }
    }
}

void DoSendAction(DialogPtr dialog) {
    char inputCStr[256]; // Buffer for input text
    char formattedMsg[BUFFER_SIZE]; // Buffer for network message
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100]; // Buffer for display in messages TE

    // 1. Get text from input field
    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_message("Error: Could not get text from input field.");
        SysBeep(10);
        return;
    }
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        // Optionally beep or do nothing
        return;
    }

    // 2. Check broadcast checkbox state
    GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false;
    }

    // 3. Format the message for network/display
    int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                      gMyUsername, gMyLocalIPStr, inputCStr);

    if (formatResult <= 0) {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
        return;
    }

    // 4. Determine recipient and send (or simulate sending)
    if (isBroadcast) {
        log_message("Broadcasting: %s (Network send not implemented yet)", inputCStr);
        // TODO: Implement actual broadcast network send using formattedMsg
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r"); // Add newline for display
    } else {
        peer_t targetPeer;
        if (GetSelectedPeerInfo(&targetPeer)) {
            log_message("Sending to selected peer %s@%s: %s (Network send not implemented yet)",
                         targetPeer.username, targetPeer.ip, inputCStr);
            // TODO: Implement actual unicast network send using formattedMsg to targetPeer.ip
            sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r"); // Add newline for display
        } else {
             log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
             SysBeep(10);
             return; // Don't clear input if send failed due to no selection
        }
    }

    // 5. Clear input field and return focus
    ClearInputText();
    ActivateInputTE(true); // Set focus back to input field
}

void ActivateDialogTE(Boolean activating) {
    // Call component activation functions
    // Note: TEActivate/TEDeactivate is handled within these component functions now
    ActivateMessagesTEAndScrollbar(activating);
    ActivateInputTE(activating);
}

void UpdateDialogControls(void) {
    GrafPtr oldPort;
    GrafPtr windowPort = GetWindowPort(gMainWindow);

    if (windowPort == NULL) {
        log_message("UpdateDialogControls Error: Window port is NULL!");
        return;
    }

    GetPort(&oldPort);
    SetPort(windowPort);

    // Call component update functions
    HandleMessagesTEUpdate(gMainWindow);
    HandleInputTEUpdate(gMainWindow);
    HandlePeerListUpdate(gMainWindow);

    // Redraw standard controls (buttons, checkboxes)
    DrawControls(gMainWindow);

    SetPort(oldPort);
}


/*----------------------------------------------------------*/
/* Pascal Callback for Scrollbar                            */
/*----------------------------------------------------------*/

pascal void MyScrollAction(ControlHandle theControl, short partCode) {
    // This function MUST remain pascal calling convention.
    // It acts as a simple wrapper to call the C handler in the component.
    if (theControl == gMessagesScrollBar) {
        // Call the C function that handles the logic
        HandleMessagesScrollClick(theControl, partCode);
    } else {
         // Log if called for an unexpected control
         log_to_file_only("MyScrollAction: Ignoring unknown control 0x%lX", (unsigned long)theControl);
    }
}