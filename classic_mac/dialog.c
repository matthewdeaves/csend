//====================================
// FILE: ./classic_mac/dialog.c
//====================================

#include "dialog.h"
#include "logging.h"
#include "network.h" // Includes YieldTimeToSystem, gMyUsername, gMyLocalIPStr
#include "protocol.h"
#include "peer_mac.h"
#include "common_defs.h"
#include "dialog_messages.h"
#include "dialog_input.h"
#include "dialog_peerlist.h"
#include "tcp.h" // Includes TCP_SendTextMessageSync, GetTCPListenerState
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <Errors.h> // For inProgress error code

DialogPtr gMainWindow = NULL;
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false;
// gMyUsername is extern from network.h
// gLastSelectedCell is extern from dialog_peerlist.h

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

    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);
    SetPort((GrafPtr)gMainWindow); // Set current port for drawing
    log_message("Window shown, selected, port set.");

    // Initialize dialog components
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);

    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components failed to initialize. Cleaning up.");
        CleanupDialog(); // Clean up partially initialized dialog
        return false;
    }

    UpdatePeerDisplayList(true); // Populate peer list initially

    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true); // Activate input TE

    log_message("InitDialog finished successfully.");
    return true;
}

void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

    // Cleanup components in reverse order of initialization
    CleanupPeerListControl();
    CleanupInputTE();
    CleanupMessagesTEAndScrollbar();

    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }

    gDialogTEInitialized = false;
    gDialogListInitialized = false;
    log_message("Dialog cleanup complete.");
}

// This function is called by DialogSelect for *any* click in the dialog,
// but we only care about specific items handled *after* DialogSelect finishes.
// Most item handling (buttons, checkboxes, TE activation, list selection)
// is done within the MainEventLoop's DialogSelect handling logic.
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent) {
    if (dialog != gMainWindow) return;
    // This log might be redundant if DialogSelect already logged the itemHit.
    log_to_file_only("HandleDialogClick called for item %d (Potentially redundant).", itemHit);
    // Add specific logic here ONLY if DialogSelect doesn't fully handle an item's click behavior.
}

// Handles the logic when the Send button is clicked.
void HandleSendButtonClick(void) {
    char inputCStr[256];
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100];
    OSErr sendErr;

    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_message("Error: Could not get text from input field.");
        SysBeep(10);
        return;
    }
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        return; // Don't send empty messages
    }

    // Get broadcast checkbox state
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_to_file_only("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false;
    }

    if (isBroadcast) {
        log_message("Broadcasting: '%s' (Broadcast send not implemented)", inputCStr);
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
        // Add actual broadcast logic here if needed (e.g., UDP broadcast)
    } else {
        peer_t targetPeer;
        if (GetSelectedPeerInfo(&targetPeer)) {
            log_message("Attempting sync send to selected peer %s@%s: '%s'",
                         targetPeer.username, targetPeer.ip, inputCStr);

            // Use the SYNC send function now
            sendErr = TCP_SendTextMessageSync(targetPeer.ip, inputCStr, YieldTimeToSystem); // <<< UPDATED CALL

            if (sendErr == noErr) {
                // Successfully SENT message
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_message("Sync send completed successfully.");
            }
            // NOTE: We removed the check for 'inProgress' here because the Sync function
            // either succeeds (noErr) or fails with a specific error. It doesn't return inProgress.
            else {
                // Error during send attempt
                log_message("Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
             log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
             SysBeep(10);
             return; // Don't clear input if no peer selected
        }
    }

    // Clear input field only if broadcast or send succeeded
    // (Don't clear if send failed, user might want to retry)
    if (isBroadcast || sendErr == noErr) {
        ClearInputText();
    }
    // Ensure input field keeps focus
    ActivateInputTE(true);
}


void ActivateDialogTE(Boolean activating) {
    // Activate/Deactivate both TE fields
    ActivateMessagesTEAndScrollbar(activating); // Handles message TE and its scrollbar
    ActivateInputTE(activating);                // Handles input TE
}

void UpdateDialogControls(void) {
    GrafPtr oldPort;
    GrafPtr windowPort = GetWindowPort(gMainWindow);

    if (windowPort == NULL) {
        log_message("UpdateDialogControls Error: Window port is NULL!");
        return;
    }

    GetPort(&oldPort);
    SetPort(windowPort); // Set port to dialog's window

    // Update custom items that need redrawing
    HandleMessagesTEUpdate(gMainWindow); // Update Messages TE content
    HandleInputTEUpdate(gMainWindow);    // Update Input TE content
    HandlePeerListUpdate(gMainWindow);   // Update Peer List content

    // Standard controls are usually redrawn by DrawDialog, but ensure scrollbar is correct
    if (gMessagesScrollBar != NULL) {
        Draw1Control(gMessagesScrollBar); // Redraw scrollbar explicitly if needed
    }

    SetPort(oldPort); // Restore original port
}