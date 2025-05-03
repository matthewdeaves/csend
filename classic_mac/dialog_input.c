#include "dialog_input.h"
#include "dialog.h" // Needed for kInputTextEdit define
#include "logging.h"

#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>   // For HLock, HSetState, BlockMoveData
#include <string.h>   // For strlen

/*----------------------------------------------------------*/
/* Implementation                                           */
/*----------------------------------------------------------*/

Boolean InitInputTE(DialogPtr dialog) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectInput, viewRectInput;

    log_message("Initializing Input TE...");
    GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        viewRectInput = destRectInput;
        // Inset slightly for border/padding
        InsetRect(&viewRectInput, 1, 1);

        log_message("Calling TENew for Input TE (Rect: T%d,L%d,B%d,R%d; View: T%d,L%d,B%d,R%d)",
                    destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right,
                    viewRectInput.top, viewRectInput.left, viewRectInput.bottom, viewRectInput.right);

        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            return false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE);
            // Could set other properties like TESetWordWrap(true, gInputTE); if needed
            return true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        return false;
    }
}

void CleanupInputTE(void) {
    log_message("Cleaning up Input TE...");
    if (gInputTE != NULL) {
        TEDispose(gInputTE);
        gInputTE = NULL;
    }
    log_message("Input TE cleanup finished.");
}

void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent) {
    if (gInputTE != NULL) {
        Point localPt = theEvent->where;
        GrafPtr oldPort;

        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog)); // Use the dialog's port for coordinate conversion
        GlobalToLocal(&localPt);
        SetPort(oldPort); // Restore original port

        // Check if the click was within the TE's view rectangle
        // Need to lock the handle briefly to access viewRect safely
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
            // TEClick handles setting the selection range/insertion point
            TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
            // No need to call TEActivate here, main event loop handles focus
        } else {
             log_to_file_only("HandleInputTEClick: Click was outside Input TE viewRect.");
        }
    }
}


void HandleInputTEUpdate(DialogPtr dialog) {
    if (gInputTE != NULL) {
        Rect itemRect;
        DialogItemType itemTypeIgnored;
        Handle itemHandleIgnored;
        GrafPtr oldPort;

        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog)); // Ensure correct port

        // Get the item's rectangle
        GetDialogItem(dialog, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // TEUpdate redraws the text within the intersection of itemRect and the port's visRgn
        TEUpdate(&itemRect, gInputTE);

        SetPort(oldPort);
    }
}

void ActivateInputTE(Boolean activating) {
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
        } else {
            TEDeactivate(gInputTE);
        }
    }
}

Boolean GetInputText(char *buffer, short bufferSize) {
    if (gInputTE == NULL || buffer == NULL || bufferSize <= 0) {
        if (buffer && bufferSize > 0) buffer[0] = '\0';
        log_message("Error: GetInputText called with NULL TE/buffer or zero size.");
        return false;
    }

    SignedByte teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);

    Boolean success = false;
    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        // Get the length of the text in the TE record
        Size textLen = (**gInputTE).teLength;

        // Determine how much to copy, respecting buffer size and null terminator
        Size copyLen = textLen;
        if (copyLen >= bufferSize) {
             copyLen = bufferSize - 1; // Leave space for null terminator
             log_message("Warning: Input text truncated during GetInputText (buffer size %d, needed %ld).", bufferSize, (long)textLen + 1);
        }

        // Copy the text from the TE's text handle
        // Need to lock the text handle itself too
        SignedByte textHandleState = HGetState((**gInputTE).hText);
        HLock((**gInputTE).hText);
        BlockMoveData(*((**gInputTE).hText), buffer, copyLen);
        HSetState((**gInputTE).hText, textHandleState); // Unlock text handle

        buffer[copyLen] = '\0'; // Null-terminate the C string
        success = true;

    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
        buffer[0] = '\0'; // Ensure buffer is empty on failure
        success = false;
    }

    HSetState((Handle)gInputTE, teState); // Unlock TE handle
    return success;
}

void ClearInputText(void) {
    if (gInputTE != NULL) {
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);

        if (*gInputTE != NULL) {
            // Set the text to an empty string
            TESetText((Ptr)"", 0, gInputTE);
            // Optional: Recalculate line breaks if word wrap is on
            TECalText(gInputTE);
            // Optional: Scroll back to top if needed, though likely not for input
            // TEScroll(0, -(**gInputTE).destRect.top, gInputTE);
            // AdjustMessagesScrollbar(); // Not needed for input TE
        } else {
             log_message("ClearInputText Error: gInputTE deref failed!");
        }

        HSetState((Handle)gInputTE, teState);
        log_message("Input field cleared.");

        // Ensure the view is updated if the window is active
        // This might be redundant if an update event follows, but can be useful
        // InvalRect(&(**gInputTE).viewRect);
    }
}