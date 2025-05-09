// FILE: ./classic_mac/dialog.c
//====================================

//====================================
// FILE: ./classic_mac/dialog.c
//====================================

#include "dialog.h"
#include "logging.h"
#include "network.h" // For gMyUsername (extern), gMyLocalIPStr
#include "peer.h"    // For gPeerManager
#include "./mactcp_messaging.h" // For MacTCP_SendMessageSync, streamBusyErr
#include "../shared/logging.h" // For is_debug_output_enabled, set_debug_output_enabled
#include "../shared/protocol.h" // For MSG_TEXT

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
#include <Errors.h> // For noErr

// Global variables related to the main dialog window and its components
DialogPtr gMainWindow = NULL;
Boolean gDialogTEInitialized = false;   // True if both Messages and Input TEs are set up
Boolean gDialogListInitialized = false; // True if Peer List is set up
// char gMyUsername[32];                // REMOVED - extern declaration comes from network.h
// Cell gLastSelectedCell = {0, -1};    // REMOVED - extern declaration comes from dialog_peerlist.h

Boolean InitDialog(void)
{
    Boolean messagesOk = false;
    Boolean inputOk = false;
    Boolean listOk = false;
    ControlHandle ctrlHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    GrafPtr oldPort;

    log_message("Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr) - 1L);
    if (gMainWindow == NULL) {
        log_message("Fatal: GetNewDialog failed (Error: %d). Check DLOG resource ID %d.", ResError(), kBaseResID);
        return false;
    }
    log_message("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);

    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow)); // Set current port to the dialog's window

    // Initialize components
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);

    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components (TEs, List) failed to initialize. Cleaning up.");
        if (listOk) CleanupPeerListControl();
        if (inputOk) CleanupInputTE();
        if (messagesOk) CleanupMessagesTEAndScrollbar();
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
        SetPort(oldPort);
        return false;
    }

    // Initialize Debug Checkbox state
    GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
            ctrlHandle = (ControlHandle)itemHandle;
            SetControlValue(ctrlHandle, is_debug_output_enabled() ? 1 : 0);
            log_message("Debug checkbox (Item %d) initialized to: %s", kDebugCheckbox, is_debug_output_enabled() ? "ON" : "OFF");
        } else {
            log_message("Warning: Item %d (kDebugCheckbox) is not a checkbox (Type: %d)! Cannot set initial debug state.", kDebugCheckbox, itemType);
        }
    } else {
         log_message("Warning: Item %d (kDebugCheckbox) handle is NULL! Cannot set initial state.", kDebugCheckbox);
    }

    // Initialize Broadcast Checkbox state (default to off)
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
             ctrlHandle = (ControlHandle)itemHandle;
             SetControlValue(ctrlHandle, 0); // Off by default
             log_message("Broadcast checkbox (Item %d) initialized to: OFF", kBroadcastCheckbox);
        } else {
            log_message("Warning: Item %d (kBroadcastCheckbox) is not a checkbox (Type: %d)! Cannot set initial state.", kBroadcastCheckbox, itemType);
        }
    } else {
        log_message("Warning: Item %d (kBroadcastCheckbox) handle is NULL! Cannot set initial state.", kBroadcastCheckbox);
    }


    UpdatePeerDisplayList(true); // Populate peer list
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);      // Activate input TE for typing

    UpdateDialogControls(); // Initial draw/update of all controls
    log_message("Initial UpdateDialogControls() called from InitDialog.");

    SetPort(oldPort); // Restore original port
    log_message("InitDialog finished successfully.");
    return true;
}

void CleanupDialog(void)
{
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

void HandleSendButtonClick(void)
{
    char inputCStr[256]; // Buffer for input text
    ControlHandle broadcastCheckboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100]; // For messages displayed in the TE
    OSErr sendErr = noErr;
    int i;

    if (!gDialogTEInitialized || gInputTE == NULL) {
        log_message("Error (HandleSendButtonClick): Input TE not initialized.");
        SysBeep(10);
        return;
    }

    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_message("Error: Could not get text from input field for sending.");
        SysBeep(10);
        ActivateInputTE(true); // Reactivate for user
        return;
    }

    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty. No action taken.");
        ActivateInputTE(true); // Reactivate for user
        return;
    }

    // Check broadcast checkbox state
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false; // Default to not broadcast
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        broadcastCheckboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(broadcastCheckboxHandle) == 1);
        log_to_file_only("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox or handle is NULL! Assuming not broadcast.", kBroadcastCheckbox);
    }

    if (isBroadcast) {
        int sent_count = 0;
        log_message("Attempting broadcast of: '%s'", inputCStr);
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");

        for (i = 0; i < MAX_PEERS; i++) {
            if (gPeerManager.peers[i].active) {
                sendErr = MacTCP_SendMessageSync(gPeerManager.peers[i].ip,
                                                 inputCStr,
                                                 MSG_TEXT,
                                                 gMyUsername,
                                                 gMyLocalIPStr,
                                                 YieldTimeToSystem);
                if (sendErr == noErr) {
                    sent_count++;
                } else {
                    log_message("Broadcast send to %s@%s failed: %d",
                                gPeerManager.peers[i].username, gPeerManager.peers[i].ip, sendErr);
                    // Optionally display error for this specific peer in messages TE
                }
            }
        }
        sprintf(displayMsg, "Broadcast sent to %d active peer(s).", sent_count);
        AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
        log_message("Broadcast of '%s' completed. Sent to %d peers.", inputCStr, sent_count);
        ClearInputText();
    } else { // Unicast send
        peer_t targetPeer;
        // Use the new function from dialog_peerlist.c
        if (DialogPeerList_GetSelectedPeer(&targetPeer)) {
            log_message("Attempting sync send to selected peer %s@%s: '%s'",
                        targetPeer.username, targetPeer.ip, inputCStr);

            sendErr = MacTCP_SendMessageSync(targetPeer.ip,
                                             inputCStr,
                                             MSG_TEXT,
                                             gMyUsername,
                                             gMyLocalIPStr,
                                             YieldTimeToSystem);
            if (sendErr == noErr) {
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
                log_message("Sync send completed successfully.");
                ClearInputText();
            } else {
                if (sendErr == streamBusyErr) { // MacTCP specific error
                     sprintf(displayMsg, "Could not send to %s: network busy. Try again.", targetPeer.username);
                } else {
                     sprintf(displayMsg, "Error sending to %s: %d", targetPeer.username, sendErr);
                }
                AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
                log_message("Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
            log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
            AppendToMessagesTE("Please select a peer to send to, or check Broadcast.\r");
            SysBeep(10);
        }
    }
    ActivateInputTE(true); // Reactivate input field
}

void ActivateDialogTE(Boolean activating)
{
    ActivateMessagesTEAndScrollbar(activating);
    ActivateInputTE(activating);
}

void UpdateDialogControls(void)
{
    GrafPtr oldPort;
    GrafPtr windowPort = GetWindowPort(gMainWindow);

    if (windowPort == NULL) {
        log_message("UpdateDialogControls Error: Window port is NULL for gMainWindow!");
        return;
    }

    GetPort(&oldPort);
    SetPort(windowPort);

    HandleMessagesTEUpdate(gMainWindow); // Update messages TE content
    HandleInputTEUpdate(gMainWindow);    // Update input TE content (e.g., draw frame)
    HandlePeerListUpdate(gMainWindow);   // Update peer list display

    // Any other controls that need explicit update can be added here.
    // For example, if scrollbar visibility/hilite depends on window activation,
    // it's often handled in activateEvt, but a general redraw might also touch it.

    SetPort(oldPort);
}