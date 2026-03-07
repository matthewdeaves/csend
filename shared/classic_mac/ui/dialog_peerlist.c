#include "dialog_peerlist.h"
#include "dialog.h"
#include "clog.h"
#include "peertalk.h"
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Fonts.h>
#include <Memory.h>
#include <Resources.h>
#include <Controls.h>
#include <stdio.h>
#include <string.h>

/* Global context pointer set by classic_mac/main.c */
extern PT_Context *g_ctx;

ListHandle gPeerListHandle = NULL;
Cell gLastSelectedCell = {0, -1};

Boolean InitPeerListControl(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectList, dataBounds;
    Point cellSize;
    FontInfo fontInfo;
    Boolean listOk = false;

    CLOG_DEBUG("Initializing Peer List Control...");
    GetDialogItem(dialog, kPeerListUserItem, &itemType, &itemHandle, &destRectList);

    if (itemType == userItem) {
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GetFontInfo(&fontInfo);
        SetPort(oldPort);

        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading;
        if (cellSize.v <= 0) cellSize.v = 12;
        cellSize.h = destRectList.right - destRectList.left;

        SetRect(&dataBounds, 0, 0, 1, 0);
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)dialog,
                               true, false, false, true);
        if (gPeerListHandle == NULL) {
            CLOG_ERR("LNew failed for Peer List!");
            listOk = false;
        } else {
            (*gPeerListHandle)->selFlags = lOnlyOne;
            LActivate(true, gPeerListHandle);
            listOk = true;
        }
    } else {
        CLOG_ERR("Item %d is NOT a UserItem (Type: %d)", kPeerListUserItem, itemType);
        gPeerListHandle = NULL;
        listOk = false;
    }
    return listOk;
}

void CleanupPeerListControl(void)
{
    if (gPeerListHandle != NULL) {
        LActivate(false, gPeerListHandle);
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    gLastSelectedCell.v = -1;
}

Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent)
{
    Boolean clickWasInContent = false;

    if (gPeerListHandle != NULL) {
        Point localClick = theEvent->where;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GlobalToLocal(&localClick);
        SetPort(oldPort);

        SignedByte listState = HGetState((Handle)gPeerListHandle);
        HLock((Handle)gPeerListHandle);

        if (*gPeerListHandle == NULL) {
            HSetState((Handle)gPeerListHandle, listState);
            return false;
        }

        if (PtInRect(localClick, &(**gPeerListHandle).rView)) {
            clickWasInContent = true;
            (void)LClick(localClick, theEvent->modifiers, gPeerListHandle);
            Cell clickedCell = LLastClick(gPeerListHandle);
            Cell cellToVerify = clickedCell;

            if (clickedCell.v >= 0 && clickedCell.h >= 0) {
                if (LGetSelect(false, &cellToVerify, gPeerListHandle)) {
                    gLastSelectedCell = clickedCell;
                    /* Uncheck broadcast checkbox when peer selected */
                    DialogItemType itemType;
                    Handle itemHandle;
                    Rect itemRect;
                    GrafPtr clickOldPort;
                    GetPort(&clickOldPort);
                    SetPort((GrafPtr)GetWindowPort(gMainWindow));
                    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
                        SetControlValue((ControlHandle)itemHandle, 0);
                    }
                    SetPort(clickOldPort);
                } else {
                    SetPt(&gLastSelectedCell, 0, -1);
                }
            } else {
                SetPt(&gLastSelectedCell, 0, -1);
            }
        }
        HSetState((Handle)gPeerListHandle, listState);
    }
    return clickWasInContent;
}

