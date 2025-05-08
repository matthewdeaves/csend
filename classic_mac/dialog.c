#include "dialog.h"
#include "logging.h"
#include "network.h"
#include "peer.h"
#include "./mactcp_messaging.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
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
#include <Errors.h>
DialogPtr gMainWindow = NULL;
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false;
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
    SetPort(GetWindowPort(gMainWindow));
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
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
             ctrlHandle = (ControlHandle)itemHandle;
             SetControlValue(ctrlHandle, 0);
             log_message("Broadcast checkbox (Item %d) initialized to: OFF", kBroadcastCheckbox);
        } else {
            log_message("Warning: Item %d (kBroadcastCheckbox) is not a checkbox (Type: %d)! Cannot set initial state.", kBroadcastCheckbox, itemType);
        }
    } else {
        log_message("Warning: Item %d (kBroadcastCheckbox) handle is NULL! Cannot set initial state.", kBroadcastCheckbox);
    }
    UpdatePeerDisplayList(true);
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);
    UpdateDialogControls();
    log_message("Initial UpdateDialogControls() called from InitDialog.");
    SetPort(oldPort);
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
    char inputCStr[256];
    ControlHandle broadcastCheckboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100];
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
        ActivateInputTE(true);
        return;
    }
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty. No action taken.");
        ActivateInputTE(true);
        return;
    }
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false;
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
                sendErr = TCP_SendTextMessageSync(gPeerManager.peers[i].ip, inputCStr, YieldTimeToSystem);
                if (sendErr == noErr) {
                    sent_count++;
                } else {
                    log_message("Broadcast send to %s@%s failed: %d",
                                gPeerManager.peers[i].username, gPeerManager.peers[i].ip, sendErr);
                }
            }
        }
        sprintf(displayMsg, "Broadcast sent to %d active peer(s).", sent_count);
        AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
        log_message("Broadcast of '%s' completed. Sent to %d peers.", inputCStr, sent_count);
        ClearInputText();
    } else {
        peer_t targetPeer;
        if (GetSelectedPeerInfo(&targetPeer)) {
            log_message("Attempting sync send to selected peer %s@%s: '%s'",
                        targetPeer.username, targetPeer.ip, inputCStr);
            sendErr = TCP_SendTextMessageSync(targetPeer.ip, inputCStr, YieldTimeToSystem);
            if (sendErr == noErr) {
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
                log_message("Sync send completed successfully.");
                ClearInputText();
            } else {
                if (sendErr == streamBusyErr) {
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
    ActivateInputTE(true);
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
    HandleMessagesTEUpdate(gMainWindow);
    HandleInputTEUpdate(gMainWindow);
    HandlePeerListUpdate(gMainWindow);
    SetPort(oldPort);
}
