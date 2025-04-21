// FILE: ./classic_mac/dialog.c
/**
 * @file dialog.c (Classic Mac)
 * @brief Handles the main user interface dialog for the Classic Mac P2P Chat Client.
 *        Manages dialog creation, control handling (buttons, checkboxes), TextEdit fields,
 *        and initiates actions like sending messages.
 */

// --- Project Includes ---
#include "dialog.h"      // Header for this module (defines constants, globals, prototypes)
#include "logging.h"     // For log_message()
#include "network.h"     // For gMyLocalIPStr (local IP for formatting messages)
#include "protocol.h"    // For MSG_TEXT, format_message()
#include "common_defs.h" // For BUFFER_SIZE, peer_t
#include "peer_mac.h"    // For GetPeerByIndex() and peer list access

// --- Mac OS Includes ---
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Memory.h>      // For HLock, HUnlock, HSetState, HGetState, BlockMoveData
#include <Sound.h>       // For SysBeep()
#include <Resources.h>   // For GetResource, ReleaseResource (though not used directly here now)
#include <string.h>      // For strlen, sprintf, strcpy
#include <stdlib.h>      // For atoi()

// --- Global Variable Definitions ---
// These are declared 'extern' in dialog.h and defined here.
DialogPtr   gMainWindow = NULL;          // Pointer to our main dialog window
TEHandle    gMessagesTE = NULL;          // Handle for the received messages TextEdit item
TEHandle    gInputTE = NULL;             // Handle for the input TextEdit item
Boolean     gDialogTEInitialized = false; // Flag indicating if TE fields are initialized
char        gMyUsername[32] = "MacUser"; // Default username (can be changed later)

/**
 * @brief Initializes the main application dialog window and its controls.
 * @details Loads the DLOG resource (ID kBaseResID), creates the TextEdit fields
 *          within the UserItem placeholders defined in the DITL resource, shows
 *          the window, and sets the initial focus to the input field.
 * @return true if initialization was successful (dialog loaded, TEs created).
 * @return false if any critical step failed (e.g., resource not found, TENew failed).
 */
