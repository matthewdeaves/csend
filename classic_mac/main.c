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
#include "network.h"
#include "dialog.h"
#include "peer_mac.h"
#include "dialog_peerlist.h"
#include "tcp.h"
#include "discovery.h"
#include <Sound.h>
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
const unsigned long kPeerListUpdateIntervalTicks = 5 * 60;
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);
void HandleMouseDownInContent(WindowPtr whichWindow, EventRecord *event);
void HandleIdleTasks(void);
int main(void)
{
    OSErr networkErr;
    Boolean dialogOk;
    InitLogFile();
    log_message("Starting application (Single Stream / Sync Poll Strategy)...");
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
    OSErr quitErr = TCP_SendQuitMessagesSync(YieldTimeToSystem);
    if (quitErr == streamBusyErr) {
        log_message("Warning: TCP_SendQuitMessagesSync failed because stream was busy.");
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
        if (gInputTE != NULL) TEIdle(gInputTE);
        HandleIdleTasks();
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
        if (gotEvent) {
            Boolean eventHandled = false;
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
                             foundControlPart == inPageUp || foundControlPart == inPageDown)) {
                        if ((**foundControl).contrlHilite == 0) {
                            TrackControl(foundControl, localPt, &MyScrollAction);
                        }
                        eventHandled = true;
                    }
                }
            }
            if (!eventHandled) {
                if (IsDialogEvent(&event)) {
                    DialogPtr whichDialog;
                    short itemHit;
                    if (DialogSelect(&event, &whichDialog, &itemHit)) {
                        if (whichDialog == gMainWindow && itemHit > 0) {
                            switch (itemHit) {
                            case kSendButton:
                                HandleSendButtonClick();
                                break;
                            case kMessagesScrollbar: {
                                short newValue = GetControlValue(gMessagesScrollBar);
                                if (gMessagesTE != NULL) {
                                    SignedByte teState = HGetState((Handle)gMessagesTE);
                                    HLock((Handle)gMessagesTE);
                                    if (*gMessagesTE != NULL) {
                                        short lineHeight = (**gMessagesTE).lineHeight;
                                        if (lineHeight > 0) {
                                            short desiredTopPixel = -newValue * lineHeight;
                                            short currentTopPixel = (**gMessagesTE).destRect.top;
                                            short scrollDeltaPixels = desiredTopPixel - currentTopPixel;
                                            if (scrollDeltaPixels != 0) {
                                                ScrollMessagesTE(scrollDeltaPixels);
                                            }
                                        }
                                    }
                                    HSetState((Handle)gMessagesTE, teState);
                                }
                            }
                            break;
                            case kPeerListUserItem: {
                                Cell tempCell = {0, -1};
                                if (LGetSelect(true, &tempCell, gPeerListHandle)) {
                                    gLastSelectedCell = tempCell;
                                } else {
                                    SetPt(&gLastSelectedCell, 0, -1);
                                }
                            }
                            break;
                            default:
                                break;
                            }
                        }
                        eventHandled = true;
                    }
                }
                if (!eventHandled) {
                    HandleEvent(&event);
                }
            }
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
    char theChar;
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
            if (whichWindow == (WindowPtr)gMainWindow) {
                DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
            }
            break;
        case inGoAway:
            if (whichWindow == (WindowPtr)gMainWindow) {
                if (TrackGoAway(whichWindow, event->where)) {
                    log_message("Close box clicked. Setting gDone = true.");
                    gDone = true;
                }
            }
            break;
        case inContent:
            if (whichWindow == (WindowPtr)gMainWindow) { }
            else {
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
        if ((event->modifiers & cmdKey) != 0) { } break;
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
            ActivateMessagesTEAndScrollbar(becomingActive);
        }
        break;
    default:
        break;
    }
}
void HandleMouseDownInContent(WindowPtr whichWindow, EventRecord *event)
{
    Point localPt = event->where;
    GrafPtr oldPort;
    GetPort(&oldPort);
    SetPort(GetWindowPort(whichWindow));
    GlobalToLocal(&localPt);
    log_to_file_only("HandleMouseDownInContent: Processing click at local (%d, %d)", localPt.v, localPt.h);
    SetPort(oldPort);
}
