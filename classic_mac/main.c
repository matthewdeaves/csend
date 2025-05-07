#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Devices.h>
#include <Lists.h>
#include <Controls.h>
#include <stdlib.h>
#include "logging.h"
#include "network.h"
#include "dialog.h"
#include "peer_mac.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "tcp.h"
#include "discovery.h"
#include "../shared/logging_shared.h"
#include <Sound.h>
#ifndef inThumb
#define inThumb 129
#endif
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
Boolean gDone = false;
unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);
int main(void)
{
    OSErr networkErr;
    Boolean dialogOk;
    InitLogFile();
    log_message("Starting Classic Mac P2P Messenger...");
    MaxApplZone();
    log_message("MaxApplZone called.");
    InitializeToolbox();
    log_message("Toolbox Initialized.");
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        log_message("Fatal: Network initialization failed (%d). Exiting.", networkErr);
        CloseLogFile();
        return 1;
    }
    InitPeerList();
    log_message("Peer list data structure initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_message("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking();
        CloseLogFile();
        return 1;
    }
    log_message("Entering main event loop...");
    MainEventLoop();
    log_message("Exited main event loop.");
    log_message("Initiating shutdown sequence...");
    OSErr quitErr = TCP_SendQuitMessagesSync(YieldTimeToSystem);
    if (quitErr == streamBusyErr) {
        log_message("Warning: TCP_SendQuitMessagesSync failed because stream was busy.");
    } else if (quitErr != noErr) {
        log_message("Warning: TCP_SendQuitMessagesSync reported last error: %d.", quitErr);
    } else {
        log_message("Finished sending shutdown notifications (or no peers).");
    }
    CleanupDialog();
    CleanupNetworking();
    CloseLogFile();
    log_message("Application terminated gracefully.");
    return 0;
}
void InitializeToolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}
void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L;
    while (!gDone) {
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);
        HandleIdleTasks();
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        if (gotEvent) {
            Boolean eventHandledByApp = false;
            if (event.what == mouseDown) {
                WindowPtr whichWindow;
                short windowPart = FindWindow(event.where, &whichWindow);
                if (whichWindow == (WindowPtr)gMainWindow && windowPart == inContent) {
                    Point localPt = event.where;
                    ControlHandle foundControl;
                    short foundControlPart;
                    Rect inputTERectDITL;
                    DialogItemType itemTypeDITL;
                    Handle itemHandleDITL;
                    GrafPtr oldPort;
                    GetPort(&oldPort);
                    SetPort(GetWindowPort(gMainWindow));
                    GlobalToLocal(&localPt);
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                        (**foundControl).contrlVis && (**foundControl).contrlHilite == 0 ) {
                        log_to_file_only("MouseDown: Handling click in Messages Scrollbar (part %d).", foundControlPart);
                        if (foundControlPart == inThumb) {
                            short oldValue = GetControlValue(foundControl);
                            TrackControl(foundControl, localPt, nil);
                            short newValue = GetControlValue(foundControl);
                            log_to_file_only("MouseDown: Scrollbar thumb drag finished. OldVal=%d, NewVal=%d", oldValue, newValue);
                            if (newValue != oldValue && gMessagesTE != NULL) {
                                SignedByte teState = HGetState((Handle)gMessagesTE);
                                HLock((Handle)gMessagesTE);
                                if (*gMessagesTE != NULL) {
                                    short lineHeight = (**gMessagesTE).lineHeight;
                                    if (lineHeight > 0) {
                                        short currentActualTopLine = -(**gMessagesTE).destRect.top / lineHeight;
                                        short scrollDeltaPixels = (currentActualTopLine - newValue) * lineHeight;
                                        log_to_file_only("MouseDown: Scrolling TE after thumb drag by %d pixels.", scrollDeltaPixels);
                                        ScrollMessagesTE(scrollDeltaPixels);
                                    } else {
                                        log_message("MouseDown WARNING: Scrollbar thumb drag, but lineHeight is 0.");
                                    }
                                } else {
                                    log_message("MouseDown ERROR: Scrollbar thumb drag, but gMessagesTE deref failed!");
                                }
                                HSetState((Handle)gMessagesTE, teState);
                            }
                        } else {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandledByApp = true;
                    }
                    else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    }
                    else {
                         GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                         if (gInputTE != NULL && PtInRect(localPt, &(**gInputTE).viewRect)) {
                            TEClick(localPt, (event.modifiers & shiftKey) != 0, gInputTE);
                            eventHandledByApp = true;
                         }
                    }
                    SetPort(oldPort);
                }
            }
            if (!eventHandledByApp) {
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog;
                    short itemHit;
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        if (whichDialog == gMainWindow && itemHit > 0) {
                            ControlHandle controlH;
                            DialogItemType itemType;
                            Handle itemHandle;
                            Rect itemRect;
                            GrafPtr oldPortForDrawing;
                            short currentValue;
                            switch (itemHit) {
                                case kSendButton:
                                    HandleSendButtonClick();
                                    break;
                                case kDebugCheckbox:
                                    GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
                                    if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                        controlH = (ControlHandle)itemHandle;
                                        currentValue = GetControlValue(controlH);
                                        SetControlValue(controlH, !currentValue);
                                        Boolean newState = (GetControlValue(controlH) == 1);
                                        set_debug_output_enabled(newState);
                                        log_message("Debug output %s.", newState ? "ENABLED" : "DISABLED");
                                        GetPort(&oldPortForDrawing);
                                        SetPort(GetWindowPort(gMainWindow));
                                        InvalRect(&itemRect);
                                        SetPort(oldPortForDrawing);
                                    }
                                    break;
                                case kBroadcastCheckbox:
                                    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                                    if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                        controlH = (ControlHandle)itemHandle;
                                        currentValue = GetControlValue(controlH);
                                        SetControlValue(controlH, !currentValue);
                                        short newCheckboxValue = GetControlValue(controlH);
                                        if (newCheckboxValue == 1) {
                                            log_message("Broadcast checkbox checked. Deselecting peer if any.");
                                            if (gPeerListHandle != NULL && gLastSelectedCell.v >= 0) {
                                                GrafPtr oldPortForList;
                                                GetPort(&oldPortForList);
                                                SetPort(GetWindowPort(gMainWindow));
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
                                        } else {
                                            log_message("Broadcast checkbox unchecked.");
                                        }
                                        GetPort(&oldPortForDrawing);
                                        SetPort(GetWindowPort(gMainWindow));
                                        InvalRect(&itemRect);
                                        SetPort(oldPortForDrawing);
                                    }
                                    break;
                                case kMessagesScrollbar:
                                    log_message("WARNING: DialogSelect returned item kMessagesScrollbar - This should ideally be handled in mouseDown.");
                                    if (gMessagesTE != NULL && gMessagesScrollBar != NULL) {
                                        short currentScrollVal = GetControlValue(gMessagesScrollBar);
                                        short currentTopLine, desiredTopPixel, currentTopPixel, scrollDeltaPixels = 0, lineHeight = 0;
                                        GrafPtr scrollPort;
                                        GetPort(&scrollPort);
                                        SetPort(GetWindowPort(gMainWindow));
                                        SignedByte teState = HGetState((Handle)gMessagesTE);
                                        HLock((Handle)gMessagesTE);
                                        if (gMessagesTE == NULL || *gMessagesTE == NULL) {
                                             log_message("ERROR: (DialogSelect Scrollbar Case) gMessagesTE became invalid!");
                                             if (gMessagesTE != NULL) HSetState((Handle)gMessagesTE, teState);
                                             SetPort(scrollPort);
                                             break;
                                        }
                                        lineHeight = (**gMessagesTE).lineHeight;
                                        if (lineHeight > 0) {
                                            currentTopPixel = (**gMessagesTE).destRect.top;
                                            desiredTopPixel = -currentScrollVal * lineHeight;
                                            scrollDeltaPixels = desiredTopPixel - currentTopPixel;
                                            if (scrollDeltaPixels != 0) ScrollMessagesTE(scrollDeltaPixels);
                                        }
                                        HSetState((Handle)gMessagesTE, teState);
                                        SetPort(scrollPort);
                                    }
                                    break;
                                default:
                                     log_to_file_only("DialogSelect returned unhandled item: %d", itemHit);
                                    break;
                            }
                        }
                        eventHandledByApp = true;
                    }
                }
            }
            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        } else {
             HandleIdleTasks();
        }
    }
}
void HandleIdleTasks(void)
{
    unsigned long currentTimeTicks = TickCount();
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    PollTCP(YieldTimeToSystem);
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (gLastPeerListUpdateTime == 0 ||
        (currentTimeTicks < gLastPeerListUpdateTime) ||
        (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks)
    {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false);
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}
void HandleEvent(EventRecord *event)
{
    short windowPart;
    WindowPtr whichWindow;
    char theChar;
    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    break;
                case inSysWindow:
                    SystemClick(event, whichWindow);
                    break;
                case inDrag:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (TrackGoAway(whichWindow, event->where)) {
                            log_message("Close box clicked. Setting gDone = true.");
                            gDone = true;
                        }
                    }
                    break;
                case inContent:
                     if (whichWindow != FrontWindow()) {
                        SelectWindow(whichWindow);
                     } else {
                         log_to_file_only("HandleEvent: mouseDown in content fell through all handlers.");
                     }
                    break;
                default:
                     log_to_file_only("HandleEvent: mouseDown in unknown window part: %d", windowPart);
                    break;
            }
            break;
        case keyDown:
        case autoKey:
            theChar = event->message & charCodeMask;
            if ((event->modifiers & cmdKey) != 0) {
            } else {
                if (gInputTE != NULL && gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow) {
                    TEKey(theChar, gInputTE);
                }
            }
            break;
        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            BeginUpdate(whichWindow);
            if (whichWindow == (WindowPtr)gMainWindow) {
                DrawDialog(whichWindow);
                UpdateDialogControls();
            } else {
            }
            EndUpdate(whichWindow);
            break;
        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
                ActivateDialogTE(becomingActive);
                ActivatePeerList(becomingActive);
                if (gMessagesScrollBar != NULL) {
                     short maxScroll = GetControlMaximum(gMessagesScrollBar);
                     short hiliteValue = 255;
                     if (becomingActive && maxScroll > 0 && (**gMessagesScrollBar).contrlVis) {
                         hiliteValue = 0;
                     }
                     HiliteControl(gMessagesScrollBar, hiliteValue);
                }
            }
            break;
        default:
            break;
    }
}
