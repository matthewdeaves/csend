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
    InitializeToolbox();
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("MaxApplZone called.");
    log_debug("Toolbox Initialized.");
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        log_app_event("Fatal: Network initialization failed (%d). Exiting.", (int)networkErr);
        log_shutdown();
        return 1;
    }
    InitPeerList();
    log_debug("Peer list data structure initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking();
        log_shutdown();
        return 1;
    }
    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    log_debug("Entering main event loop...");
    MainEventLoop();
    log_debug("Exited main event loop.");
    log_app_event("Initiating shutdown sequence...");
    AppendToMessagesTE("Shutting down...\r");
    int quit_sent_count = 0;
    int quit_active_peers = 0;
    OSErr last_quit_err = noErr;
    unsigned long dummyTimerForDelay;
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
            YieldTimeToSystem();
            Delay(kQuitMessageDelayTicks, &dummyTimerForDelay);
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
    CleanupDialog();
    CleanupNetworking();
    log_app_event("Application terminated gracefully.");
    log_shutdown();
    return 0;
}
void InitializeToolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}
void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 1L;
    while (!gDone) {
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        IdleInputTE();
        HandleIdleTasks();
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
                    SetPort(GetWindowPort(gMainWindow));
                    GlobalToLocal(&localPt);
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                            (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
                        log_debug("MouseDown: Click in Messages Scrollbar (part %d).", foundControlPart);
                        if (foundControlPart == kControlIndicatorPart) {
                            short oldValue = GetControlValue(foundControl);
                            TrackControl(foundControl, localPt, nil);
                            short newValue = GetControlValue(foundControl);
                            log_debug("MouseDown: Scrollbar thumb drag. OldVal=%d, NewVal=%d", oldValue, newValue);
                            if (newValue != oldValue) {
                                ScrollMessagesTEToValue(newValue);
                            }
                        } else {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandledByApp = true;
                    } else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else {
                        Rect inputTERectDITL;
                        DialogItemType itemTypeDITL;
                        Handle itemHandleDITL;
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
                                    SetControlValue(controlH, !currentValue);
                                    Boolean newState = (GetControlValue(controlH) == 1);
                                    set_debug_output_enabled(newState);
                                    log_debug("Debug output %s.", newState ? "ENABLED" : "DISABLED");
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
                                    SetControlValue(controlH, !currentValue);
                                    if (GetControlValue(controlH) == 1) {
                                        log_debug("Broadcast checkbox checked. Deselecting peer.");
                                        DialogPeerList_DeselectAll();
                                    } else {
                                        log_debug("Broadcast checkbox unchecked.");
                                    }
                                    GetPort(&oldPortForDrawing);
                                    SetPort(GetWindowPort(gMainWindow));
                                    InvalRect(&itemRect);
                                    SetPort(oldPortForDrawing);
                                }
                                break;
                            case kMessagesScrollbar:
                                log_debug("WARNING: DialogSelect returned kMessagesScrollbar for itemHit %d.", itemHit);
                                break;
                            default:
                                log_debug("DialogSelect unhandled item: %d", itemHit);
                                break;
                            }
                        }
                        eventHandledByApp = true;
                    }
                }
            }
            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        } else {
            HandleIdleTasks();
        }
    }
}
void HandleIdleTasks(void)
{
    unsigned long currentTimeTicks = TickCount();
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    PollTCP(YieldTimeToSystem);
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (gLastPeerListUpdateTime == 0 ||
            (currentTimeTicks < gLastPeerListUpdateTime) ||
            (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false);
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}
void HandleEvent(EventRecord *event)
{
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
            if (whichWindow == (WindowPtr)gMainWindow)
                DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            break;
        case inGoAway:
            if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) {
                log_debug("Close box clicked. Setting gDone = true.");
                gDone = true;
            }
            break;
        case inContent:
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else {
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
        if (HandleInputTEKeyDown(event)) {
        } else {
        }
        break;
    case updateEvt:
        whichWindow = (WindowPtr)event->message;
        BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) {
            DrawDialog(whichWindow);
            UpdateDialogControls();
        }
        EndUpdate(whichWindow);
        break;
    case activateEvt:
        whichWindow = (WindowPtr)event->message;
        if (whichWindow == (WindowPtr)gMainWindow) {
            Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
            ActivateDialogTE(becomingActive);
            ActivatePeerList(becomingActive);
            if (gMessagesScrollBar != NULL) {
                short maxScroll = GetControlMaximum(gMessagesScrollBar);
                short hiliteValue = 255;
                if (becomingActive && maxScroll > 0 && (**gMessagesScrollBar).contrlVis) {
                    hiliteValue = 0;
                }
                HiliteControl(gMessagesScrollBar, hiliteValue);
            }
        }
        break;
    default:
        break;
    }
}