Boolean InitDialog(void) {
    DialogItemType itemType;        // To store the type of dialog item retrieved
    Handle         itemHandle;      // Generic handle to the dialog item
    Rect           destRectMessages, viewRectMessages; // Rectangles for Messages TE
    Rect           destRectInput, viewRectInput;       // Rectangles for Input TE
    Boolean        messagesOk = false; // Flag for successful Messages TE creation
    Boolean        inputOk = false;    // Flag for successful Input TE creation

    log_message("Loading dialog resource ID %d...", kBaseResID);
    // GetNewDialog loads the DLOG resource and creates the window structure.
    // NULL: No specific memory allocation requested.
    // (WindowPtr)-1L: Place the window in front of all other windows.
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
    if (gMainWindow == NULL) {
        // ResError() returns the result code from the last Resource Manager call.
        log_message("Fatal: GetNewDialog failed (Error: %d).", ResError());
        return false; // Critical failure, cannot proceed without the dialog.
    }
    log_message("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);

    // Make the dialog visible and active.
    log_message("Showing window...");
    ShowWindow(gMainWindow);    // Make the window visible.
    SelectWindow(gMainWindow);  // Bring the window to the front.
    log_message("Setting port...");
    // SetPort makes the dialog window the current drawing environment (GrafPort).
    // Necessary before drawing or interacting with controls within the dialog.
    SetPort((GrafPtr)gMainWindow);
    log_message("Port set.");

    // --- Initialize Dialog Controls ---
    // The DITL resource defines the layout, but UserItems need manual setup (like TextEdit).

    // Messages TE (Item kMessagesTextEdit - UserItem)
    log_message("Getting item %d info (Messages UserItem)...", kMessagesTextEdit);
    // GetDialogItem retrieves information about a specific item in the dialog.
    GetDialogItem(gMainWindow, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    // Check if the item type matches 'userItem' as defined in the DITL.
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kMessagesTextEdit,
                    destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right);
        // The viewRect defines the area where text is visible (can be smaller than destRect for scrolling).
        // The destRect defines the logical area where text can exist.
        viewRectMessages = destRectMessages; // Start with view = dest for simplicity.

        log_message("Calling TENew for Messages TE...");
        // TENew creates a new TextEdit record and handle within the specified rectangles.
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE! Out of memory?");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            // TEAutoView enables automatic scrolling when text exceeds the viewRect.
            TEAutoView(true, gMessagesTE);
            log_message("TEAutoView finished for Messages TE.");
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL; // Ensure handle is NULL if setup failed.
        messagesOk = false;
    }

    // Input TE (Item kInputTextEdit - UserItem)
    log_message("Getting item %d info (Input UserItem)...", kInputTextEdit);
    GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kInputTextEdit,
                    destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right);
        viewRectInput = destRectInput; // Start with view = dest.

        log_message("Calling TENew for Input TE...");
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            inputOk = false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            // AutoView less critical for single-line input, but doesn't hurt.
            TEAutoView(true, gInputTE);
            log_message("TEAutoView finished for Input TE.");
            inputOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL; // Ensure handle is NULL if setup failed.
        inputOk = false;
    }

    // Set flag indicating TE fields are now initialized (or attempted).
    // Crucial for log_message to know if it can append to the dialog.
    gDialogTEInitialized = (messagesOk && inputOk);
    log_message("Dialog TE fields initialization complete (Success: %s). Enabling dialog logging.", gDialogTEInitialized ? "YES" : "NO");

    if (!gDialogTEInitialized) {
        log_message("Error: One or both TextEdit fields failed to initialize.");
        // Let main handle cleanup of the partially created dialog.
        return false;
    }

    // --- Set initial focus AFTER successful TE init ---
    // Make the input field ready for typing immediately.
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    TEActivate(gInputTE); // Explicitly activate the input TE field.
    log_message("Input TE activated.");

    // TODO: Initialize the Peer List User Item (kPeerListUserItem)
    // This would involve:
    // 1. GetDialogItem for kPeerListUserItem.
    // 2. Create a List Manager list using LNew within its rectangle.
    // 3. Store the ListHandle globally or associate it with the dialog.
    log_message("Peer list (item %d) initialization needed (List Manager).", kPeerListUserItem);


    return true; // Initialization successful
}

/**
 * @brief Cleans up dialog resources before application termination.
 * @details Disposes of the TextEdit handles and the main dialog window
 *          to release allocated memory.
 */
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

    // Dispose TE Handles if they were created. TEDispose releases the TE record and associated data.
    if (gMessagesTE != NULL) {
        log_message("Disposing Messages TE...");
        TEDispose(gMessagesTE);
        gMessagesTE = NULL; // Set handle to NULL after disposal.
    }
    if (gInputTE != NULL) {
        log_message("Disposing Input TE...");
        TEDispose(gInputTE);
        gInputTE = NULL; // Set handle to NULL after disposal.
    }

    // TODO: Dispose the List Manager list handle if it was created.
    // LDispose(gPeerListHandle);

    // Dispose the main dialog window if it was created.
    // DisposeDialog releases the window record and associated resources.
    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL; // Set pointer to NULL after disposal.
    }

    gDialogTEInitialized = false; // Reset the initialization flag.
    log_message("Dialog cleanup complete.");
}

/**
 * @brief Handles clicks on active dialog items (buttons, checkboxes).
 * @details This function is called by the main event loop when DialogSelect
 *          indicates a click occurred in an enabled item within our dialog.
 * @param dialog The dialog where the click occurred (should be gMainWindow).
 * @param itemHit The item number (from DITL) that was clicked.
 */
