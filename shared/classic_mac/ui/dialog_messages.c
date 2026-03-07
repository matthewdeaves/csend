#include "dialog_messages.h"
#include "dialog.h"
#include "clog.h"
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
TEHandle gMessagesTE = NULL;
ControlHandle gMessagesScrollBar = NULL;
pascal void MyScrollAction(ControlHandle theControl, short partCode)
{
    if (theControl == gMessagesScrollBar) {
        if (partCode != 0 && partCode != kControlIndicatorPart) {
            HandleMessagesScrollClick(theControl, partCode);
        } else if (partCode == kControlIndicatorPart) {
            CLOG_DEBUG("MyScrollAction: WARNING - Called with inThumb (part %d) for control 0x%lX. Main.c should handle this.",
                          partCode, (unsigned long)theControl);
        }
    } else {
        CLOG_DEBUG("MyScrollAction: Called for unexpected control 0x%lX, part %d",
                      (unsigned long)theControl, partCode);
    }
}
Boolean InitMessagesTEAndScrollbar(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect destRectMessages, viewRectMessages;
    Rect scrollBarRect;
    Boolean teOk = false;
    Boolean scrollBarOk = false;
    CLOG_DEBUG("Initializing Messages TE...");
    GetDialogItem(dialog, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    if (itemType == userItem) {
        viewRectMessages = destRectMessages;
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            CLOG_DEBUG("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            teOk = false;
        } else {
            CLOG_DEBUG("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(false, gMessagesTE);
            teOk = true;
        }
    } else {
        CLOG_DEBUG("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        teOk = false;
    }
    if (teOk) {
        CLOG_DEBUG("Initializing Messages Scrollbar...");
        GetDialogItem(dialog, kMessagesScrollbar, &itemType, &itemHandle, &scrollBarRect);
        if (itemHandle != NULL) {
            gMessagesScrollBar = (ControlHandle)itemHandle;
            CLOG_DEBUG("Scrollbar handle obtained: 0x%lX (ItemType was %d).", (unsigned long)gMessagesScrollBar, itemType);
            SetControlMinimum(gMessagesScrollBar, 0);
            SetControlMaximum(gMessagesScrollBar, 0);
            SetControlValue(gMessagesScrollBar, 0);
            HideControl(gMessagesScrollBar);
            HiliteControl(gMessagesScrollBar, 255);
            scrollBarOk = true;
        } else {
            CLOG_DEBUG("ERROR: Item %d (Messages Scrollbar) handle is NULL! Check DITL resource.", kMessagesScrollbar);
            gMessagesScrollBar = NULL;
            scrollBarOk = false;
            TEDispose(gMessagesTE);
            gMessagesTE = NULL;
            teOk = false;
        }
    }
    return (teOk && scrollBarOk);
}
void CleanupMessagesTEAndScrollbar(void)
{
    CLOG_DEBUG("Cleaning up Messages TE...");
    if (gMessagesTE != NULL) {
        TEDispose(gMessagesTE);
        gMessagesTE = NULL;
    }
    gMessagesScrollBar = NULL;
    CLOG_DEBUG("Messages TE cleanup finished.");
}
void AppendToMessagesTE(const char *text)
{
    GrafPtr oldPort;
    Boolean scrolledToBottom = false;
    if (gMessagesTE == NULL) {
        CLOG_DEBUG("AppendToMessagesTE: gMessagesTE is NULL. Cannot append.");
        return;
    }
    if (text == NULL || text[0] == '\0') {
        return;
    }
    GetPort(&oldPort);
    if (gMainWindow != NULL) {
        SetPort((GrafPtr)GetWindowPort(gMainWindow));
    } else {
        CLOG_DEBUG("AppendToMessagesTE Warning: gMainWindow is NULL! Port not set.");
    }
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE != NULL) {
        long currentLength = (**gMessagesTE).teLength;
        long textLen = strlen(text);
        if (gMessagesScrollBar != NULL) {
            scrolledToBottom = (GetControlValue(gMessagesScrollBar) >= GetControlMaximum(gMessagesScrollBar));
        }
        if (currentLength + textLen < 30000) {
            TESetSelect(currentLength, currentLength, gMessagesTE);
            TEInsert((Ptr)text, textLen, gMessagesTE);
            AdjustMessagesScrollbar();
            if (scrolledToBottom && gMessagesScrollBar != NULL) {
                short newMaxScroll = GetControlMaximum(gMessagesScrollBar);
                SetControlValue(gMessagesScrollBar, newMaxScroll);
                ScrollMessagesTEToValue(newMaxScroll);
            }
        } else {
            CLOG_DEBUG("Warning: Messages TE field near full. Cannot append.");
        }
    } else {
        CLOG_DEBUG("ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!");
    }
    HSetState((Handle)gMessagesTE, teState);
    SetPort(oldPort);
}
void AdjustMessagesScrollbar(void)
{
    if (gMessagesTE == NULL || gMessagesScrollBar == NULL) {
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
        short currentVal = 0;
        if (lineHeight > 0) {
            linesInView = viewHeight / lineHeight;
            if (linesInView < 1) linesInView = 1;
            currentVal = -(**gMessagesTE).destRect.top / lineHeight;
        } else {
            linesInView = 1;
            totalLines = 0;
            currentVal = 0;
            CLOG_DEBUG("AdjustMessagesScrollbar Warning: lineHeight is %d!", lineHeight);
        }
        maxScroll = totalLines - linesInView;
        if (maxScroll < 0) maxScroll = 0;
        SetControlMaximum(gMessagesScrollBar, maxScroll);
        if (currentVal > maxScroll) {
            currentVal = maxScroll;
        }
        if (currentVal < 0) currentVal = 0;
        SetControlValue(gMessagesScrollBar, currentVal);
        Boolean shouldBeVisible = (maxScroll > 0);
        Boolean isVisible = ((**gMessagesScrollBar).contrlVis != 0);
        Boolean windowIsActive = (gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow);
        short hiliteValue = 255;
        if (shouldBeVisible) {
            if (!isVisible) ShowControl(gMessagesScrollBar);
            if (windowIsActive) hiliteValue = 0;
        } else {
            if (isVisible) HideControl(gMessagesScrollBar);
        }
        HiliteControl(gMessagesScrollBar, hiliteValue);
    } else {
        CLOG_DEBUG("AdjustMessagesScrollbar Error: gMessagesTE deref failed!");
    }
    HSetState((Handle)gMessagesTE, teState);
}
void HandleMessagesScrollClick(ControlHandle theControl, short partCode)
{
    if (gMessagesTE == NULL || partCode == 0 || partCode == kControlIndicatorPart) {
        if (partCode == kControlIndicatorPart) {
            CLOG_DEBUG("HandleMessagesScrollClick: Received inThumb (part %d), ignoring as main.c handles it.", partCode);
        }
        return;
    }
    short linesToScroll = 0;
    short currentScroll, maxScroll, newScroll;
    short lineHeight = 0;
    short viewHeight = 0;
    short pageScroll = 1;
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE == NULL) {
        CLOG_DEBUG("HandleMessagesScrollClick Error: gMessagesTE dereference failed!");
        HSetState((Handle)gMessagesTE, teState);
        return;
    }
    lineHeight = (**gMessagesTE).lineHeight;
    viewHeight = (**gMessagesTE).viewRect.bottom - (**gMessagesTE).viewRect.top;
    HSetState((Handle)gMessagesTE, teState);
    if (lineHeight <= 0) {
        CLOG_DEBUG("HandleMessagesScrollClick Warning: lineHeight is %d! Cannot scroll.", lineHeight);
        return;
    }
    pageScroll = viewHeight / lineHeight;
    if (pageScroll > 1) pageScroll -= 1;
    if (pageScroll < 1) pageScroll = 1;
    currentScroll = GetControlValue(theControl);
    maxScroll = GetControlMaximum(theControl);
    switch (partCode) {
    case inUpButton:
        linesToScroll = -1;
        break;
    case inDownButton:
        linesToScroll = 1;
        break;
    case inPageUp:
        linesToScroll = -pageScroll;
        break;
    case inPageDown:
        linesToScroll = pageScroll;
        break;
    default:
        CLOG_DEBUG("HandleMessagesScrollClick: Ignoring unknown partCode %d", partCode);
        return;
    }
    newScroll = currentScroll + linesToScroll;
    if (newScroll < 0) newScroll = 0;
    if (newScroll > maxScroll) newScroll = maxScroll;
    if (newScroll != currentScroll) {
        short scrollDeltaPixels = (currentScroll - newScroll) * lineHeight;
        SetControlValue(theControl, newScroll);
        ScrollMessagesTE(scrollDeltaPixels);
    }
}
void ScrollMessagesTEToValue(short newScrollValue)
{
    if (gMessagesTE == NULL) return;
    SignedByte teState = HGetState((Handle)gMessagesTE);
    HLock((Handle)gMessagesTE);
    if (*gMessagesTE == NULL) {
        CLOG_DEBUG("ScrollMessagesTEToValue Error: gMessagesTE deref failed!");
        HSetState((Handle)gMessagesTE, teState);
        return;
    }
    short lineHeight = (**gMessagesTE).lineHeight;
    if (lineHeight <= 0) {
        CLOG_DEBUG("ScrollMessagesTEToValue Warning: lineHeight is %d! Cannot scroll.", lineHeight);
        HSetState((Handle)gMessagesTE, teState);
        return;
    }
    short currentActualTopLine = -(**gMessagesTE).destRect.top / lineHeight;
    short scrollDeltaPixels = (currentActualTopLine - newScrollValue) * lineHeight;
    HSetState((Handle)gMessagesTE, teState);
    if (scrollDeltaPixels != 0) {
        ScrollMessagesTE(scrollDeltaPixels);
    }
}
void HandleMessagesTEUpdate(DialogPtr dialog)
{
    if (gMessagesTE != NULL) {
        Rect itemRect;
        DialogItemType itemTypeIgnored;
        Handle itemHandleIgnored;
        GrafPtr oldPort;
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GetDialogItem(dialog, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            EraseRect(&(**gMessagesTE).viewRect);
            TEUpdate(&(**gMessagesTE).viewRect, gMessagesTE);
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
    }
}
void ActivateMessagesTEAndScrollbar(Boolean activating)
{
    if (gMessagesTE != NULL) {
        if (activating) {
        } else {
            TEDeactivate(gMessagesTE);
        }
    }
    if (gMessagesScrollBar != NULL) {
        short maxScroll = GetControlMaximum(gMessagesScrollBar);
        short hiliteValue = 255;
        if (activating && maxScroll > 0 && (**gMessagesScrollBar).contrlVis) {
            hiliteValue = 0;
        }
        HiliteControl(gMessagesScrollBar, hiliteValue);
    }
}
void ScrollMessagesTE(short deltaPixels)
{
    if (gMessagesTE != NULL && deltaPixels != 0) {
        GrafPtr oldPort;
        GetPort(&oldPort);
        if (gMainWindow != NULL) {
            SetPort((GrafPtr)GetWindowPort(gMainWindow));
        } else {
            CLOG_DEBUG("ScrollMessagesTE Warning: gMainWindow is NULL! Port not set.");
        }
        SignedByte teState = HGetState((Handle)gMessagesTE);
        HLock((Handle)gMessagesTE);
        if (*gMessagesTE != NULL) {
            Rect viewRectToInvalidate = (**gMessagesTE).viewRect;
            TEScroll(0, deltaPixels, gMessagesTE);
            InvalRect(&viewRectToInvalidate);
        } else {
            CLOG_DEBUG("ScrollMessagesTE Error: gMessagesTE dereference failed before TEScroll!");
        }
        HSetState((Handle)gMessagesTE, teState);
        SetPort(oldPort);
    }
}
