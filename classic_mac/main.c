// FILE: ./classic_mac/main.c
//====================================

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
#include <OSUtils.h> // For Delay

#include "logging.h"
#include "network.h" // For InitializeNetworking, CleanupNetworking, gMyUsername, gMyLocalIPStr
#include "dialog.h"  // For InitDialog, CleanupDialog, gMainWindow, k* defines
#include "peer.h"    // For InitPeerList, gPeerManager
#include "dialog_peerlist.h" // For UpdatePeerDisplayList, HandlePeerListClick, DialogPeerList_DeselectAll, ActivatePeerList
#include "dialog_input.h"    // For gInputTE, TEClick, TEKey
#include "dialog_messages.h" // For gMessagesTE, MyScrollAction, ScrollMessagesTE
#include "./mactcp_messaging.h" // For PollTCP, MacTCP_SendMessageSync, streamBusyErr
#include "discovery.h"       // For PollUDPListener, CheckSendBroadcast
#include "../shared/logging.h" // For set_debug_output_enabled
#include "../shared/protocol.h" // For MSG_QUIT

#include <Sound.h> // For SysBeep

// Constants for scrollbar part codes (ensure these are standard or defined if custom)
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

// Global application state
Boolean gDone = false; // Flag to terminate the main event loop
unsigned long gLastPeerListUpdateTime = 0; // For periodic peer list pruning/refresh
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60; // 5 seconds (60 ticks per second)
const unsigned long kQuitMessageDelayTicks = 120; // 2 seconds delay between sending QUIT messages

// Forward declarations
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

    MaxApplZone(); // Expand application heap
    log_message("MaxApplZone called.");

    InitializeToolbox();
    log_message("Toolbox Initialized.");

    // Initialize networking (MacTCP, DNR, UDP, TCP streams)
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        log_message("Fatal: Network initialization failed (%d). Exiting.", networkErr);
        CloseLogFile();
        return 1;
    }

    InitPeerList(); // Initialize the gPeerManager structure
    log_message("Peer list data structure initialized.");

    // Initialize the main dialog window and its components
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

    // Shutdown sequence
    log_message("Initiating shutdown sequence...");

    // Send QUIT messages to all active peers
    log_message("Sending QUIT messages to active peers...");
    int quit_sent_count = 0;
    int quit_active_peers = 0;
    OSErr last_quit_err = noErr;
    unsigned long dummyTimerForDelay; // For Delay()

    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            quit_active_peers++;
            log_message("Attempting to send QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            OSErr current_quit_err = MacTCP_SendMessageSync(
                gPeerManager.peers[i].ip,
                "", // No content for QUIT message
                MSG_QUIT,
                gMyUsername,
                gMyLocalIPStr,
                YieldTimeToSystem
            );

            if (current_quit_err == noErr) {
                quit_sent_count++;
            } else {
                log_message("Failed to send QUIT to %s@%s: Error %d", gPeerManager.peers[i].username, gPeerManager.peers[i].ip, current_quit_err);
                if (last_quit_err == noErr || (last_quit_err == streamBusyErr && current_quit_err != streamBusyErr) ) {
                    // Store the first "hard" error, or any error if previous was just busy
                    last_quit_err = current_quit_err;
                }
            }
            // Yield and delay between sending QUIT messages to avoid flooding and give MacTCP time
            YieldTimeToSystem();
            Delay(kQuitMessageDelayTicks, &dummyTimerForDelay);
        }
    }

    if (quit_active_peers > 0) {
        log_message("Finished sending QUIT messages. Sent to %d of %d active peers. Last error (if any): %d", quit_sent_count, quit_active_peers, last_quit_err);
    } else {
        log_message("No active peers to send QUIT messages to.");
    }
    if (last_quit_err == streamBusyErr) {
        log_message("Warning: Sending QUIT messages encountered a stream busy error during the process.");
    } else if (last_quit_err != noErr) {
        log_message("Warning: Sending QUIT messages encountered error: %d during the process.", last_quit_err);
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
    InitDialogs(NULL); // No resume procedure
    InitCursor();
}

