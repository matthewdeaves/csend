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
#include <OSUtils.h>
#include <Resources.h>
#include <Memory.h>
#include <stdlib.h>
#include <Sound.h>
#include <stdio.h>
#include <string.h>
#ifndef HiWord
#define HiWord(x) ((short)(((long)(x) >> 16) & 0xFFFF))
#endif
#ifndef LoWord
#define LoWord(x) ((short)((long)(x) & 0xFFFF))
#endif
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
extern Boolean gSystemInitiatedQuit;
unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
const unsigned long kQuitMessageDelayTicks = 120;
static unsigned long gMainLoopIterations = 0;
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
    OSErr err;
    Boolean dialogOk;
    char ui_message_buffer[256];
    platform_logging_callbacks_t classic_mac_log_callbacks = {
        .get_timestamp = classic_mac_platform_get_timestamp,
        .display_debug_log = classic_mac_platform_display_debug_log
    };
    log_init("csend_mac.log", &classic_mac_log_callbacks);
    log_debug("main: Application starting.");
    MaxApplZone();
    log_debug("main: MaxApplZone called.");
    InitializeToolbox();
    if (gSystemInitiatedQuit) {
        log_app_event("main: System quit during InitializeToolbox. Exiting.");
        if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        log_shutdown();
        return 0;
    }
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("main: Toolbox Initialized.");
    InitPeerList();
    if (gSystemInitiatedQuit) { log_app_event("main: System quit after InitPeerList. Exiting."); if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP); log_shutdown(); return 0; }
    log_debug("main: Peer list initialized.");
    dialogOk = InitDialog();
    if (gSystemInitiatedQuit) {
        log_app_event("main: System quit during InitDialog. Exiting.");
        if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        log_shutdown();
        return 0;
    }
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        if (gAEQuitAppUPP) DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        log_shutdown();
        return 1;
    }
    log_debug("main: Dialog initialized.");
    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    log_debug("main: Entering main event loop...");
    MainEventLoop();
    log_debug("main: Exited main event loop.");
    if (gSystemInitiatedQuit) {
        log_app_event("main: System-initiated quit processing. Minimal cleanup.");
        if (gMainWindow) CleanupDialog();
    } else {
        log_app_event("main: User-initiated quit. Initiating graceful shutdown sequence...");
        AppendToMessagesTE("Shutting down...\r");
        int quit_sent_count = 0;
        int quit_active_peers = 0;
        OSErr last_quit_err = noErr;
        unsigned long dummyTimerForDelay;
        log_debug("main: Sending QUIT messages to peers...");
        for (int i = 0; i < MAX_PEERS; i++) {
            if (gPeerManager.peers[i].active) {
                quit_active_peers++;
                OSErr current_quit_err = MacTCP_SendMessageSync(
                                             gPeerManager.peers[i].ip, "", MSG_QUIT, gMyUsername, gMyLocalIPStr, YieldTimeToSystem);
                if (current_quit_err == noErr) quit_sent_count++;
                else {
                    log_debug("main: Failed to send QUIT to %s@%s: Error %d", gPeerManager.peers[i].username, gPeerManager.peers[i].ip, (int)current_quit_err);
                    if (last_quit_err == noErr || (last_quit_err == streamBusyErr && current_quit_err != streamBusyErr)) last_quit_err = current_quit_err;
                }
                YieldTimeToSystem();
                Delay(kQuitMessageDelayTicks, &dummyTimerForDelay);
            }
        }
        if (quit_active_peers > 0) {
            sprintf(ui_message_buffer, "Finished sending QUIT messages. Sent to %d of %d active peers. Last error (if any): %d", quit_sent_count, quit_active_peers, (int)last_quit_err);
        } else {
            strcpy(ui_message_buffer, "No active peers to send QUIT messages to.");
        }
        AppendToMessagesTE(ui_message_buffer);
        AppendToMessagesTE("\r");
        log_app_event("%s", ui_message_buffer);
        log_debug("main: Cleaning up dialog...");
        CleanupDialog();
        log_debug("main: Cleaning up networking...");
        log_app_event("Application terminated gracefully by user.");
    }
    if (gAEQuitAppUPP) {
        log_debug("main: Disposing AEQuitAppUPP.");
        DisposeAEEventHandlerUPP(gAEQuitAppUPP);
        gAEQuitAppUPP = NULL;
    }
    log_debug("main: Shutting down log.");
    log_shutdown();
    log_debug("main: Application returning 0.");
    return 0;
}
void InitializeToolbox(void)
{
    log_debug("InitializeToolbox: Entry.");
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    log_debug("InitializeToolbox: Initializing Menus.");
    InitMenus();
    Handle menuBar = GetNewMBar(128);
    if (menuBar == NULL) {
        log_debug("CRITICAL: GetNewMBar(128) failed! Check MBAR resource. App will likely be unstable.");
    } else {
        log_debug("InitializeToolbox: GetNewMBar(128) successful. Handle: 0x%lX", (unsigned long)menuBar);
        SetMenuBar(menuBar);
        MenuHandle appleMenu = GetMenuHandle(kAppleMenuID);
        if (appleMenu) {
            AppendResMenu(appleMenu, 'DRVR');
            log_debug("InitializeToolbox: Apple Menu (ID %d) DAs added using AppendResMenu.", kAppleMenuID);
        } else {
            log_debug("Warning: Could not get Apple Menu (ID %d).", kAppleMenuID);
        }
        DrawMenuBar();
        log_debug("InitializeToolbox: Menu bar set and drawn.");
    }
    TEInit();
    InitDialogs(NULL);
    log_debug("InitializeToolbox: Installing Apple Event Handlers.");
    InstallAppleEventHandlers();
    InitCursor();
    log_debug("InitializeToolbox: Exit.");
}
void InstallAppleEventHandlers(void)
{
    OSErr err;
    log_debug("InstallAppleEventHandlers: Entry.");
    if (gAEQuitAppUPP == NULL) {
        gAEQuitAppUPP = NewAEEventHandlerUPP(MyAEQuitApplication);
        if (gAEQuitAppUPP == NULL) {
            log_debug("CRITICAL: NewAEEventHandlerUPP failed for MyAEQuitApplication!");
            return;
        }
    }
    err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, gAEQuitAppUPP, 0L, false);
    if (err != noErr) {
        log_debug("CRITICAL: AEInstallEventHandler failed for kAEQuitApplication: %d", err);
    } else {
        log_debug("InstallAppleEventHandlers: kAEQuitApplication handler installed.");
    }
    log_debug("InstallAppleEventHandlers: Exit.");
}
pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon)
{
#pragma unused(theAppleEvent, reply, handlerRefCon)
    log_app_event("MyAEQuitApplication: Received kAEQuitApplication Apple Event.");
    gSystemInitiatedQuit = true;
    gDone = true;
    log_debug("MyAEQuitApplication: Set gSystemInitiatedQuit=true, gDone=true.");
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
            log_app_event("HandleMenuChoice: File->Quit selected by user.");
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
    gMainLoopIterations = 0;
    log_debug("MainEventLoop: Entered.");
    while (!gDone) {
        gMainLoopIterations++;
        if ((gMainLoopIterations % 600) == 0) {
            log_debug("MainEventLoop: Iteration %lu. gDone=%d, gSystemInitiatedQuit=%d", gMainLoopIterations, gDone, gSystemInitiatedQuit);
        }
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        IdleInputTE();
        HandleIdleTasks();
        if (gDone) break;
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        if (gDone) break;
        if (gotEvent) {
            Boolean eventHandledByApp = false;
            if (event.what == mouseDown) {
                WindowPtr whichWindow;
                short windowPart = FindWindow(event.where, &whichWindow);
                if (windowPart == inMenuBar) {
                    log_debug("MainEventLoop iter %lu: MouseDown inMenuBar.", gMainLoopIterations);
                    long menuResult = MenuSelect(event.where);
                    if (HiWord(menuResult) != 0) HandleMenuChoice(menuResult);
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
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 && (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
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
                        if (HandlePeerListClick(gMainWindow, &event)) {
                            eventHandledByApp = true;
                        }
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
                                    set_debug_output_enabled(GetControlValue(controlH) == 1);
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
                                    if (GetControlValue(controlH) == 1) DialogPeerList_DeselectAll();
                                    GetPort(&oldPortForDrawing);
                                    SetPort(GetWindowPort(gMainWindow));
                                    InvalRect(&itemRect);
                                    SetPort(oldPortForDrawing);
                                }
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
        }
    }
    log_debug("MainEventLoop: Exited loop. gDone=%d, gSystemInitiatedQuit=%d, Iterations=%lu", gDone, gSystemInitiatedQuit, gMainLoopIterations);
}
void HandleIdleTasks(void)
{
    if (gDone || gSystemInitiatedQuit) return;
    unsigned long currentTimeTicks = TickCount();
    if (gLastPeerListUpdateTime == 0 || (currentTimeTicks < gLastPeerListUpdateTime)
            || (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) {
            UpdatePeerDisplayList(false);
        }
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}
void HandleEvent(EventRecord *event)
{
    if (gSystemInitiatedQuit && event->what != kHighLevelEvent && event->what != osEvt) {
        return;
    }
    short windowPart;
    WindowPtr whichWindow;
    char theChar;
    WindowPeek peek;
    switch (event->what) {
    case mouseDown:
        if (gSystemInitiatedQuit) { return; }
        windowPart = FindWindow(event->where, &whichWindow);
        switch (windowPart) {
        case inSysWindow:
            SystemClick(event, whichWindow);
            break;
        case inDrag:
            DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            break;
        case inGoAway:
            if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) {
                log_app_event("HandleEvent: Close box clicked, setting gDone=true.");
                gDone = true;
            }
            break;
        case inContent:
            if (whichWindow != FrontWindow()) {
                log_debug("HandleEvent: mouseDown inContent of non-front window 0x%lX. Calling SelectWindow.", (unsigned long)whichWindow);
                SelectWindow(whichWindow);
            }
            break;
        default:
            break;
        }
        break;
    case keyDown:
    case autoKey:
        if (gSystemInitiatedQuit) { return; }
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
        if (gSystemInitiatedQuit) { return; }
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
        Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
        log_debug("HandleEvent: >>> activateEvt for window 0x%lX. BecomingActive: %d. <<<", (unsigned long)whichWindow, becomingActive);
        if (whichWindow == (WindowPtr)gMainWindow) {
            GrafPtr oldPort;
            log_debug("HandleEvent: activateEvt - gMainWindow (0x%lX) is target.", (unsigned long)gMainWindow);
            GetPort(&oldPort);
            if (GetWindowPort(gMainWindow)) {
                SetPort(GetWindowPort(gMainWindow));
                ActivateDialogTE(becomingActive);
                ActivatePeerList(becomingActive);
                if (gMessagesScrollBar != NULL) {
                    short hiliteValue = (becomingActive && GetControlMaximum(gMessagesScrollBar) > 0 && (**gMessagesScrollBar).contrlVis) ? 0 : 255;
                    HiliteControl(gMessagesScrollBar, hiliteValue);
                }
                SetPort(oldPort);
            } else {
                log_debug("HandleEvent: activateEvt - gMainWindow port is NULL, skipping SetPort and activations.");
            }
        } else {
            log_debug("HandleEvent: activateEvt for a window OTHER than gMainWindow: 0x%lX (likely a DA).", (unsigned long)whichWindow);
            if (IsDialogEvent(event)) {
                log_debug("HandleEvent: activateEvt for DA was also a DialogEvent. DialogSelect might handle it.");
            }
            if (!becomingActive && FrontWindow() == (WindowPtr)gMainWindow) {
                log_debug("HandleEvent: activateEvt - DA window 0x%lX deactivated, gMainWindow is now front. Invalidating gMainWindow portRect.", (unsigned long)whichWindow);
                if (gMainWindow) InvalRect(&gMainWindow->portRect);
            }
        }
        log_debug("HandleEvent: >>> activateEvt processing complete for window 0x%lX. <<<", (unsigned long)whichWindow);
        break;
    case kHighLevelEvent:
        log_debug("HandleEvent: kHighLevelEvent received. Calling AEProcessAppleEvent.");
        OSErr aeErr = AEProcessAppleEvent(event);
        if (aeErr != noErr && aeErr != errAEEventNotHandled) {
            log_debug("HandleEvent: AEProcessAppleEvent returned error: %d", aeErr);
        } else if (aeErr == errAEEventNotHandled) {
            log_debug("HandleEvent: AEProcessAppleEvent: errAEEventNotHandled (event was not for us or not one we handle).");
        }
        break;
    case osEvt:
        log_debug("HandleEvent: osEvt, message high byte: 0x%lX, full message: 0x%lX.", (event->message >> 24) & 0xFF, event->message);
        switch ((event->message >> 24) & 0xFF) {
        case suspendResumeMessage: {
            Boolean isResume = ((event->message & resumeMask) != 0);
            if (!isResume) {
                log_debug("HandleEvent: osEvt is SUSPEND event. Application is being backgrounded.");
            } else {
                log_debug("HandleEvent: osEvt is RESUME event. Application is being foregrounded.");
                if (gSystemInitiatedQuit) {
                    log_debug("HandleEvent: osEvt (resume) ignored, gSystemInitiatedQuit is true (app is quitting).");
                } else {
                    if (gMainWindow) {
                        log_debug("HandleEvent: osEvt (resume) - Invalidating gMainWindow portRect to ensure redraw.");
                        InvalRect(&gMainWindow->portRect);
                    }
                }
            }
            break;
        }
        default:
            log_debug("HandleEvent: Unhandled osEvt type (high byte of message): 0x%lX", (event->message >> 24) & 0xFF);
            break;
        }
        break;
    default:
        break;
    }
}
