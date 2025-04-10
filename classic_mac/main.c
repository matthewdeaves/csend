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
#include "network.h"   // Networking functions (InitializeNetworking, CleanupNetworking)
#include "dialog.h"    // Dialog functions (InitDialog, CleanupDialog, HandleDialogClick, etc.)
// #include "common_defs.h" // Included indirectly via other headers if needed
// #include "protocol.h"    // Included indirectly via dialog.h

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
    LogToDialog("Starting application..."); // Log file only initially

    // 2. Basic Mac OS Initialization
    MaxApplZone(); // Maximize application heap zone
    LogToDialog("MaxApplZone called."); // Log file only
    InitializeToolbox(); // Initialize standard Mac managers
    LogToDialog("Toolbox Initialized."); // Log file only

    // 3. Initialize Networking
    networkErr = InitializeNetworking();
    if (networkErr != noErr) {
        LogToDialog("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
        CloseLogFile(); // Close log file before exiting
        ExitToShell();
        return 1; // Indicate failure
    }
    // LogToDialog("InitializeNetworking finished. Local IP reported: %s", gMyLocalIPStr); // Logged within InitializeNetworking

    // 4. Initialize the Main Dialog Window and its controls
    dialogOk = InitDialog();
    if (!dialogOk) {
        LogToDialog("Fatal: Dialog initialization failed. Exiting.");
        CleanupNetworking(); // Clean up network resources
        CloseLogFile();      // Close log file
        ExitToShell();
        return 1; // Indicate failure
    }
    // LogToDialog("InitDialog finished successfully."); // Logged within InitDialog

    // 5. Enter the Main Event Loop
    LogToDialog("Entering main event loop...");
    MainEventLoop(); // Handles events until gDone is true
    LogToDialog("Exited main event loop.");

    // 6. Cleanup Resources
    CleanupDialog();     // Dispose dialog, TE handles
    CleanupNetworking(); // Close MacTCP driver, DNR
    CloseLogFile();      // Close the log file

    // Application terminated normally
    return 0;
}

// --- Initialization Functions ---

/**
 * @brief Initializes standard Macintosh Toolbox managers.
 */
void InitializeToolbox(void) {
    InitGraf(&qd.thePort); // Initialize QuickDraw globals
    InitFonts();           // Initialize Font Manager
    InitWindows();         // Initialize Window Manager
    InitMenus();           // Initialize Menu Manager
    TEInit();              // Initialize TextEdit
    InitDialogs(NULL);     // Initialize Dialog Manager
    InitCursor();          // Initialize Cursor
}

// --- Event Handling ---

/**
 * @brief The main event loop for the application.
 * @details Continuously waits for events using WaitNextEvent. If an event occurs,
 *          it dispatches the event to either DialogSelect (if it's a dialog event)
 *          or HandleEvent (for other event types). It also calls TEIdle periodically
 *          to allow the TextEdit cursor to blink. The loop terminates when the
 *          global flag gDone is set to true.
 */
void MainEventLoop(void) {
    EventRecord event;
    Boolean     gotEvent;
    long        sleepTime = 5L; // Sleep time for WaitNextEvent (in ticks)

    while (!gDone) {
        // Allow TextEdit fields to idle (e.g., blink cursor)
        // Check handles before calling TEIdle
        if (gMessagesTE != NULL) {
            TEIdle(gMessagesTE);
        }
        if (gInputTE != NULL) {
            TEIdle(gInputTE);
        }

        // Wait for the next event, allowing background tasks to run
        gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

        if (gotEvent) {
            // Check if the event belongs to a dialog window
            if (IsDialogEvent(&event)) {
                DialogPtr whichDialog;
                short itemHit;
                // Let DialogSelect handle the event (clicks, keys in TE fields, etc.)
                if (DialogSelect(&event, &whichDialog, &itemHit)) {
                    // DialogSelect returns true if it handled the event AND
                    // the event was a click in an active item (button, checkbox, etc.)
                    // or a key press in an EditText item.
                    if (whichDialog == gMainWindow && itemHit > 0) {
                        // If it was a click in one of *our* dialog's active items,
                        // call our specific handler for buttons/checkboxes.
                        // Clicks in TE userItems are handled internally by DialogSelect/TEClick.
                        HandleDialogClick(whichDialog, itemHit);
                    }
                    // If whichDialog is not gMainWindow, it's likely a Desk Accessory event,
                    // which DialogSelect handles automatically.
                }
            } else {
                // If it's not a dialog event, pass it to our general event handler.
                HandleEvent(&event);
            }
        } else {
            // No event occurred, WaitNextEvent timed out.
            // This is where background tasks could be performed if needed.
            // For now, just loop again.
        }
    } // end while(!gDone)
}

