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
#include <Lists.h>
#include <Controls.h>
#include <stdlib.h>
#include "logging.h"
#include "network.h" // Includes gMacTCPRefNum, gMyLocalIP, gMyLocalIPStr, YieldTimeToSystem
#include "dialog.h"  // Includes HandleSendButtonClick declaration
#include "peer_mac.h"
#include "dialog_peerlist.h"
#include "tcp.h"       // Includes PollTCPListener declaration, GetTCPStreamState
#include "discovery.h" // Includes PollUDPListener declaration
#include <Sound.h>     // Include for SysBeep

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
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60; // Update peer list every 5 seconds

// Forward declarations
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleMouseDownInContent(WindowPtr whichWindow, EventRecord *event);
void HandleIdleTasks(void);
// HandleSendButtonClick is now declared in dialog.h and defined in dialog.c

int main(void) {
    OSErr networkErr;
    Boolean dialogOk;

    InitLogFile();
    log_message("Starting application (Async Poll / Async Send Attempt)..."); // Updated log message

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

    log_message("Initiating shutdown sequence...");
    // Use the potentially blocking sync function for shutdown
    OSErr quitErr = TCP_SendQuitMessagesSync(YieldTimeToSystem);
    if (quitErr != noErr) {
        // Log specific errors like timeout or stream busy if needed
        log_message("Warning: TCP_SendQuitMessagesSync reported last error: %d.", quitErr);
    } else {
        log_message("Finished sending shutdown notifications (or no peers).");
    }

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
    long sleepTime = 1L; // Use 1 tick sleep for better responsiveness

    while (!gDone) {
        // Idle TextEdit fields
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);

        // Handle background network tasks and peer list pruning
        HandleIdleTasks();

        // Wait for events, yielding time
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            Boolean eventHandled = false;

            // --- Special handling for scrollbar clicks BEFORE DialogSelect ---
            // This ensures TrackControl works correctly for continuous scrolling.
            if (event.what == mouseDown) {
                WindowPtr whichWindow;
                short windowPart = FindWindow(event.where, &whichWindow);
                if (whichWindow == (WindowPtr)gMainWindow && windowPart == inContent) {
                    Point localPt = event.where;
                    ControlHandle foundControl;
                    short foundControlPart;
                    GrafPtr oldPort;
                    GetPort(&oldPort);
                    SetPort(GetWindowPort(gMainWindow));
                    GlobalToLocal(&localPt);
                    // FindControl checks if the click hit *any* part of the control
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    SetPort(oldPort);

                    // Only handle clicks in the up/down arrows or page regions here
                    if (foundControl == gMessagesScrollBar &&
                        (foundControlPart == inUpButton || foundControlPart == inDownButton ||
                         foundControlPart == inPageUp || foundControlPart == inPageDown))
                    {
                        log_to_file_only("MainEventLoop: Handling mouseDown on Messages Scrollbar arrows/page (part %d) BEFORE DialogSelect.", foundControlPart);
                        // Check if control is active before tracking
                        if ((**foundControl).contrlHilite == 0) {
                            // TrackControl handles repeated actions while mouse is down
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        } else {
                            log_to_file_only("MainEventLoop: Messages scrollbar clicked but inactive.");
                        }
                        eventHandled = true; // Mark event as handled
                    }
                }
            } // End scrollbar pre-handling

            // --- Standard Event Handling ---
            if (!eventHandled) {
                // Let DialogSelect handle clicks on buttons, checkboxes, TE fields, list selection, scrollbar thumb
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog;
                    short itemHit;
                    // DialogSelect returns true if it handled the event AND itemHit is > 0
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        if (whichDialog == gMainWindow && itemHit > 0) {
                             log_to_file_only("DialogSelect returned TRUE for item %d.", itemHit);
                             switch (itemHit) {
                                 case kSendButton:
                                     HandleSendButtonClick(); // Defined in dialog.c
                                     break;
                                 case kBroadcastCheckbox:
                                     log_to_file_only("Broadcast checkbox state changed by DialogSelect.");
                                     // Add logic if needed
                                     break;
                                 case kMessagesScrollbar:
                                      // DialogSelect handles thumb tracking automatically
                                      log_to_file_only("DialogSelect handled event for Messages Scrollbar (item %d). Assumed thumb drag.", itemHit);
                                      // Update TE scroll based on new control value after thumb drag
                                      // short oldValue = GetControlValue(gMessagesScrollBar); // Might not be needed
                                      short newValue = GetControlValue(gMessagesScrollBar);
                                      if (gMessagesTE != NULL) {
                                            SignedByte teState = HGetState((Handle)gMessagesTE); HLock((Handle)gMessagesTE);
                                            if (*gMessagesTE != NULL) {
                                                short lineHeight = (**gMessagesTE).lineHeight;
                                                if (lineHeight > 0) {
                                                    // Calculate desired top line based on scrollbar value
                                                    short desiredTopPixel = -newValue * lineHeight;
                                                    short currentTopPixel = (**gMessagesTE).destRect.top;
                                                    short scrollDeltaPixels = desiredTopPixel - currentTopPixel;
                                                    if (scrollDeltaPixels != 0) {
                                                        ScrollMessagesTE(scrollDeltaPixels);
                                                    }
                                                }
                                            } HSetState((Handle)gMessagesTE, teState);
                                      }
                                     break;
                                 case kInputTextEdit: // DialogSelect handles TE activation/clicks
                                 case kMessagesTextEdit: // Read-only, but DS might handle clicks
                                     log_to_file_only("DialogSelect handled event in item %d (TE).", itemHit);
                                     break;
                                 case kPeerListUserItem: // DialogSelect handles list clicks via LClick
                                     log_to_file_only("DialogSelect handled event in item %d (List).", itemHit);
                                     // Update selection state if needed (LClick should handle it)
                                     Cell tempCell = {0,-1};
                                     if (LGetSelect(true, &tempCell, gPeerListHandle)) {
                                         gLastSelectedCell = tempCell;
                                     } else {
                                         SetPt(&gLastSelectedCell, 0, -1);
                                     }
                                     break;
                                  default:
                                     log_to_file_only("DialogSelect handled unknown item %d.", itemHit);
                                     break;
                             }
                        }
                        eventHandled = true; // DialogSelect handled it
                    }
                } // End IsDialogEvent

                // If not handled by DialogSelect or special cases, use standard handler
                if (!eventHandled) {
                     HandleEvent(&event);
                }
            } // End standard handling
        } // End gotEvent
    } // End while !gDone
}

