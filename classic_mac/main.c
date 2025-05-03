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
int main(void) {
    OSErr networkErr;
    Boolean dialogOk;
    InitLogFile();
    log_message("Starting application...");
    MaxApplZone();
    log_message("MaxApplZone called.");
    InitializeToolbox();
    log_message("Toolbox Initialized.");
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        log_message("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
        CloseLogFile();
        ExitToShell();
        return 1;
    }
    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (Error: %d). Exiting.", networkErr);
        CleanupNetworking();
        CloseLogFile();
        ExitToShell();
        return 1;
    }
    InitPeerList();
    log_message("Peer list initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_message("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking();
        CloseLogFile();
        ExitToShell();
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
                        HandleDialogClick(whichDialog, itemHit);
                    }
                }
            } else {
                HandleEvent(&event);
            }
        } else {
            CheckUDPReceive(gMacTCPRefNum, gMyLocalIP);
            CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
            PruneTimedOutPeers();
        }
    }
}
void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
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
                        }
                    }
                    break;
                default:
                    break;
            }
            break;
        case keyDown: case autoKey:
            break;
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
        default:
            break;
    }
}
