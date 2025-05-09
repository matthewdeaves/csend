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
#include <OSUtils.h>
#include <Sound.h>
#include <stdio.h>

// Shared and platform-specific headers
#include "../shared/logging.h"    // For shared log_init, log_shutdown, log_debug, log_app_event
#include "../shared/protocol.h"   // For MSG_QUIT, etc.
#include "logging.h"              // For Classic Mac platform-specific callback declarations (classic_mac_platform_...)
#include "mactcp_network.h"
#include "mactcp_discovery.h"
#include "mactcp_messaging.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "peer.h"

// Global variables
Boolean gDone = false;
unsigned long gLastPeerListUpdateTime = 0;

// Constants
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60; // 5 seconds
const unsigned long kQuitMessageDelayTicks = 120;          // 2 seconds

// Forward declarations
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);

int main(void) {
    OSErr networkErr;
    Boolean dialogOk;
    char ui_message_buffer[256]; // For messages directly to UI, not through logger's debug display

    // 1. Initialize platform-specific logging callbacks structure
    platform_logging_callbacks_t classic_mac_log_callbacks = {
        .get_timestamp = classic_mac_platform_get_timestamp,
        .display_debug_log = classic_mac_platform_display_debug_log
    };

    // 2. Call shared log_init
    // It's generally safe to call this early. If file opening fails, logging to file
    // will be skipped, but UI logging (if enabled) and other functionalities can proceed.
    log_init("csend_mac.log", &classic_mac_log_callbacks);
    // set_debug_output_enabled(true); // Uncomment to have debug messages in UI by default

    // 3. Standard Mac application initialization
    MaxApplZone();
    InitializeToolbox(); // This calls InitGraf, InitFonts, etc.

    // 4. Log application start and initial setup steps
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("MaxApplZone called.");
    log_debug("Toolbox Initialized.");

    // 5. Initialize networking
    networkErr = InitializeNetworking(); // This function now uses log_debug internally
    if (networkErr != noErr) {
        // Log the fatal error. This will go to the file if it was opened.
        log_app_event("Fatal: Network initialization failed (%d). Exiting.", (int)networkErr);
        // Since the dialog isn't up, we can't use AppendToMessagesTE.
        // A simple Alert could be shown here, or rely on the log file for post-mortem.
        // Example: StopAlert(128, NULL); where 128 is an ALRT resource ID.
        log_shutdown();
        return 1;
    }

    // 6. Initialize peer list data structure
    InitPeerList(); // This function might call log_debug
    log_debug("Peer list data structure initialized.");

    // 7. Initialize the main dialog window
    dialogOk = InitDialog(); // This function now uses log_debug internally
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        // Dialog init failed, so can't use AppendToMessagesTE.
        CleanupNetworking(); // Clean up what was initialized
        log_shutdown();
        return 1;
    }

    // 8. Display initial message in the dialog and log main loop entry
    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    log_debug("Entering main event loop...");

    // 9. Run the main event loop
    MainEventLoop();

    // 10. Shutdown sequence
    log_debug("Exited main event loop.");
    log_app_event("Initiating shutdown sequence...");
    AppendToMessagesTE("Shutting down...\r");

    int quit_sent_count = 0;
    int quit_active_peers = 0;
    OSErr last_quit_err = noErr;
    unsigned long dummyTimerForDelay;

    // Send QUIT messages to active peers
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            quit_active_peers++;
            log_debug("Attempting to send QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            OSErr current_quit_err = MacTCP_SendMessageSync(
                                         gPeerManager.peers[i].ip, "", MSG_QUIT, gMyUsername, gMyLocalIPStr, YieldTimeToSystem);
            if (current_quit_err == noErr) {
                quit_sent_count++;
            } else {
                log_debug("Failed to send QUIT to %s@%s: Error %d", gPeerManager.peers[i].username, gPeerManager.peers[i].ip, (int)current_quit_err);
                if (last_quit_err == noErr || (last_quit_err == streamBusyErr && current_quit_err != streamBusyErr)) {
                    last_quit_err = current_quit_err;
                }
            }
            YieldTimeToSystem(); // Give time for MacTCP to process
            Delay(kQuitMessageDelayTicks, &dummyTimerForDelay); // Brief pause
        }
    }

    if (quit_active_peers > 0) {
        sprintf(ui_message_buffer, "Finished sending QUIT messages. Sent to %d of %d active peers. Last error (if any): %d", quit_sent_count, quit_active_peers, (int)last_quit_err);
        log_app_event("%s", ui_message_buffer);
        AppendToMessagesTE(ui_message_buffer);
        AppendToMessagesTE("\r");
    } else {
        log_app_event("No active peers to send QUIT messages to.");
        AppendToMessagesTE("No active peers to send QUIT messages to.\r");
    }

    if (last_quit_err == streamBusyErr) {
        log_debug("Warning: Sending QUIT messages encountered a stream busy error.");
    } else if (last_quit_err != noErr) {
        log_debug("Warning: Sending QUIT messages encountered error: %d.", (int)last_quit_err);
    }

    // 11. Clean up resources
    CleanupDialog();    // Uses log_debug internally
    CleanupNetworking(); // Uses log_debug internally

    log_app_event("Application terminated gracefully.");
    log_shutdown(); // Finalize and close the log file
    return 0;
}

