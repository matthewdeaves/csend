// FILE: ./classic_mac/main.c
// REASON: Implemented File->Quit menu, Cmd-Q, and a basic kAEQuitApplication Apple Event handler.
//         Uses AppendResMenu instead of AddResMenu due to previous linker issues.
//         Manually defines HiWord/LoWord.
//         Differentiates between user-initiated quits (graceful) and system-initiated quits (fast/aggressive).
//         Includes diagnostic logging.

#include <MacTypes.h>   // For HiWord, LoWord, Point, Rect, Boolean, OSErr, Handle, Ptr, etc.
#include <Quickdraw.h>  // For GrafPtr, Rect, Point, qd global
#include <Fonts.h>      // For InitFonts, FontInfo
#include <Events.h>     // For EventRecord, WaitNextEvent, GetKeys, modifiers
#include <Windows.h>    // For WindowPtr, FindWindow, DragWindow, SelectWindow, FrontWindow
#include <TextEdit.h>   // For TEHandle, TEInit, TEClick, TEKey, TEIdle, etc.
#include <Dialogs.h>    // For DialogPtr, GetNewDialog, IsDialogEvent, DialogSelect, Alert
#include <Menus.h>      // For MenuHandle, InitMenus, GetNewMBar, SetMenuBar, DrawMenuBar, MenuSelect, MenuKey, AppendResMenu, HiliteMenu, GetMenuItemText, OpenDeskAcc
#include <Devices.h>    // For OpenDeskAcc (indirectly, via Control Panel DAs)
#include <Lists.h>      // For ListHandle, LNew, LClick, etc.
#include <Controls.h>   // For ControlHandle, GetNewControl, TrackControl, etc.
#include <AppleEvents.h> // For AppleEvent, AEInstallEventHandler, AEProcessAppleEvent, kCoreEventClass, etc.
#include <OSUtils.h>    // For ExitToShell, Delay, DateTimeRec, GetTime
#include <Resources.h>  // For GetResource, GetNamedResource, DetachResource, ResError (used by DNR.c, good to be aware of)
#include <Memory.h>     // For MaxApplZone, NewPtr, DisposePtr, NewHandle, DisposeHandle, HLock, HUnlock, HSetState (used extensively)


#include <stdlib.h>     // For standard C functions like atoi, malloc, free (if used directly)
#include <Sound.h>      // For SysBeep
#include <stdio.h>      // For sprintf, etc.
#include <string.h>     // For strcpy, strlen, strcmp, etc.

// --- Manual definitions for HiWord and LoWord if not found by linker ---
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

// --- Global Variables ---
Boolean gDone = false;
extern Boolean gSystemInitiatedQuit; // Defined in mactcp_network.c

unsigned long gLastPeerListUpdateTime = 0;
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
const unsigned long kQuitMessageDelayTicks = 120;

// For diagnostic logging
static unsigned long gMainLoopIterations = 0;

// Menu IDs and Item Numbers (ensure these match your .r file)
#define kAppleMenuID 1
#define kFileMenuID 128     // Example ID, ensure it matches your 'MENU' resource for File
#define kAboutItem 1        // Assuming "About" is the first item in Apple Menu
#define kQuitItem 1         // Assuming "Quit" is the first item in File Menu

// Apple Event Handler UPPs
static AEEventHandlerUPP gAEQuitAppUPP = NULL;
// static AEEventHandlerUPP gAEOpenAppUPP = NULL; // For future
// static AEEventHandlerUPP gAEOpenDocsUPP = NULL; // For future
// static AEEventHandlerUPP gAEPrintDocsUPP = NULL; // For future


// --- Forward Declarations ---
void InitializeToolbox(void);
void InstallAppleEventHandlers(void);
pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon);
void HandleMenuChoice(long menuResult);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleIdleTasks(void);


