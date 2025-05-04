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
#ifndef scrollBarProc
#define scrollBarProc 16
#endif
TEHandle gMessagesTE = NULL;
ControlHandle gMessagesScrollBar = NULL;
pascal void MyScrollAction(ControlHandle theControl, short partCode) {
    log_to_file_only("MyScrollAction called: Control=0x%lX, Part=%d", (unsigned long)theControl, partCode);
    if (theControl == gMessagesScrollBar) {
         if (partCode != 0) {
             HandleMessagesScrollClick(theControl, partCode);
         }
    } else {
         log_to_file_only("MyScrollAction: Called for unexpected control 0x%lX", (unsigned long)theControl);
    }
}
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
        log_message("Calling TENew for Messages TE (Dest: T%d,L%d,B%d,R%d; View: T%d,L%d,B%d,R%d)",
                    destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right,
                    viewRectMessages.top, viewRectMessages.left, viewRectMessages.bottom, viewRectMessages.right);
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            teOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(false, gMessagesTE);
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
             HiliteControl(gMessagesScrollBar, 255);
             scrollBarOk = true;
        } else {
             log_message("ERROR: Item %d (Messages Scrollbar) handle is NULL! Check DITL resource.", kMessagesScrollbar);
             gMessagesScrollBar = NULL;
             scrollBarOk = false;
             TEDispose(gMessagesTE);
             gMessagesTE = NULL;
             teOk = false;
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
    Boolean scrolledToBottom = false;
    if (gMessagesTE == NULL) {
        log_to_file_only("Skipping AppendToMessagesTE: gMessagesTE is NULL.");
        return;
    }
    GetPort(&oldPort);
    if (gMainWindow != NULL) {
        SetPort(GetWindowPort(gMainWindow));
    } else {
        log_message("AppendToMessagesTE Warning: gMainWindow is NULL!");
    }
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        long textLen = strlen(text);
        short currentScrollVal = 0;
        short maxScroll = 0;
        if (gMessagesScrollBar != NULL) {
            currentScrollVal = GetControlValue(gMessagesScrollBar);
            maxScroll = GetControlMaximum(gMessagesScrollBar);
            scrolledToBottom = (currentScrollVal >= maxScroll);
             log_to_file_only("AppendToMessagesTE: Before insert - currentScroll=%d, maxScroll=%d, scrolledToBottom=%d",
                             currentScrollVal, maxScroll, scrolledToBottom);
        }
        if (currentLength + textLen < 30000) {
            TESetSelect(currentLength, currentLength, gMessagesTE);
            TEInsert((Ptr)text, textLen, gMessagesTE);
            AdjustMessagesScrollbar();
            if (scrolledToBottom && gMessagesScrollBar != NULL) {
                short newMaxScroll = GetControlMaximum(gMessagesScrollBar);
                 log_to_file_only("AppendToMessagesTE: After insert - newMaxScroll=%d", newMaxScroll);
                if (newMaxScroll > maxScroll || GetControlValue(gMessagesScrollBar) < newMaxScroll) {
                    short lineHeight = (**gMessagesTE).lineHeight;
                    if (lineHeight > 0) {
                        short currentTopLine = -(**gMessagesTE).destRect.top / lineHeight;
                        short scrollDeltaPixels = (currentTopLine - newMaxScroll) * lineHeight;
                        log_to_file_only("AppendToMessagesTE: Auto-scrolling to new bottom. DeltaPixels=%d", scrollDeltaPixels);
                        ScrollMessagesTE(scrollDeltaPixels);
                        SetControlValue(gMessagesScrollBar, newMaxScroll);
                    }
                }
            } else if (gMessagesScrollBar != NULL) {
                 log_to_file_only("AppendToMessagesTE: Not auto-scrolling (wasn't at bottom or scrollbar NULL).");
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
        short viewHeight = (**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top;
        short linesInView = 0;
        short totalLines = (**gMessagesTE).nLines;
        short maxScroll = 0;
        short firstVisibleLine = 0;
        if (lineHeight > 0) {
            linesInView = viewHeight / lineHeight;
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
        SetControlMaximum(gMessagesScrollBar, maxScroll);
        if (firstVisibleLine > maxScroll) {
            log_to_file_only("AdjustMessagesScrollbar: Clamping firstVisibleLine from %d to %d.", firstVisibleLine, maxScroll);
            short scrollDeltaPixels = (firstVisibleLine - maxScroll) * lineHeight;
            ScrollMessagesTE(scrollDeltaPixels);
            firstVisibleLine = maxScroll;
        }
         if (firstVisibleLine < 0) firstVisibleLine = 0;
        SetControlValue(gMessagesScrollBar, firstVisibleLine);
        log_to_file_only("AdjustMessagesScrollbar: Set Max=%d, Value=%d", maxScroll, firstVisibleLine);
        Boolean shouldBeVisible = (maxScroll > 0);
        Boolean isVisible = ((**gMessagesScrollBar).contrlVis != 0);
        Boolean windowIsActive = (gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow);
        short hiliteValue = 255;
        if (shouldBeVisible) {
            if (!isVisible) {
                log_to_file_only("AdjustMessagesScrollbar: Showing scrollbar.");
                ShowControl(gMessagesScrollBar);
            }
            if (windowIsActive) {
                hiliteValue = 0;
            }
        } else {
            if (isVisible) {
                log_to_file_only("AdjustMessagesScrollbar: Hiding scrollbar.");
                HideControl(gMessagesScrollBar);
            }
        }
        log_to_file_only("AdjustMessagesScrollbar: Setting scrollbar hilite to %d (shouldBeVisible=%d, windowIsActive=%d)",
                         hiliteValue, shouldBeVisible, windowIsActive);
        HiliteControl(gMessagesScrollBar, hiliteValue);
    } else {
        log_message("AdjustMessagesScrollbar Error: gMessagesTE deref failed!");
    }
    HSetState((Handle)gMessagesTE, teState);
}
void HandleMessagesScrollClick(ControlHandle theControl, short partCode) {
    log_to_file_only("HandleMessagesScrollClick called: PartCode=%d", partCode);
    if (gMessagesTE == NULL || partCode == 0 || partCode == inThumb) {
        log_to_file_only("HandleMessagesScrollClick: Ignoring (TE NULL or partCode 0 or inThumb).");
        return;
    }
    short linesToScroll = 0;
    short currentScroll, maxScroll;
    short lineHeight = 0;
    short viewHeight = 0;
    short pageScroll = 1;
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        lineHeight = (**gMessagesTE).lineHeight;
        viewHeight = (**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top;
        if (lineHeight > 0) {
            pageScroll = viewHeight / lineHeight;
             if (pageScroll > 1) pageScroll -= 1;
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
        case inUpButton: linesToScroll = -1; break;
        case inDownButton: linesToScroll = 1; break;
        case inPageUp: linesToScroll = -pageScroll;break;
        case inPageDown: linesToScroll = pageScroll; break;
        default:
            log_to_file_only("HandleMessagesScrollClick: Ignoring unknown partCode %d", partCode);
            return;
    }
    short newScroll = currentScroll + linesToScroll;
    if (newScroll < 0) newScroll = 0;
    if (newScroll > maxScroll) newScroll = maxScroll;
    log_to_file_only("HandleMessagesScrollClick: linesToScroll=%d, newScroll=%d (clamped)", linesToScroll, newScroll);
    if (newScroll != currentScroll) {
        short scrollDeltaPixels = (currentScroll - newScroll) * lineHeight;
        SetControlValue(theControl, newScroll);
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
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
             TEUpdate(&itemRect, gMessagesTE);
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
    }
}
void ActivateMessagesTEAndScrollbar(Boolean activating) {
    if (gMessagesTE != NULL) {
        if (activating) {
             log_to_file_only("ActivateMessagesTEAndScrollbar: Activating TE (No-op for read-only).");
        } else {
            TEDeactivate(gMessagesTE);
             log_to_file_only("ActivateMessagesTEAndScrollbar: Deactivating TE.");
        }
    }
    if (gMessagesScrollBar != NULL) {
        short maxScroll = GetControlMaximum(gMessagesScrollBar);
        short hiliteValue = 255;
        if (activating && maxScroll > 0) {
            hiliteValue = 0;
        }
        log_to_file_only("ActivateMessagesTEAndScrollbar: Setting scrollbar hilite to %d (activating=%d, maxScroll=%d)",
                         hiliteValue, activating, maxScroll);
        HiliteControl(gMessagesScrollBar, hiliteValue);
    }
}
void ScrollMessagesTE(short deltaPixels) {
     if (gMessagesTE != NULL && deltaPixels != 0) {
        GrafPtr oldPort;
        GetPort(&oldPort);
        if (gMainWindow != NULL) {
            SetPort(GetWindowPort(gMainWindow));
        } else {
             log_message("ScrollMessagesTE Warning: gMainWindow is NULL!");
        }
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            Rect viewRectToInvalidate = (**gMessagesTE).viewRect;
            log_to_file_only("ScrollMessagesTE: Scrolling TE content by %d pixels.", deltaPixels);
            TEScroll(0, deltaPixels, gMessagesTE);
            InvalRect(&viewRectToInvalidate);
            log_to_file_only("ScrollMessagesTE: Invalidated TE viewRect (T%d,L%d,B%d,R%d).",
                             viewRectToInvalidate.top, viewRectToInvalidate.left,
                             viewRectToInvalidate.bottom, viewRectToInvalidate.right);
        } else {
             log_to_file_only("ScrollMessagesTE Error: gMessagesTE dereference failed before TEScroll!");
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
     } else if (deltaPixels == 0) {
         log_to_file_only("ScrollMessagesTE: deltaPixels is 0, no scroll needed.");
     }
}
