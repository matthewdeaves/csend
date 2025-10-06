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
#include "logging_mac.h"
#include "network_init.h"
#include "discovery.h"
#include "messaging.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "../shared/peer_wrapper.h"
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
    log_debug_cat(LOG_CAT_SYSTEM, "MaxApplZone called. Toolbox Initialized.");
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        sprintf(ui_message_buffer, "Fatal: Network initialization failed (Error: %d). Application cannot continue.", (int)networkErr);
        log_app_event("%s", ui_message_buffer);
        Str255 pErrorMsg;
        sprintf((char *)pErrorMsg + 1, "Network Init Failed: %d. See log.", (int)networkErr);
        pErrorMsg[0] = strlen((char *)pErrorMsg + 1);
        StopAlert(128, nil);
        if (gAEQuitAppUPP) {
            DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        }
        log_shutdown();
        return 1;
    }
    log_info_cat(LOG_CAT_NETWORKING, "Networking stack initialized.");
    InitPeerList();
    log_debug_cat(LOG_CAT_PEER_MGMT, "Peer list data structure initialized.");
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking();
        if (gAEQuitAppUPP) {
            DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        }
        log_shutdown();
        return 1;
    }
    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    sprintf(ui_message_buffer, "My IP: %s, Username: %s\r", gMyLocalIPStr, gMyUsername);
    AppendToMessagesTE(ui_message_buffer);
    log_info_cat(LOG_CAT_UI, "Dialog initialized. Entering main event loop...");
    MainEventLoop();
    log_debug_cat(LOG_CAT_SYSTEM, "Exited main event loop.");
    log_app_event("Initiating shutdown sequence...");
    AppendToMessagesTE("Shutting down...\r");

    /* Send quit message via UDP broadcast */
    OSErr quit_err = BroadcastQuitMessage(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (quit_err != noErr) {
        log_warning_cat(LOG_CAT_MESSAGING, "Failed to broadcast quit message: %d", (int)quit_err);
    }
    CleanupDialog();
    log_debug_cat(LOG_CAT_UI, "Dialog resources cleaned up.");
    CleanupNetworking();
    log_debug_cat(LOG_CAT_NETWORKING, "Networking stack cleaned up.");
    if (gAEQuitAppUPP) {
        log_debug_cat(LOG_CAT_SYSTEM, "Disposing AEQuitAppUPP.");
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
            log_warning_cat(LOG_CAT_UI, "Could not get Apple Menu (ID %d). Desk Accessories may not be available.", kAppleMenuID);
        }
        DrawMenuBar();
        log_debug_cat(LOG_CAT_UI, "Menu bar initialized and drawn.");
    }
    InstallAppleEventHandlers();
    InitCursor();
}
void InstallAppleEventHandlers(void)
{
    OSErr err;
    log_debug_cat(LOG_CAT_SYSTEM, "InstallAppleEventHandlers: Entry.");
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
        log_debug_cat(LOG_CAT_SYSTEM, "InstallAppleEventHandlers: kAEQuitApplication handler installed.");
    }
    log_debug_cat(LOG_CAT_SYSTEM, "InstallAppleEventHandlers: Exit.");
}
pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon)
{
    (void)theAppleEvent; /* Unused parameter */
    (void)reply; /* Unused parameter */
    (void)handlerRefCon; /* Unused parameter */
    log_app_event("MyAEQuitApplication: Received kAEQuitApplication Apple Event. Setting gDone=true.");
    gDone = true;
    return noErr;
}
void HandleMenuChoice(long menuResult)
{
    short menuID = HiWord(menuResult);
    short menuItem = LoWord(menuResult);
    Str255 daName;
    log_debug_cat(LOG_CAT_UI, "HandleMenuChoice: menuID=%d, menuItem=%d", menuID, menuItem);
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
                log_debug_cat(LOG_CAT_UI, "HandleMenuChoice: Desk Accessory '%p' selected.", daName);
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
        log_debug_cat(LOG_CAT_UI, "HandleMenuChoice: Unhandled menuID %d.", menuID);
        break;
    }
    HiliteMenu(0);
}
void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 15L; /* 15 ticks = 250ms, optimal for TextEdit per Inside Macintosh */
    static unsigned long lastIdleTime = 0;
    while (!gDone) {
        /* Only call TEIdle at cursor blink rate (15 ticks) to reduce updates */
        unsigned long currentTime = TickCount();
        if (currentTime - lastIdleTime >= 15) {
            if (gMessagesTE != NULL) TEIdle(gMessagesTE);
            IdleInputTE();
            lastIdleTime = currentTime;
        }
        HandleIdleTasks();
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        if (gotEvent) {
            Boolean eventHandledByApp = false;
            if (event.what == mouseDown) {
                WindowPtr whichWindow;
                short windowPart = FindWindow(event.where, &whichWindow);
                if (windowPart == inMenuBar) {
                    log_debug_cat(LOG_CAT_UI, "MainEventLoop: MouseDown inMenuBar.");
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
                    SetPort((GrafPtr)GetWindowPort(gMainWindow));
                    GlobalToLocal(&localPt);
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 &&
                            (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
                        log_debug_cat(LOG_CAT_UI, "MouseDown: Click in Messages Scrollbar (part %d).", foundControlPart);
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
                        log_debug_cat(LOG_CAT_UI, "MouseDown: Click potentially in Peer List user item.");
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else if (!eventHandledByApp) {
                        Rect inputTERectDITL;
                        DialogItemType itemTypeDITL;
                        Handle itemHandleDITL;
                        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL);
                        if (PtInRect(localPt, &inputTERectDITL)) {
                            log_debug_cat(LOG_CAT_UI, "MouseDown: Click in Input TE user item.");
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
                                log_info_cat(LOG_CAT_SYSTEM, "Debug output %s.", newState ? "ENABLED" : "DISABLED");
                            }
                            break;
                        case kBroadcastCheckbox:
                            GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                            if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                controlH = (ControlHandle)itemHandle;
                                currentValue = GetControlValue(controlH);
                                SetControlValue(controlH, !currentValue);
                                if (GetControlValue(controlH) == 1) {
                                    log_debug_cat(LOG_CAT_UI, "Broadcast checkbox checked. Deselecting peer.");
                                    DialogPeerList_DeselectAll();
                                } else {
                                    log_debug_cat(LOG_CAT_UI, "Broadcast checkbox unchecked.");
                                }
                            }
                            break;
                        case kMessagesScrollbar:
                            log_debug_cat(LOG_CAT_UI, "DialogSelect returned kMessagesScrollbar (item %d). Typically handled by FindControl.", itemHit);
                            break;
                        default:
                            log_debug_cat(LOG_CAT_UI, "DialogSelect unhandled item: %d", itemHit);
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
                log_debug_cat(LOG_CAT_UI, "Close box clicked on main window. Setting gDone = true.");
                gDone = true;
            }
            break;
        case inContent:
            if (whichWindow == (WindowPtr)gMainWindow && whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else if (whichWindow != (WindowPtr)gMainWindow && whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else {
                log_debug_cat(LOG_CAT_UI, "HandleEvent: mouseDown in content of front window (unhandled by specific checks). Window: 0x%lX", (unsigned long)whichWindow);
            }
            break;
        default:
            log_debug_cat(LOG_CAT_UI, "HandleEvent: mouseDown in unknown window part: %d", windowPart);
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
            /* Throttle dialog control updates to reduce excessive redraws */
            static unsigned long lastUpdateTime = 0;
            unsigned long currentTime = TickCount();
            if (currentTime - lastUpdateTime >= 6) { /* ~100ms throttle */
                UpdateDialogControls();
                lastUpdateTime = currentTime;
            }
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
            log_error_cat(LOG_CAT_SYSTEM, "HandleEvent: AEProcessAppleEvent returned error: %d", aeErr);
        } else if (aeErr == errAEEventNotHandled) {
        }
        break;
    case osEvt:
        log_debug_cat(LOG_CAT_SYSTEM, "HandleEvent: osEvt, message: 0x%lX (HighByte: 0x%lX)", event->message, (event->message >> 24) & 0xFF);
        break;
    default:
        break;
    }
}
