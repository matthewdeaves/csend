#include "dialog.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "common_defs.h"
#include "peer_mac.h"
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Memory.h>
#include <Sound.h>
#include <Resources.h>
#include <Lists.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <string.h>
#include <stdio.h>
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
void UpdatePeerDisplayList(Boolean forceRedraw);
pascal void MyScrollAction(ControlHandle theControl, short partCode) {
    GrafPtr oldPort;
    log_to_file_only("MyScrollAction called: Control=0x%lX, PartCode=%d", (unsigned long)theControl, partCode);
    if (theControl == gMessagesScrollBar && gMessagesTE != NULL && partCode != 0) {
        short linesToScroll = 0;
        short currentScroll, maxScroll;
        short lineHeight = 0;
        short pageScroll = 1;
        short scrollDeltaPixels = 0;
        Rect viewRectToInvalidate;
        GetPort(&oldPort);
        SetPort(GetWindowPort(gMainWindow));
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            lineHeight = (**gMessagesTE).lineHeight;
            viewRectToInvalidate = (**gMessagesTE).viewRect;
            if (lineHeight <= 0) {
                log_to_file_only("MyScrollAction Warning: lineHeight is %d!", lineHeight);
                HSetState((Handle)gMessagesTE, teState);
                SetPort(oldPort);
                return;
            }
             pageScroll = (viewRectToInvalidate.bottom - viewRectToInvalidate.top) / lineHeight - 1;
             if (pageScroll < 1) pageScroll = 1;
        } else {
             log_to_file_only("MyScrollAction Error: gMessagesTE dereference failed!");
             HSetState((Handle)gMessagesTE, teState);
             SetPort(oldPort);
             return;
        }
        HSetState((Handle)gMessagesTE, teState);
        currentScroll = GetControlValue(theControl);
        maxScroll = GetControlMaximum(theControl);
        log_to_file_only("MyScrollAction: currentScroll=%d, maxScroll=%d, lineHeight=%d, pageScroll=%d",
                    currentScroll, maxScroll, lineHeight, pageScroll);
        switch (partCode) {
            case 20: linesToScroll = -1; break;
            case 21: linesToScroll = 1; break;
            case 22: linesToScroll = -pageScroll; break;
            case 23: linesToScroll = pageScroll; break;
            default:
                log_to_file_only("MyScrollAction: Ignoring partCode %d", partCode);
                SetPort(oldPort);
                return;
        }
        short newScroll = currentScroll + linesToScroll;
        if (newScroll < 0) newScroll = 0;
        if (newScroll > maxScroll) newScroll = maxScroll;
        log_to_file_only("MyScrollAction: linesToScroll=%d, newScroll=%d (clamped)", linesToScroll, newScroll);
        if (newScroll != currentScroll) {
            SetControlValue(theControl, newScroll);
            scrollDeltaPixels = (currentScroll - newScroll) * lineHeight;
            log_to_file_only("MyScrollAction: Scrolling TE by %d pixels.", scrollDeltaPixels);
            teState = HGetState((Handle)gMessagesTE);
            HLock((Handle)gMessagesTE);
            if (*gMessagesTE != NULL) {
                TEScroll(0, scrollDeltaPixels, gMessagesTE);
                InvalRect(&viewRectToInvalidate);
                log_to_file_only("MyScrollAction: Invalidated TE viewRect.");
            } else {
                 log_to_file_only("MyScrollAction Error: gMessagesTE dereference failed before TEScroll!");
            }
            HSetState((Handle)gMessagesTE, teState);
        } else {
             log_to_file_only("MyScrollAction: newScroll == currentScroll, no action needed.");
        }
        SetPort(oldPort);
    } else {
         log_to_file_only("MyScrollAction: Control mismatch (0x%lX vs 0x%lX) or TE NULL (0x%lX) or partCode 0.",
                     (unsigned long)theControl, (unsigned long)gMessagesScrollBar, (unsigned long)gMessagesTE);
    }
}
Boolean InitDialog(void) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect destRectInput, viewRectInput;
    Rect destRectList, dataBounds;
    Rect scrollBarRect;
    Point cellSize;
    FontInfo fontInfo;
    Boolean messagesOk = false;
    Boolean inputOk = false;
    Boolean listOk = false;
    Boolean scrollBarOk = false;
    log_message("Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
    if (gMainWindow == NULL) {
        log_message("Fatal: GetNewDialog failed (Error: %d).", ResError());
        return false;
    }
    log_message("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);
    log_message("Showing window...");
    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);
    log_message("Setting port...");
    SetPort((GrafPtr)gMainWindow);
    log_message("Port set.");
    log_message("Getting item %d info (Messages UserItem)...", kMessagesTextEdit);
    GetDialogItem(gMainWindow, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    if (itemType == userItem) {
        viewRectMessages = destRectMessages;
        viewRectMessages.right -= 16;
        log_message("Calling TENew for Messages TE (Adjusted Rect)...");
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(true, gMessagesTE);
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        messagesOk = false;
    }
    if (messagesOk) {
        log_message("Getting item %d info (Messages Scrollbar)...", kMessagesScrollbar);
        GetDialogItem(gMainWindow, kMessagesScrollbar, &itemType, &itemHandle, &scrollBarRect);
        log_message("DEBUG: GetDialogItem for item %d returned type %d, handle 0x%lX.", kMessagesScrollbar, itemType, (unsigned long)itemHandle);
        if (itemHandle != NULL) {
             gMessagesScrollBar = (ControlHandle)itemHandle;
             log_message("Scrollbar handle obtained: 0x%lX (Ignoring unexpected itemType %d).", (unsigned long)gMessagesScrollBar, itemType);
             AdjustMessagesScrollbar();
             scrollBarOk = true;
        } else {
             log_message("ERROR: Item %d (Scrollbar) handle is NULL! Type was %d.", kMessagesScrollbar, itemType);
             gMessagesScrollBar = NULL;
             scrollBarOk = false;
        }
    }
    log_message("Getting item %d info (Input UserItem)...", kInputTextEdit);
    GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        viewRectInput = destRectInput;
        InsetRect(&viewRectInput, 1, 1);
        log_message("Calling TENew for Input TE...");
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            inputOk = false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE);
            inputOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        inputOk = false;
    }
    gDialogTEInitialized = (messagesOk && inputOk && scrollBarOk);
    log_message("Dialog TE fields & Scrollbar initialization complete (Success: %s).", gDialogTEInitialized ? "YES" : "NO");
    log_message("Getting item %d info (Peer List UserItem)...", kPeerListUserItem);
    GetDialogItem(gMainWindow, kPeerListUserItem, &itemType, &itemHandle, &destRectList);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kPeerListUserItem,
                    destRectList.top, destRectList.left, destRectList.bottom, destRectList.right);
        GetFontInfo(&fontInfo);
        cellSize.v = fontInfo.ascent + fontInfo.descent + fontInfo.leading;
        cellSize.h = destRectList.right - destRectList.left;
        SetRect(&dataBounds, 0, 0, 1, 0);
        log_message("Calling LNew for Peer List...");
        gPeerListHandle = LNew(&destRectList, &dataBounds, cellSize, 0, (WindowPtr)gMainWindow, true, false, false, true);
        if (gPeerListHandle == NULL) {
            log_message("CRITICAL ERROR: LNew failed for Peer List! (Error: %d)", ResError());
            listOk = false;
        } else {
            log_message("LNew succeeded for Peer List. Handle: 0x%lX", (unsigned long)gPeerListHandle);
            (*gPeerListHandle)->selFlags = lOnlyOne;
            listOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for LNew.", kPeerListUserItem, itemType);
        gPeerListHandle = NULL;
        listOk = false;
    }
    gDialogListInitialized = listOk;
    log_message("Dialog List Manager initialization complete (Success: %s).", gDialogListInitialized ? "YES" : "NO");
    if (!gDialogTEInitialized || !gDialogListInitialized) {
        log_message("Error: One or more dialog components failed to initialize. Cleaning up.");
        CleanupDialog();
        return false;
    }
    UpdatePeerDisplayList(true);
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    if (gInputTE) TEActivate(gInputTE);
    log_message("Input TE activated.");
    log_message("InitDialog finished successfully.");
    return true;
}
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");
    gMessagesScrollBar = NULL;
    if (gPeerListHandle != NULL) {
        log_message("Disposing Peer List...");
        LDispose(gPeerListHandle);
        gPeerListHandle = NULL;
    }
    if (gMessagesTE != NULL) {
        log_message("Disposing Messages TE...");
        TEDispose(gMessagesTE);
        gMessagesTE = NULL;
    }
    if (gInputTE != NULL) {
        log_message("Disposing Input TE...");
        TEDispose(gInputTE);
        gInputTE = NULL;
    }
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
    Boolean listClicked = false;
    Point clickLoc = theEvent->where;
    if (dialog != gMainWindow) return;
    if (gPeerListHandle != NULL && itemHit == kPeerListUserItem) {
         Point localClick = clickLoc;
         SetPort((GrafPtr)dialog);
         GlobalToLocal(&localClick);
         if (PtInRect(localClick, &(**gPeerListHandle).rView)) {
             log_message("Click is inside Peer List view rect. Calling LClick.");
             listClicked = LClick(localClick, theEvent->modifiers, gPeerListHandle);
             if (!LGetSelect(true, &gLastSelectedCell, gPeerListHandle)) {
                 SetPt(&gLastSelectedCell, 0, 0);
             } else {
                 log_message("Peer list item selected: Row %d, Col %d", gLastSelectedCell.v, gLastSelectedCell.h);
             }
         } else {
              log_message("Click in Peer List item rect, but outside LClick view rect.");
         }
    }
    if (!listClicked) {
        switch (itemHit) {
            case kSendButton:
                log_message("Send button clicked.");
                DoSendAction(dialog);
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
                break;
            default:
                 {
                     Point localPt = clickLoc;
                     SetPort((GrafPtr)dialog);
                     GlobalToLocal(&localPt);
                     if (gInputTE && PtInRect(localPt, &(**gInputTE).viewRect)) {
                         TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
                     } else if (gMessagesTE && PtInRect(localPt, &(**gMessagesTE).viewRect)) {
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
    SignedByte teState;
    if (gInputTE == NULL) {
        log_message("Error: Input TextEdit not initialized. Cannot send.");
        SysBeep(10);
        return;
    }
    teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);
    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        Size textLen = (**gInputTE).teLength;
        if (textLen > sizeof(inputCStr) - 1) {
             textLen = sizeof(inputCStr) - 1;
             log_message("Warning: Input text truncated to %d bytes.", textLen);
        }
        BlockMoveData(*((**gInputTE).hText), inputCStr, textLen);
        inputCStr[textLen] = '\0';
    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
        HSetState((Handle)gInputTE, teState);
        SysBeep(10);
        return;
    }
    HSetState((Handle)gInputTE, teState);
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
    if (formatResult > 0) {
        if (isBroadcast) {
            log_message("Broadcasting: %s (Network send not implemented yet)", inputCStr);
            sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r");
        } else {
            int selectedRow = gLastSelectedCell.v;
            int activePeerIndex = -1;
            int current_active_count = 0;
            PruneTimedOutPeers();
            for (int i = 0; i < MAX_PEERS; i++) {
                if (gPeerList[i].active) {
                    if (current_active_count == selectedRow) {
                        activePeerIndex = i;
                        break;
                    }
                    current_active_count++;
                }
            }
            if (activePeerIndex != -1) {
                peer_t targetPeer = gPeerList[activePeerIndex];
                log_message("Sending to selected peer %d (%s@%s): %s (Network send not implemented yet)",
                             selectedRow, targetPeer.username, targetPeer.ip, inputCStr);
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
            } else {
                 log_message("Error: Cannot send, no peer selected in the list or selection invalid.");
                 SysBeep(10);
            }
        }
        if (gInputTE != NULL) {
            teState = HGetState((Handle)gInputTE);
            HLock((Handle)gInputTE);
            if (*gInputTE != NULL) {
                TESetText((Ptr)"", 0, gInputTE);
                TECalText(gInputTE);
            }
            HSetState((Handle)gInputTE, teState);
            log_message("Input field cleared.");
        }
    } else {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
    }
    if (gInputTE != NULL) {
       TEActivate(gInputTE);
    }
}
void AppendToMessagesTE(const char *text) {
    GrafPtr oldPort;
    if (gMessagesTE == NULL || !gDialogTEInitialized) {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "Skipping AppendToMessagesTE: gMessagesTE is NULL or dialog not initialized. Msg: %s\n", text);
            fflush(gLogFile);
        }
        return;
    }
    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow));
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        if (currentLength + strlen(text) < 32000) {
            TESetSelect(currentLength, currentLength, gMessagesTE);
            TEInsert((Ptr)text, strlen(text), gMessagesTE);
            AdjustMessagesScrollbar();
        } else {
             if (gLogFile != NULL) {
                 fprintf(gLogFile, "Warning: Messages TE field is full. Cannot append.\n");
                 fflush(gLogFile);
             }
        }
    } else {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!\n");
            fflush(gLogFile);
        }
    }
    HSetState((Handle)gMessagesTE, teState);
    SetPort(oldPort);
}
void ActivateDialogTE(Boolean activating) {
    if (gMessagesTE != NULL) {
        if (activating) {
        } else {
            TEDeactivate(gMessagesTE);
        }
    }
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
        } else {
            TEDeactivate(gInputTE);
        }
    }
    if (gMessagesScrollBar != NULL) {
        HiliteControl(gMessagesScrollBar, activating ? 0 : 255);
    }
}
void UpdateDialogControls(void) {
    Rect itemRect;
    DialogItemType itemTypeIgnored;
    Handle itemHandleIgnored;
    GrafPtr oldPort;
    GrafPtr windowPort;
    GetPort(&oldPort);
    windowPort = GetWindowPort(gMainWindow);
    if (windowPort == NULL) {
        log_message("UpdateDialogControls Error: Window port is NULL!");
        SetPort(oldPort);
        return;
    }
    SetPort(windowPort);
    if (gMessagesTE != NULL) {
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gMessagesTE);
    }
    if (gInputTE != NULL) {
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gInputTE);
    }
    if (gPeerListHandle != NULL) {
        LUpdate(GetWindowPort(gMainWindow)->visRgn, gPeerListHandle);
    }
    DrawControls(gMainWindow);
    SetPort(oldPort);
}
void UpdatePeerDisplayList(Boolean forceRedraw) {
    Cell theCell;
    char peerStr[INET_ADDRSTRLEN + 32 + 2];
    int currentListLength;
    int activePeerCount = 0;
    SignedByte listState;
    Boolean selectionStillValid = false;
    Cell oldSelection = gLastSelectedCell;
    if (gPeerListHandle == NULL || !gDialogListInitialized) {
        log_message("Skipping UpdatePeerDisplayList: List not initialized.");
        return;
    }
    listState = HGetState((Handle)gPeerListHandle);
    HLock((Handle)gPeerListHandle);
    currentListLength = (**gPeerListHandle).dataBounds.bottom;
    if (currentListLength > 0) {
        LDelRow(0, 0, gPeerListHandle);
    }
    PruneTimedOutPeers();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            sprintf(peerStr, "%s@%s", gPeerList[i].username, gPeerList[i].ip);
            LAddRow(1, activePeerCount, gPeerListHandle);
            SetPt(&theCell, 0, activePeerCount);
            LSetCell(peerStr, strlen(peerStr), theCell, gPeerListHandle);
            if (oldSelection.v == activePeerCount) {
                 selectionStillValid = true;
            }
            activePeerCount++;
        }
    }
     (**gPeerListHandle).dataBounds.bottom = activePeerCount;
     if (selectionStillValid && oldSelection.v < activePeerCount) {
         LSetSelect(true, oldSelection, gPeerListHandle);
         gLastSelectedCell = oldSelection;
     } else {
         SetPt(&gLastSelectedCell, 0, 0);
         if (currentListLength > 0 && oldSelection.v < currentListLength) {
            LSetSelect(false, oldSelection, gPeerListHandle);
         }
     }
    HSetState((Handle)gPeerListHandle, listState);
    if (forceRedraw || activePeerCount != currentListLength) {
        GrafPtr windowPort = GetWindowPort(gMainWindow);
        if (windowPort != NULL) {
            GrafPtr oldPort;
            GetPort(&oldPort);
            SetPort(windowPort);
            InvalRect(&(**gPeerListHandle).rView);
            SetPort(oldPort);
            log_message("Peer list updated. Active peers: %d. Invalidating list rect.", activePeerCount);
        } else {
            log_message("Peer list updated, but cannot invalidate rect (window port is NULL).");
        }
    } else {
    }
}
void AdjustMessagesScrollbar(void) {
    if (gMessagesTE == NULL || gMessagesScrollBar == NULL) {
        return;
    }
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        short lineHeight = (**gMessagesTE).lineHeight;
        short linesInView = 0;
        short firstVisibleLine = 0;
        short totalLines = (**gMessagesTE).nLines;
        short maxScroll = 0;
        short currentVal = GetControlValue(gMessagesScrollBar);
        if (lineHeight > 0) {
            linesInView = ((**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top) / lineHeight;
            firstVisibleLine = -(**gMessagesTE).destRect.top / lineHeight;
        } else {
            linesInView = 0;
            firstVisibleLine = 0;
            totalLines = 0;
            log_message("AdjustMessagesScrollbar Warning: lineHeight is %d!", lineHeight);
        }
        if (linesInView < 1) linesInView = 1;
        maxScroll = totalLines - linesInView;
        if (maxScroll < 0) maxScroll = 0;
        log_to_file_only("AdjustMessagesScrollbar: totalLines=%d, linesInView=%d, maxScroll=%d, firstVisibleLine=%d",
                    totalLines, linesInView, maxScroll, firstVisibleLine);
        if (GetControlMaximum(gMessagesScrollBar) != maxScroll) {
             log_to_file_only("AdjustMessagesScrollbar: Setting Max to %d (was %d)", maxScroll, GetControlMaximum(gMessagesScrollBar));
             SetControlMaximum(gMessagesScrollBar, maxScroll);
             if (currentVal > maxScroll) {
                 currentVal = maxScroll;
                 if (firstVisibleLine > maxScroll) {
                     short scrollDeltaPixels = (firstVisibleLine - maxScroll) * lineHeight;
                     log_to_file_only("AdjustScrollbar: Clamping scroll value, scrolling TE by %d pixels.", scrollDeltaPixels);
                     log_to_file_only("AdjustScrollbar: Calling TEScroll(0, %d, ...)", scrollDeltaPixels);
                     TEScroll(0, scrollDeltaPixels, gMessagesTE);
                     log_to_file_only("AdjustScrollbar: TEScroll finished.");
                     firstVisibleLine = maxScroll;
                 }
             }
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (after max change)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        } else if (GetControlValue(gMessagesScrollBar) != firstVisibleLine) {
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (max unchanged)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        }
        Boolean shouldBeVisible = (maxScroll > 0);
        Boolean isVisible = (**gMessagesScrollBar).contrlVis;
        if (shouldBeVisible && !isVisible) {
            log_to_file_only("AdjustMessagesScrollbar: Showing scrollbar.");
            ShowControl(gMessagesScrollBar);
        } else if (!shouldBeVisible && isVisible) {
            log_to_file_only("AdjustMessagesScrollbar: Hiding scrollbar.");
            HideControl(gMessagesScrollBar);
        }
    } else {
        log_message("AdjustMessagesScrollbar Error: gMessagesTE deref failed!");
    }
    HSetState((Handle)gMessagesTE, teState);
}
