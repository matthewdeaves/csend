#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Devices.h>
#include <stdlib.h>
#include "logging.h"
#include "network.h"
#include "discovery.h"
#include "dialog.h"
#include "peer_mac.h"
Boolean gDone = false;
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
    if (networkErr != noErr) { return 1; }
    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) { return 1; }
    InitPeerList();
    log_message("Peer list initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) { return 1; }
    log_message("Entering main event loop...");
    MainEventLoop();
    log_message("Exited main event loop.");
    CleanupDialog();
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
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
    long sleepTime = 1L;
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
                        HandleDialogClick(whichDialog, itemHit);
                    }
                }
            } else {
                HandleEvent(&event);
            }
        } else {
            HandleIdleTasks();
        }
    }
}
void HandleIdleTasks(void) {
    OSErr err;
    OSErr readResult;
    OSErr bfrReturnResult;
    if (gUDPReadPending) {
        readResult = gUDPReadPB.ioResult;
        if (readResult != 1) {
            gUDPReadPending = false;
            if (readResult == noErr) {
                ProcessUDPReceive(gMacTCPRefNum, gMyLocalIP);
            } else {
                log_message("Error (Idle): Polled async udpRead completed with error: %d", readResult);
                if (!gUDPReadPending && !gUDPBfrReturnPending) {
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
}
void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar: break;
                case inSysWindow: SystemClick(event, whichWindow); break;
                case inDrag:
                    if (whichWindow == (WindowPtr)gMainWindow) DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
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
                        if (whichWindow != FrontWindow()) SelectWindow(whichWindow);
                    }
                    break;
                default: break;
            }
            break;
        case keyDown: case autoKey: break;
        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow);
                DrawDialog(whichWindow);
                UpdateDialogTE();
                EndUpdate(whichWindow);
            }
            break;
        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            break;
        default: break;
    }
}