// --- Main Application ---
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
        log_app_event("main: System quit during InitializeToolbox. ExitToShell should have been called.");
        return 0; 
    }
    log_app_event("Starting Classic Mac P2P Messenger...");
    log_debug("main: Toolbox Initialized.");

    err = InitializeNetworking();
    if (gSystemInitiatedQuit) {
        log_app_event("main: System quit during InitializeNetworking. ExitToShell should have been called.");
        return 0;
    }
    if (err != noErr) {
        log_app_event("Fatal: Network initialization failed (%d). Exiting.", (int)err);
        if (!gSystemInitiatedQuit) log_shutdown();
        return 1;
    }
    log_debug("main: Networking initialized.");

    InitPeerList();
    if (gSystemInitiatedQuit) { log_app_event("main: System quit after InitPeerList. ExitToShell should have been called."); return 0; }
    log_debug("main: Peer list initialized.");

    dialogOk = InitDialog();
    if (gSystemInitiatedQuit) {
        log_app_event("main: System quit during InitDialog. ExitToShell should have been called.");
        return 0;
    }
    if (!dialogOk) {
        log_app_event("Fatal: Dialog initialization failed. Exiting.");
        if (!gSystemInitiatedQuit) {
             CleanupNetworking();
             log_shutdown();
        }
        return 1;
    }
    log_debug("main: Dialog initialized.");

    AppendToMessagesTE("Classic Mac P2P Messenger Started.\r");
    log_debug("main: Entering main event loop...");
    MainEventLoop();
    log_debug("main: Exited main event loop.");


    if (!gSystemInitiatedQuit) {
        // USER-INITIATED QUIT (File->Quit, Cmd-Q, Close Box)
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
        AppendToMessagesTE(ui_message_buffer); AppendToMessagesTE("\r");
        log_app_event("%s", ui_message_buffer);

        log_debug("main: Cleaning up dialog...");
        CleanupDialog();
        log_debug("main: Cleaning up networking...");
        CleanupNetworking(); 
        log_app_event("Application terminated gracefully by user.");
        log_debug("main: Shutting down log.");
        log_shutdown();
    } else {
        // SYSTEM-INITIATED QUIT (osEvt or kAEQuitApplication)
        log_app_event("main: System shutdown. ExitToShell should have handled termination earlier or main loop exited due to AE.");
        if (gSystemInitiatedQuit) { 
             log_app_event("main: System-initiated quit path in main() reached. Minimal action before return.");
        }
    }
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
    Handle menuBar = GetNewMBar(128); // MBAR ID 128 assumed from your ResEdit screenshot
    if (menuBar == NULL) {
        log_debug("CRITICAL: GetNewMBar(128) failed! Check MBAR resource.");
    } else {
        SetMenuBar(menuBar);
        MenuHandle appleMenu = GetMenuHandle(kAppleMenuID); 
        if (appleMenu) {
            // Using AppendResMenu as AddResMenu caused linker issues previously.
            // This is the older call but should work for 'DRVR' type.
            AppendResMenu(appleMenu, 'DRVR'); 
            log_debug("InitializeToolbox: Apple Menu (ID %d) DAs added using AppendResMenu.", kAppleMenuID);
        } else {
            log_debug("Warning: Could not get Apple Menu (ID %d).", kAppleMenuID);
        }
        DisposeHandle(menuBar); // SetMenuBar makes a copy
        DrawMenuBar();
        log_debug("InitializeToolbox: Menu bar set and drawn.");
    }

    TEInit();
    InitDialogs(NULL); // Calls InitEvents()

    log_debug("InitializeToolbox: Installing Apple Event Handlers.");
    InstallAppleEventHandlers();

    InitCursor();
    log_debug("InitializeToolbox: Exit.");
}

void InstallAppleEventHandlers(void) {
    OSErr err;
    log_debug("InstallAppleEventHandlers: Entry.");

    gAEQuitAppUPP = NewAEEventHandlerUPP(MyAEQuitApplication);
    if (gAEQuitAppUPP == NULL) {
        log_debug("CRITICAL: NewAEEventHandlerUPP failed for MyAEQuitApplication!");
    } else {
        err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, gAEQuitAppUPP, 0L, false);
        if (err != noErr) {
            log_debug("CRITICAL: AEInstallEventHandler failed for kAEQuitApplication: %d", err);
        } else {
            log_debug("InstallAppleEventHandlers: kAEQuitApplication handler installed.");
        }
    }
    log_debug("InstallAppleEventHandlers: Exit.");
}

pascal OSErr MyAEQuitApplication(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefCon) {
    #pragma unused(theAppleEvent, reply, handlerRefCon)

    log_app_event("MyAEQuitApplication: Received kAEQuitApplication Apple Event.");
    
    if (!gSystemInitiatedQuit) {
        gSystemInitiatedQuit = true;
        log_debug("MyAEQuitApplication: Setting gSystemInitiatedQuit = true.");
    }
    gDone = true;
    return noErr;
}

