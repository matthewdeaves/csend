// FILE: ./classic_mac/main.c
/**
 * @file main.c (Classic Mac)
 * @brief Main application file for the Classic Macintosh P2P Chat Client.
 *        Handles initialization, the main event loop, and cleanup orchestration.
 */

// --- Mac OS Includes ---
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Devices.h>   // For SystemTask
#include <stdlib.h>    // For exit()

// --- Project Includes ---
#include "logging.h"   // Logging functions (InitLogFile, CloseLogFile, log_message)
#include "network.h"   // Networking functions (InitializeNetworking, CleanupNetworking)
#include "discovery.h" // UDP Discovery functions (InitUDPBroadcastEndpoint, CheckSendBroadcast)
#include "dialog.h"    // Dialog functions (InitDialog, CleanupDialog, HandleDialogClick, etc.)

// --- Global Variables ---
Boolean gDone = false; // Controls the main event loop termination

// --- Function Prototypes ---
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);

// --- Main Application ---

int main(void) {
    OSErr   networkErr;
    Boolean dialogOk;

    // 1. Initialize Logging FIRST
    InitLogFile();
    log_message("Starting application...");

    // 2. Basic Mac OS Initialization
    MaxApplZone();
    log_message("MaxApplZone called.");
    InitializeToolbox();
    log_message("Toolbox Initialized.");

    // 3. Initialize Networking (TCP Driver, IP, DNR)
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        log_message("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
        CloseLogFile(); // Close log file before exiting
        ExitToShell();
        return 1; // Indicate failure
    }
    // At this point, gMacTCPRefNum should be valid if InitializeNetworking succeeded

    // 4. Initialize UDP Broadcast Endpoint (using the ref num from network init)
    networkErr = InitUDPBroadcastEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) {
        log_message("Fatal: UDP Broadcast initialization failed (Error: %d). Exiting.", networkErr);
        CleanupNetworking(); // Clean up TCP/DNR part (will also attempt UDP cleanup)
        CloseLogFile();      // Close log file
        ExitToShell();
        return 1; // Indicate failure
    }

    // 5. Initialize the Main Dialog Window and its controls
    dialogOk = InitDialog();
    if (!dialogOk) {
        log_message("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking(); // Clean up network resources (TCP/DNR and UDP)
        CloseLogFile();
        ExitToShell();
        return 1;
    }
    // Now gMyUsername should be set (default or loaded)

    // 6. Enter the Main Event Loop
    log_message("Entering main event loop...");
    MainEventLoop();
    log_message("Exited main event loop.");

    // 7. Cleanup Resources
    CleanupDialog();
    CleanupNetworking(); // Cleans up TCP/DNR and UDP
    CloseLogFile();

    return 0;
}

// --- Initialization Functions ---

/**
 * @brief Initializes standard Macintosh Toolbox managers.
 */
void InitializeToolbox(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}

// --- Event Handling ---

/**
 * @brief The main event loop for the application.
 */
void MainEventLoop(void) {
    EventRecord event;
    Boolean     gotEvent;
    long        sleepTime = 5L; // Sleep time for WaitNextEvent (in ticks)

    while (!gDone) {
        // Allow TextEdit fields to idle
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);

        // Wait for the next event
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        HandleDialogClick(whichDialog, itemHit);
                    }
                }
            } else {
                HandleEvent(&event);
            }
        } else {
            // --- Idle Time ---
            // Pass necessary info to CheckSendBroadcast
            CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);
        }
    } // end while(!gDone)
}

/**
 * @brief Handles non-dialog events (mouse clicks, window updates, activation).
 * (No changes needed in this function)
 */
void HandleEvent(EventRecord *event) {
    short     windowPart;
    WindowPtr whichWindow;

    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    // log_message("Menu bar clicked (not implemented)."); // Less noisy
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
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (whichWindow != FrontWindow()) {
                            SelectWindow(whichWindow);
                        }
                    }
                    break;
                default:
                    break;
            }
            break;

        case keyDown: case autoKey:
            // Handled by DialogSelect
            break;

        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow);
                DrawDialog(whichWindow);
                UpdateDialogTE();
                EndUpdate(whichWindow);
            }
            break;

        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            break;

        default:
            break;
    }
}