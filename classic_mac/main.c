// FILE: ./classic_mac/main.c

// --- Standard C Includes ---
#include <stddef.h>  // For NULL definition

// --- Macintosh Toolbox Includes ---
// Core Types and Basic Utilities
#include <MacTypes.h>
#include <Quickdraw.h> // For GrafPort, qd globals, Rect, Point, InitGraf, InitCursor etc.
#include <Memory.h>    // For MaxApplZone, memory management (though less direct use here)
#include <OSUtils.h>   // For OSErr, noErr, TickCount, SysBeep
#include <ToolUtils.h> // For FindWindow, DragWindow, TrackGoAway etc.

// Managers used by the application
#include <Fonts.h>     // For InitFonts
#include <Windows.h>   // For WindowPtr, GetNewWindow, ShowWindow, SelectWindow, FindWindow, DragWindow, TrackGoAway, BeginUpdate, EndUpdate, FrontWindow, DisposeWindow
#include <Menus.h>     // For InitMenus (even if no menus yet)
#include <TextEdit.h>  // For TEHandle, TEInit, TEIdle, TEActivate, TEDeactivate, TEUpdate
#include <Dialogs.h>   // For DialogPtr, InitDialogs, GetNewDialog, IsDialogEvent, DialogSelect, DrawDialog, DisposeDialog
#include <Events.h>    // For EventRecord, WaitNextEvent, SystemClick, everyEvent, keyDown, autoKey, updateEvt, activateEvt, mouseDown, activeFlag
#include <Devices.h>   // For ExitToShell (though maybe Processes.h is more modern, Devices works)
#include <Processes.h> // For ExitToShell alternative

// --- Project-Specific Includes ---
#include "logging.h"   // For InitLogFile, LogToDialog, CloseLogFile
#include "network.h"   // For InitializeNetworking, CleanupNetworking, InitUDPDiscovery, CheckSendBroadcast, PruneInactivePeers, ProcessUDPReceive, CheckAndSendDeferredResponse, gUDPReadPending
#include "dialog.h"    // For InitDialog, CleanupDialog, HandleDialogClick, ActivateDialogTE, UpdateDialogTE, gMainWindow, gMessagesTE, gInputTE

// --- Global Variables ---
Boolean gDone = false; // Flag to control the main event loop exit

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
    // MoreMasters(); // Consider adding MoreMasters calls for handle allocation robustness
    // MoreMasters();
    // MoreMasters();
    MaxApplZone(); // Maximize application heap zone
    LogToDialog("MaxApplZone called.");
    InitializeToolbox(); // Initialize standard managers
    LogToDialog("Toolbox Initialized.");

    // 3. Initialize Networking (TCP Driver, IP, DNR)
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        LogToDialog("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
        CloseLogFile(); // Close log file before exiting
        ExitToShell();  // Exit back to Finder
        return 1;       // Indicate failure (though ExitToShell usually doesn't return)
    }

    // 4. Initialize UDP Discovery (Create UDP Endpoint and start listening)
    networkErr = InitUDPDiscovery();
    if (networkErr != noErr) {
        LogToDialog("Fatal: UDP Discovery initialization failed (Error: %d). Exiting.", networkErr);
        CleanupNetworking(); // Clean up TCP/DNR part
        CloseLogFile();      // Close log file
        ExitToShell();       // Exit back to Finder
        return 1;            // Indicate failure
    }
    LogToDialog("UDP Discovery Initialized.");

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

    // ExitToShell(); // Call ExitToShell at the very end if needed, though returning 0 is also fine for Retro68 apps
    return 0; // Indicate successful completion
}

/**
 * @brief Initializes standard Macintosh Toolbox managers.
 */
void InitializeToolbox(void) {
    // Order generally matters, follow standard practice
    InitGraf(&qd.thePort); // Initialize QuickDraw globals
    InitFonts();           // Initialize Font Manager
    InitWindows();         // Initialize Window Manager
    InitMenus();           // Initialize Menu Manager
    TEInit();              // Initialize TextEdit
    InitDialogs(NULL);     // Initialize Dialog Manager (NULL = no resume proc)
    InitCursor();          // Initialize Cursor to the standard arrow
}


// --- Event Handling ---

/**
 * @brief The main event loop for the application.
 * @details Handles event processing, background tasks (network checks, TE idling),
 *          and checks the gDone flag to determine when to exit.
 */