void HandleMenuChoice(long menuResult) {
    short menuID = HiWord(menuResult); 
    short menuItem = LoWord(menuResult); 
    Str255 daName;

    log_debug("HandleMenuChoice: menuID=%d, menuItem=%d", menuID, menuItem);

    switch (menuID) {
        case kAppleMenuID:
            if (menuItem == kAboutItem) { 
                log_app_event("HandleMenuChoice: 'About csend-mac...' selected.");
                Alert(129, nil); // Ensure ALRT ID 129 exists in csend.r
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
                gDone = true; // User-initiated quit, gSystemInitiatedQuit remains false
            }
            break;
        default:
            log_debug("HandleMenuChoice: Unhandled menuID %d.", menuID);
            break;
    }
    HiliteMenu(0); // Unhighlight menu after selection
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
        if (gSystemInitiatedQuit && gDone) {
            log_debug("MainEventLoop iter %lu: gSystemInitiatedQuit and gDone are true. Breaking for fast exit.", gMainLoopIterations);
            break;
        }
         if ((gMainLoopIterations % 500) == 0) { // Log progress periodically
            log_debug("MainEventLoop: Iteration %lu. gDone=%d, gSystemInitiatedQuit=%d", gMainLoopIterations, gDone, gSystemInitiatedQuit);
        }

        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        IdleInputTE();
        HandleIdleTasks();
        
        if (gSystemInitiatedQuit && gDone) { break; } 
        
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        
        if (gSystemInitiatedQuit && gDone) { break; } 

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
                    ControlHandle foundControl; short foundControlPart; GrafPtr oldPort;
                    GetPort(&oldPort); SetPort(GetWindowPort(gMainWindow)); GlobalToLocal(&localPt);
                    foundControlPart = FindControl(localPt, whichWindow, &foundControl);
                    if (foundControl == gMessagesScrollBar && foundControlPart != 0 && (**foundControl).contrlVis && (**foundControl).contrlHilite == 0) {
                        if (foundControlPart == kControlIndicatorPart) { short oldValue = GetControlValue(foundControl); TrackControl(foundControl, localPt, nil); short newValue = GetControlValue(foundControl); if (newValue != oldValue) ScrollMessagesTEToValue(newValue);
                        } else { TrackControl(foundControl, localPt, &MyScrollAction); }
                        eventHandledByApp = true;
                    } else if (gPeerListHandle != NULL && PtInRect(localPt, &(**gPeerListHandle).rView)) {
                        eventHandledByApp = HandlePeerListClick(gMainWindow, &event); 
                    } else { Rect inputTERectDITL; DialogItemType itemTypeDITL; Handle itemHandleDITL; GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeDITL, &itemHandleDITL, &inputTERectDITL); if (PtInRect(localPt, &inputTERectDITL)) { HandleInputTEClick(gMainWindow, &event); eventHandledByApp = true; } 
                    } SetPort(oldPort);
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
                            case kDebugCheckbox: GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect); if (itemHandle && itemType == (ctrlItem + chkCtrl)) { controlH = (ControlHandle)itemHandle; currentValue = GetControlValue(controlH); SetControlValue(controlH, !currentValue); set_debug_output_enabled(GetControlValue(controlH) == 1); GetPort(&oldPortForDrawing); SetPort(GetWindowPort(gMainWindow)); InvalRect(&itemRect); SetPort(oldPortForDrawing); } break;
                            case kBroadcastCheckbox: GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect); if (itemHandle && itemType == (ctrlItem + chkCtrl)) { controlH = (ControlHandle)itemHandle; currentValue = GetControlValue(controlH); SetControlValue(controlH, !currentValue); if (GetControlValue(controlH) == 1) DialogPeerList_DeselectAll(); GetPort(&oldPortForDrawing); SetPort(GetWindowPort(gMainWindow)); InvalRect(&itemRect); SetPort(oldPortForDrawing); } break;
                            }
                        }
                        eventHandledByApp = true;
                    }
                }
            }
            if (!eventHandledByApp) HandleEvent(&event); 
            
            if (gSystemInitiatedQuit && gDone) { break; }
        }
    }
    log_debug("MainEventLoop: Exited loop. gDone=%d, gSystemInitiatedQuit=%d, Iterations=%lu", gDone, gSystemInitiatedQuit, gMainLoopIterations);
}

void HandleIdleTasks(void)
{
    if (gSystemInitiatedQuit) return;
    unsigned long currentTimeTicks = TickCount();
    PollUDPListener(gMacTCPRefNum, gMyLocalIP);
    if (gSystemInitiatedQuit) return;
    PollTCP(YieldTimeToSystem);
    if (gSystemInitiatedQuit) return;
    CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
    if (gSystemInitiatedQuit) return;
    if (gLastPeerListUpdateTime == 0 || (currentTimeTicks < gLastPeerListUpdateTime) || (currentTimeTicks - gLastPeerListUpdateTime) >= kPeerListUpdateIntervalTicks) {
        if (gPeerListHandle != NULL) UpdatePeerDisplayList(false);
        gLastPeerListUpdateTime = currentTimeTicks;
    }
}

