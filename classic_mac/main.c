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

#include "logging.h" // Includes log_message AND log_to_file_only
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
            // Prioritize DialogSelect for events it handles (clicks in items, keypresses for TE)
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                // Pass the address of the event record to DialogSelect
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        // Pass the address of the event record to HandleDialogClick
                        HandleDialogClick(whichDialog, itemHit, &event);
                    }
                }
                 // If DialogSelect returns false, it might still be an event we handle manually (like scrollbar)
                 // Fall through to HandleEvent only if DialogSelect didn't handle it *and* it's relevant?
                 // For now, let's assume DialogSelect handles what it needs to, and HandleEvent handles the rest.
                 // This might need refinement if DialogSelect consumes events we need later.
            }
            // Handle events not processed by DialogSelect (like mouseDown in scrollbar, updates, activates)
            HandleEvent(&event);

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
    ControlHandle foundControl; // Use a different name to avoid confusion with gMessagesScrollBar
    short foundControlPart; // Use a different name
    GrafPtr oldPort; // To save/restore port

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
                             // Don't process click further if window wasn't front
                         } else {
                             Point localPt = event->where;
                             // Ensure correct port before GlobalToLocal and FindControl
                             GetPort(&oldPort);
                             SetPort(GetWindowPort(gMainWindow));
                             GlobalToLocal(&localPt);
                             // *** Use file-only logging for scrollbar interaction ***
                             log_to_file_only("HandleEvent: MouseDown inContent at local (%d, %d)", localPt.v, localPt.h); // Log local coords

                             // Check for clicks in controls FIRST
                             foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                             log_to_file_only("HandleEvent: FindControl result: part=%d, control=0x%lX", foundControlPart, (unsigned long)foundControl);

                             // *** SCROLLBAR HANDLING ***
                             if (foundControl == gMessagesScrollBar && foundControlPart != 0) {
                                 log_to_file_only("HandleEvent: Click identified in gMessagesScrollBar (part %d).", foundControlPart);
                                 // Check if the control is active before tracking
                                 short hiliteState = (**foundControl).contrlHilite;
                                 log_to_file_only("HandleEvent: Scrollbar hilite state = %d (0=active, 255=inactive)", hiliteState);

                                 if (hiliteState == 0) { // 0 = active
                                     if (foundControlPart == 129 /* inThumb */) {
                                         // Click in the thumb - Use TrackControl WITHOUT Action Proc
                                         log_to_file_only("HandleEvent: Tracking scrollbar thumb...");
                                         short oldValue = GetControlValue(foundControl);
                                         foundControlPart = TrackControl(foundControl, localPt, nil); // Pass nil for action proc
                                         log_to_file_only("HandleEvent: TrackControl(thumb) returned part %d", foundControlPart);

                                         // Check if tracking occurred (TrackControl returns partcode if successful)
                                         // Even if it returns 0, the value might have been set on mouseDown
                                         short newValue = GetControlValue(foundControl);
                                         log_to_file_only("HandleEvent: Thumb tracking finished. OldVal=%d, NewVal=%d", oldValue, newValue);

                                         if (newValue != oldValue && gMessagesTE != NULL) {
                                             // Value changed, scroll the TE field
                                             SignedByte teState = HGetState((Handle)gMessagesTE);
                                             HLock((Handle)gMessagesTE);
                                             if (*gMessagesTE != NULL) {
                                                 short lineHeight = (**gMessagesTE).lineHeight;
                                                 Rect viewRectToInvalidate = (**gMessagesTE).viewRect; // Copy viewRect
                                                 if (lineHeight > 0) {
                                                     short scrollDeltaPixels = (oldValue - newValue) * lineHeight;
                                                     log_to_file_only("HandleEvent: Scrolling TE by %d pixels due to thumb drag.", scrollDeltaPixels);
                                                     TEScroll(0, scrollDeltaPixels, gMessagesTE);
                                                     // *** ADD INVALIDATION AFTER SCROLL ***
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
                                         // Click in arrows or page region - Use TrackControl with Action Proc
                                         log_to_file_only("HandleEvent: Tracking scrollbar part %d with action proc at 0x%lX...", foundControlPart, (unsigned long)&MyScrollAction);
                                         foundControlPart = TrackControl(foundControl, localPt, &MyScrollAction);
                                         log_to_file_only("HandleEvent: TrackControl with action proc finished (returned %d).", foundControlPart);
                                         // MyScrollAction handles SetControlValue, TEScroll, AND InvalRect now
                                     }
                                 } else {
                                     log_to_file_only("HandleEvent: Scrollbar clicked but inactive (hilite=%d).", hiliteState);
                                 }
                             // *** END SCROLLBAR HANDLING ***

                             } else {
                                 // Click was not in the scrollbar, or FindControl returned 0.
                                 // Let DialogSelect handle clicks in TE/List items if it hasn't already.
                                 // We might need TEClick here if DialogSelect isn't used or doesn't cover it.
                                 log_to_file_only("HandleEvent: Click not in scrollbar or FindControl returned 0.");
                                 // Example: Check for TE click if DialogSelect doesn't handle it
                                 // if (gInputTE && PtInRect(localPt, &(**gInputTE).viewRect)) {
                                 //    log_message("HandleEvent: Passing click to TEClick for Input TE."); // Normal log ok here
                                 //    TEClick(localPt, (event->modifiers & shiftKey) != 0, gInputTE);
                                 // }
                             }
                             SetPort(oldPort); // Restore port after handling click
                         }
                     }
                    break;
                default:
                    break;
            }
            break;

        case keyDown:
        case autoKey:
            // Let DialogSelect handle key events if possible.
            // If DialogSelect isn't used or doesn't handle keys for the active TE field,
            // you would call TEKey here.
            // Example:
            // if (gMainWindow == (DialogPtr)FrontWindow()) {
            //    // Determine which TE field has focus (if any) and call TEKey
            //    // This requires tracking focus, which DialogSelect normally does.
            //    // For simplicity, assume DialogSelect handles it if IsDialogEvent was true.
            //    if (gInputTE != NULL) { // Crude check - assumes input TE always has focus if window front
            //         TEKey(event->message & charCodeMask, gInputTE);
            //    }
            // }
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
            // Check if the window being activated/deactivated is our main window
            if (whichWindow == (WindowPtr)gMainWindow) {
                log_message("HandleEvent: ActivateEvt for gMainWindow. Modifiers: %d (activeFlag=%d)", event->modifiers, activeFlag);
                // Activate/Deactivate TextEdit fields and potentially controls
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