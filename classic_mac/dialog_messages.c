#include "dialog_messages.h"
#include "dialog.h"
#include "logging.h"
#include <MacTypes.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>
#include <string.h>
#ifndef inUpButton
#define inUpButton 20
#endif
#ifndef inDownButton
#define inDownButton 21
#endif
#ifndef inPageUp
#define inPageUp 22
#endif
#ifndef inPageDown
#define inPageDown 23
#endif
#ifndef inThumb
#define inThumb 129
#endif
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
        viewRectMessages.right -= 16;
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
        if (itemHandle != NULL) {
             gMessagesScrollBar = (ControlHandle)itemHandle;
             log_message("Scrollbar handle obtained: 0x%lX (ItemType was %d).", (unsigned long)gMessagesScrollBar, itemType);
             SetControlMinimum(gMessagesScrollBar, 0);
             SetControlMaximum(gMessagesScrollBar, 0);
             SetControlValue(gMessagesScrollBar, 0);
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
    gMessagesScrollBar = NULL;
    log_message("Messages TE cleanup finished.");
}
void AppendToMessagesTE(const char *text) {
    GrafPtr oldPort;
    if (gMessagesTE == NULL) {
        log_to_file_only("Skipping AppendToMessagesTE: gMessagesTE is NULL.");
        return;
    }
    GetPort(&oldPort);
    SetPort(GetWindowPort(gMainWindow));
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        long textLen = strlen(text);
        if (currentLength + textLen < 32000) {
            TESetSelect(currentLength, currentLength, gMessagesTE);
            TEInsert((Ptr)text, textLen, gMessagesTE);
            AdjustMessagesScrollbar();
            short maxScroll = GetControlMaximum(gMessagesScrollBar);
            if (GetControlValue(gMessagesScrollBar) != maxScroll) {
                 short currentScrollVal = GetControlValue(gMessagesScrollBar);
                 SetControlValue(gMessagesScrollBar, maxScroll);
                 short lineHeight = (**gMessagesTE).lineHeight;
                 if (lineHeight > 0) {
                     ScrollMessagesTE((currentScrollVal - maxScroll) * lineHeight);
                 }
            }
        } else {
             log_message("Warning: Messages TE field near full. Cannot append.");
        }
    } else {
        log_message("ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!");
    }
    HSetState((Handle)gMessagesTE, teState);
    SetPort(oldPort);
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
        short firstVisibleLine = 0;
        if (lineHeight > 0) {
            linesInView = ((**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top) / lineHeight;
            firstVisibleLine = -(**gMessagesTE).destRect.top / lineHeight;
        } else {
            linesInView = 1;
            totalLines = 0;
            firstVisibleLine = 0;
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
             if (firstVisibleLine > maxScroll) {
                 log_to_file_only("AdjustScrollbar: Clamping scroll value from %d to %d.", firstVisibleLine, maxScroll);
                 short scrollDeltaPixels = (firstVisibleLine - maxScroll) * lineHeight;
                 ScrollMessagesTE(scrollDeltaPixels);
                 firstVisibleLine = maxScroll;
             }
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        } else if (GetControlValue(gMessagesScrollBar) != firstVisibleLine) {
             log_to_file_only("AdjustMessagesScrollbar: Setting Value to %d (max unchanged)", firstVisibleLine);
             SetControlValue(gMessagesScrollBar, firstVisibleLine);
        }
        Boolean shouldBeVisible = (maxScroll > 0);
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
    log_to_file_only("HandleMessagesScrollClick called: PartCode=%d", partCode);
    if (gMessagesTE == NULL || partCode == 0) {
        log_to_file_only("HandleMessagesScrollClick: Ignoring (TE NULL or partCode 0).");
        return;
    }
    short linesToScroll = 0;
    short currentScroll, maxScroll;
    short lineHeight = 0;
    short pageScroll = 1;
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        lineHeight = (**gMessagesTE).lineHeight;
        if (lineHeight > 0) {
            Rect viewRect = (**gMessagesTE).viewRect;
            pageScroll = (viewRect.bottom - viewRect.top) / lineHeight - 1;
            if (pageScroll < 1) pageScroll = 1;
        } else {
            log_to_file_only("HandleMessagesScrollClick Warning: lineHeight is %d!", lineHeight);
            HSetState((Handle)gMessagesTE, teState);
            return;
        }
    } else {
         log_to_file_only("HandleMessagesScrollClick Error: gMessagesTE dereference failed!");
         HSetState((Handle)gMessagesTE, teState);
         return;
    }
    HSetState((Handle)gMessagesTE, teState);
    currentScroll = GetControlValue(theControl);
    maxScroll = GetControlMaximum(theControl);
    log_to_file_only("HandleMessagesScrollClick: currentScroll=%d, maxScroll=%d, lineHeight=%d, pageScroll=%d",
                currentScroll, maxScroll, lineHeight, pageScroll);
    switch (partCode) {
        case inUpButton:
        case inPageUp:
            linesToScroll = (partCode == inUpButton) ? -1 : -pageScroll;
            break;
        case inDownButton:
        case inPageDown:
            linesToScroll = (partCode == inDownButton) ? 1 : pageScroll;
            break;
        default:
            log_to_file_only("HandleMessagesScrollClick: Ignoring unknown partCode %d", partCode);
            return;
    }
    short newScroll = currentScroll + linesToScroll;
    if (newScroll < 0) newScroll = 0;
    if (newScroll > maxScroll) newScroll = maxScroll;
    log_to_file_only("HandleMessagesScrollClick: linesToScroll=%d, newScroll=%d (clamped)", linesToScroll, newScroll);
    if (newScroll != currentScroll) {
        SetControlValue(theControl, newScroll);
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
        SetPort(GetWindowPort(dialog));
        GetDialogItem(dialog, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gMessagesTE);
        SetPort(oldPort);
    }
}
void ActivateMessagesTEAndScrollbar(Boolean activating) {
    if (gMessagesTE != NULL) {
        if (!activating) {
            TEDeactivate(gMessagesTE);
        }
    }
    if (gMessagesScrollBar != NULL) {
        HiliteControl(gMessagesScrollBar, activating ? 0 : 255);
    }
}
void ScrollMessagesTE(short deltaPixels) {
     if (gMessagesTE != NULL && deltaPixels != 0) {
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort(GetWindowPort(gMainWindow));
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            Rect viewRectToInvalidate = (**gMessagesTE).viewRect;
            log_to_file_only("ScrollMessagesTE: Scrolling TE by %d pixels.", deltaPixels);
            TEScroll(0, deltaPixels, gMessagesTE);
            InvalRect(&viewRectToInvalidate);
            log_to_file_only("ScrollMessagesTE: Invalidated TE viewRect.");
        } else {
             log_to_file_only("ScrollMessagesTE Error: gMessagesTE dereference failed before TEScroll!");
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
     }
}
