#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Resources.h> // Needed for GetResource, ReleaseResource

// Shared constant definitions
#include "common_defs.h"

// --- Constants ---
#define kBaseResID 128 // Resource ID for our DLOG/DITL

// --- Globals ---
DialogPtr   gMainWindow = NULL; // Pointer to our main dialog window
Boolean     gDone = false;      // Controls the main event loop termination

// --- Forward Declarations ---
void InitializeToolbox(void);
void MainEventLoop(void);
void HandleEvent(EventRecord *event);

//-------------------------------------------------------------------------
// InitializeToolbox
//-------------------------------------------------------------------------
// Initializes the essential Macintosh Toolbox managers.
//-------------------------------------------------------------------------
void InitializeToolbox(void)
{
    // Initialize QuickDraw globals
    InitGraf(&qd.thePort);

    // Initialize other managers
    InitFonts();
    InitWindows();
    InitMenus();   // Even if we don't have menus yet, Dialogs might need it
    TEInit();      // Initialize TextEdit for the EditText item
    InitDialogs(nil); // Pass nil for resumeProc
    InitCursor();  // Initialize the cursor to the arrow
}

//-------------------------------------------------------------------------
// MainEventLoop
//-------------------------------------------------------------------------
// The main event loop for the application.
// Waits for events and dispatches them.
//-------------------------------------------------------------------------
void MainEventLoop(void)
{
    EventRecord event;
    Boolean     gotEvent;

    // --- Load and Display the Dialog ---
    // GetNewDialog creates the window based on DLOG and DITL resources.
    // kBaseResID: The resource ID of the DLOG.
    // nil: Optional storage for the dialog record (we let the Dialog Manager allocate it).
    // (WindowPtr)-1L: Place the window in front of all other windows.
    gMainWindow = GetNewDialog(kBaseResID, nil, (WindowPtr)-1L);

    if (gMainWindow == NULL) {
        // Very basic error handling: Exit if dialog couldn't load.
        // In a real app, show an alert.
        ExitToShell();
        return;
    }

    // --- Event Loop ---
    while (!gDone) // Use the global gDone flag
    {
        // Wait for the next event.
        // systemTask allows background tasks (like MacTCP later) to run.
        // Adjust sleep value (ticks) as needed; 60 ticks = 1 second.
        gotEvent = WaitNextEvent(everyEvent, &event, 60L, nil);

        if (gotEvent)
        {
            // --- Handle Dialog Events ---
            // IsDialogEvent checks if the event is relevant to any modeless dialog.
            // If it is, DialogSelect handles standard interactions (button clicks, text entry).
            if (IsDialogEvent(&event)) {
                 DialogPtr whichDialog;
                 short itemHit;

                 // FindDialogItem checks if the event hit an active item in *any* modeless dialog.
                 // DialogSelect handles standard item interactions (like button clicks, text edit focus).
                 // It returns true if it handled the event and provides the item number hit.
                 if (DialogSelect(&event, &whichDialog, &itemHit)) {
                     // Check if it was our main dialog
                     if (whichDialog == gMainWindow) {
                         // Handle specific item hits if needed
                         // For now, ModalDialog handles basic clicks (like highlighting the button)
                         // We'll add specific logic for the Send button later.

                         // Example: Check if the 'Send' button (assuming item #4) was hit
                         // if (itemHit == 4) {
                         //     // Send message logic here...
                         // }
                     }
                 }
            } else {
                // Handle non-dialog events (like menu clicks, window activation, etc.)
                HandleEvent(&event);
            }
        }
        else
        {
            // No event - opportunity for idle processing (e.g., MacTCP polling)
            // MacTCPIdle(); // We'll add this later
        }
    }

    // --- Cleanup ---
    // This happens *after* the loop terminates (gDone is true)
    if (gMainWindow != NULL) {
        DisposeDialog(gMainWindow);
        gMainWindow = NULL; // Good practice
    }
}

//-------------------------------------------------------------------------
// HandleEvent
//-------------------------------------------------------------------------
// Handles non-dialog events.
//-------------------------------------------------------------------------
void HandleEvent(EventRecord *event)
{
    short     windowPart;
    WindowPtr whichWindow;

    switch (event->what)
    {
        case mouseDown:
            windowPart = FindWindow(event->where, &whichWindow);
            switch (windowPart) {
                case inMenuBar:
                    // HandleMenuClick(MenuSelect(event->where)); // Add later
                    break;
                case inSysWindow:
                    // Handle system clicks (e.g., desk accessories)
                    SystemClick(event, whichWindow);
                    break;
                case inDrag:
                     // Allow dragging the dialog window
                     // Ensure bounds are reasonable, screenBits.bounds covers the main screen
                     DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                     break;
                case inGoAway:
                    // Check if the click is in the close box of our main window
                    // Need to cast gMainWindow to WindowPtr for comparison
                    if (whichWindow == (WindowPtr)gMainWindow) {
                        // TrackGoAway provides visual feedback (highlights the box)
                        // and returns true if the mouse is released inside the box.
                        if (TrackGoAway(whichWindow, event->where)) {
                            gDone = true; // Set the global flag to terminate the loop
                        }
                    }
                    break;
                // Add cases for inZoomIn, inZoomOut if needed
                default:
                    break;
            }
            break;

        case keyDown:
        case autoKey:
            // Handle key presses - needed for menus, potentially dialogs too
            // char key = event->message & charCodeMask;
            // if (event->modifiers & cmdKey) {
            //     HandleMenuClick(MenuKey(key)); // Add later
            // }
            break;

        case updateEvt:
            // Handle window updates (redraw content)
            whichWindow = (WindowPtr)event->message;
            BeginUpdate(whichWindow);
            // Redraw custom content (User Items) here later
            // Example: DrawPeerList(whichWindow);
            // Example: DrawMessageHistory(whichWindow);
            // The Dialog Manager handles drawing standard controls like buttons/EditText
            DrawDialog(whichWindow); // Ensures standard controls are drawn
            EndUpdate(whichWindow);
            break;

        case activateEvt:
            // Handle window activation/deactivation
            // Useful for highlighting controls, etc.
            // whichWindow = (WindowPtr)event->message;
            // if (event->modifiers & activeFlag) {
            //     // Window activated
            // } else {
            //     // Window deactivated
            // }
            break;

        // Add other event types as needed (diskEvt, osEvt, etc.)

        default:
            break;
    }
}


//-------------------------------------------------------------------------
// main
//-------------------------------------------------------------------------
// Entry point of the application.
//-------------------------------------------------------------------------
int main(void)
{
    InitializeToolbox();
    MainEventLoop();
    // Cleanup (DisposeDialog) happens at the end of MainEventLoop
    return 0;
}