void UpdatePeerDisplayList(Boolean forceRedraw)
{
    Cell theCell;
    char peerStr[64];
    int currentListLengthInRows;
    int connectedCount;
    SignedByte listState;
    Boolean oldSelectionStillValid = false;
    int oldSelectedConnectedIndex = -1;
    char oldSelectedName[32] = {0};
    char oldSelectedAddr[32] = {0};
    int i, total, connectedIdx;

    if (gPeerListHandle == NULL) return;

    /* Save old selection info */
    if (gLastSelectedCell.v >= 0) {
        /* Find which connected peer was at that row */
        connectedIdx = 0;
        total = PT_GetPeerCount(g_ctx);
        for (i = 0; i < total; i++) {
            PT_Peer *p = PT_GetPeer(g_ctx, i);
            if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
                if (connectedIdx == gLastSelectedCell.v) {
                    strncpy(oldSelectedName, PT_PeerName(p), sizeof(oldSelectedName) - 1);
                    strncpy(oldSelectedAddr, PT_PeerAddress(p), sizeof(oldSelectedAddr) - 1);
                    oldSelectedConnectedIndex = connectedIdx;
                    break;
                }
                connectedIdx++;
            }
        }
    }

    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);
    if (*gPeerListHandle == NULL) {
        HSetState((Handle)gPeerListHandle, listState);
        return;
    }

    currentListLengthInRows = (**gPeerListHandle).dataBounds.bottom;
    if (currentListLengthInRows > 0) {
        LDelRow(currentListLengthInRows, 0, gPeerListHandle);
    }

    Cell tempNewSelectedCell = {0, -1};
    connectedCount = 0;

    total = PT_GetPeerCount(g_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(g_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            const char *name = PT_PeerName(p);
            const char *addr = PT_PeerAddress(p);
            sprintf(peerStr, "%s@%s", name[0] ? name : "???", addr);

            LAddRow(1, connectedCount, gPeerListHandle);
            SetPt(&theCell, 0, connectedCount);
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);

            /* Check if this was the old selection */
            if (oldSelectedConnectedIndex >= 0 &&
                    strcmp(addr, oldSelectedAddr) == 0 &&
                    strcmp(name, oldSelectedName) == 0) {
                tempNewSelectedCell = theCell;
                oldSelectionStillValid = true;
            }

            connectedCount++;
        }
    }

    (**gPeerListHandle).dataBounds.bottom = connectedCount;

    if (oldSelectionStillValid) {
        LSetSelect(true, tempNewSelectedCell, gPeerListHandle);
        gLastSelectedCell = tempNewSelectedCell;
    } else {
        SetPt(&gLastSelectedCell, 0, -1);
    }

    HSetState((Handle)gPeerListHandle, listState);

    if (forceRedraw || connectedCount != currentListLengthInRows) {
        GrafPtr windowPort = (GrafPtr)GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPortForDrawing;
            GetPort(&oldPortForDrawing);
            SetPort(windowPort);
            listState = HGetState((Handle)gPeerListHandle);
            HLock((Handle)gPeerListHandle);
            if (*gPeerListHandle != NULL) {
                InvalRect(&(**gPeerListHandle).rView);
            }
            HSetState((Handle)gPeerListHandle, listState);
            SetPort(oldPortForDrawing);
            InvalidatePeerList();
        }
    }
}

void HandlePeerListUpdate(DialogPtr dialog)
{
    if (gPeerListHandle != NULL) {
        GrafPtr windowPort = (GrafPtr)GetWindowPort(dialog);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort);
            LUpdate(windowPort->visRgn, gPeerListHandle);
            SetPort(oldPort);
        }
    }
}

PT_Peer *DialogPeerList_GetSelectedPeer(void)
{
    int connectedIdx, i, total;

    if (gPeerListHandle == NULL || gLastSelectedCell.v < 0) {
        return NULL;
    }

    connectedIdx = 0;
    total = PT_GetPeerCount(g_ctx);
    for (i = 0; i < total; i++) {
        PT_Peer *p = PT_GetPeer(g_ctx, i);
        if (p && PT_GetPeerState(p) == PT_PEER_CONNECTED) {
            if (connectedIdx == gLastSelectedCell.v) {
                return p;
            }
            connectedIdx++;
        }
    }

    SetPt(&gLastSelectedCell, 0, -1);
    return NULL;
}

void DialogPeerList_DeselectAll(void)
{
    if (gPeerListHandle != NULL && gLastSelectedCell.v >= 0) {
        GrafPtr oldPortForList;
        GetPort(&oldPortForList);
        SetPort((GrafPtr)GetWindowPort(gMainWindow));
        LSetSelect(false, gLastSelectedCell, gPeerListHandle);
        SetPt(&gLastSelectedCell, 0, -1);
        SignedByte listState = HGetState((Handle)gPeerListHandle);
        HLock((Handle)gPeerListHandle);
        if (*gPeerListHandle != NULL) {
            InvalRect(&(**gPeerListHandle).rView);
        }
        HSetState((Handle)gPeerListHandle, listState);
        SetPort(oldPortForList);
    }
}

void ActivatePeerList(Boolean activating)
{
    if (gPeerListHandle != NULL) {
        LActivate(activating, gPeerListHandle);
    }
}
