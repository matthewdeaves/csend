#include "dialog_input.h"
#include "dialog.h"
#include "../shared/logging.h"
#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>
#include <string.h>
#include <Quickdraw.h>
TEHandle gInputTE = NULL;
Boolean InitInputTE(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;
    log_debug_cat(LOG_CAT_UI, "Initializing Input TE (as UserItem)...");
    GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &itemRect);
    if (itemType == userItem) {
        Rect teViewRect = itemRect;
        Rect teDestRect = itemRect;
        InsetRect(&teViewRect, 1, 1);
        InsetRect(&teDestRect, 1, 1);
        if (teViewRect.bottom <= teViewRect.top || teViewRect.right <= teViewRect.left) {
            log_debug_cat(LOG_CAT_UI, "ERROR: Input TE itemRect too small after insetting for border. Original: (%d,%d,%d,%d)",
                          itemRect.top, itemRect.left, itemRect.bottom, itemRect.right);
            gInputTE = NULL;
            return false;
        }
        gInputTE = TENew(&teDestRect, &teViewRect);
        if (gInputTE == NULL) {
            log_debug_cat(LOG_CAT_UI, "CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            return false;
        } else {
            log_debug_cat(LOG_CAT_UI, "TENew succeeded for Input TE. Handle: 0x%lX. ViewRect for TE: (%d,%d,%d,%d)",
                          (unsigned long)gInputTE, teViewRect.top, teViewRect.left, teViewRect.bottom, teViewRect.right);
            TESetText((Ptr)"", 0, gInputTE);
            TECalText(gInputTE);
            TESetSelect(0, 0, gInputTE);
            return true;
        }
    } else {
        log_debug_cat(LOG_CAT_UI, "ERROR: Item %d (kInputTextEdit) is Type: %d. Expected userItem. Cannot initialize Input TE.", kInputTextEdit, itemType);
        gInputTE = NULL;
        return false;
    }
}
void CleanupInputTE(void)
{
    log_debug_cat(LOG_CAT_UI, "Cleaning up Input TE...");
    if (gInputTE != NULL) {
        TEDispose(gInputTE);
        gInputTE = NULL;
    }
    log_debug_cat(LOG_CAT_UI, "Input TE cleanup finished.");
}
void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent)
{
    if (gInputTE != NULL) {
        Point localPt = theEvent->where;
        GrafPtr oldPort;
        Rect teViewRect = (**gInputTE).viewRect;
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GlobalToLocal(&localPt);
        if (PtInRect(localPt, &teViewRect)) {
            TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
        }
        SetPort(oldPort);
    }
}
void HandleInputTEUpdate(DialogPtr dialog)
{
    if (gInputTE != NULL) {
        Rect userItemRect;
        DialogItemType itemTypeIgnored;
        Handle itemHandleIgnored;
        GrafPtr oldPort;
        Rect teActualViewRect;
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GetDialogItem(dialog, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &userItemRect);
        FrameRect(&userItemRect);
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);
        if (*gInputTE != NULL) {
            teActualViewRect = (**gInputTE).viewRect;
            EraseRect(&teActualViewRect);
            TEUpdate(&teActualViewRect, gInputTE);
        } else {
            log_debug_cat(LOG_CAT_UI, "HandleInputTEUpdate ERROR: gInputTE deref failed after HLock!");
        }
        HSetState((Handle)gInputTE, teState);
        SetPort(oldPort);
    } else {
        log_debug_cat(LOG_CAT_UI, "HandleInputTEUpdate: gInputTE is NULL, skipping update.");
    }
}
void ActivateInputTE(Boolean activating)
{
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
            log_debug_cat(LOG_CAT_UI, "ActivateInputTE: Activating Input TE.");
        } else {
            TEDeactivate(gInputTE);
            log_debug_cat(LOG_CAT_UI, "ActivateInputTE: Deactivating Input TE.");
        }
    }
}
Boolean GetInputText(char *buffer, short bufferSize)
{
    if (gInputTE == NULL || buffer == NULL || bufferSize <= 0) {
        if (buffer && bufferSize > 0) buffer[0] = '\0';
        log_debug_cat(LOG_CAT_UI, "Error: GetInputText called with NULL TE/buffer or zero size.");
        return false;
    }
    SignedByte teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);
    Boolean success = false;
    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        Handle textH = (**gInputTE).hText;
        Size textLen = (**gInputTE).teLength;
        Size copyLen = textLen;
        if (copyLen >= bufferSize) {
            copyLen = bufferSize - 1;
            log_debug_cat(LOG_CAT_UI, "Warning: Input text truncated during GetInputText (buffer size %d, needed %ld).", bufferSize, (long)textLen + 1);
        }
        SignedByte textHandleState = HGetState(textH);
        HLock(textH);
        BlockMoveData(*textH, buffer, copyLen);
        HSetState(textH, textHandleState);
        buffer[copyLen] = '\0';
        success = true;
    } else {
        log_debug_cat(LOG_CAT_UI, "Error: Cannot get text from Input TE (NULL TE record or hText).");
        buffer[0] = '\0';
        success = false;
    }
    HSetState((Handle)gInputTE, teState);
    return success;
}
void ClearInputText(void)
{
    if (gInputTE != NULL) {
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);
        if (*gInputTE != NULL) {
            TESetText((Ptr)"", 0, gInputTE);
            TECalText(gInputTE);
            TESetSelect(0, 0, gInputTE);
        } else {
            log_debug_cat(LOG_CAT_UI, "ClearInputText Error: gInputTE deref failed!");
        }
        HSetState((Handle)gInputTE, teState);
        log_debug_cat(LOG_CAT_UI, "Input field cleared.");
        if (gMainWindow != NULL) {
            /* Force immediate visual update of the input field */
            HandleInputTEUpdate(gMainWindow);
        }
    }
}
void IdleInputTE(void)
{
    if (gInputTE != NULL) {
        TEIdle(gInputTE);
    }
}
Boolean HandleInputTEKeyDown(EventRecord *theEvent)
{
    char theChar;
    if (gInputTE != NULL && gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow) {
        if (!(theEvent->modifiers & cmdKey)) {
            theChar = theEvent->message & charCodeMask;
            TEKey(theChar, gInputTE);
            return true;
        }
    }
    return false;
}