void HandleDialogClick(DialogPtr dialog, short itemHit) {
    ControlHandle   checkboxHandle; // Handle specific to control items
    DialogItemType  itemType;       // Type of the item clicked
    Handle          itemHandle;     // Generic handle to the item
    Rect            itemRect;       // Rectangle of the item (needed for GetDialogItem)
    short           currentValue;   // Current value of a control (e.g., checkbox state)

    // Ensure the click is for our main window. Defensive check.
    if (dialog != gMainWindow) return;

    switch (itemHit) {
        case kSendButton:
            // User clicked the 'Send' button.
            log_message("Send button clicked.");
            DoSendAction(dialog); // Call the function to handle sending.
            break;

        case kBroadcastCheckbox:
            // User clicked the 'Broadcast' checkbox.
            // Retrieve the control handle for the checkbox.
            GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
            // Verify it's actually a checkbox control (safety check).
            // Control items have types like ctrlItem + chkCtrl, ctrlItem + btnCtrl, etc.
            if (itemType == (ctrlItem + chkCtrl)) {
                checkboxHandle = (ControlHandle)itemHandle;
                // Get the current value (0 for unchecked, 1 for checked).
                currentValue = GetControlValue(checkboxHandle);
                // Toggle the value (0 becomes 1, 1 becomes 0).
                SetControlValue(checkboxHandle, !currentValue);
                log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
            } else {
                log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
            }
            break;

        // TODO: Handle clicks in the Peer List (kPeerListUserItem)
        // This would involve:
        // 1. Checking if itemHit == kPeerListUserItem.
        // 2. Getting the click location relative to the list's rectangle.
        // 3. Calling LClick() to handle the click within the List Manager list.
        // 4. LClick() will update the selection; you might need to redraw or store the selection.

        default:
            // Clicks in userItems that are TextEdit fields (kMessagesTextEdit, kInputTextEdit)
            // are automatically handled by DialogSelect calling TEClick.
            // We don't need specific handling here unless we want custom behavior.
            // Clicks in other items (like static text or icons) are usually ignored.
            // log_message("HandleDialogClick: Click on unhandled item %d", itemHit);
            break;
    }
}

/**
 * @brief Performs the action when the 'Send' button is clicked.
 * @details Retrieves text from the input field, checks the broadcast checkbox state,
 *          formats the message using the shared protocol, calls the appropriate
 *          network send function (TODO: Implement network send), appends the sent
 *          message ("You: ...") to the display area, and clears the input field.
 * @param dialog The dialog containing the controls (should be gMainWindow).
 */