void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L; // Minimal sleep time for WaitNextEvent, effectively polling often

    while (!gDone) {
        // Idle TextEdit fields if they exist
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);

        HandleIdleTasks(); // Handle networking and other background tasks

        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            Boolean eventHandledByApp = false; // Flag to see if we need to call DialogSelect/system handlers

            if (event.what == mouseDown) {
                WindowPtr whichWindow;
                short windowPart = FindWindow(event.where, &whichWindow);

                if (whichWindow == (WindowPtr)gMainWindow && windowPart == inContent) {
                    Point localPt = event.where;
                    ControlHandle foundControl;
                    short foundControlPart;
                    Rect inputTERectDITL; // DITL rect for input TE user item
                    DialogItemType itemTypeDITL;
                    Handle itemHandleDITL;
                    GrafPtr oldPort;

                    GetPort(&oldPort);
                    SetPort(GetWindowPort(gMainWindow)); // Work in dialog's coordinate system
                    GlobalToLocal(&localPt);

                    // Check for clicks in scrollbar first (as it's a control)
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                        (**foundControl).contrlVis && (**foundControl).contrlHilite == 0 ) {
                        log_to_file_only("MouseDown: Handling click in Messages Scrollbar (part %d).", foundControlPart);
                        if (foundControlPart == inThumb) {
                            short oldValue = GetControlValue(foundControl);
                            TrackControl(foundControl, localPt, nil); // Track thumb directly
                            short newValue = GetControlValue(foundControl);
                            log_to_file_only("MouseDown: Scrollbar thumb drag finished. OldVal=%d, NewVal=%d", oldValue, newValue);
                            if (newValue != oldValue && gMessagesTE != NULL) {
                                SignedByte teState = HGetState((Handle)gMessagesTE);
                                HLock((Handle)gMessagesTE);
                                if (*gMessagesTE != NULL) {
                                    short lineHeight = (**gMessagesTE).lineHeight;
                                    if (lineHeight > 0) {
                                        // Calculate scroll based on new thumb value (which is line number)
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
                        } else { // Up/down arrows, page regions
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandledByApp = true;
                    }
                    // Check for clicks in Peer List (UserItem for List Manager)
                    // Must check gPeerListHandle itself as it's a UserItem, not a standard control FindControl would get
                    else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        // HandlePeerListClick expects the original event with global coordinates
                        // but it internally converts. For consistency, ensure it knows its dialog.
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    }
                    // Check for clicks in Input TE (UserItem for TextEdit)
                    else {
                         // Get the DITL rect for the input TE user item to check PtInRect against TE's viewRect
                         GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                         if (gInputTE != NULL && PtInRect(localPt, &(**gInputTE).viewRect)) {
                            // TEClick expects local coordinates relative to the TE's GrafPort (the window)
                            TEClick(localPt, (event.modifiers & shiftKey) != 0, gInputTE);
                            eventHandledByApp = true;
                         }
                    }
                    SetPort(oldPort);
                }
            } // end mouseDown

            // If not handled by custom logic above, try DialogSelect for standard items
            if (!eventHandledByApp) {
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog;
                    short itemHit;
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        // DialogSelect returns true if it handled the event (e.g. button click, TE interaction for edittext items)
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
                                        set_debug_output_enabled(newState); // Update shared state
                                        log_message("Debug output %s.", newState ? "ENABLED" : "DISABLED");

                                        // Redraw checkbox
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
                                        short newCheckboxValue = GetControlValue(controlH);

                                        if (newCheckboxValue == 1) { // If broadcast is now checked
                                            log_message("Broadcast checkbox checked. Deselecting peer if any.");
                                            DialogPeerList_DeselectAll(); // Use new encapsulated function
                                        } else {
                                            log_message("Broadcast checkbox unchecked.");
                                        }
                                        // Redraw checkbox
                                        GetPort(&oldPortForDrawing);
                                        SetPort(GetWindowPort(gMainWindow));
                                        InvalRect(&itemRect);
                                        SetPort(oldPortForDrawing);
                                    }
                                    break;
                                case kMessagesScrollbar:
                                    // This case might be hit if DialogSelect handles a click on a scrollbar DITL item
                                    // that wasn't caught by FindControl/TrackControl in mouseDown.
                                    // This is less common for scrollbars handled with TrackControl(..., actionProc).
                                    log_message("WARNING: DialogSelect returned item kMessagesScrollbar - This should ideally be handled in mouseDown.");
                                    if (gMessagesTE != NULL && gMessagesScrollBar != NULL) {
                                        short currentScrollVal = GetControlValue(gMessagesScrollBar);
                                        short scrollDeltaPixels = 0, lineHeight = 0;
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
                                            short currentActualTopLine = -(**gMessagesTE).destRect.top / lineHeight;
                                            scrollDeltaPixels = (currentActualTopLine - currentScrollVal) * lineHeight;

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
                        eventHandledByApp = true; // DialogSelect handled it
                    }
                }
            }

            // If not handled by custom logic or DialogSelect, pass to system/general handlers
            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        } else { // No event from WaitNextEvent
             HandleIdleTasks(); // Still do idle tasks if WNE returned false (e.g. on timeout)
        }
    }
}

