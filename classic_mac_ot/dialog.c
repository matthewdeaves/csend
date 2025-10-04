#include "dialog.h"
#include "../shared/logging.h"
#include "opentransport_impl.h"
#include "peer.h"
#include "messaging.h"
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

/* Update tracking to reduce excessive redraws */
Boolean gInputTENeedsUpdate = false;
Boolean gMessagesTENeedsUpdate = false;
Boolean gPeerListNeedsUpdate = false;
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
    log_debug_cat(LOG_CAT_UI, "Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr) - 1L);
    if (gMainWindow == NULL) {
        log_error_cat(LOG_CAT_UI, "Fatal: GetNewDialog failed (Error: %d). Check DLOG resource ID %d.", ResError(), kBaseResID);
        return false;
    }
    log_info_cat(LOG_CAT_UI, "Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);
    GetPort(&oldPort);
    SetPort((GrafPtr)GetWindowPort(gMainWindow));
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);
    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;
    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_error_cat(LOG_CAT_UI, "Error: One or more dialog components (TEs, List) failed to initialize. Cleaning up.");
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
            log_debug_cat(LOG_CAT_UI, "Debug checkbox (Item %d) initialized to: %s", kDebugCheckbox, is_debug_output_enabled() ? "ON" : "OFF");
        } else {
            log_warning_cat(LOG_CAT_UI, "Item %d (kDebugCheckbox) is not a checkbox (Type: %d)! Cannot set initial debug state.", kDebugCheckbox, itemType);
        }
    } else {
        log_warning_cat(LOG_CAT_UI, "Item %d (kDebugCheckbox) handle is NULL! Cannot set initial state.", kDebugCheckbox);
    }
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
            ctrlHandle = (ControlHandle)itemHandle;
            SetControlValue(ctrlHandle, 0);
            log_debug_cat(LOG_CAT_UI, "Broadcast checkbox (Item %d) initialized to: OFF", kBroadcastCheckbox);
        } else {
            log_warning_cat(LOG_CAT_UI, "Item %d (kBroadcastCheckbox) is not a checkbox (Type: %d)! Cannot set initial state.", kBroadcastCheckbox, itemType);
        }
    } else {
        log_warning_cat(LOG_CAT_UI, "Item %d (kBroadcastCheckbox) handle is NULL! Cannot set initial state.", kBroadcastCheckbox);
    }
    UpdatePeerDisplayList(true);
    log_debug_cat(LOG_CAT_UI, "Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);
    /* Mark all components for initial update */
    gInputTENeedsUpdate = true;
    gMessagesTENeedsUpdate = true;
    gPeerListNeedsUpdate = true;
    UpdateDialogControls();
    log_debug_cat(LOG_CAT_UI, "Initial UpdateDialogControls() called from InitDialog.");
    SetPort(oldPort);
    log_info_cat(LOG_CAT_UI, "InitDialog finished successfully.");
    return true;
}
void CleanupDialog(void)
{
    log_debug_cat(LOG_CAT_UI, "Cleaning up Dialog...");
    CleanupPeerListControl();
    CleanupInputTE();
    CleanupMessagesTEAndScrollbar();
    if (gMainWindow != NULL) {
        log_debug_cat(LOG_CAT_UI, "Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
    gDialogTEInitialized = false;
    gDialogListInitialized = false;
    log_debug_cat(LOG_CAT_UI, "Dialog cleanup complete.");
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

    if (!gDialogTEInitialized || gInputTE == NULL) {
        log_error_cat(LOG_CAT_UI, "Error (HandleSendButtonClick): Input TE not initialized.");
        SysBeep(10);
        return;
    }

    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_error_cat(LOG_CAT_UI, "Error: Could not get text from input field for sending.");
        SysBeep(10);
        ActivateInputTE(true);
        return;
    }

    if (strlen(inputCStr) == 0) {
        log_debug_cat(LOG_CAT_UI, "Send Action: Input field is empty. No action taken.");
        ActivateInputTE(true);
        return;
    }

    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false;
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        broadcastCheckboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(broadcastCheckboxHandle) == 1);
        log_debug_cat(LOG_CAT_UI, "Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_warning_cat(LOG_CAT_UI, "Broadcast item %d is not a checkbox or handle is NULL! Assuming not broadcast.", kBroadcastCheckbox);
    }

    if (isBroadcast) {
        /* Open Transport broadcast */
        log_debug_cat(LOG_CAT_MESSAGING, "Broadcasting: '%s'", inputCStr);
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");

        sendErr = BroadcastMessage(inputCStr);
        if (sendErr == noErr) {
            sprintf(displayMsg, "Broadcast sent.");
        } else {
            sprintf(displayMsg, "Broadcast failed: %d", sendErr);
        }
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");

        if (sendErr == noErr) {
            ClearInputText();
        }
    } else {
        peer_t targetPeer;
        if (DialogPeerList_GetSelectedPeer(&targetPeer)) {
            log_debug_cat(LOG_CAT_MESSAGING, "Attempting to send to selected peer %s@%s: '%s'",
                          targetPeer.username, targetPeer.ip, inputCStr);

            /* Send via Open Transport */
            sendErr = SendMessageToPeer(targetPeer.ip, inputCStr, MSG_TEXT);

            if (sendErr == noErr) {
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_debug_cat(LOG_CAT_MESSAGING, "Message sent successfully.");
                ClearInputText();
            } else {
                sprintf(displayMsg, "Error sending to %s: %d", targetPeer.username, sendErr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_error_cat(LOG_CAT_MESSAGING, "Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
            log_error_cat(LOG_CAT_UI, "Error: Cannot send, no peer selected in the list or selection invalid.");
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
    GrafPtr windowPort = (GrafPtr)GetWindowPort(gMainWindow);
    if (windowPort == NULL) {
        log_error_cat(LOG_CAT_UI, "UpdateDialogControls Error: Window port is NULL for gMainWindow!");
        return;
    }
    GetPort(&oldPort);
    SetPort(windowPort);
    
    /* Only update components that have been marked as needing updates */
    if (gMessagesTENeedsUpdate) {
        HandleMessagesTEUpdate(gMainWindow);
        gMessagesTENeedsUpdate = false;
    }
    if (gInputTENeedsUpdate) {
        HandleInputTEUpdate(gMainWindow);
        gInputTENeedsUpdate = false;
    }
    if (gPeerListNeedsUpdate) {
        HandlePeerListUpdate(gMainWindow);
        gPeerListNeedsUpdate = false;
    }
    
    SetPort(oldPort);
}

/* Invalidation functions to mark components for update */
void InvalidateInputTE(void)
{
    gInputTENeedsUpdate = true;
}

void InvalidateMessagesTE(void)
{
    gMessagesTENeedsUpdate = true;
}

void InvalidatePeerList(void)
{
    gPeerListNeedsUpdate = true;
}
