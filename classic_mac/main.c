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
#include "discovery.h"
#include "dialog.h"
#include "peer_mac.h"
Boolean gDone = false;
unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);
int main(void) {
    OSErr networkErr;
    Boolean dialogOk;
    InitLogFile();
    log_message("Starting application (Async Read Poll / Sync Write)...");
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
    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (%d). Exiting.", networkErr);
        CleanupNetworking();
        CloseLogFile();
        return 1;
    }
    InitPeerList();
    log_message("Peer list initialized.");
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
    CleanupDialog();
    CleanupNetworking();
    CloseLogFile();
    return 0;
}
void InitializeToolbox(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}
void MainEventLoop(void) {
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 5L;
    while (!gDone) {
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        if (gotEvent) {
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        HandleDialogClick(whichDialog, itemHit, &event);
                    }
                }
            }
            HandleEvent(&event);
        } else {
            HandleIdleTasks();
        }
    }
}
void HandleIdleTasks(void) {
    OSErr readResult;
    OSErr bfrReturnResult;
    unsigned long currentTimeTicks = TickCount();
    if (gUDPReadPending) {
        readResult = gUDPReadPB.ioResult;
        if (readResult != 1) {
            gUDPReadPending = false;
            if (readResult == noErr) {
                ProcessUDPReceive(gMacTCPRefNum, gMyLocalIP);
            } else {
                log_message("Error (Idle): Polled async udpRead completed with error: %d", readResult);
                if (!gUDPBfrReturnPending) {
                    StartAsyncUDPRead();
                }
            }
        }
    }
    if (gUDPBfrReturnPending) {
        bfrReturnResult = gUDPBfrReturnPB.ioResult;
        if (bfrReturnResult != 1) {
            gUDPBfrReturnPending = false;
            if (bfrReturnResult != noErr) {
                log_message("CRITICAL Error (Idle): Polled async udpBfrReturn completed with error: %d.", bfrReturnResult);
            } else {
            }
            if (!gUDPReadPending) {
                StartAsyncUDPRead();
            }
        }
    }
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (!gUDPReadPending && !gUDPBfrReturnPending) {
        StartAsyncUDPRead();
    }
    if (currentTimeTicks < gLastPeerListUpdateTime) {
         gLastPeerListUpdateTime = currentTimeTicks;
    }
    if ((currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false);
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}
void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
    ControlHandle foundControl;
    short foundControlPart;
    GrafPtr oldPort;
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
                     if (whichWindow == (WindowPtr)gMainWindow) {
                         if (whichWindow != FrontWindow()) {
                             SelectWindow(whichWindow);
                         } else {
                             Point localPt = event->where;
                             GetPort(&oldPort);
                             SetPort(GetWindowPort(gMainWindow));
                             GlobalToLocal(&localPt);
                             log_to_file_only("HandleEvent: MouseDown inContent at local (%d, %d)", localPt.v, localPt.h);
                             foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                             log_to_file_only("HandleEvent: FindControl result: part=%d, control=0x%lX", foundControlPart, (unsigned long)foundControl);
                             if (foundControl == gMessagesScrollBar && foundControlPart != 0) {
                                 log_to_file_only("HandleEvent: Click identified in gMessagesScrollBar (part %d).", foundControlPart);
                                 short hiliteState = (**foundControl).contrlHilite;
                                 log_to_file_only("HandleEvent: Scrollbar hilite state = %d (0=active, 255=inactive)", hiliteState);
                                 if (hiliteState == 0) {
                                     if (foundControlPart == 129 ) {
                                         log_to_file_only("HandleEvent: Tracking scrollbar thumb...");
                                         short oldValue = GetControlValue(foundControl);
                                         foundControlPart = TrackControl(foundControl, localPt, nil);
                                         log_to_file_only("HandleEvent: TrackControl(thumb) returned part %d", foundControlPart);
                                         short newValue = GetControlValue(foundControl);
                                         log_to_file_only("HandleEvent: Thumb tracking finished. OldVal=%d, NewVal=%d", oldValue, newValue);
                                         if (newValue != oldValue && gMessagesTE != NULL) {
                                             SignedByte teState = HGetState((Handle)gMessagesTE);
                                             HLock((Handle)gMessagesTE);
                                             if (*gMessagesTE != NULL) {
                                                 short lineHeight = (**gMessagesTE).lineHeight;
                                                 Rect viewRectToInvalidate = (**gMessagesTE).viewRect;
                                                 if (lineHeight > 0) {
                                                     short scrollDeltaPixels = (oldValue - newValue) * lineHeight;
                                                     log_to_file_only("HandleEvent: Scrolling TE by %d pixels due to thumb drag.", scrollDeltaPixels);
                                                     TEScroll(0, scrollDeltaPixels, gMessagesTE);
                                                     InvalRect(&viewRectToInvalidate);
                                                     log_to_file_only("HandleEvent: Invalidated TE viewRect after thumb drag.");
                                                 } else {
                                                      log_to_file_only("HandleEvent: Thumb drag - lineHeight is 0!");
                                                 }
                                             } else {
                                                 log_to_file_only("HandleEvent: Thumb drag - gMessagesTE deref failed!");
                                             }
                                             HSetState((Handle)gMessagesTE, teState);
                                         } else if (newValue == oldValue) {
                                             log_to_file_only("HandleEvent: Thumb tracking finished, value didn't change.");
                                         }
                                     } else {
                                         log_to_file_only("HandleEvent: Tracking scrollbar part %d with action proc at 0x%lX...", foundControlPart, (unsigned long)&MyScrollAction);
                                         foundControlPart = TrackControl(foundControl, localPt, &MyScrollAction);
                                         log_to_file_only("HandleEvent: TrackControl with action proc finished (returned %d).", foundControlPart);
                                     }
                                 } else {
                                     log_to_file_only("HandleEvent: Scrollbar clicked but inactive (hilite=%d).", hiliteState);
                                 }
                             } else {
                                 log_to_file_only("HandleEvent: Click not in scrollbar or FindControl returned 0.");
                             }
                             SetPort(oldPort);
                         }
                     }
                    break;
                default:
                    break;
            }
            break;
        case keyDown:
        case autoKey:
            break;
        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow);
                DrawDialog(whichWindow);
                UpdateDialogControls();
                EndUpdate(whichWindow);
            }
            break;
        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                log_message("HandleEvent: ActivateEvt for gMainWindow. Modifiers: %d (activeFlag=%d)", event->modifiers, activeFlag);
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            break;
        default:
            break;
    }
}
