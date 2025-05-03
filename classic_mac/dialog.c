#include "dialog.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "peer_mac.h"
#include "common_defs.h"
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
DialogPtr gMainWindow = NULL;
TEHandle gMessagesTE = NULL;
TEHandle gInputTE = NULL;
ListHandle gPeerListHandle = NULL;
ControlHandle gMessagesScrollBar = NULL;
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false;
char gMyUsername[32] = "MacUser";
Cell gLastSelectedCell = {0, 0};
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
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");
    CleanupMessagesTEAndScrollbar();
    CleanupInputTE();
    CleanupPeerListControl();
    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
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
    Boolean clickHandled = false;
    if (dialog != gMainWindow) return;
    GetDialogItem(dialog, kPeerListUserItem, &itemType, &itemHandle, &itemRect);
    if (PtInRect(theEvent->where, &itemRect)) {
        Point localClick = theEvent->where;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GlobalToLocal(&localClick);
        SetPort(oldPort);
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
    if (!clickHandled) {
        switch (itemHit) {
            case kSendButton:
                log_message("Send button clicked.");
                DoSendAction(dialog);
                clickHandled = true;
                break;
            case kBroadcastCheckbox:
                GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                if (itemType == (ctrlItem + chkCtrl)) {
                    checkboxHandle = (ControlHandle)itemHandle;
                    currentValue = GetControlValue(checkboxHandle);
                    SetControlValue(checkboxHandle, !currentValue);
                    log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
                } else {
                    log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
                }
                clickHandled = true;
                break;
            case kMessagesTextEdit:
            case kInputTextEdit:
            case kPeerListUserItem:
            case kMessagesScrollbar:
                log_to_file_only("Ignoring click on userItem/scrollbar item %d in HandleDialogClick.", itemHit);
                break;
            default:
                 {
                     Point localPt = theEvent->where;
                     GrafPtr oldPort;
                     GetPort(&oldPort);
                     SetPort(GetWindowPort(dialog));
                     GlobalToLocal(&localPt);
                     SetPort(oldPort);
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
                         log_to_file_only("HandleDialogClick: Click in content not handled (itemHit %d).", itemHit);
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
    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_message("Error: Could not get text from input field.");
        SysBeep(10);
        return;
    }
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        return;
    }
    GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false;
    }
    int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                      gMyUsername, gMyLocalIPStr, inputCStr);
    if (formatResult <= 0) {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
        return;
    }
    if (isBroadcast) {
        log_message("Broadcasting: %s (Network send not implemented yet)", inputCStr);
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
    } else {
        peer_t targetPeer;
        if (GetSelectedPeerInfo(&targetPeer)) {
            log_message("Sending to selected peer %s@%s: %s (Network send not implemented yet)",
                         targetPeer.username, targetPeer.ip, inputCStr);
            sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r");
        } else {
             log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
             SysBeep(10);
             return;
        }
    }
    ClearInputText();
    ActivateInputTE(true);
}
void ActivateDialogTE(Boolean activating) {
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
    HandleMessagesTEUpdate(gMainWindow);
    HandleInputTEUpdate(gMainWindow);
    HandlePeerListUpdate(gMainWindow);
    DrawControls(gMainWindow);
    SetPort(oldPort);
}
pascal void MyScrollAction(ControlHandle theControl, short partCode) {
    if (theControl == gMessagesScrollBar) {
        HandleMessagesScrollClick(theControl, partCode);
    } else {
         log_to_file_only("MyScrollAction: Ignoring unknown control 0x%lX", (unsigned long)theControl);
    }
}
