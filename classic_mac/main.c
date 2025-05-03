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
#include "network.h"    // For gMacTCPRefNum, gMyLocalIP, InitializeNetworking, CleanupNetworking
#include "discovery.h"  // For async UDP functions, flags, and PBs
#include "dialog.h"     // For gMainWindow, gMyUsername, InitDialog, CleanupDialog, etc.
#include "peer_mac.h"   // For InitPeerList, PruneTimedOutPeers

Boolean gDone = false;

// Prototypes
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void); // New function for handling async results and periodic tasks

int main(void) {
    // (Content unchanged)
    OSErr networkErr;
    Boolean dialogOk;

    InitLogFile();
    log_message("Starting application (Async Read Poll / Sync Write)..."); // Log message reflects config

    MaxApplZone();
    log_message("MaxApplZone called.");
    InitializeToolbox();
    log_message("Toolbox Initialized.");

    networkErr = InitializeNetworking();
    if (networkErr != noErr) { /* log, exit */ return 1; }

    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) { /* log, cleanup, exit */ return 1; }

    InitPeerList();
    log_message("Peer list initialized.");

    dialogOk = InitDialog();
    if (!dialogOk) { /* log, cleanup, exit */ return 1; }

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
    // (Content unchanged)
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}

void MainEventLoop(void) {
    // (Content unchanged)
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

    // --- Poll Asynchronous Operations ---

    // 1. Check pending asynchronous read
    if (gUDPReadPending) {
        readResult = gUDPReadPB.ioResult;
        if (readResult != 1) { // 1 means inProgress
            // log_message("DEBUG: Polled udpRead completed with result: %d", readResult);
            gUDPReadPending = false; // Operation finished (success or error)

            if (readResult == noErr) {
                ProcessUDPReceive(gMacTCPRefNum, gMyLocalIP);
                // ProcessUDPReceive initiates the async buffer return (with nil completion)
            } else {
                log_message("Error (Idle): Polled async udpRead completed with error: %d", readResult);
                // Try to start a new read immediately if an error occurred
                if (!gUDPReadPending && !gUDPBfrReturnPending) {
                    StartAsyncUDPRead();
                }
            }
        }
        // else: read is still in progress, do nothing this cycle
    }

    // 2. Check pending asynchronous buffer return
    if (gUDPBfrReturnPending) {
        bfrReturnResult = gUDPBfrReturnPB.ioResult;
        if (bfrReturnResult != 1) { // 1 means inProgress
            // log_message("DEBUG: Polled udpBfrReturn completed with result: %d", bfrReturnResult);
            gUDPBfrReturnPending = false; // Operation finished

            if (bfrReturnResult != noErr) {
                log_message("CRITICAL Error (Idle): Polled async udpBfrReturn completed with error: %d.", bfrReturnResult);
            } else {
                // log_message("DEBUG: Polled async udpBfrReturn completed successfully.");
            }

            // Whether buffer return succeeded or failed, try to start the next read
            if (!gUDPReadPending) { // Make sure a read isn't already pending somehow
                StartAsyncUDPRead();
            }
        }
        // else: buffer return is still in progress, do nothing this cycle
    }


    // --- Initiate periodic tasks ---

    // 3. Check if it's time to send the periodic broadcast (Calls SYNC version)
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);


    // --- Keep Peer Pruning Disabled ---
    /*
    // 4. Prune timed-out peers periodically
    PruneTimedOutPeers();
    */
    // --- END ---


    // 5. Ensure a read is always pending if possible (Safety net using polling logic)
    // If no read is pending AND no buffer return is pending, start a new read.
    if (!gUDPReadPending && !gUDPBfrReturnPending) {
        StartAsyncUDPRead();
        // Ignore immediate errors here, they'll be caught by polling next cycle
    }
}


void HandleEvent(EventRecord *event) {
    // (Content unchanged)
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