/**
 * @brief Handles non-dialog events (mouse clicks, window updates, activation).
 * @details Processes events that are not automatically handled by DialogSelect.
 *          This includes clicks in the menu bar, system windows, window dragging,
 *          close box clicks, window updates, and window activation/deactivation.
 * @param event Pointer to the EventRecord containing the event details.
 */
void HandleEvent(EventRecord *event) {
    short     windowPart;
    WindowPtr whichWindow;

    switch (event->what) {
        case mouseDown:
            // Find where the mouse click occurred
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    // TODO: Handle menu selections
                    // HandleMenuClick(MenuSelect(event->where));
                    LogToDialog("Menu bar clicked (not implemented).");
                    break;
                case inSysWindow:
                    // Click in a system window (e.g., Desk Accessory)
                    SystemClick(event, whichWindow);
                    break;
                case inDrag:
                    // Click in the drag region (title bar) of our window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                    }
                    break;
                case inGoAway:
                    // Click in the close box of our window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        if (TrackGoAway(whichWindow, event->where)) {
                            LogToDialog("Close box clicked. Setting gDone = true.");
                            gDone = true; // Signal the main loop to terminate
                        }
                    }
                    break;
                case inContent:
                    // Click in the content region of a window
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // If our window wasn't frontmost, bring it to front
                        if (whichWindow != FrontWindow()) {
                            SelectWindow(whichWindow);
                        } else {
                            // Click in our window's content, but not in an active item
                            // (DialogSelect handles clicks in active items).
                            // Could potentially deactivate TE fields here if needed.
                            // GlobalToLocal(&event->where); // Convert to local coordinates if needed
                            // TEClick(event->where, event->modifiers & shiftKey, gInputTE); // Example, needs care
                        }
                    }
                    // Clicks in other windows' content regions are ignored here
                    break;
                default:
                    break;
            }
            break;

        case keyDown: case autoKey:
            // Key presses are generally handled by DialogSelect if a TE field has focus.
            // If menus need command-key equivalents, handle them here.
            // Example: if (event->modifiers & cmdKey) HandleMenuClick(MenuKey((char)event->message & charCodeMask));
            break;

        case updateEvt:
            // A window needs to be redrawn
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                BeginUpdate(whichWindow); // Set up clipping region
                DrawDialog(whichWindow);  // Redraw standard dialog items (buttons, checkboxes, static text)
                                          // May or may not draw frames for userItems.
                UpdateDialogTE();         // Call our function to redraw TE content
                EndUpdate(whichWindow);   // Restore clipping region
            } else {
                // Handle updates for other windows (e.g., Desk Accessories) if necessary
            }
            break;

        case activateEvt:
            // A window is becoming active or inactive
            whichWindow = (WindowPtr)event->message;
            if (whichWindow == (WindowPtr)gMainWindow) {
                // Call our function to activate/deactivate TE fields
                ActivateDialogTE((event->modifiers & activeFlag) != 0);
            }
            // Handle activation for other windows if necessary
            break;

        default:
            // Ignore other event types
            break;
    }
}