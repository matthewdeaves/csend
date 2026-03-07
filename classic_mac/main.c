/*
 * Classic Mac P2P Messenger - PeerTalk Implementation
 *
 * Single-threaded event-driven architecture using PeerTalk SDK
 * for all networking. PT_Poll() is called from HandleIdleTasks()
 * in the main event loop.
 */

/* Required Toolbox headers in dependency order */
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
#include <OSUtils.h>
#include <Sound.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "clog.h"
#include "peertalk.h"
#include "peertalk_bridge.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_input.h"
#include "dialog_messages.h"
#include "../shared/common_defs.h"
#include "../shared/test.h"
#include "test.h"

/* Compatibility macros for extracting high/low words from MenuSelect result */
#ifndef HiWord
#define HiWord(x) ((short)(((long)(x) >> 16) & 0xFFFF))
#endif
#ifndef LoWord
#define LoWord(x) ((short)((long)(x) & 0xFFFF))
#endif

/* Global application state */
Boolean gDone = false;
PT_Context *g_ctx = NULL;

/* Timing constants using TickCount() (60 ticks per second) */
unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;  /* 5 seconds */

#define kAppleMenuID 1
#define kFileMenuID 128
#define kAboutItem 1
#define kPerformTestItem 1
#define kQuitItem 2

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
    Boolean dialogOk;
    PT_Status st;

    clog_init("csend_mac.log", CLOG_LVL_INFO);

    /* Expand application heap to maximum before Toolbox initialization */
    MaxApplZone();
    InitializeToolbox();

    CLOG_INFO("Starting Classic Mac P2P Messenger...");

    /* Initialize PeerTalk */
    st = PT_Init(&g_ctx, "Mac");
    if (st != PT_OK) {
        CLOG_ERR("PT_Init failed: %d", (int)st);
        StopAlert(128, nil);
        if (gAEQuitAppUPP) {
            DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        }
        clog_shutdown();
        return 1;
    }

    /* Register message types and callbacks */
    bridge_mac_init(g_ctx);

    CLOG_INFO("PeerTalk initialized.");

    dialogOk = InitDialog();
    if (!dialogOk) {
        CLOG_ERR("Dialog initialization failed. Exiting.");
        PT_Shutdown(g_ctx);
        if (gAEQuitAppUPP) {
            DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        }
        clog_shutdown();
        return 1;
    }

    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");

    /* Start peer discovery */
    st = PT_StartDiscovery(g_ctx);
    if (st != PT_OK) {
        CLOG_WARN("PT_StartDiscovery failed: %d", (int)st);
        AppendToMessagesTE("Warning: Discovery failed to start.\r");
    } else {
        CLOG_INFO("Peer discovery started.");
        AppendToMessagesTE("Peer discovery started.\r");
    }

    CLOG_INFO("Dialog initialized. Entering main event loop...");

    MainEventLoop();

    CLOG_INFO("Initiating shutdown sequence...");
    AppendToMessagesTE("Shutting down...\r");

    PT_Shutdown(g_ctx);
    g_ctx = NULL;
    CLOG_INFO("PeerTalk shut down.");

    CleanupDialog();
    CLOG_INFO("Dialog resources cleaned up.");

    if (gAEQuitAppUPP) {
        DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        gAEQuitAppUPP = NULL;
    }

    CLOG_INFO("Application terminated gracefully.");
    clog_shutdown();
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
        CLOG_ERR("GetNewMBar(128) failed! Check MBAR resource.");
    } else {
        SetMenuBar(menuBar);
        appleMenu = GetMenuHandle(kAppleMenuID);
        if (appleMenu) {
            AppendResMenu(appleMenu, 'DRVR');
        }
        DrawMenuBar();
    }

    InstallAppleEventHandlers();
    InitCursor();
}

void InstallAppleEventHandlers(void)
{
    OSErr err;

    if (gAEQuitAppUPP == NULL) {
        gAEQuitAppUPP = NewAEEventHandlerUPP(MyAEQuitApplication);
        if (gAEQuitAppUPP == NULL) {
            CLOG_ERR("NewAEEventHandlerUPP failed for MyAEQuitApplication!");
            return;
        }
    }

    err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, gAEQuitAppUPP,
                                0L, false);
    if (err != noErr) {
        CLOG_ERR("AEInstallEventHandler failed for kAEQuitApplication: %d", err);
    }
}

pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon)
{
    (void)theAppleEvent;
    (void)reply;
    (void)handlerRefCon;
    CLOG_INFO("Received kAEQuitApplication Apple Event. Setting gDone=true.");
    gDone = true;
    return noErr;
}

void HandleMenuChoice(long menuResult)
{
    short menuID = HiWord(menuResult);
    short menuItem = LoWord(menuResult);
    Str255 daName;

    switch (menuID) {
    case kAppleMenuID:
        if (menuItem == kAboutItem) {
            Alert(129, nil);
        } else {
            MenuHandle appleMenu = GetMenuHandle(kAppleMenuID);
            if (appleMenu) {
                GetMenuItemText(appleMenu, menuItem, daName);
                OpenDeskAcc(daName);
            }
        }
        break;
    case kFileMenuID:
        if (menuItem == kPerformTestItem) {
            CLOG_INFO("File->Perform Test selected");
            if (!is_automated_test_running()) {
                PerformAutomatedTest();
            } else {
                CLOG_INFO("Test is already in progress.");
            }
        } else if (menuItem == kQuitItem) {
            CLOG_INFO("File->Quit selected. Setting gDone=true.");
            gDone = true;
        }
        break;
    default:
        break;
    }
    HiliteMenu(0);
}

void MainEventLoop(void)
{
    EventRecord event;
    Boolean gotEvent;
    long sleepTime = 15L; /* 15 ticks = 250ms for TextEdit cursor blinking */
    static unsigned long lastIdleTime = 0;

    while (!gDone) {
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
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event);
                    } else if (!eventHandledByApp) {
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
                                clog_set_level(GetControlValue(controlH) ? CLOG_LVL_DBG : CLOG_LVL_INFO);
                                CLOG_INFO("Debug output %s.", GetControlValue(controlH) ? "ENABLED" : "DISABLED");
                            }
                            break;
                        case kBroadcastCheckbox:
                            GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
                            if (itemHandle && itemType == (ctrlItem + chkCtrl)) {
                                controlH = (ControlHandle)itemHandle;
                                currentValue = GetControlValue(controlH);
                                SetControlValue(controlH, !currentValue);
                                if (GetControlValue(controlH) == 1) {
                                    DialogPeerList_DeselectAll();
                                }
                            }
                            break;
                        default:
                            break;
                        }
                    }
                    eventHandledByApp = true;
                }
            }

            if (!eventHandledByApp) {
                HandleEvent(&event);
            }
        }
    }
}

void HandleIdleTasks(void)
{
    unsigned long currentTimeTicks = TickCount();

    /* Poll peertalk for network events */
    PT_Poll(g_ctx);

    /* Process automated test if running */
    process_automated_test();

    /* Periodic peer list refresh */
    if (gPeerListNeedsRefresh ||
            gLastPeerListUpdateTime == 0 ||
            (currentTimeTicks < gLastPeerListUpdateTime) ||
            (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false);
        }
        gPeerListNeedsRefresh = false;
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
                gDone = true;
            }
            break;
        case inContent:
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            }
            break;
        default:
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
            HandleInputTEKeyDown(event);
        }
        break;
    case updateEvt:
        whichWindow = (WindowPtr)event->message;
        BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) {
            DrawDialog(whichWindow);
            static unsigned long lastUpdateTime = 0;
            unsigned long currentTime = TickCount();
            if (currentTime - lastUpdateTime >= 6) {
                UpdateDialogControls();
                lastUpdateTime = currentTime;
            }
        }
        EndUpdate(whichWindow);
        break;
    case activateEvt:
        whichWindow = (WindowPtr)event->message;
        {
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
            }
        }
        break;
    case kHighLevelEvent:
        {
            OSErr aeErr = AEProcessAppleEvent(event);
            if (aeErr != noErr && aeErr != errAEEventNotHandled) {
                CLOG_ERR("AEProcessAppleEvent error: %d", aeErr);
            }
        }
        break;
    default:
        break;
    }
}
