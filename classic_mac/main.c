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
#include <OSUtils.h> // For ExitToShell
#include <Sound.h>
#include <stdio.h>

#ifndef resumeMask
#define resumeMask 0x00000001L
#endif

#include "../shared/logging.h"
#include "../shared/protocol.h"
#include "logging.h"
#include "mactcp_network.h"
#include "mactcp_discovery.h"
#include "mactcp_messaging.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "peer.h"

Boolean gDone = false;
extern Boolean gSystemInitiatedQuit; // Defined in mactcp_network.c or another shared spot

unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
const unsigned long kQuitMessageDelayTicks = 120;

void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);

int main(void)
{
    OSErr networkErr;
    Boolean dialogOk;
    char ui_message_buffer[256];

    platform_logging_callbacks_t classic_mac_log_callbacks = {
        .get_timestamp = classic_mac_platform_get_timestamp,
        .display_debug_log = classic_mac_platform_display_debug_log
    };
    log_init("csend_mac.log", &classic_mac_log_callbacks);

    MaxApplZone();
    InitializeToolbox(); // Calls InitEvents, could process an early quit via osEvt
    if (gSystemInitiatedQuit) { // If osEvt quit happened during InitToolbox
        log_app_event("System quit during InitializeToolbox. ExitToShell should have been called.");
        // ExitToShell in HandleEvent should prevent reaching here. This is a fallback.
        return 0;
    }
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("MaxApplZone called.");
    log_debug("Toolbox Initialized.");

    networkErr = InitializeNetworking();
    if (gSystemInitiatedQuit) { // If osEvt quit happened during InitializeNetworking
        log_app_event("System quit during InitializeNetworking. ExitToShell should have been called.");
        return 0;
    }
    if (networkErr != noErr) {
        log_app_event("Fatal: Network initialization failed (%d). Exiting.", (int)networkErr);
        log_shutdown(); // Only if not a system quit that already exited
        return 1;
    }

    InitPeerList();
    if (gSystemInitiatedQuit) { log_app_event("System quit after InitPeerList. ExitToShell should have been called."); return 0; }
    log_debug("Peer list data structure initialized.");

    dialogOk = InitDialog();
    if (gSystemInitiatedQuit) { // If osEvt quit happened during InitDialog
        log_app_event("System quit during InitDialog. ExitToShell should have been called.");
        return 0;
    }
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        if (!gSystemInitiatedQuit) { // Check again, though ExitToShell should prevent this path
             CleanupNetworking();
             log_shutdown();
        }
        return 1;
    }

    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    log_debug("Entering main event loop...");
    MainEventLoop();

    // If ExitToShell() was called from HandleEvent due to gSystemInitiatedQuit,
    // we should NOT reach this point. This block is primarily for user-initiated quits.
    if (!gSystemInitiatedQuit) {
        log_debug("Exited main event loop (user quit).");
        log_app_event("User-initiated quit: Initiating shutdown sequence...");
        AppendToMessagesTE("Shutting down...\r");

        int quit_sent_count = 0;
        int quit_active_peers = 0;
        OSErr last_quit_err = noErr;
        unsigned long dummyTimerForDelay;

        log_app_event("User-initiated quit: Sending QUIT messages to peers...");
        for (int i = 0; i < MAX_PEERS; i++) {
            if (gPeerManager.peers[i].active) {
                quit_active_peers++;
                OSErr current_quit_err = MacTCP_SendMessageSync(
                                             gPeerManager.peers[i].ip, "", MSG_QUIT, gMyUsername, gMyLocalIPStr, YieldTimeToSystem);
                if (current_quit_err == noErr) quit_sent_count++;
                else {
                    log_debug("Failed to send QUIT to %s@%s: Error %d", gPeerManager.peers[i].username, gPeerManager.peers[i].ip, (int)current_quit_err);
                    if (last_quit_err == noErr || (last_quit_err == streamBusyErr && current_quit_err != streamBusyErr)) last_quit_err = current_quit_err;
                }
                YieldTimeToSystem();
                Delay(kQuitMessageDelayTicks, &dummyTimerForDelay);
            }
        }
        if (quit_active_peers > 0) {
            sprintf(ui_message_buffer, "Finished sending QUIT messages. Sent to %d of %d active peers. Last error (if any): %d", quit_sent_count, quit_active_peers, (int)last_quit_err);
            AppendToMessagesTE(ui_message_buffer); AppendToMessagesTE("\r");
        } else AppendToMessagesTE("No active peers to send QUIT messages to.\r");
        log_app_event(ui_message_buffer); // Log this regardless of AppendToMessagesTE success

        CleanupDialog();
        CleanupNetworking();
        log_app_event("Application terminated gracefully by user.");
        log_shutdown();
    } else {
        // This path should ideally not be taken if ExitToShell() in HandleEvent works as expected.
        log_app_event("System shutdown: Reached end of main, but ExitToShell should have handled termination earlier.");
        // Minimal cleanup if ExitToShell was somehow bypassed or failed.
        // No UI, no extensive network cleanup.
        // log_shutdown() might be risky here if ExitToShell failed due to deep system issues.
    }
    return 0;
}

void InitializeToolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL); // This calls InitEvents() internally.
                       // InitEvents() processes pending events, including an osEvt (quit).
                       // If a quit osEvt is processed here, HandleEvent will call ExitToShell().
    InitCursor();
}

void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L;

    while (!gDone) { // gDone is set by user actions or by the osEvt handler (along with gSystemInitiatedQuit)
        // If gSystemInitiatedQuit is true, ExitToShell should have been called.
        // This loop should not continue if that's the case.
        // The checks for gSystemInitiatedQuit here are defensive.
        if (gSystemInitiatedQuit) {
            log_debug("MainEventLoop: gSystemInitiatedQuit is true at loop top. ExitToShell should have occurred. Breaking defensively.");
            break;
        }

        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        IdleInputTE();
        HandleIdleTasks(); // This will return immediately if gSystemInitiatedQuit is true (set by HandleEvent)
        
        if (gSystemInitiatedQuit) { // Defensive check
            log_debug("MainEventLoop: gSystemInitiatedQuit is true after HandleIdleTasks. Breaking defensively.");
            break;
        }
        
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        // If WaitNextEvent processes an osEvt (quit), HandleEvent will be called by the system
        // *before* WaitNextEvent returns control here in some cases, or HandleEvent will be called
        // in the 'if (gotEvent)' block. In either path, ExitToShell should be called.

        if (gotEvent) {
            // If gSystemInitiatedQuit is true here, it means HandleEvent was called,
            // processed the quit osEvt, but somehow ExitToShell() didn't terminate or wasn't called.
            // This is a fallback / defensive coding.
            if (gSystemInitiatedQuit) {
                log_debug("MainEventLoop: gSystemInitiatedQuit true after WNE/event processing. Breaking defensively.");
                break;
            }

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
                    SetPort(GetWindowPort(gMainWindow));
                    GlobalToLocal(&localPt);
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                        (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
                        if (foundControlPart == kControlIndicatorPart) {
                            short oldValue = GetControlValue(foundControl);
                            TrackControl(foundControl, localPt, nil);
                            short newValue = GetControlValue(foundControl);
                            if (newValue != oldValue) ScrollMessagesTEToValue(newValue);
                        } else {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandledByApp = true;
                    } else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else {
                        Rect inputTERectDITL; DialogItemType itemTypeDITL; Handle itemHandleDITL;
                        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                        if (PtInRect(localPt, &inputTERectDITL)) {
                            HandleInputTEClick(gMainWindow, &event);
                            eventHandledByApp = true;
                        }
                    }
                    SetPort(oldPort);
                }
            }

            if (!eventHandledByApp) {
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog; short itemHit;
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        if (whichDialog == gMainWindow && itemHit > 0) {
                            ControlHandle controlH; DialogItemType itemType; Handle itemHandle; Rect itemRect; GrafPtr oldPortForDrawing; short currentValue;
                            switch (itemHit) {
                            case kSendButton: HandleSendButtonClick(); break;
                            case kDebugCheckbox:
                                GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
                                if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                    controlH = (ControlHandle)itemHandle; currentValue = GetControlValue(controlH); SetControlValue(controlH, !currentValue);
                                    set_debug_output_enabled(GetControlValue(controlH) == 1);
                                    GetPort(&oldPortForDrawing); SetPort(GetWindowPort(gMainWindow)); InvalRect(&itemRect); SetPort(oldPortForDrawing);
                                } break;
                            case kBroadcastCheckbox:
                                GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                                if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                    controlH = (ControlHandle)itemHandle; currentValue = GetControlValue(controlH); SetControlValue(controlH, !currentValue);
                                    if (GetControlValue(controlH) == 1) DialogPeerList_DeselectAll();
                                    GetPort(&oldPortForDrawing); SetPort(GetWindowPort(gMainWindow)); InvalRect(&itemRect); SetPort(oldPortForDrawing);
                                } break;
                            }
                        }
                        eventHandledByApp = true;
                    }
                }
            }
            // HandleEvent is called here. If it's an osEvt (quit), it will call ExitToShell().
            if (!eventHandledByApp) HandleEvent(&event); 
            
            // If HandleEvent called ExitToShell, we ideally won't get here for a system quit.
            // If it set gSystemInitiatedQuit (e.g. from a different path) but didn't ExitToShell, break.
            if (gSystemInitiatedQuit) {
                 log_debug("MainEventLoop: gSystemInitiatedQuit is true after event handling. Breaking defensively.");
                 break;
            }
        }
    }
}

