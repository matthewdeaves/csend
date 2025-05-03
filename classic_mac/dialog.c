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
#include <string.h>
#include <stdlib.h>
DialogPtr gMainWindow = NULL;
TEHandle gMessagesTE = NULL;
TEHandle gInputTE = NULL;
Boolean gDialogTEInitialized = false;
char gMyUsername[32] = "MacUser";
Boolean InitDialog(void) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect destRectInput, viewRectInput;
    Boolean messagesOk = false;
    Boolean inputOk = false;
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
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kMessagesTextEdit,
                    destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right);
        viewRectMessages = destRectMessages;
        log_message("Calling TENew for Messages TE...");
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(true, gMessagesTE);
            log_message("TEAutoView finished for Messages TE.");
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        messagesOk = false;
    }
    log_message("Getting item %d info (Input UserItem)...", kInputTextEdit);
    GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kInputTextEdit,
                    destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right);
        viewRectInput = destRectInput;
        log_message("Calling TENew for Input TE...");
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            inputOk = false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE);
            log_message("TEAutoView finished for Input TE.");
            inputOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        inputOk = false;
    }
    gDialogTEInitialized = (messagesOk && inputOk);
    log_message("Dialog TE fields initialization complete (Success: %s). Enabling dialog logging.", gDialogTEInitialized ? "YES" : "NO");
    if (!gDialogTEInitialized) {
        log_message("Error: One or both TextEdit fields failed to initialize.");
        return false;
    }
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    TEActivate(gInputTE);
    log_message("Input TE activated.");
    log_message("Peer list (item %d) initialization needed (List Manager).", kPeerListUserItem);
    return true;
}
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");
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
    log_message("Dialog cleanup complete.");
}
void HandleDialogClick(DialogPtr dialog, short itemHit) {
    ControlHandle checkboxHandle;
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    short currentValue;
    if (dialog != gMainWindow) return;
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
            break;
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
    if (*gInputTE != NULL && (*gInputTE)->hText != NULL) {
        Size textLen = (**gInputTE).teLength;
        if (textLen > sizeof(inputCStr) - 1) {
             textLen = sizeof(inputCStr) - 1;
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
        SysBeep(5);
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
    if (formatResult == 0) {
        if (isBroadcast) {
            log_message("Broadcasting: %s (Network send not implemented)", inputCStr);
        } else {
            int selectedPeerIndex = 1;
            if (selectedPeerIndex > 0) {
                peer_t targetPeer;
                if (GetPeerByIndex(selectedPeerIndex, &targetPeer)) {
                     log_message("Sending to peer %d (%s): %s (Network send not implemented)",
                                 selectedPeerIndex, targetPeer.ip, inputCStr);
                } else {
                     log_message("Error: Cannot send, selected peer index %d not found or invalid.", selectedPeerIndex);
                     SysBeep(10);
                }
            } else {
                log_message("Error: Cannot send, no peer selected in the list.");
                SysBeep(10);
            }
        }
        sprintf(displayMsg, "You: %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
    } else {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
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
    if (gInputTE != NULL) {
       TEActivate(gInputTE);
       log_message("Input field activated.");
    }
}
void AppendToMessagesTE(const char *text) {
    if (gMessagesTE == NULL || !gDialogTEInitialized) {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "Skipping AppendToMessagesTE: gMessagesTE is NULL or dialog not initialized.\n");
            fflush(gLogFile);
        }
        return;
    }
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        TESetSelect(currentLength, currentLength, gMessagesTE);
        TEInsert((Ptr)text, strlen(text), gMessagesTE);
    } else {
        if (gLogFile != NULL) {
            fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!\n");
            fflush(gLogFile);
        }
    }
    HSetState((Handle)gMessagesTE, teState);
}
void ActivateDialogTE(Boolean activating) {
    if (gMessagesTE != NULL) {
        if (activating) {
            TEActivate(gMessagesTE);
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
}
void UpdateDialogTE(void) {
    Rect itemRect;
    DialogItemType itemTypeIgnored;
    Handle itemHandleIgnored;
    if (gMessagesTE != NULL) {
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gMessagesTE);
    }
    if (gInputTE != NULL) {
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gInputTE);
    }
}
