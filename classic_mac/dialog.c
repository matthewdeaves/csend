#include "dialog.h"
#include "clog.h"
#include "peertalk.h"
#include "../shared/common_defs.h"
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

extern PT_Context *g_ctx;

DialogPtr gMainWindow = NULL;
Boolean gDialogTEInitialized = false;
Boolean gDialogListInitialized = false;

Boolean gInputTENeedsUpdate = false;
Boolean gMessagesTENeedsUpdate = false;
Boolean gPeerListNeedsUpdate = false;

Boolean InitDialog(void)
{
    Boolean messagesOk = false;
    Boolean inputOk = false;
    Boolean listOk = false;
    GrafPtr oldPort;

    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
    if (gMainWindow == NULL) {
        CLOG_ERR("GetNewDialog failed (Error: %d)", ResError());
        return false;
    }

    GetPort(&oldPort);
    SetPort((GrafPtr)GetWindowPort(gMainWindow));

    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);
    inputOk = InitInputTE(gMainWindow);
    listOk = InitPeerListControl(gMainWindow);

    gDialogTEInitialized = (messagesOk && inputOk);
    gDialogListInitialized = listOk;

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        CLOG_ERR("One or more dialog components failed to initialize");
        if (listOk) CleanupPeerListControl();
        if (inputOk) CleanupInputTE();
        if (messagesOk) CleanupMessagesTEAndScrollbar();
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
        SetPort(oldPort);
        return false;
    }

    /* Initialize checkboxes */
    {
        DialogItemType itemType;
        Handle itemHandle;
        Rect itemRect;

        GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
        if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
            SetControlValue((ControlHandle)itemHandle, 0);
        }

        GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
        if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
            SetControlValue((ControlHandle)itemHandle, 0);
        }
    }

    UpdatePeerDisplayList(true);
    ActivateInputTE(true);

    gInputTENeedsUpdate = true;
    gMessagesTENeedsUpdate = true;
    gPeerListNeedsUpdate = true;
    UpdateDialogControls();

    SetPort(oldPort);
    CLOG_INFO("Dialog initialized successfully");
    return true;
}

void CleanupDialog(void)
{
    CleanupPeerListControl();
    CleanupInputTE();
    CleanupMessagesTEAndScrollbar();
    if (gMainWindow != NULL) {
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
    gDialogTEInitialized = false;
    gDialogListInitialized = false;
}

void HandleSendButtonClick(void)
{
    char inputCStr[256];
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    Boolean isBroadcast;
    char displayMsg[BUFFER_SIZE + 100];

    if (!gDialogTEInitialized || gInputTE == NULL) {
        CLOG_ERR("Input TE not initialized");
        SysBeep(10);
        return;
    }

    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        SysBeep(10);
        ActivateInputTE(true);
        return;
    }

    if (strlen(inputCStr) == 0) {
        ActivateInputTE(true);
        return;
    }

    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false;
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        isBroadcast = (GetControlValue((ControlHandle)itemHandle) == 1);
    }

    if (isBroadcast) {
        PT_Status st;

        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");

        st = PT_Broadcast(g_ctx, MSG_CHAT, inputCStr, strlen(inputCStr));
        if (st == PT_OK) {
            int count = 0, i, total = PT_GetPeerCount(g_ctx);
            for (i = 0; i < total; i++) {
                PT_Peer *p = PT_GetPeer(g_ctx, i);
                if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) count++;
            }
            sprintf(displayMsg, "Broadcast sent to %d peer(s).", count);
            ClearInputText();
        } else {
            sprintf(displayMsg, "Broadcast failed.");
            SysBeep(10);
        }
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
    } else {
        PT_Peer *target = DialogPeerList_GetSelectedPeer();
        if (target) {
            PT_Status st = PT_Send(g_ctx, target, MSG_CHAT, inputCStr, strlen(inputCStr));
            if (st == PT_OK) {
                sprintf(displayMsg, "You (to %s): %s", PT_PeerName(target), inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                ClearInputText();
            } else {
                sprintf(displayMsg, "Error sending to %s", PT_PeerName(target));
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                SysBeep(10);
            }
        } else {
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
    if (windowPort == NULL) return;

    GetPort(&oldPort);
    SetPort(windowPort);

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

Boolean IsDebugEnabled(void)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;

    if (gMainWindow == NULL) return false;
    GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        return GetControlValue((ControlHandle)itemHandle) == 1;
    }
    return false;
}