void HandleIdleTasks(void) {
    unsigned long currentTimeTicks = TickCount();

    // Poll network listeners/state machines
    PollUDPListener(gMacTCPRefNum, gMyLocalIP); // UDP needs local IP to ignore self
    PollTCPListener(gMacTCPRefNum);             // TCP state machine manages itself

    // Send periodic discovery broadcast
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

    // Prune and update peer list display periodically
    if (gLastPeerListUpdateTime == 0 ||
        (currentTimeTicks < gLastPeerListUpdateTime) || // Handle TickCount rollover
        (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks)
    {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false); // false = don't force redraw if count hasn't changed
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}

void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
    char theChar;

    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    // HandleMenuClick(MenuSelect(event->where)); // Add menu handling if needed
                    break;
                case inSysWindow:
                    SystemClick(event, whichWindow);
                    break;
                case inDrag:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // Use screen bounds, adjusted for menu bar height if necessary
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                         if (TrackGoAway(whichWindow, event->where)) {
                              log_message("Close box clicked. Setting gDone = true.");
                              gDone = true; // Signal main loop to exit
                         }
                    }
                    break;
                case inContent:
                     // Clicks in content not handled by DialogSelect or scrollbar pre-handler
                     if (whichWindow == (WindowPtr)gMainWindow) {
                         // HandleMouseDownInContent(whichWindow, event); // Might be needed for custom items
                         log_to_file_only("HandleEvent: Click in content not handled by DS.");
                     } else {
                         // Click in non-front window brings it to front
                         SelectWindow(whichWindow);
                     }
                    break;
                 case inZoomIn: // Handle zoom box if present
                 case inZoomOut:
                    // if (whichWindow == (WindowPtr)gMainWindow) {
                    //     if (TrackBox(whichWindow, event->where, windowPart)) {
                    //         ZoomWindow(whichWindow, windowPart, true); // Or false based on context
                    //     }
                    // }
                    break;
                default:
                    break;
            }
            break;

        case keyDown:
        case autoKey:
             theChar = event->message & charCodeMask;
             if ((event->modifiers & cmdKey) != 0) {
                 // Handle Command keys if menus are added
                 // HandleMenuClick(MenuKey(theChar));
             } else {
                 // Key events are generally handled by DialogSelect/TEKey for active TE fields
                 // No specific handling needed here unless overriding TE behavior
             }
            break;

        case updateEvt:
            whichWindow = (WindowPtr)event->message;
             BeginUpdate(whichWindow);
             // EraseRgn(whichWindow->visRgn); // Optional: Erase region first
             if (whichWindow == (WindowPtr)gMainWindow) {
                 DrawDialog(whichWindow); // Redraw standard dialog items
                 UpdateDialogControls(); // Redraw custom items (TE, List, Scrollbar)
             }
             // DrawGrowIcon(whichWindow); // Draw grow icon if window is resizable
             EndUpdate(whichWindow);
            break;

        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
                log_to_file_only("HandleEvent: ActivateEvt for gMainWindow -> BecomingActive=%d", becomingActive);
                // Activate/Deactivate custom controls
                ActivateDialogTE(becomingActive); // Handles both TEs
                ActivatePeerList(becomingActive);
                ActivateMessagesTEAndScrollbar(becomingActive); // Handles scrollbar hilite
                // DrawGrowIcon(whichWindow); // Redraw grow icon based on active state
            }
            break;

        // Handle other events if needed (diskEvt, osEvt, etc.)
        // case diskEvt:
        //     if (HiWord(event->message) != noErr) {
        //         // Handle disk errors
        //     }
        //     break;

        default:
            break;
    }
}

// Handles clicks in content area NOT handled by DialogSelect
// (e.g., clicks outside any active control/item)
void HandleMouseDownInContent(WindowPtr whichWindow, EventRecord *event) {
    // Currently unused as DialogSelect handles TE/List clicks,
    // and scrollbar arrows are pre-handled.
    // If you add other custom clickable areas, handle them here.
    Point localPt = event->where;
    GrafPtr oldPort;
    GetPort(&oldPort);
    SetPort(GetWindowPort(whichWindow));
    GlobalToLocal(&localPt);
    log_to_file_only("HandleMouseDownInContent: Processing click at local (%d, %d)", localPt.v, localPt.h);
    // Example: Check if click hit a custom drawing area
    // if (PtInRect(localPt, &gMyCustomRect)) { ... }
    SetPort(oldPort);
}