void HandleEvent(EventRecord *event) 
{
    if (gSystemInitiatedQuit && event->what != osEvt && event->what != kHighLevelEvent) {
        return;
    }

    short windowPart; WindowPtr whichWindow; char theChar;
    switch (event->what) {
    case mouseDown:
        if (gSystemInitiatedQuit) return;
        // MouseDown inMenuBar is handled in MainEventLoop, other mouseDowns here.
        windowPart = FindWindow(event->where, &whichWindow); 
        switch (windowPart) {
        // case inMenuBar: // Handled in MainEventLoop
        //    break;
        case inSysWindow: SystemClick(event, whichWindow); break;
        case inDrag: if (whichWindow == (WindowPtr)gMainWindow) DragWindow(whichWindow, event->where, &qd.screenBits.bounds); break;
        case inGoAway: if (whichWindow == (WindowPtr)gMainWindow && TrackGoAway(whichWindow, event->where)) { log_debug("HandleEvent: Close box clicked, setting gDone=true."); gDone = true; } break;
        case inContent: if (whichWindow != FrontWindow()) SelectWindow(whichWindow); break;
        }
        break;
    case keyDown: case autoKey:
        if (gSystemInitiatedQuit) return;
        theChar = event->message & charCodeMask;
        if ((event->modifiers & cmdKey) != 0) {
            long menuResult = MenuKey(theChar); 
            if (HiWord(menuResult) != 0) { 
                HandleMenuChoice(menuResult); // This will set gDone for File->Quit (Cmd-Q)
            }
        } else { HandleInputTEKeyDown(event); } 
        break;
    case updateEvt:
        if (gSystemInitiatedQuit) return;
        whichWindow = (WindowPtr)event->message; BeginUpdate(whichWindow);
        if (whichWindow == (WindowPtr)gMainWindow) { DrawDialog(whichWindow); UpdateDialogControls(); }
        EndUpdate(whichWindow); break;
    case activateEvt:
        if (gSystemInitiatedQuit) return;
        whichWindow = (WindowPtr)event->message;
        if (whichWindow == (WindowPtr)gMainWindow) {
            Boolean becomingActive = ((event->modifiers & activeFlag) != 0);
            ActivateDialogTE(becomingActive); ActivatePeerList(becomingActive);
            if (gMessagesScrollBar != NULL) {
                short hiliteValue = (becomingActive && GetControlMaximum(gMessagesScrollBar) > 0 && (**gMessagesScrollBar).contrlVis) ? 0 : 255;
                HiliteControl(gMessagesScrollBar, hiliteValue);
            }
        } break;
    case kHighLevelEvent: // Apple Event
        log_debug("HandleEvent: kHighLevelEvent received. Calling AEProcessAppleEvent.");
        OSErr aeErr = AEProcessAppleEvent(event); 
        if (aeErr != noErr && aeErr != errAEEventNotHandled) { // errAEEventNotHandled means we don't have a specific handler
            log_debug("HandleEvent: AEProcessAppleEvent returned error: %d", aeErr);
        } else if (aeErr == errAEEventNotHandled) {
            log_debug("HandleEvent: AEProcessAppleEvent: errAEEventNotHandled.");
        }
        // If it was kAEQuitApplication, MyAEQuitApplication would have set gDone and gSystemInitiatedQuit.
        break;
    case osEvt:
        switch ((event->message >> 24) & 0xFF) { // High byte of message is osEvt type
        case suspendResumeMessage: // suspendResumeMessage == 0x01
            if ((event->message & resumeMask) == 0) { // System wants us to quit (suspend with resumeMask bit 0 clear)
                if (!gSystemInitiatedQuit) {
                    gSystemInitiatedQuit = true;
                    gDone = true;
                    log_app_event("osEvt: System quit signal (suspend/resume). Calling ExitToShell() NOW.");
                    ExitToShell(); // Exit immediately
                }
            } else { // Resume event
                if (gSystemInitiatedQuit) return;
                if (gMainWindow) { SelectWindow((WindowPtr)gMainWindow); InvalRect(&gMainWindow->portRect); }
                log_debug("osEvt: Resume event.");
            } break;
        default:
             log_debug("HandleEvent: Unhandled osEvt type 0x%lX", (event->message >> 24) & 0xFF);
             break;
        } break;
    }
}