void InitializeToolbox(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL); // Pass NULL for resumeProc
    InitCursor();
}

void MainEventLoop(void) {
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L; // Minimal sleep for WaitNextEvent polling

    while (!gDone) {
        // Handle background TE idling
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        IdleInputTE(); // For the input TextEdit field

        // Handle other periodic tasks
        HandleIdleTasks();

        // Wait for an event
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
                    GrafPtr oldPort;

                    GetPort(&oldPort);
                    SetPort(GetWindowPort(gMainWindow)); // Target the dialog's port
                    GlobalToLocal(&localPt);

                    // Check for clicks in controls first
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);

                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                        (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
                        log_debug("MouseDown: Click in Messages Scrollbar (part %d).", foundControlPart);
                        if (foundControlPart == kControlIndicatorPart) { // Thumb drag
                            short oldValue = GetControlValue(foundControl);
                            TrackControl(foundControl, localPt, nil); // Use nil for direct tracking
                            short newValue = GetControlValue(foundControl);
                            log_debug("MouseDown: Scrollbar thumb drag. OldVal=%d, NewVal=%d", oldValue, newValue);
                            if (newValue != oldValue) {
                                ScrollMessagesTEToValue(newValue);
                            }
                        } else { // Click in arrows or page regions
                            TrackControl(foundControl, localPt, &MyScrollAction); // Use action procedure
                        }
                        eventHandledByApp = true;
                    } else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        // Click in Peer List (List Manager user item)
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else {
                        // Check for click in Input TextEdit user item
                        Rect inputTERectDITL;
                        DialogItemType itemTypeDITL;
                        Handle itemHandleDITL;
                        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                        if (PtInRect(localPt, &inputTERectDITL)) {
                            HandleInputTEClick(gMainWindow, &event);
                            eventHandledByApp = true;
                        }
                    }
                    SetPort(oldPort); // Restore original port
                }
            }

            // If not handled by specific content clicks, try DialogSelect for DITL items
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
                                    SetControlValue(controlH, !currentValue); // Toggle
                                    Boolean newState = (GetControlValue(controlH) == 1);
                                    set_debug_output_enabled(newState); // Update shared log state
                                    log_debug("Debug output %s.", newState ? "ENABLED" : "DISABLED");

                                    // Redraw the checkbox
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
                                    SetControlValue(controlH, !currentValue); // Toggle
                                    if (GetControlValue(controlH) == 1) { // If now checked
                                        log_debug("Broadcast checkbox checked. Deselecting peer.");
                                        DialogPeerList_DeselectAll(); // Unselect any peer
                                    } else {
                                        log_debug("Broadcast checkbox unchecked.");
                                    }
                                    // Redraw the checkbox
                                    GetPort(&oldPortForDrawing);
                                    SetPort(GetWindowPort(gMainWindow));
                                    InvalRect(&itemRect);
                                    SetPort(oldPortForDrawing);
                                }
                                break;
                            case kMessagesScrollbar:
                                // This case should ideally be handled by FindControl/TrackControl above
                                // if it's a direct click on the scrollbar parts.
                                // DialogSelect returning a scrollbar itemHit is less common for active parts.
                                log_debug("WARNING: DialogSelect returned kMessagesScrollbar for itemHit %d.", itemHit);
                                break;
                            default:
                                log_debug("DialogSelect unhandled item: %d", itemHit);
                                break;
                            }
                        }
                        eventHandledByApp = true; // DialogSelect handled it
                    }
                }
            }

            // If still not handled, pass to generic event handler
            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        } else { // No event from WaitNextEvent
            HandleIdleTasks(); // Perform idle tasks if no event occurred
        }
    }
}

