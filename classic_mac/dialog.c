#include "dialog.h"
#include "logging.h"
#include "mactcp_network.h"
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
    log_debug("Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr) - 1L);
    if (gMainWindow == NULL) {
        log_debug("Fatal: GetNewDialog failed (Error: %d). Check DLOG resource ID %d.", ResError(), kBaseResID);
        return false;
    }
    log_debug("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);
    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow));
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);
    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;
    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_debug("Error: One or more dialog components (TEs, List) failed to initialize. Cleaning up.");
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
            log_debug("Debug checkbox (Item %d) initialized to: %s", kDebugCheckbox, is_debug_output_enabled() ? "ON" : "OFF");
        } else {
            log_debug("Warning: Item %d (kDebugCheckbox) is not a checkbox (Type: %d)! Cannot set initial debug state.", kDebugCheckbox, itemType);
        }
    } else {
        log_debug("Warning: Item %d (kDebugCheckbox) handle is NULL! Cannot set initial state.", kDebugCheckbox);
    }
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
            ctrlHandle = (ControlHandle)itemHandle;
            SetControlValue(ctrlHandle, 0);
            log_debug("Broadcast checkbox (Item %d) initialized to: OFF", kBroadcastCheckbox);
        } else {
            log_debug("Warning: Item %d (kBroadcastCheckbox) is not a checkbox (Type: %d)! Cannot set initial state.", kBroadcastCheckbox, itemType);
        }
    } else {
        log_debug("Warning: Item %d (kBroadcastCheckbox) handle is NULL! Cannot set initial state.", kBroadcastCheckbox);
    }
    UpdatePeerDisplayList(true);
    log_debug("Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);
    UpdateDialogControls();
    log_debug("Initial UpdateDialogControls() called from InitDialog.");
    SetPort(oldPort);
    log_debug("InitDialog finished successfully.");
    return true;
}
void CleanupDialog(void)
{
    log_debug("Cleaning up Dialog...");
    CleanupPeerListControl();
    CleanupInputTE();
    CleanupMessagesTEAndScrollbar();
    if (gMainWindow != NULL) {
        log_debug("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
    gDialogTEInitialized = false;
    gDialogListInitialized = false;
    log_debug("Dialog cleanup complete.");
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
        log_debug("Error (HandleSendButtonClick): Input TE not initialized.");
        SysBeep(10);
        return;
    }
    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_debug("Error: Could not get text from input field for sending.");
        SysBeep(10);
        ActivateInputTE(true);
        return;
    }
    if (strlen(inputCStr) == 0) {
        log_debug("Send Action: Input field is empty. No action taken.");
        ActivateInputTE(true);
        return;
    }
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false;
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        broadcastCheckboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(broadcastCheckboxHandle) == 1);
        log_debug("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_debug("Warning: Broadcast item %d is not a checkbox or handle is NULL! Assuming not broadcast.", kBroadcastCheckbox);
    }
    if (isBroadcast) {
        int sent_count = 0;
        log_debug("Attempting broadcast of: '%s'", inputCStr);
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
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
                    log_debug("Broadcast send to %s@%s failed: %d",
                              gPeerManager.peers[i].username, gPeerManager.peers[i].ip, sendErr);
                }
            }
        }
        sprintf(displayMsg, "Broadcast sent to %d active peer(s).", sent_count);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
        log_debug("Broadcast of '%s' completed. Sent to %d peers.", inputCStr, sent_count);
        ClearInputText();
    } else {
        peer_t targetPeer;
        if (DialogPeerList_GetSelectedPeer(&targetPeer)) {
            log_debug("Attempting sync send to selected peer %s@%s: '%s'",
                      targetPeer.username, targetPeer.ip, inputCStr);
            sendErr = MacTCP_SendMessageSync(targetPeer.ip,
                                             inputCStr,
                                             MSG_TEXT,
                                             gMyUsername,
                                             gMyLocalIPStr,
                                             YieldTimeToSystem);
            if (sendErr == noErr) {
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_debug("Sync send completed successfully.");
                ClearInputText();
            } else {
                if (sendErr == streamBusyErr) {
                    sprintf(displayMsg, "Could not send to %s: network busy. Try again.", targetPeer.username);
                } else {
                    sprintf(displayMsg, "Error sending to %s: %d", targetPeer.username, sendErr);
                }
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_debug("Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
            log_debug("Error: Cannot send, no peer selected in the list or selection invalid.");
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
        log_debug("UpdateDialogControls Error: Window port is NULL for gMainWindow!");
        return;
    }
    GetPort(&oldPort);
    SetPort(windowPort);
    HandleMessagesTEUpdate(gMainWindow);
    HandleInputTEUpdate(gMainWindow);
    HandlePeerListUpdate(gMainWindow);
    SetPort(oldPort);
}