void HandleIdleTasks(void)
{
    unsigned long currentTimeTicks = TickCount();

    // Poll network listeners
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    PollTCP(YieldTimeToSystem); // Polls TCP listener and handles incoming connections/data

    // Send periodic discovery broadcast
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

    // Periodically update peer list (prune timeouts, redraw if needed)
    if (gLastPeerListUpdateTime == 0 || // First time
        (currentTimeTicks < gLastPeerListUpdateTime) || // TickCount wrapped around
        (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks)
    {
        if (gPeerListHandle != NULL) { // Ensure list is initialized
            UpdatePeerDisplayList(false); // false = don't force redraw if no data change
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
                    // Menu handling would go here if we had menus
                    // MenuSelect(event->where);
                    break;
                case inSysWindow: // Click in a DA window
                    SystemClick(event, whichWindow);
                    break;
                case inDrag: // Click in title bar to drag
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway: // Click in close box
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (TrackGoAway(whichWindow, event->where)) {
                            log_message("Close box clicked. Setting gDone = true.");
                            gDone = true; // Signal to exit main loop
                        }
                    }
                    break;
                case inContent:
                     // If click in content was not handled by specific item handlers (scrollbar, list, TE)
                     // it might be a click to activate the window if it's not front.
                     if (whichWindow != FrontWindow()) {
                        SelectWindow(whichWindow);
                     } else {
                         // Click in content of front window, not handled by other logic.
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
                // Command-key handling would go here if we had menus/shortcuts
                // short menuResult = MenuKey(theChar);
                // HandleMenuChoice(menuResult);
            } else {
                // Pass regular key presses to the active TE (Input TE)
                if (gInputTE != NULL && gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow) {
                    TEKey(theChar, gInputTE);
                }
            }
            break;

        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            BeginUpdate(whichWindow);
            if (whichWindow == (WindowPtr)gMainWindow) {
                DrawDialog(whichWindow);    // Redraws standard DITL items
                UpdateDialogControls();     // Redraws custom items (TEs, List)
            } else {
                // Handle updates for other windows if any (e.g. DAs)
            }
            EndUpdate(whichWindow);
            break;

        case activateEvt: // Window activation/deactivation
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
                ActivateDialogTE(becomingActive); // Activate/deactivate TEs
                ActivatePeerList(becomingActive); // Activate/deactivate List Manager

                // Adjust scrollbar hilite state based on window activation
                if (gMessagesScrollBar != NULL) {
                     short maxScroll = GetControlMaximum(gMessagesScrollBar);
                     short hiliteValue = 255; // Inactive
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