void HandleIdleTasks(void)
{
    // If gSystemInitiatedQuit is true, ExitToShell should have already been called
    // from HandleEvent. This function should ideally not be running.
    if (gSystemInitiatedQuit) {
        // log_debug("HandleIdleTasks: Called while gSystemInitiatedQuit is true. Returning immediately."); // Can be noisy
        return;
    }
    unsigned long currentTimeTicks = TickCount();
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    PollTCP(YieldTimeToSystem);
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (gLastPeerListUpdateTime == 0 || (currentTimeTicks < gLastPeerListUpdateTime) || (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) UpdatePeerDisplayList(false);
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}

void HandleEvent(EventRecord *event)
{
    // If gSystemInitiatedQuit is already true (e.g. from a previous event this cycle,
    // though unlikely if ExitToShell is used), and this isn't the osEvt itself, skip.
    if (gSystemInitiatedQuit && event->what != osEvt) {
        return;
    }

    short windowPart; WindowPtr whichWindow; char theChar;
    switch (event->what) {
    case mouseDown:
        if (gSystemInitiatedQuit) return; // No UI interaction if system is quitting
        windowPart = FindWindow(event->where, &whichWindow);
        switch (windowPart) {
        case inMenuBar: break;
        case inSysWindow: SystemClick(event, whichWindow); break;
        case inDrag: if (whichWindow == (WindowPtr)gMainWindow) DragWindow(whichWindow, event->where, &qd.screenBits.bounds); break;
        case inGoAway: if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) { gDone = true; } break;
        case inContent: if (whichWindow != FrontWindow()) SelectWindow(whichWindow); break;
        }
        break;
    case keyDown: case autoKey:
        if (gSystemInitiatedQuit) return; // No UI interaction
        theChar = event->message & charCodeMask;
        if ((event->modifiers & cmdKey) != 0) { if (theChar == 'q' || theChar == 'Q') gDone = true; }
        else { HandleInputTEKeyDown(event); }
        break;
    case updateEvt:
        if (gSystemInitiatedQuit) return; // No UI interaction
        whichWindow = (WindowPtr)event->message; BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) { DrawDialog(whichWindow); UpdateDialogControls(); }
        EndUpdate(whichWindow); break;
    case activateEvt:
        if (gSystemInitiatedQuit) return; // No UI interaction
        whichWindow = (WindowPtr)event->message;
        if (whichWindow == (WindowPtr)gMainWindow) {
            Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
            ActivateDialogTE(becomingActive); ActivatePeerList(becomingActive);
            if (gMessagesScrollBar != NULL) {
                short hiliteValue = (becomingActive && GetControlMaximum(gMessagesScrollBar) > 0 && (**gMessagesScrollBar).contrlVis) ? 0 : 255;
                HiliteControl(gMessagesScrollBar, hiliteValue);
            }
        } break;
    case osEvt:
        switch ((event->message >> 24) & 0xFF) {
        case suspendResumeMessage:
            if ((event->message & resumeMask) == 0) { // System wants us to quit
                if (!gSystemInitiatedQuit) { // Check to prevent re-entry
                    gSystemInitiatedQuit = true;
                    gDone = true; // Also set gDone as a fallback
                    log_app_event("osEvt: System quit signal. Calling ExitToShell() NOW.");
                    ExitToShell(); // <<<< CRITICAL: Exit immediately. This does not return.
                }
            } else { // Resume event
                if (gSystemInitiatedQuit) return; // Ignore resume if we are in forced quit
                
                if (gMainWindow) { 
                    SelectWindow((WindowPtr)gMainWindow); 
                    InvalRect(&gMainWindow->portRect); 
                }
                log_debug("osEvt: Resume event.");
            } break;
        } break;
    }
}