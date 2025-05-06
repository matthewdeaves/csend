#include "dialog.h"
#include "logging.h"
#include "network.h" // Includes YieldTimeToSystem, gMyUsername, gMyLocalIPStr
#include "protocol.h"
#include "peer_mac.h"
#include "common_defs.h"
#include "dialog_messages.h"
#include "dialog_input.h"
#include "dialog_peerlist.h"
#include "tcp.h"       // Includes TCP_SendTextMessageSync, GetTCPState, streamBusyErr
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
#include <Errors.h> // For standard error codes

DialogPtr gMainWindow = NULL;
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false;
// gMyUsername is extern from network.h
// gLastSelectedCell is extern from dialog_peerlist.h

// InitDialog remains unchanged
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
    SetPort((GrafPtr)gMainWindow);
    log_message("Window shown, selected, port set.");

    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);

    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components failed to initialize. Cleaning up.");
        CleanupDialog();
        return false;
    }

    UpdatePeerDisplayList(true);

    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);

    log_message("InitDialog finished successfully.");
    return true;
}

// CleanupDialog remains unchanged
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

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

// HandleDialogClick remains unchanged
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent) {
    if (dialog != gMainWindow) return;
    log_to_file_only("HandleDialogClick called for item %d (Potentially redundant).", itemHit);
}

// HandleSendButtonClick - UPDATED error handling
void HandleSendButtonClick(void) {
    char inputCStr[256];
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100];
    OSErr sendErr = noErr; // Initialize error status

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

            // Use the SYNC send function that now uses the single stream
            sendErr = TCP_SendTextMessageSync(targetPeer.ip, inputCStr, YieldTimeToSystem);

            if (sendErr == noErr) {
                // Successfully SENT message
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_message("Sync send completed successfully.");
            }
            // *** ADDED: Handle stream busy error specifically ***
            else if (sendErr == streamBusyErr) {
                log_message("Send failed: Stream is busy (receiving or sending). Please try again later.");
                SysBeep(5); // Different beep? Or just log?
            }
            // Handle other errors
            else {
                log_message("Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
             log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
             SysBeep(10);
             return; // Don't clear input if no peer selected
        }
    }

    // Clear input field only if broadcast or send succeeded (not if busy or error)
    if (isBroadcast || sendErr == noErr) {
        ClearInputText();
    }
    // Ensure input field keeps focus
    ActivateInputTE(true);
}

// ActivateDialogTE remains unchanged
void ActivateDialogTE(Boolean activating) {
    ActivateMessagesTEAndScrollbar(activating);
    ActivateInputTE(activating);
}

// UpdateDialogControls remains unchanged
void UpdateDialogControls(void) {
    GrafPtr oldPort;
    GrafPtr windowPort = GetWindowPort(gMainWindow);

    if (windowPort == NULL) {
        log_message("UpdateDialogControls Error: Window port is NULL!");
        return;
    }

    GetPort(&oldPort);
    SetPort(windowPort);

    HandleMessagesTEUpdate(gMainWindow);
    HandleInputTEUpdate(gMainWindow);
    HandlePeerListUpdate(gMainWindow);

    if (gMessagesScrollBar != NULL) {
        Draw1Control(gMessagesScrollBar);
    }

    SetPort(oldPort);
}