void MainEventLoop(void) {
    EventRecord event;       // Structure to hold event information
    Boolean     gotEvent;    // Flag indicating if WaitNextEvent returned an event
    long        sleepTime = 10L; // Ticks to sleep if no event (approx 1/6th second)
    RgnHandle   cursorRgn = NULL; // Region for cursor changes (optional)

    // Create a cursor region (optional, for advanced cursor handling)
    // cursorRgn = NewRgn(); // If using custom cursors based on region

    while (!gDone) { // Loop until gDone is set to true

        // --- Process Deferred Network Actions FIRST ---
        // Check if a UDP response needs to be sent (set by completion routine)
        CheckAndSendDeferredResponse();

        // --- Idle Processing ---
        // Give time to TextEdit fields if they exist
        if (gMessagesTE != NULL) TEIdle(gMessagesTE);
        if (gInputTE != NULL) TEIdle(gInputTE);
        // Add other idle tasks here if needed

        // --- Wait for the next event ---
        // WaitNextEvent allows background processing and cooperative multitasking
        // everyEvent: Mask for all event types
        // &event: Pointer to store the event record
        // sleepTime: Max time to wait in ticks if no event is pending
        // cursorRgn: Region where cursor might change (NULL for default handling)
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, cursorRgn);

        if (gotEvent) {
            // --- Process Actual Events ---
            // Check if the event is relevant to a dialog
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog; // Pointer to the dialog the event belongs to
                short itemHit;         // Item number hit within the dialog (if any)

                // DialogSelect handles most dialog events (clicks, keypresses in TE fields)
                // Returns true if an item was clicked/interacted with
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    // Check if the event was for our main window and an item was hit
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        // Handle clicks on specific items (buttons, checkboxes)
                        HandleDialogClick(whichDialog, itemHit);
                    }
                    // Note: Clicks/keys in the TE fields are handled internally by DialogSelect -> TEClick/TEKey
                }
            } else {
                // If it's not a dialog event, handle it as a general window/application event
                HandleEvent(&event);
            }
        } else {
            // --- No Event Occurred: Perform Background Network Tasks ---

            // Check if the pending UDP read has completed (flag set by completion routine)
            if (!gUDPReadPending) {
                // Process the result of the completed read (parse data, update peers, etc.)
                // This function will also re-issue the next asynchronous UDPRead.
                ProcessUDPReceive();
            }

            // Check if it's time to send the periodic discovery broadcast
            CheckSendBroadcast();

            // Periodically remove inactive peers from the list
            PruneInactivePeers();

            // Add other background tasks here (e.g., check TCP connections)
        }
    } // end while(!gDone)

    // Dispose of cursor region if created
    // if (cursorRgn != NULL) DisposeRgn(cursorRgn);
}

/**
 * @brief Handles non-dialog events (window activation, dragging, closing, updates).
 * @param event Pointer to the EventRecord containing the event details.
 */
void HandleEvent(EventRecord *event) {
    short     windowPart;  // Code indicating where in the window the mouse was clicked
    WindowPtr whichWindow; // Pointer to the window where the event occurred

    switch (event->what) { // Determine the type of event
        case mouseDown: // Mouse button pressed
            // Find out which part of which window was clicked
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar: // Click in the menu bar
                    // TODO: Handle menu selections if menus are added
                    LogToDialog("Menu bar clicked (not implemented).");
                    // MenuSelect(event->where); // Example call if menus existed
                    break;
                case inSysWindow: // Click in a system window (e.g., a Desk Accessory)
                    SystemClick(event, whichWindow); // Let the system handle it
                    break;
                case inDrag: // Click in the title bar (drag region)
                    // Allow dragging only our main window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // Drag the window, constrained to screen bounds (minus menu bar)
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway: // Click in the close box
                    // Allow closing only our main window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // TrackGoAway visually handles the click and returns true if closed
                        if (TrackGoAway(whichWindow, event->where)) {
                            LogToDialog("Close box clicked. Setting gDone = true.");
                            gDone = true; // Set flag to exit the main loop
                        }
                    }
                    break;
                case inContent: // Click in the content region of the window
                    // Bring our window to the front if it's not already
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (whichWindow != FrontWindow()) {
                            SelectWindow(whichWindow);
                        } else {
                            // Handle content clicks NOT in TE fields here if needed
                            // Clicks in TE fields are handled by DialogSelect -> TEClick
                        }
                    } else {
                        // Handle clicks in other windows' content regions if applicable
                    }
                    break;
                default: // Clicks in other parts (zoom box, grow box - not handled here)
                    break;
            }
            break;

        case keyDown: // A key was pressed
        case autoKey: // A key is being held down (auto-repeat)
            // Keyboard events are generally handled by DialogSelect if a TE field
            // in the active dialog has focus. If not, they might be handled here
            // (e.g., for command-key equivalents if menus existed).
            // char theChar = event->message & charCodeMask; // Example: Get the character code
            // if ((event->modifiers & cmdKey) != 0) { // Check for Command key modifier
            //     // Handle command keys
            // }
            break;

        case updateEvt: // Window needs to be redrawn
            // The event->message field contains a pointer to the window needing update
            whichWindow = (WindowPtr)event->message;
            // Check if it's our main window
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow); // Set up clipping region for drawing
                // EraseRect(&whichWindow->portRect); // Optional: Erase entire window first
                DrawDialog(whichWindow); // Redraws standard dialog items (buttons, checkboxes)
                UpdateDialogTE();        // Redraws the content of our TextEdit fields
                // TODO: Redraw custom user items like the peer list here
                // Example: DrawPeerList();
                EndUpdate(whichWindow);   // Restore clipping region
            }
            // Handle updates for other windows if necessary
            break;

        case activateEvt: // A window is becoming active or inactive
            // The event->message field contains a pointer to the window
            whichWindow = (WindowPtr)event->message;
            // Check if it's our main window
            if (whichWindow == (WindowPtr)gMainWindow) {
                // The activeFlag bit in event->modifiers indicates activation (set) or deactivation (clear)
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            // Handle activation/deactivation for other windows if necessary
            break;

        // Handle application-defined events if needed later (e.g., from completion routines)
        // case app4Evt: // Often used for network completion or background tasks
        //     HandleApp4Event(); // Example custom handler
        //     break;

        default: // Ignore other event types
            break;
    }
}
