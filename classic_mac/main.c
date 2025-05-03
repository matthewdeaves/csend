//====================================
// FILE: ./classic_mac/main.c
//====================================

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Devices.h>
#include <Lists.h> // <-- Include List Manager header
#include <Controls.h> // <-- Include Controls header
#include <stdlib.h>

#include "logging.h"
#include "network.h"
#include "discovery.h"
#include "dialog.h"
#include "peer_mac.h"

Boolean gDone = false;
unsigned long gLastPeerListUpdateTime = 0; // Timer for list updates
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60; // Update every 5 seconds

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
        CloseLogFile(); // Close log before exiting
        return 1;
    }

    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (%d). Exiting.", networkErr);
        CleanupNetworking(); // Cleanup network before exiting
        CloseLogFile();
        return 1;
    }

    InitPeerList();
    log_message("Peer list initialized.");

    dialogOk = InitDialog();
    if (!dialogOk) {
        log_message("Fatal: Dialog initialization failed. Exiting.");
        // CleanupUDPDiscoveryEndpoint called within CleanupNetworking now
        CleanupNetworking(); // Cleanup network
        CloseLogFile();
        return 1;
    }

    log_message("Entering main event loop...");
    MainEventLoop();
    log_message("Exited main event loop.");

    CleanupDialog();
    // CleanupUDPDiscoveryEndpoint called within CleanupNetworking now
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
    // Use a shorter sleep time to allow more frequent idle task checks
    long sleepTime = 5L; // Check roughly 12 times per second

    while (!gDone) {
        // Idle TextEdit fields
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);
        // List Manager doesn't have an Idle function

        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                // Pass the address of the event record to DialogSelect
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        // Pass the address of the event record to HandleDialogClick
                        HandleDialogClick(whichDialog, itemHit, &event); // <-- Corrected call
                    }
                }
            } else {
                HandleEvent(&event);
            }
        } else {
            // No event, perform idle tasks
            HandleIdleTasks();
        }
    }
}

void HandleIdleTasks(void) {
    OSErr readResult;
    OSErr bfrReturnResult;
    unsigned long currentTimeTicks = TickCount();

    // --- Check UDP Async Operations (Polling) ---
    if (gUDPReadPending) {
        readResult = gUDPReadPB.ioResult;
        if (readResult != 1) { // 1 means still pending
            gUDPReadPending = false;
            if (readResult == noErr) {
                // Successfully received data
                ProcessUDPReceive(gMacTCPRefNum, gMyLocalIP);
            } else {
                log_message("Error (Idle): Polled async udpRead completed with error: %d", readResult);
                // Attempt to restart read if buffer return isn't pending
                if (!gUDPBfrReturnPending) {
                    StartAsyncUDPRead();
                }
            }
        }
    }

    if (gUDPBfrReturnPending) {
        bfrReturnResult = gUDPBfrReturnPB.ioResult;
        if (bfrReturnResult != 1) { // 1 means still pending
            gUDPBfrReturnPending = false;
            if (bfrReturnResult != noErr) {
                log_message("CRITICAL Error (Idle): Polled async udpBfrReturn completed with error: %d.", bfrReturnResult);
            } else {
                 // Buffer successfully returned
            }
            // Always try to restart read after buffer return attempt completes
            if (!gUDPReadPending) {
                StartAsyncUDPRead();
            }
        }
    }

    // --- Send Discovery Broadcast Periodically ---
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

    // --- Ensure UDP Read is Active if Possible ---
    // If neither read nor buffer return is pending, start a new read
    if (!gUDPReadPending && !gUDPBfrReturnPending) {
        StartAsyncUDPRead();
    }

    // --- Update Peer List Display Periodically ---
    if (currentTimeTicks < gLastPeerListUpdateTime) { // Handle TickCount rollover
         gLastPeerListUpdateTime = currentTimeTicks;
    }
    if ((currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) { // Only update if list exists
            UpdatePeerDisplayList(false); // Update, don't force redraw unless content changes
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}

void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
    ControlHandle whichControl; // For scrollbar handling
    short controlPart; // For scrollbar handling

    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    // TODO: Handle menu selections if menus are added
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
                            gDone = true; // Signal loop to terminate
                        }
                    }
                    break;
                case inContent:
                     if (whichWindow == (WindowPtr)gMainWindow) {
                         if (whichWindow != FrontWindow()) {
                             SelectWindow(whichWindow);
                         } else {
                             Point localPt = event->where;
                             GlobalToLocal(&localPt);
                             // Check for clicks in controls FIRST
                             controlPart = FindControl(localPt, whichWindow, &whichControl);
                             if (whichControl == gMessagesScrollBar && controlPart != 0) {
                                 // Click in our scroll bar
                                 controlPart = TrackControl(whichControl, localPt, &MyScrollAction);
                                 // MyScrollAction handles the actual scrolling
                             } else if (gInputTE && PtInRect(localPt, &(**gInputTE).viewRect)) {
                                 // Let DialogSelect handle TE activation/clicks if possible
                                 // (DialogSelect is called if IsDialogEvent is true)
                             } else if (gPeerListHandle && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                                 // LClick is handled via DialogSelect now
                             }
                             // Clicks elsewhere in content are ignored for now
                         }
                     }
                    break;
                default:
                    break;
            }
            break;

        case keyDown:
        case autoKey:
            // Key events are usually handled by DialogSelect for TE fields
            // If not using modal dialog loop, might need TEKey here.
            // For now, assume DialogSelect handles it.
            break;

        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow);
                // Draw standard dialog items (buttons, checkboxes, static text)
                DrawDialog(whichWindow);
                // Custom drawing for user items (TE and List) AND controls
                UpdateDialogControls(); // Call the renamed update function
                EndUpdate(whichWindow);
            }
            break;

        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)event->message) {
                // Activate/Deactivate TextEdit fields
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
                // List Manager selection visibility is handled by LUpdate/LClick
            }
            break;

        // Handle other events like disk insertion, network events if needed
        // kHighLevelEvent for MacTCP async completions if not polling

        default:
            break;
    }
}