void HandleIdleTasks(void) {
    unsigned long currentTimeTicks = TickCount();

    // Poll network services
    PollUDPListener(gMacTCPRefNum, gMyLocalIP); // For discovery packets
    PollTCP(YieldTimeToSystem);                 // For incoming TCP messages/connections

    // Periodically send discovery broadcast
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

    // Periodically update the peer list display (prunes timed-out peers internally)
    if (gLastPeerListUpdateTime == 0 ||                               // First time
        (currentTimeTicks < gLastPeerListUpdateTime) ||               // TickCount wrapped
        (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false); // false = don't force redraw if no data change
        }
        gLastPeerListUpdateTime = currentTimeTicks;
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
            // Menu handling would go here if menus were implemented
            // e.g., DoMenuCommand(MenuSelect(event->where));
            break;
        case inSysWindow: // Click in a Desk Accessory window
            SystemClick(event, whichWindow);
            break;
        case inDrag: // Click in title bar to drag
            if (whichWindow == (WindowPtr)gMainWindow)
                DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            break;
        case inGoAway: // Click in close box
            if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) {
                log_debug("Close box clicked. Setting gDone = true.");
                gDone = true; // Signal to exit main loop
            }
            break;
        case inContent: // Click in content area not handled by DialogSelect or specific checks
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow); // Bring it to front
            } else {
                // If it's our main window and click wasn't handled, it might be a click
                // in an empty area or an unhandled part of a user item.
                log_debug("HandleEvent: mouseDown in content of front window (unhandled by specific checks).");
            }
            break;
        default:
            log_debug("HandleEvent: mouseDown in unknown window part: %d", windowPart);
            break;
        }
        break;

    case keyDown:
    case autoKey:
        // Handle key events, primarily for TextEdit in the input field
        if (HandleInputTEKeyDown(event)) {
            // Event was handled by the input TE (e.g., character typed)
        } else {
            // Key event not handled by input TE (e.g., command key, or TE not active)
            // Could add global key commands here if needed
        }
        break;

    case updateEvt: // Window needs to be redrawn
        whichWindow = (WindowPtr)event->message;
        BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) {
            DrawDialog(whichWindow);    // Redraws standard dialog items
            UpdateDialogControls();     // Redraws custom user items (TEs, List)
        }
        // Could handle updates for other windows here if any
        EndUpdate(whichWindow);
        break;

    case activateEvt: // Window activation/deactivation
        whichWindow = (WindowPtr)event->message;
        if (whichWindow == (WindowPtr)gMainWindow) {
            Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
            ActivateDialogTE(becomingActive);     // Activate/deactivate TEs
            ActivatePeerList(becomingActive);     // Activate/deactivate List Manager
            // Activate/deactivate scrollbar
            if (gMessagesScrollBar != NULL) {
                short maxScroll = GetControlMaximum(gMessagesScrollBar);
                short hiliteValue = 255; // Dimmed (inactive)
                if (becomingActive && maxScroll > 0 && (**gMessagesScrollBar).contrlVis) {
                    hiliteValue = 0; // Active
                }
                HiliteControl(gMessagesScrollBar, hiliteValue);
            }
        }
        break;

    // Other event types (diskEvt, osEvt, etc.) can be handled here if needed
    default:
        break;
    }
}