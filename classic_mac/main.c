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
#include "tcp.h"       // Includes PollTCP declaration, GetTCPState
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
    // Updated log message to reflect the new strategy
    log_message("Starting application (Async Completion Routine Strategy)..."); // Updated strategy name

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
    // Use the sync function for shutdown. Handle potential busy error.
    OSErr quitErr = TCP_SendQuitMessagesSync(YieldTimeToSystem);
    if (quitErr == streamBusyErr) {
        log_message("Warning: TCP_SendQuitMessagesSync failed because stream was busy or kill failed during shutdown."); // Updated msg
    } else if (quitErr != noErr) {
        log_message("Warning: TCP_SendQuitMessagesSync reported last error: %d.", quitErr);
    } else {
        log_message("Finished sending shutdown notifications (or no peers).");
    }

    CleanupDialog();
    CleanupNetworking();
    CloseLogFile();
    return 0;
}

// InitializeToolbox remains unchanged
void InitializeToolbox(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}

// MainEventLoop remains unchanged
void MainEventLoop(void) {
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L; // Use 1 tick sleep for better responsiveness

    while (!gDone) {
        // Idle TextEdit fields
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);

        // Handle background network tasks and peer list pruning
        HandleIdleTasks(); // Calls PollTCP internally

        // Wait for events, yielding time
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            Boolean eventHandled = false;

            // --- Special handling for scrollbar clicks BEFORE DialogSelect ---
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
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    SetPort(oldPort);

                    if (foundControl == gMessagesScrollBar &&
                        (foundControlPart == inUpButton || foundControlPart == inDownButton ||
                         foundControlPart == inPageUp || foundControlPart == inPageDown))
                    {
                        log_to_file_only("MainEventLoop: Handling mouseDown on Messages Scrollbar arrows/page (part %d) BEFORE DialogSelect.", foundControlPart);
                        if ((**foundControl).contrlHilite == 0) {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        } else {
                            log_to_file_only("MainEventLoop: Messages scrollbar clicked but inactive.");
                        }
                        eventHandled = true;
                    }
                }
            } // End scrollbar pre-handling

            // --- Standard Event Handling ---
            if (!eventHandled) {
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog;
                    short itemHit;
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        if (whichDialog == gMainWindow && itemHit > 0) {
                             log_to_file_only("DialogSelect returned TRUE for item %d.", itemHit);
                             switch (itemHit) {
                                 case kSendButton:
                                     HandleSendButtonClick();
                                     break;
                                 case kBroadcastCheckbox:
                                     log_to_file_only("Broadcast checkbox state changed by DialogSelect.");
                                     break;
                                 case kMessagesScrollbar:
                                      log_to_file_only("DialogSelect handled event for Messages Scrollbar (item %d). Assumed thumb drag.", itemHit);
                                      short newValue = GetControlValue(gMessagesScrollBar);
                                      if (gMessagesTE != NULL) {
                                            SignedByte teState = HGetState((Handle)gMessagesTE); HLock((Handle)gMessagesTE);
                                            if (*gMessagesTE != NULL) {
                                                short lineHeight = (**gMessagesTE).lineHeight;
                                                if (lineHeight > 0) {
                                                    short desiredTopPixel = -newValue * lineHeight;
                                                    short currentTopPixel = (**gMessagesTE).destRect.top;
                                                    short scrollDeltaPixels = desiredTopPixel - currentTopPixel;
                                                    if (scrollDeltaPixels != 0) { ScrollMessagesTE(scrollDeltaPixels); }
                                                }
                                            } HSetState((Handle)gMessagesTE, teState);
                                      }
                                     break;
                                 case kInputTextEdit:
                                 case kMessagesTextEdit:
                                     log_to_file_only("DialogSelect handled event in item %d (TE).", itemHit);
                                     break;
                                 case kPeerListUserItem:
                                     log_to_file_only("DialogSelect handled event in item %d (List).", itemHit);
                                     Cell tempCell = {0,-1};
                                     if (LGetSelect(true, &tempCell, gPeerListHandle)) { gLastSelectedCell = tempCell; }
                                     else { SetPt(&gLastSelectedCell, 0, -1); }
                                     break;
                                  default:
                                     log_to_file_only("DialogSelect handled unknown item %d.", itemHit);
                                     break;
                             }
                        }
                        eventHandled = true;
                    }
                } // End IsDialogEvent

                if (!eventHandled) {
                     HandleEvent(&event);
                }
            } // End standard handling
        } // End gotEvent
    } // End while !gDone
}

// HandleIdleTasks - *** UPDATED call to PollTCP ***
void HandleIdleTasks(void) {
    unsigned long currentTimeTicks = TickCount();

    // Poll network listeners/state machines
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    PollTCP(); // <<< UPDATED: Call PollTCP with no arguments

    // Send periodic discovery broadcast
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

    // Prune and update peer list display periodically
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

// HandleEvent remains unchanged
void HandleEvent(EventRecord *event) {
    short windowPart;
    WindowPtr whichWindow;
    char theChar;

    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar: break;
                case inSysWindow: SystemClick(event, whichWindow); break;
                case inDrag: if (whichWindow == (WindowPtr)gMainWindow) { DragWindow(whichWindow, event->where, &qd.screenBits.bounds); } break;
                case inGoAway: if (whichWindow == (WindowPtr)gMainWindow) { if (TrackGoAway(whichWindow, event->where)) { log_message("Close box clicked. Setting gDone = true."); gDone = true; } } break;
                case inContent: if (whichWindow == (WindowPtr)gMainWindow) { log_to_file_only("HandleEvent: Click in content not handled by DS."); } else { SelectWindow(whichWindow); } break;
                 case inZoomIn: case inZoomOut: break;
                default: break;
            }
            break;
        case keyDown: case autoKey:
             theChar = event->message & charCodeMask; if ((event->modifiers & cmdKey) != 0) { /* Handle menus */ } else { /* Delegate to DialogSelect/TEKey */ } break;
        case updateEvt:
            whichWindow = (WindowPtr)event->message; BeginUpdate(whichWindow);
             if (whichWindow == (WindowPtr)gMainWindow) { DrawDialog(whichWindow); UpdateDialogControls(); }
             EndUpdate(whichWindow); break;
        case activateEvt:
            whichWindow = (WindowPtr)event->message; if (whichWindow == (WindowPtr)gMainWindow) {
                Boolean becomingActive = ((event->modifiers & activeFlag) != 0); log_to_file_only("HandleEvent: ActivateEvt for gMainWindow -> BecomingActive=%d", becomingActive);
                ActivateDialogTE(becomingActive); ActivatePeerList(becomingActive); ActivateMessagesTEAndScrollbar(becomingActive);
            } break;
        default: break;
    }
}

// HandleMouseDownInContent remains unchanged
void HandleMouseDownInContent(WindowPtr whichWindow, EventRecord *event) {
    Point localPt = event->where; GrafPtr oldPort; GetPort(&oldPort); SetPort(GetWindowPort(whichWindow));
    GlobalToLocal(&localPt); log_to_file_only("HandleMouseDownInContent: Processing click at local (%d, %d)", localPt.v, localPt.h); SetPort(oldPort);
}