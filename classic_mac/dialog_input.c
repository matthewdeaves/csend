#include "dialog_input.h"
#include "dialog.h"
#include "logging.h"
#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>
#include <string.h>
TEHandle gInputTE = NULL;
Boolean InitInputTE(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectInput, viewRectInput;
    log_message("Initializing Input TE...");
    GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        viewRectInput = destRectInput;
        InsetRect(&viewRectInput, 1, 1);
        log_message("Calling TENew for Input TE (Dest: T%d,L%d,B%d,R%d; View: T%d,L%d,B%d,R%d)",
                    destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right,
                    viewRectInput.top, viewRectInput.left, viewRectInput.bottom, viewRectInput.right);
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            return false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE);
            return true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        return false;
    }
}
void CleanupInputTE(void)
{
    log_message("Cleaning up Input TE...");
    if (gInputTE != NULL) {
        TEDispose(gInputTE);
        gInputTE = NULL;
    }
    log_message("Input TE cleanup finished.");
}
void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent)
{
    if (gInputTE != NULL) {
        Point localPt = theEvent->where;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GlobalToLocal(&localPt);
        SetPort(oldPort);
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);
        Boolean clickedInside = false;
        if (*gInputTE != NULL) {
            clickedInside = PtInRect(localPt, &(**gInputTE).viewRect);
        } else {
            log_message("HandleInputTEClick Error: gInputTE deref failed!");
        }
        HSetState((Handle)gInputTE, teState);
        if (clickedInside) {
            log_to_file_only("HandleInputTEClick: Click inside Input TE viewRect. Calling TEClick.");
            TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
        } else {
            log_to_file_only("HandleInputTEClick: Click was outside Input TE viewRect.");
        }
    }
}
void HandleInputTEUpdate(DialogPtr dialog)
{
    if (gInputTE != NULL) {
        Rect itemRect;
        DialogItemType itemTypeIgnored;
        Handle itemHandleIgnored;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog));
        GetDialogItem(dialog, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);
        if (*gInputTE != NULL) {
            TEUpdate(&itemRect, gInputTE);
        }
        HSetState((Handle)gInputTE, teState);
        SetPort(oldPort);
    }
}
void ActivateInputTE(Boolean activating)
{
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
            TESetSelect((**gInputTE).teLength, (**gInputTE).teLength, gInputTE);
            log_to_file_only("ActivateInputTE: Activating Input TE.");
        } else {
            TEDeactivate(gInputTE);
            log_to_file_only("ActivateInputTE: Deactivating Input TE.");
        }
    }
}
Boolean GetInputText(char *buffer, short bufferSize)
{
    if (gInputTE == NULL || buffer == NULL || bufferSize <= 0) {
        if (buffer && bufferSize > 0) buffer[0] = '\0';
        log_message("Error: GetInputText called with NULL TE/buffer or zero size.");
        return false;
    }
    SignedByte teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);
    Boolean success = false;
    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        Size textLen = (**gInputTE).teLength;
        Size copyLen = textLen;
        if (copyLen >= bufferSize) {
            copyLen = bufferSize - 1;
            log_message("Warning: Input text truncated during GetInputText (buffer size %d, needed %ld).", bufferSize, (long)textLen + 1);
        }
        SignedByte textHandleState = HGetState((**gInputTE).hText);
        HLock((**gInputTE).hText);
        BlockMoveData(*((**gInputTE).hText), buffer, copyLen);
        HSetState((**gInputTE).hText, textHandleState);
        buffer[copyLen] = '\0';
        success = true;
    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
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
        } else {
            log_message("ClearInputText Error: gInputTE deref failed!");
        }
        HSetState((Handle)gInputTE, teState);
        log_message("Input field cleared.");
    }
}