void DoSendAction(DialogPtr dialog) {
    char            inputCStr[256]; // Buffer for C string from input TE field
    char            formattedMsg[BUFFER_SIZE]; // Buffer for protocol-formatted message
    ControlHandle   checkboxHandle; // Handle for the broadcast checkbox
    DialogItemType  itemType;       // Type of the checkbox item
    Handle          itemHandle;     // Generic handle for the checkbox item
    Rect            itemRect;       // Rectangle of the checkbox item
    Boolean         isBroadcast;    // Flag indicating if broadcast is checked
    char            displayMsg[BUFFER_SIZE + 100]; // Buffer for displaying "You: ..."
    SignedByte      teState;        // To preserve TE handle state during locking

    // 1. Get text from the input TextEdit field (Item #kInputTextEdit)
    if (gInputTE == NULL) {
        log_message("Error: Input TextEdit not initialized. Cannot send.");
        SysBeep(10); // Audible error feedback
        return;
    }

    // --- Safely get text from TEHandle ---
    // Handles can move in memory, so lock them before dereferencing.
    teState = HGetState((Handle)gInputTE); // Save current handle state (locked/unlocked, purgeable/not)
    HLock((Handle)gInputTE);               // Lock the handle in place

    // Check if the handle and its internal text handle are valid before accessing text.
    if (*gInputTE != NULL && (*gInputTE)->hText != NULL) {
        Size textLen = (**gInputTE).teLength; // Get the length of the text in the TE record.
        // Ensure we don't copy more than our C string buffer can hold.
        if (textLen > sizeof(inputCStr) - 1) {
             textLen = sizeof(inputCStr) - 1;
        }
        // BlockMoveData is efficient for copying raw bytes.
        // Source: The text buffer pointed to by the TE record's hText handle.
        // Destination: Our local C string buffer.
        // Length: The number of bytes to copy.
        BlockMoveData(*((**gInputTE).hText), inputCStr, textLen);
        inputCStr[textLen] = '\0'; // Manually null-terminate the C string.
    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
        HSetState((Handle)gInputTE, teState); // Restore original handle state on error.
        SysBeep(10);
        return;
    }
    HSetState((Handle)gInputTE, teState); // Restore original handle state after access.
    // --- End safe text retrieval ---

    // Check if the input field was empty.
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        SysBeep(5); // Different beep for non-critical feedback
        return;
    }

    // 2. Check state of the Broadcast CheckBox (Item #kBroadcastCheckbox)
    GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    // Verify it's the correct control type.
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        // GetControlValue returns 1 if checked, 0 if unchecked.
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false; // Default to false if the item isn't a checkbox.
    }

    // 3. Format the message using the shared protocol function
    // This creates the "TYPE|SENDER@IP|CONTENT" string.
    int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                      gMyUsername, gMyLocalIPStr, inputCStr);

    if (formatResult == 0) {
        // --- TODO: Send the message ---
        if (isBroadcast) {
            log_message("Broadcasting: %s (Network send not implemented)", inputCStr);
            // TODO: Call the actual UDP broadcast function here, passing formattedMsg.
            // Example: SendUDPBroadcast(gMacTCPRefNum, formattedMsg);
        } else {
            // --- Send to Selected Peer (Requires List Manager Interaction) ---
            // This part needs the List Manager list to be implemented for kPeerListUserItem.
            // We need to get the index of the currently selected peer in that list.

            // Placeholder: Assume we have a function GetSelectedPeerIndex() that returns
            // the 1-based index of the selected peer in the visible list, or 0 if none selected.
            int selectedPeerIndex = 1; // <<-- Replace with actual List Manager selection retrieval
            // Example: selectedPeerIndex = GetSelectedPeerIndex(gPeerListHandle);

            if (selectedPeerIndex > 0) {
                peer_t targetPeer;
                // GetPeerByIndex retrieves the peer_t data for the Nth *active* peer.
                if (GetPeerByIndex(selectedPeerIndex, &targetPeer)) {
                     log_message("Sending to peer %d (%s): %s (Network send not implemented)",
                                 selectedPeerIndex, targetPeer.ip, inputCStr);
                     // TODO: Call the actual TCP send function here using targetPeer.ip and formattedMsg.
                     // Example: SendTCPMessage(gMacTCPRefNum, targetPeer.ip, formattedMsg);
                } else {
                     log_message("Error: Cannot send, selected peer index %d not found or invalid.", selectedPeerIndex);
                     SysBeep(10);
                }
            } else {
                log_message("Error: Cannot send, no peer selected in the list.");
                SysBeep(10);
            }
        }

        // --- Append *sent* message to the display area (Item #kMessagesTextEdit) ---
        // Format the message to show "You: " prefix for clarity in the chat history.
        sprintf(displayMsg, "You: %s", inputCStr);
        AppendToMessagesTE(displayMsg); // Append "You: message"
        AppendToMessagesTE("\r");       // Append Mac-style newline (carriage return) for TE display.

    } else {
        // format_message failed (e.g., buffer too small).
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20); // More urgent beep for formatting error.
    }

    // 4. Clear the input EditText field (Item #kInputTextEdit)
    if (gInputTE != NULL) {
        teState = HGetState((Handle)gInputTE); // Save state
        HLock((Handle)gInputTE);               // Lock handle
        if (*gInputTE != NULL) {
            // Set the text content to an empty string.
            TESetText((Ptr)"", 0, gInputTE);
            // Recalculate line breaks and other formatting after changing text.
            TECalText(gInputTE);
        }
        HSetState((Handle)gInputTE, teState); // Restore state
        log_message("Input field cleared.");
    }

    // 5. Set focus back to input field
    // DialogSelect usually handles focus correctly after button clicks,
    // but explicitly activating ensures the caret is ready for typing.
    if (gInputTE != NULL) {
       TEActivate(gInputTE); // Ensure the input field is the active TE field.
       log_message("Input field activated.");
    }
}

/**
 * @brief Appends a C string to the messages TextEdit field (gMessagesTE).
 * @details Safely appends the given text to the end of the existing content
 *          in the message display area. Handles locking/unlocking the TE handle.
 * @param text The null-terminated C string to append.
 */
