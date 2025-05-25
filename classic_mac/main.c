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
#include <AppleEvents.h>
#include <Resources.h>
#include <stdlib.h>
#include <OSUtils.h>
#include <Sound.h>
#include <stdio.h>
#include <string.h>
#include "../shared/logging.h"
#include "../shared/common_defs.h"
#include "../shared/protocol.h"
#include "logging.h"
#include "network_init.h"
#include "discovery.h"
#include "messaging.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "peer.h"
#ifndef HiWord
#define HiWord(x) ((short)(((long)(x) >> 16) & 0xFFFF))
#endif
#ifndef LoWord
#define LoWord(x) ((short)((long)(x) & 0xFFFF))
#endif
Boolean gDone = false;
unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
const unsigned long kQuitMessageDelayTicks = 30;
#define kAppleMenuID 1
#define kFileMenuID 128
#define kEditMenuID 129
#define kAboutItem 1
#define kQuitItem 1
static AEEventHandlerUPP gAEQuitAppUPP = NULL;
void InitializeToolbox(void);
void InstallAppleEventHandlers(void);
pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon);
void HandleMenuChoice(long menuResult);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);
int main(void)
{
    OSErr networkErr;
    Boolean dialogOk;
    char ui_message_buffer[BUFFER_SIZE + 100];
    platform_logging_callbacks_t classic_mac_log_callbacks = {
        .get_timestamp = classic_mac_platform_get_timestamp,
        .display_debug_log = classic_mac_platform_display_debug_log
    };
    log_init("csend_mac.log", &classic_mac_log_callbacks);
    MaxApplZone();
    InitializeToolbox();
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("MaxApplZone called. Toolbox Initialized.");
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        sprintf(ui_message_buffer, "Fatal: Network initialization failed (Error: %d). Application cannot continue.", (int)networkErr);
        log_app_event("%s", ui_message_buffer);
        Str255 pErrorMsg;
        sprintf((char *)pErrorMsg + 1, "Network Init Failed: %d. See log.", (int)networkErr);
        pErrorMsg[0] = strlen((char *)pErrorMsg + 1);
        StopAlert(128, nil);
        if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        log_shutdown();
        return 1;
    }
    log_debug("Networking stack initialized.");
    InitPeerList();
    log_debug("Peer list data structure initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking();
        if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        log_shutdown();
        return 1;
    }
    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    sprintf(ui_message_buffer, "My IP: %s, Username: %s\r", gMyLocalIPStr, gMyUsername);
    AppendToMessagesTE(ui_message_buffer);
    log_debug("Dialog initialized. Entering main event loop...");
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
            OSErr current_quit_err = MacTCP_QueueMessage(
                                         gPeerManager.peers[i].ip,
                                         "",
                                         MSG_QUIT);
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
    
    /* Process the message queue to ensure QUIT messages are sent */
    if (quit_sent_count > 0) {
        log_debug("Processing message queue for QUIT messages...");
        unsigned long startTime = TickCount();
        unsigned long timeoutTicks = 180; /* 3 seconds timeout */
        
        while ((TickCount() - startTime) < timeoutTicks) {
            /* Process TCP state machine to handle async operations */
            ProcessTCPStateMachine(YieldTimeToSystem);
            
            /* Check if all messages have been sent */
            if (GetQueuedMessageCount() == 0 && GetTCPSendStreamState() == TCP_STATE_IDLE) {
                log_debug("All QUIT messages processed");
                break;
            }
            
            /* Give time to system */
            YieldTimeToSystem();
        }
    }
    
    if (quit_active_peers > 0) {
        sprintf(ui_message_buffer, "Finished sending QUIT. Sent to %d of %d active peers. Last error (if any): %d", quit_sent_count, quit_active_peers, (int)last_quit_err);
        log_app_event("%s", ui_message_buffer);
        AppendToMessagesTE(ui_message_buffer);
        AppendToMessagesTE("\r");
    } else {
        log_app_event("No active peers to send QUIT messages to.");
        AppendToMessagesTE("No active peers to send QUIT messages to.\r");
    }
    if (last_quit_err != noErr) {
        log_debug("Warning: Sending QUIT messages encountered error: %d.", (int)last_quit_err);
    }
    CleanupDialog();
    log_debug("Dialog resources cleaned up.");
    CleanupNetworking();
    log_debug("Networking stack cleaned up.");
    if (gAEQuitAppUPP) {
        log_debug("Disposing AEQuitAppUPP.");
        DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        gAEQuitAppUPP = NULL;
    }
    log_app_event("Application terminated gracefully.");
    log_shutdown();
    return 0;
}
void InitializeToolbox(void)
{
    Handle menuBar;
    MenuHandle appleMenu;
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    menuBar = GetNewMBar(128);
    if (menuBar == NULL) {
        log_app_event("CRITICAL: GetNewMBar(128) failed! Check MBAR resource. Cannot proceed with menus.");
    } else {
        SetMenuBar(menuBar);
        appleMenu = GetMenuHandle(kAppleMenuID);
        if (appleMenu) {
            AppendResMenu(appleMenu, 'DRVR');
        } else {
            log_debug("Warning: Could not get Apple Menu (ID %d). Desk Accessories may not be available.", kAppleMenuID);
        }
        DrawMenuBar();
        log_debug("Menu bar initialized and drawn.");
    }
    InstallAppleEventHandlers();
    InitCursor();
}
void InstallAppleEventHandlers(void)
{
    OSErr err;
    log_debug("InstallAppleEventHandlers: Entry.");
    if (gAEQuitAppUPP == NULL) {
        gAEQuitAppUPP = NewAEEventHandlerUPP(MyAEQuitApplication);
        if (gAEQuitAppUPP == NULL) {
            log_app_event("CRITICAL: NewAEEventHandlerUPP failed for MyAEQuitApplication! AppleEvent Quit may not work.");
            return;
        }
    }
    err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, gAEQuitAppUPP,
                                0L,
                                false);
    if (err != noErr) {
        log_app_event("CRITICAL: AEInstallEventHandler failed for kAEQuitApplication: %d", err);
    } else {
        log_debug("InstallAppleEventHandlers: kAEQuitApplication handler installed.");
    }
    log_debug("InstallAppleEventHandlers: Exit.");
}
pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon)
{
#pragma unused(theAppleEvent, reply, handlerRefCon)
    log_app_event("MyAEQuitApplication: Received kAEQuitApplication Apple Event. Setting gDone=true.");
    gDone = true;
    return noErr;
}
void HandleMenuChoice(long menuResult)
{
    short menuID = HiWord(menuResult);
    short menuItem = LoWord(menuResult);
    Str255 daName;
    log_debug("HandleMenuChoice: menuID=%d, menuItem=%d", menuID, menuItem);
    switch (menuID) {
    case kAppleMenuID:
        if (menuItem == kAboutItem) {
            log_app_event("HandleMenuChoice: 'About csend-mac...' selected.");
            Alert(129, nil);
        } else {
            MenuHandle appleMenu = GetMenuHandle(kAppleMenuID);
            if (appleMenu) {
                GetMenuItemText(appleMenu, menuItem, daName);
                OpenDeskAcc(daName);
                log_debug("HandleMenuChoice: Desk Accessory '%p' selected.", daName);
            }
        }
        break;
    case kFileMenuID:
        if (menuItem == kQuitItem) {
            log_app_event("HandleMenuChoice: File->Quit selected by user. Setting gDone=true.");
            gDone = true;
        }
        break;
    default:
        log_debug("HandleMenuChoice: Unhandled menuID %d.", menuID);
        break;
    }
    HiliteMenu(0);
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
                if (windowPart == inMenuBar) {
                    log_debug("MainEventLoop: MouseDown inMenuBar.");
                    long menuResult = MenuSelect(event.where);
                    if (HiWord(menuResult) != 0) {
                        HandleMenuChoice(menuResult);
                    }
                    eventHandledByApp = true;
                } else if (whichWindow == (WindowPtr)gMainWindow && windowPart == inContent) {
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
                            if (newValue != oldValue) {
                                ScrollMessagesTEToValue(newValue);
                            }
                        } else {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandledByApp = true;
                    } else if (!eventHandledByApp && gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        log_debug("MouseDown: Click potentially in Peer List user item.");
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else if (!eventHandledByApp) {
                        Rect inputTERectDITL;
                        DialogItemType itemTypeDITL;
                        Handle itemHandleDITL;
                        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                        if (PtInRect(localPt, &inputTERectDITL)) {
                            log_debug("MouseDown: Click in Input TE user item.");
                            HandleInputTEClick(gMainWindow, &event);
                            eventHandledByApp = true;
                        }
                    }
                    SetPort(oldPort);
                }
            }
            if (!eventHandledByApp && IsDialogEvent(&event)) {
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
                            }
                            break;
                        case kMessagesScrollbar:
                            log_debug("DialogSelect returned kMessagesScrollbar (item %d). Typically handled by FindControl.", itemHit);
                            break;
                        default:
                            log_debug("DialogSelect unhandled item: %d", itemHit);
                            break;
                        }
                    }
                    eventHandledByApp = true;
                }
            }
            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        } else {
        }
    }
}
void HandleIdleTasks(void)
{
    unsigned long currentTimeTicks = TickCount();
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    ProcessTCPStateMachine(YieldTimeToSystem);
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (gLastPeerListUpdateTime == 0 ||
            (currentTimeTicks < gLastPeerListUpdateTime) ||
            (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            PruneTimedOutPeers();
            UpdatePeerDisplayList(false);
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
        case inSysWindow:
            SystemClick(event, whichWindow);
            break;
        case inDrag:
            if (whichWindow == (WindowPtr)gMainWindow)
                DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            break;
        case inGoAway:
            if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) {
                log_debug("Close box clicked on main window. Setting gDone = true.");
                gDone = true;
            }
            break;
        case inContent:
            if (whichWindow == (WindowPtr)gMainWindow && whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else if (whichWindow != (WindowPtr)gMainWindow && whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else {
                log_debug("HandleEvent: mouseDown in content of front window (unhandled by specific checks). Window: 0x%lX", (unsigned long)whichWindow);
            }
            break;
        default:
            log_debug("HandleEvent: mouseDown in unknown window part: %d", windowPart);
            break;
        }
        break;
    case keyDown:
    case autoKey:
        theChar = event->message & charCodeMask;
        if ((event->modifiers & cmdKey) != 0) {
            long menuResult = MenuKey(theChar);
            if (HiWord(menuResult) != 0) {
                HandleMenuChoice(menuResult);
            }
        } else {
            if (!HandleInputTEKeyDown(event)) {
            }
        }
        break;
    case updateEvt:
        whichWindow = (WindowPtr)event->message;
        BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) {
            DrawDialog(whichWindow);
            UpdateDialogControls();
        } else {
        }
        EndUpdate(whichWindow);
        break;
    case activateEvt:
        whichWindow = (WindowPtr)event->message;
        Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
        if (whichWindow == (WindowPtr)gMainWindow) {
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
        } else {
        }
        break;
    case kHighLevelEvent:
        OSErr aeErr = AEProcessAppleEvent(event);
        if (aeErr != noErr && aeErr != errAEEventNotHandled) {
            log_debug("HandleEvent: AEProcessAppleEvent returned error: %d", aeErr);
        } else if (aeErr == errAEEventNotHandled) {
        }
        break;
    case osEvt:
        log_debug("HandleEvent: osEvt, message: 0x%lX (HighByte: 0x%lX)", event->message, (event->message >> 24) & 0xFF);
        break;
    default:
        break;
    }
}
