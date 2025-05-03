#include "dialog_messages.h"
#include "dialog.h" // Needed for kMessagesTextEdit, kMessagesScrollbar defines
#include "logging.h"

#include <MacTypes.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>   // For HLock, HSetState
#include <string.h>   // For strlen

#ifndef inUpButton
#define inUpButton      20
#endif
#ifndef inDownButton
#define inDownButton    21
#endif
#ifndef inPageUp
#define inPageUp        22
#endif
#ifndef inPageDown
#define inPageDown      23
#endif
#ifndef inThumb
#define inThumb         129
#endif

/*----------------------------------------------------------*/
/* Implementation                                           */
/*----------------------------------------------------------*/

Boolean InitMessagesTEAndScrollbar(DialogPtr dialog) {
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect scrollBarRect;
    Boolean teOk = false;
    Boolean scrollBarOk = false;

    log_message("Initializing Messages TE...");
    GetDialogItem(dialog, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    if (itemType == userItem) {
        viewRectMessages = destRectMessages;
        // Inset view rect to leave space for scrollbar
        viewRectMessages.right -= 16; // Standard scrollbar width
        // Inset slightly more for visual padding if desired
        // InsetRect(&viewRectMessages, 1, 1);

        log_message("Calling TENew for Messages TE (Rect: T%d,L%d,B%d,R%d; View: T%d,L%d,B%d,R%d)",
                    destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right,
                    viewRectMessages.top, viewRectMessages.left, viewRectMessages.bottom, viewRectMessages.right);

        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            teOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(true, gMessagesTE);
            // Consider setting other TE properties if needed (e.g., TESetWordWrap)
            teOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        teOk = false;
    }

    if (teOk) {
        log_message("Initializing Messages Scrollbar...");
        GetDialogItem(dialog, kMessagesScrollbar, &itemType, &itemHandle, &scrollBarRect);
        // Note: GetDialogItem for controls often returns userItem + ctrlItem type,
        // but the handle is the important part.
        if (itemHandle != NULL) {
             gMessagesScrollBar = (ControlHandle)itemHandle;
             log_message("Scrollbar handle obtained: 0x%lX (ItemType was %d).", (unsigned long)gMessagesScrollBar, itemType);
             // Set initial scrollbar state (max will be adjusted later)
             SetControlMinimum(gMessagesScrollBar, 0);
             SetControlMaximum(gMessagesScrollBar, 0);
             SetControlValue(gMessagesScrollBar, 0);
             // Hide scrollbar initially, AdjustMessagesScrollbar will show if needed
             HideControl(gMessagesScrollBar);
             scrollBarOk = true;
        } else {
             log_message("ERROR: Item %d (Scrollbar) handle is NULL! Type was %d.", kMessagesScrollbar, itemType);
             gMessagesScrollBar = NULL;
             scrollBarOk = false;
        }
    }

    return (teOk && scrollBarOk);
}

void CleanupMessagesTEAndScrollbar(void) {
    log_message("Cleaning up Messages TE...");
    if (gMessagesTE != NULL) {
        TEDispose(gMessagesTE);
        gMessagesTE = NULL;
    }
    // gMessagesScrollBar is part of the dialog and disposed by DisposeDialog
    gMessagesScrollBar = NULL;
    log_message("Messages TE cleanup finished.");
}

void AppendToMessagesTE(const char *text) {
    GrafPtr oldPort;
    // Check moved to log_message in original code, keeping similar logic here
    if (gMessagesTE == NULL) {
        log_to_file_only("Skipping AppendToMessagesTE: gMessagesTE is NULL.");
        return;
    }
    // Prevent recursive logging if AppendToMessagesTE is called from log_message
    // This check might need refinement depending on how logging works now.
    // static Boolean gLoggingToTE = false; // This would need to be handled carefully if used
    // if (gLoggingToTE) return;
    // gLoggingToTE = true;

    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow)); // Ensure we draw in the right window

    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);

    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        long textLen = strlen(text);

        // Check TextEdit 32K limit (be conservative)
        if (currentLength + textLen < 32000) {
            // Select the end of the text
            TESetSelect(currentLength, currentLength, gMessagesTE);
            // Insert the new text
            TEInsert((Ptr)text, textLen, gMessagesTE);
            // Scroll to the bottom (optional, but common)
            AdjustMessagesScrollbar(); // Update scrollbar range first
            short maxScroll = GetControlMaximum(gMessagesScrollBar);
            if (GetControlValue(gMessagesScrollBar) != maxScroll) {
                 short currentScrollVal = GetControlValue(gMessagesScrollBar);
                 SetControlValue(gMessagesScrollBar, maxScroll);
                 // Calculate pixel scroll needed based on the change in scroll value
                 short lineHeight = (**gMessagesTE).lineHeight;
                 if (lineHeight > 0) {
                     ScrollMessagesTE((currentScrollVal - maxScroll) * lineHeight);
                 }
            }

        } else {
             log_message("Warning: Messages TE field near full. Cannot append.");
             // Optionally: Beep or provide other user feedback
        }
    } else {
        log_message("ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!");
    }

    HSetState((Handle)gMessagesTE, teState);
    SetPort(oldPort);
    // gLoggingToTE = false;
}