void AppendToMessagesTE(const char *text) {
    // Check if the TE handle is valid before proceeding.
    // This check is important because log_message calls this function,
    // and log_message might be called before the dialog is fully initialized.
    if (gMessagesTE == NULL || !gDialogTEInitialized) {
        // Log directly to file if possible, as we can't use the dialog TE.
        if (gLogFile != NULL) {
            fprintf(gLogFile, "Skipping AppendToMessagesTE: gMessagesTE is NULL or dialog not initialized.\n");
            fflush(gLogFile);
        }
        return;
    }

    SignedByte teState = HGetState((Handle)gMessagesTE); // Save handle state
    HLock((Handle)gMessagesTE);                          // Lock handle

    // Double-check the dereferenced handle is not NULL after locking.
    if (*gMessagesTE != NULL) {
        // Get the current length of the text in the TE record.
        long currentLength = (**gMessagesTE).teLength;
        // Move the insertion point (selection range) to the very end of the existing text.
        // Start and end of selection are both set to the current length.
        TESetSelect(currentLength, currentLength, gMessagesTE);
        // Insert the new text at the current insertion point.
        TEInsert((Ptr)text, strlen(text), gMessagesTE);

        // Optional: Automatically scroll to show the newly added text.
        // This scrolls the viewRect so the last line is visible.
        // Might need adjustment based on font size/line height.
        // TEScroll(0, 32767, gMessagesTE); // Scroll vertically as far down as possible
    } else {
        // Log an error if the dereferenced handle is NULL (memory corruption?)
        if (gLogFile != NULL) {
            fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL after HLock!\n");
            fflush(gLogFile);
        }
    }

    HSetState((Handle)gMessagesTE, teState); // Restore handle state (unlocks if locked)
}

/**
 * @brief Handles activation and deactivation events for the dialog's TextEdit fields.
 * @details This function is called by the main event loop when the dialog window
 *          receives an activate or deactivate event. It calls TEActivate or TEDeactivate
 *          on the TextEdit fields to correctly handle the blinking caret and selection highlighting.
 * @param activating true if the window is becoming active, false if it's becoming inactive.
 */
void ActivateDialogTE(Boolean activating) {
    // This function assumes gMainWindow, gMessagesTE, and gInputTE are valid,
    // as it's only called in response to events on gMainWindow after InitDialog succeeded.

    // log_message("ActivateDialogTE: %s", activating ? "Activating" : "Deactivating"); // Can be noisy

    // Activate/Deactivate the messages TE field
    if (gMessagesTE != NULL) {
        if (activating) {
            TEActivate(gMessagesTE); // Show selection/caret if it had focus
        } else {
            TEDeactivate(gMessagesTE); // Hide selection/caret
        }
    }

    // Activate/Deactivate the input TE field
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE); // Show selection/caret if it had focus
        } else {
            TEDeactivate(gInputTE); // Hide selection/caret
        }
    }
}

/**
 * @brief Handles update events for the dialog's TextEdit fields.
 * @details This function is called by the main event loop when the dialog window
 *          receives an update event (e.g., after being uncovered or resized).
 *          It calls TEUpdate for each TextEdit field, instructing TextEdit to redraw
 *          its content within its designated area. This must be called between
 *          BeginUpdate and EndUpdate for the window.
 */
void UpdateDialogTE(void) {
    Rect itemRect;                  // To store the rectangle of the TE field's UserItem
    DialogItemType itemTypeIgnored; // Ignored, but needed for GetDialogItem signature
    Handle itemHandleIgnored;       // Ignored

    // This function assumes gMainWindow, gMessagesTE, and gInputTE are valid,
    // as it's only called in response to events on gMainWindow after InitDialog succeeded.

    // log_message("UpdateDialogTE called."); // Can be noisy

    // Update the messages TE field
    if (gMessagesTE != NULL) {
        // Get the current rectangle of the UserItem associated with this TE field.
        // This rectangle defines the area TEUpdate should draw within.
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // Tell TextEdit to redraw its content within the given rectangle.
        // TEUpdate is smart enough to only redraw the necessary parts within the window's update region.
        TEUpdate(&itemRect, gMessagesTE);
    }

    // Update the input TE field
    if (gInputTE != NULL) {
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        TEUpdate(&itemRect, gInputTE);
    }

    // TODO: Update the Peer List User Item
    // If using List Manager, call LUpdate(gPeerListHandle) here.
    // If drawing manually, redraw the list content here.
}