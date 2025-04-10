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
#include "logging.h"   // Logging functions (InitLogFile, CloseLogFile, LogToDialog)
#include "network.h"   // Networking functions (InitializeNetworking, CleanupNetworking, InitUDPDiscovery, CheckSendBroadcast, IssueUDPRead, ProcessUDPReceive)
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
    LogToDialog("Starting application...");

    // 2. Basic Mac OS Initialization
    MaxApplZone();
    LogToDialog("MaxApplZone called.");
    InitializeToolbox();
    LogToDialog("Toolbox Initialized.");

    // 3. Initialize Networking (TCP Driver, IP, DNR)
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        LogToDialog("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
        CloseLogFile(); // Close log file before exiting
        ExitToShell();
        return 1; // Indicate failure
    }

    // 4. Initialize UDP Discovery (Create UDP Endpoint and start listening)
    networkErr = InitUDPDiscovery(); // *** RE-ENABLED ***
    if (networkErr != noErr) {
        LogToDialog("Fatal: UDP Discovery initialization failed (Error: %d). Exiting.", networkErr);
        CleanupNetworking(); // Clean up TCP/DNR part
        CloseLogFile();      // Close log file
        ExitToShell(); // Call ExitToShell directly
        return 1; // Indicate failure
    }
    LogToDialog("UDP Discovery Initialized."); // *** UPDATED LOG ***

    // 5. Initialize the Main Dialog Window and its controls
    dialogOk = InitDialog();
    if (!dialogOk) {
        LogToDialog("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking(); // Clean up network resources
        CloseLogFile();
        ExitToShell();
        return 1;
    }

    // 6. Enter the Main Event Loop
    LogToDialog("Entering main event loop...");
    MainEventLoop();
    LogToDialog("Exited main event loop.");

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
            // Check if the pending UDP read has completed
            if (!gUDPReadPending) {
                ProcessUDPReceive(); // Process result and re-issue read if necessary
            }
            // Check if it's time to send a broadcast
            CheckSendBroadcast(); // *** RE-ENABLED ***
            // Prune inactive peers periodically during idle time
            PruneInactivePeers();
        }
    } // end while(!gDone)
}

/**
 * @brief Handles non-dialog events (mouse clicks, window updates, activation).
 */
void HandleEvent(EventRecord *event) {
    short     windowPart;
    WindowPtr whichWindow;

    switch (event->what) {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    LogToDialog("Menu bar clicked (not implemented).");
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
                            LogToDialog("Close box clicked. Setting gDone = true.");
                            gDone = true;
                        }
                    }
                    break;
                case inContent:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (whichWindow != FrontWindow()) {
                            SelectWindow(whichWindow);
                        }
                        // Note: Clicks in TE fields are handled by DialogSelect -> TEClick
                    }
                    break;
                default:
                    break;
            }
            break;

        case keyDown: case autoKey:
            // Handled by DialogSelect if it's for an active TE field
            break;

        case updateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow);
                DrawDialog(whichWindow); // Redraws standard items
                UpdateDialogTE();        // Redraws TE fields
                // TODO: Redraw peer list user item if implemented
                EndUpdate(whichWindow);
            }
            break;

        case activateEvt:
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            break;

        // Handle application-defined events if needed later (e.g., from completion routines)
        // case app4Evt:
        //     break;

        default:
            break;
    }
}