void AdjustMessagesScrollbar(void) {
    if (gMessagesTE == NULL || gMessagesScrollBar == NULL) {
        log_to_file_only("AdjustMessagesScrollbar: Skipping, TE (0x%lX) or Scrollbar (0x%lX) is NULL.",
                         (unsigned long)gMessagesTE, (unsigned long)gMessagesScrollBar);
        return;
    }

    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);

    if (*gMessagesTE != NULL) {
        short lineHeight = (**gMessagesTE).lineHeight;
        short linesInView = 0;
        short totalLines = (**gMessagesTE).nLines;
        short maxScroll = 0;
        short currentVal = GetControlValue(gMessagesScrollBar);
        short firstVisibleLine = 0; // Line number (0-based) at the top of viewRect

        if (lineHeight > 0) {
            // Calculate how many lines fit vertically in the viewRect
            linesInView = ((**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top) / lineHeight;
            // Calculate the line number currently at the top
            // destRect.top is negative when scrolled down.
            firstVisibleLine = -(**gMessagesTE).destRect.top / lineHeight;
        } else {
            // Avoid division by zero if lineHeight isn't set correctly
            linesInView = 1;
            totalLines = 0;
            firstVisibleLine = 0;
            log_message("AdjustMessagesScrollbar Warning: lineHeight is %d!", lineHeight);
        }

        if (linesInView < 1) linesInView = 1; // Must be at least 1

        // Maximum scroll value is the total lines minus the number that fit in view
        maxScroll = totalLines - linesInView;
        if (maxScroll < 0) maxScroll = 0; // Cannot scroll beyond the top

        log_to_file_only("AdjustMessagesScrollbar: totalLines=%d, linesInView=%d, maxScroll=%d, firstVisibleLine=%d",
                    totalLines, linesInView, maxScroll, firstVisibleLine);

        // Update scrollbar maximum if it has changed
        if (GetControlMaximum(gMessagesScrollBar) != maxScroll) {
             log_to_file_only("AdjustMessagesScrollbar: Setting Max to %d (was %d)", maxScroll, GetControlMaximum(gMessagesScrollBar));
             SetControlMaximum(gMessagesScrollBar, maxScroll);

             // If the current value is now invalid, clamp it and potentially scroll TE
             if (firstVisibleLine > maxScroll) {
                 log_to_file_only("AdjustScrollbar: Clamping scroll value from %d to %d.", firstVisibleLine, maxScroll);
                 // Calculate how many pixels we need to scroll TE content *up*
                 short scrollDeltaPixels = (firstVisibleLine - maxScroll) * lineHeight;
                 ScrollMessagesTE(scrollDeltaPixels); // Positive scrolls view up
                 firstVisibleLine = maxScroll; // Update our understanding of the top line
             }
             // Set the potentially clamped value
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        } else if (GetControlValue(gMessagesScrollBar) != firstVisibleLine) {
             // Max hasn't changed, but the content scrolled, update thumb position
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (max unchanged)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        }

        // Show or hide the scrollbar based on whether scrolling is possible
        Boolean shouldBeVisible = (maxScroll > 0);
        // Check the control's visibility flag
        Boolean isVisible = (**gMessagesScrollBar).contrlVis != 0;

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


void HandleMessagesScrollClick(ControlHandle theControl, short partCode) {
    // Assumes theControl == gMessagesScrollBar (checked by caller MyScrollAction)
    log_to_file_only("HandleMessagesScrollClick called: PartCode=%d", partCode);

    if (gMessagesTE == NULL || partCode == 0) {
        log_to_file_only("HandleMessagesScrollClick: Ignoring (TE NULL or partCode 0).");
        return;
    }

    short linesToScroll = 0;
    short currentScroll, maxScroll;
    short lineHeight = 0;
    short pageScroll = 1; // Default page scroll amount

    // Get necessary info (lock TE temporarily)
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        lineHeight = (**gMessagesTE).lineHeight;
        if (lineHeight > 0) {
            Rect viewRect = (**gMessagesTE).viewRect;
            pageScroll = (viewRect.bottom - viewRect.top) / lineHeight - 1; // Page is view height minus one line
            if (pageScroll < 1) pageScroll = 1;
        } else {
            log_to_file_only("HandleMessagesScrollClick Warning: lineHeight is %d!", lineHeight);
            HSetState((Handle)gMessagesTE, teState);
            return; // Cannot scroll without line height
        }
    } else {
         log_to_file_only("HandleMessagesScrollClick Error: gMessagesTE dereference failed!");
         HSetState((Handle)gMessagesTE, teState);
         return;
    }
    HSetState((Handle)gMessagesTE, teState); // Unlock TE

    currentScroll = GetControlValue(theControl);
    maxScroll = GetControlMaximum(theControl);

    log_to_file_only("HandleMessagesScrollClick: currentScroll=%d, maxScroll=%d, lineHeight=%d, pageScroll=%d",
                currentScroll, maxScroll, lineHeight, pageScroll);

    // Determine scroll amount based on part code
    switch (partCode) {
        case inUpButton:           // Usually 20 (Standard constant)
        case inPageUp:             // Usually 22 (Standard constant)
            linesToScroll = (partCode == inUpButton) ? -1 : -pageScroll;
            break;

        case inDownButton:         // Usually 21 (Standard constant)
        case inPageDown:           // Usually 23 (Standard constant)
            linesToScroll = (partCode == inDownButton) ? 1 : pageScroll;
            break;

        default:
            log_to_file_only("HandleMessagesScrollClick: Ignoring unknown partCode %d", partCode);
            return; // Ignore thumb clicks (partCode 129) here, handled by TrackControl in main loop
    }

    short newScroll = currentScroll + linesToScroll;

    // Clamp the new scroll value
    if (newScroll < 0) newScroll = 0;
    if (newScroll > maxScroll) newScroll = maxScroll;

    log_to_file_only("HandleMessagesScrollClick: linesToScroll=%d, newScroll=%d (clamped)", linesToScroll, newScroll);

    if (newScroll != currentScroll) {
        SetControlValue(theControl, newScroll);
        // Calculate pixel scroll needed (opposite sign to scroll value change)
        short scrollDeltaPixels = (currentScroll - newScroll) * lineHeight;
        ScrollMessagesTE(scrollDeltaPixels);
    } else {
         log_to_file_only("HandleMessagesScrollClick: newScroll == currentScroll, no action needed.");
    }
}

void HandleMessagesTEUpdate(DialogPtr dialog) {
    if (gMessagesTE != NULL) {
        Rect itemRect;
        DialogItemType itemTypeIgnored;
        Handle itemHandleIgnored;
        GrafPtr oldPort;

        GetPort(&oldPort);
        SetPort(GetWindowPort(dialog)); // Ensure correct port

        // Get the item's rectangle in case it moved (though unlikely for userItem)
        GetDialogItem(dialog, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // TEUpdate redraws the text within the intersection of itemRect and the port's visRgn
        TEUpdate(&itemRect, gMessagesTE);

        SetPort(oldPort);
    }
}


void ActivateMessagesTEAndScrollbar(Boolean activating) {
    if (gMessagesTE != NULL) {
        // TextEdit only needs explicit deactivation
        if (!activating) {
            TEDeactivate(gMessagesTE);
        }
        // TEActivate is handled by the main dialog logic when focus changes
    }
    if (gMessagesScrollBar != NULL) {
        // Hilite state 0 = active, 255 = inactive
        HiliteControl(gMessagesScrollBar, activating ? 0 : 255);
    }
}

void ScrollMessagesTE(short deltaPixels) {
     if (gMessagesTE != NULL && deltaPixels != 0) {
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(gMainWindow)); // Ensure correct port

        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            Rect viewRectToInvalidate = (**gMessagesTE).viewRect;
            log_to_file_only("ScrollMessagesTE: Scrolling TE by %d pixels.", deltaPixels);
            TEScroll(0, deltaPixels, gMessagesTE); // Horizontal scroll is 0
            InvalRect(&viewRectToInvalidate); // Invalidate the view area to force redraw
            log_to_file_only("ScrollMessagesTE: Invalidated TE viewRect.");
        } else {
             log_to_file_only("ScrollMessagesTE Error: gMessagesTE dereference failed before TEScroll!");
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
     }
}