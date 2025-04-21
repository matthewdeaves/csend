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
#include "network.h"   // Networking functions (InitializeNetworking, CleanupNetworking, gMacTCPRefNum, gMyLocalIP, gMyLocalIPStr)
#include "discovery.h" // UDP Discovery functions (InitUDPDiscoveryEndpoint, CheckSendBroadcast, CheckUDPReceive, CleanupUDPDiscoveryEndpoint)
#include "dialog.h"    // Dialog functions (InitDialog, CleanupDialog, HandleDialogClick, gMyUsername, etc.)
#include "peer_mac.h"  // Peer list functions (InitPeerList, PruneTimedOutPeers)

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
    // At this point, gMacTCPRefNum and gMyLocalIP should be valid if InitializeNetworking succeeded

    // 4. Initialize UDP Discovery Endpoint (using the ref num from network init)
    // Use the renamed function
    networkErr = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (networkErr != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (Error: %d). Exiting.", networkErr);
        CleanupNetworking(); // Clean up TCP/DNR part (will also attempt UDP cleanup)
        CloseLogFile();      // Close log file
        ExitToShell();
        return 1; // Indicate failure
    }

    // Initialize the peer list AFTER network is up
    InitPeerList();
    log_message("Peer list initialized.");

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
    CleanupNetworking(); // Cleans up TCP/DNR and UDP (calls CleanupUDPDiscoveryEndpoint internally)
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
    long        sleepTime = 5L; // Sleep time for WaitNextEvent (in ticks), ~1/12 second

    while (!gDone) {
        // Allow TextEdit fields to idle
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);

        // Wait for the next event
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            // Handle standard dialog events
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    // Check if it's our dialog and an item was hit
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        HandleDialogClick(whichDialog, itemHit);
                    }
                }
            } else {
                // Handle other events (window activation, mouse clicks outside controls, etc.)
                HandleEvent(&event);
            }
        } else {
            // --- Idle Time: No event received ---
            // Perform periodic tasks here:

            // 1. Check for incoming UDP packets (Discovery Responses)
            CheckUDPReceive(gMacTCPRefNum, gMyLocalIP);

            // 2. Send discovery broadcast if necessary
            CheckSendBroadcast(gMacTCPRefNum, gMyUsername, gMyLocalIPStr);

            // 3. Prune timed-out peers from the list
            PruneTimedOutPeers();

            // 4. Give time to other processes (already handled by WaitNextEvent sleepTime)
            // SystemTask(); // Might be needed on very old systems, but usually WNE is enough
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
                    // Standard menu handling would go here if menus were added
                    // MenuSelect(event->where);
                    // HandleMenuChoice(menuResult);
                    break;
                case inSysWindow:
                    SystemClick(event, whichWindow);
                    break;
                case inDrag:
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // Use screenBits bounds for dragging area
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway:
                    // Check if the click was in the close box of our window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (TrackGoAway(whichWindow, event->where)) {
                            log_message("Close box clicked. Setting gDone = true.");
                            gDone = true; // Set flag to exit main loop
                        }
                    }
                    break;
                case inContent:
                    // If clicked in the content region of our window (but not controls)
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // If our window isn't frontmost, bring it to front
                        if (whichWindow != FrontWindow()) {
                            SelectWindow(whichWindow);
                        }
                        // Clicks in TE fields are handled by DialogSelect calling TEClick
                    }
                    break;
                default:
                    // Clicks in other regions (zoom box, grow box) ignored for now
                    break;
            }
            break;

        case keyDown: case autoKey:
            // These are generally handled by DialogSelect if it's a dialog event.
            // If we needed global key handling (e.g., command keys not tied to dialog items),
            // it would go here, checking event->message for the char code.
            // Example: if ((event->modifiers & cmdKey) != 0) { HandleCommandKey(event->message & charCodeMask); }
            break;

        case updateEvt:
            // The event message points to the window needing update
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow); // Set clipping region to update area
                // EraseRgn(whichWindow->visRgn); // Optional: Erase background if needed
                DrawDialog(whichWindow); // Redraw standard dialog controls (buttons, checkboxes, static text)
                UpdateDialogTE();        // Redraw TextEdit fields
                // TODO: Redraw List Manager list if implemented
                // LUpdate(whichWindow->visRgn, gPeerListHandle);
                EndUpdate(whichWindow);   // Restore clipping region
            }
            break;

        case activateEvt:
            // The event message points to the window being activated/deactivated
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                // Check the activeFlag in the event modifiers
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            break;

        // Handle other events if necessary (diskEvt, osEvt, etc.)
        default:
            break;
    }
}