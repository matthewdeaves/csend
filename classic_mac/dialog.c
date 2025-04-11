// FILE: ./classic_mac/dialog.c
#include "dialog.h"
#include "logging.h"   // For log_message
#include "network.h"   // For gMyLocalIPStr
#include "protocol.h"  // For MSG_TEXT, format_message
#include "common_defs.h" // For BUFFER_SIZE

#include <string.h>    // For strlen, sprintf
#include <Memory.h>    // For HLock, HUnlock, HSetState, HGetState, BlockMoveData
#include <Sound.h>     // For SysBeep
#include <Resources.h> // For GetResource, ReleaseResource (though not used directly here now)

// --- Global Variable Definitions ---
DialogPtr   gMainWindow = NULL;
TEHandle    gMessagesTE = NULL;
TEHandle    gInputTE = NULL;
Boolean     gDialogTEInitialized = false;
char        gMyUsername[32] = "MacUser"; // Default username

/**
 * @brief Initializes the main application dialog window and its controls.
 */
Boolean InitDialog(void) {
    DialogItemType itemType;
    Handle         itemHandle; // Still needed for GetDialogItem
    Rect           destRectMessages, viewRectMessages; // Rects for TENew
    Rect           destRectInput, viewRectInput;       // Rects for TENew
    Boolean        messagesOk = false;
    Boolean        inputOk = false;

    log_message("Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
    if (gMainWindow == NULL) {
        log_message("Fatal: GetNewDialog failed (Error: %d).", ResError());
        return false; // Critical failure
    }
    log_message("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);

    log_message("Showing window...");
    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);
    log_message("Setting port...");
    SetPort((GrafPtr)gMainWindow);
    log_message("Port set.");

    // --- Initialize Dialog Controls ---

    // Messages TE (Item 2 - UserItem)
    log_message("Getting item %d info (Messages UserItem)...", kMessagesTextEdit);
    GetDialogItem(gMainWindow, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kMessagesTextEdit,
                    destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right);
        viewRectMessages = destRectMessages; // Start with view = dest
        // InsetRect(&viewRectMessages, 1, 1); // Optional inset for border

        log_message("Calling TENew for Messages TE...");
        gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
        if (gMessagesTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Messages TE!");
            messagesOk = false;
        } else {
            log_message("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
            TEAutoView(true, gMessagesTE); // Enable auto-scrolling
            log_message("TEAutoView finished for Messages TE.");
            messagesOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
        gMessagesTE = NULL;
        messagesOk = false;
    }

    // Input TE (Item 3 - UserItem)
    log_message("Getting item %d info (Input UserItem)...", kInputTextEdit);
    GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
    if (itemType == userItem) {
        log_message("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kInputTextEdit,
                    destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right);
        viewRectInput = destRectInput; // Start with view = dest
        // InsetRect(&viewRectInput, 1, 1); // Optional inset for border

        log_message("Calling TENew for Input TE...");
        gInputTE = TENew(&destRectInput, &viewRectInput);
        if (gInputTE == NULL) {
            log_message("CRITICAL ERROR: TENew failed for Input TE!");
            inputOk = false;
        } else {
            log_message("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
            TEAutoView(true, gInputTE); // Enable auto-scrolling (though less common for input)
            log_message("TEAutoView finished for Input TE.");
            inputOk = true;
        }
    } else {
        log_message("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
        gInputTE = NULL;
        inputOk = false;
    }

    // Set flag indicating TE fields are now initialized (or attempted)
    // Only set true if BOTH TEs were successfully created.
    gDialogTEInitialized = (messagesOk && inputOk);
    log_message("Dialog TE fields initialization complete (Success: %s). Enabling dialog logging.", gDialogTEInitialized ? "YES" : "NO");

    if (!gDialogTEInitialized) {
        log_message("Error: One or both TextEdit fields failed to initialize.");
        // Cleanup partially created dialog? Or let main handle it?
        // For now, return false and let main handle cleanup.
        return false;
    }

    // --- Set initial focus AFTER successful TE init ---
    log_message("Setting focus to input field (item %d)...", kInputTextEdit);
    TEActivate(gInputTE); // Explicitly activate the input TE
    // SelectDialogItemText(gMainWindow, kInputTextEdit, 0, 0); // Alternative focus method
    log_message("Input TE activated.");

    return true; // Initialization successful
}

/**
 * @brief Cleans up dialog resources.
 */
void CleanupDialog(void) {
    log_message("Cleaning up Dialog...");

    // Dispose TE Handles if they were created
    if (gMessagesTE != NULL) {
        log_message("Disposing Messages TE...");
        TEDispose(gMessagesTE);
        gMessagesTE = NULL;
    }
    if (gInputTE != NULL) {
        log_message("Disposing Input TE...");
        TEDispose(gInputTE);
        gInputTE = NULL;
    }

    // Dispose the main dialog window if it was created
    if (gMainWindow != NULL) {
        log_message("Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }

    gDialogTEInitialized = false; // Reset flag
    log_message("Dialog cleanup complete.");
}

/**
 * @brief Handles clicks on active dialog items (buttons, checkboxes).
 */
void HandleDialogClick(DialogPtr dialog, short itemHit) {
    ControlHandle   checkboxHandle;
    DialogItemType  itemType;
    Handle          itemHandle;
    Rect            itemRect;
    short           currentValue;

    // Ensure the click is for our main window
    if (dialog != gMainWindow) return;

    switch (itemHit) {
        case kSendButton:
            log_message("Send button clicked.");
            DoSendAction(dialog);
            break;

        case kBroadcastCheckbox:
            GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
            // Verify it's actually a checkbox control
            if (itemType == (ctrlItem + chkCtrl)) {
                checkboxHandle = (ControlHandle)itemHandle;
                currentValue = GetControlValue(checkboxHandle);
                // Toggle the value
                SetControlValue(checkboxHandle, !currentValue);
                log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
            } else {
                log_message("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
            }
            break;

        default:
            // Clicks in userItems (now TE fields) are handled by DialogSelect/TEClick.
            // Clicks in the peer list userItem will need custom handling later.
            // log_message("HandleDialogClick: Click on unhandled item %d", itemHit);
            break;
    }
}

/**
 * @brief Performs the action when the 'Send' button is clicked.
 */
void DoSendAction(DialogPtr dialog) {
    char            inputCStr[256]; // Buffer for C string from input TE
    char            formattedMsg[BUFFER_SIZE]; // Buffer for protocol-formatted message
    ControlHandle   checkboxHandle;
    DialogItemType  itemType;
    Handle          itemHandle;
    Rect            itemRect;
    Boolean         isBroadcast;
    char            displayMsg[BUFFER_SIZE + 100]; // Buffer for displaying "You: ..."
    SignedByte      teState; // To preserve handle state

    // 1. Get text from the input EditText (Item #3)
    if (gInputTE == NULL) {
        log_message("Error: Input TextEdit not initialized. Cannot send.");
        SysBeep(10);
        return;
    }

    // Get text directly from TEHandle
    teState = HGetState((Handle)gInputTE); // Save state
    HLock((Handle)gInputTE);               // Lock handle
    if (*gInputTE != NULL && (*gInputTE)->hText != NULL) {
        Size textLen = (*gInputTE)->teLength;
        if (textLen > 255) textLen = 255; // Prevent overflow into inputCStr
        BlockMoveData(*((*gInputTE)->hText), inputCStr, textLen);
        inputCStr[textLen] = '\0'; // Null-terminate the C string
    } else {
        log_message("Error: Cannot get text from Input TE (NULL handle/hText).");
        HSetState((Handle)gInputTE, teState); // Restore state on error
        SysBeep(10);
        return;
    }
    HSetState((Handle)gInputTE, teState); // Restore state

    // Check if input is empty
    if (strlen(inputCStr) == 0) {
        log_message("Send Action: Input field is empty.");
        SysBeep(5);
        return;
    }

    // 2. Check state of the Broadcast CheckBox (Item #5)
    GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemType == (ctrlItem + chkCtrl)) {
        checkboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(checkboxHandle) == 1);
        log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_message("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
        isBroadcast = false;
    }

    // 3. Format the message using the shared protocol function
    int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                      gMyUsername, gMyLocalIPStr, inputCStr);

    if (formatResult == 0) {
        // --- TODO: Send the message ---
        if (isBroadcast) {
            log_message("Broadcasting: %s (Not implemented)", inputCStr);
            // Call UDP broadcast function here later
        } else {
            log_message("Sending to selected peer: %s (Not implemented)", inputCStr);
            // Call TCP send function here later (need selected peer IP)
        }

        // --- Append *sent* message to the display area (Item #2) ---
        // Ensure the display buffer is large enough for "You: " + message
        sprintf(displayMsg, "You: %s", inputCStr);
        AppendToMessagesTE(displayMsg); // Append "You: message"
        AppendToMessagesTE("\r");       // Append newline for TE display

    } else {
        log_message("Error: Failed to format message for sending (format_message returned %d).", formatResult);
        SysBeep(20);
    }

    // 4. Clear the input EditText (Item #3)
    if (gInputTE != NULL) {
        teState = HGetState((Handle)gInputTE); // Save state
        HLock((Handle)gInputTE);               // Lock handle
        if (*gInputTE != NULL) {
            TESetText("", 0, gInputTE); // Set TE text to empty
            TECalText(gInputTE);        // Recalculate line breaks etc.
        }
        HSetState((Handle)gInputTE, teState); // Restore state
        log_message("Input field cleared.");
    }

    // 5. Set focus back to input field (DialogSelect might handle this, but explicit doesn't hurt)
    if (gInputTE != NULL) {
       TEActivate(gInputTE); // Ensure it's active for typing
       // SelectDialogItemText(gMainWindow, kInputTextEdit, 0, 0); // Alternative
       log_message("Input field activated.");
    }
}

/**
 * @brief Appends text to the messages TextEdit field.
 */
void AppendToMessagesTE(const char *text) {
    // Check if the TE handle is valid before proceeding
    if (gMessagesTE == NULL) {
        // This shouldn't happen if gDialogTEInitialized is checked by caller (log_message)
        // but add a safety check here anyway.
        if (gLogFile != NULL) { fprintf(gLogFile, "ERROR in AppendToMessagesTE: gMessagesTE is NULL!\n"); fflush(gLogFile); }
        return;
    }

    SignedByte teState = HGetState((Handle)gMessagesTE); // Save handle state
    HLock((Handle)gMessagesTE);                          // Lock handle

    // Double-check the dereferenced handle is not NULL
    if (*gMessagesTE != NULL) {
        // Move the insertion point to the very end of the existing text
        TESetSelect((*gMessagesTE)->teLength, (*gMessagesTE)->teLength, gMessagesTE);
        // Insert the new text
        TEInsert(text, strlen(text), gMessagesTE);
        // Optional: Scroll to the end after inserting
        // TEScroll(0, (*gMessagesTE)->nLines * (*gMessagesTE)->lineHeight, gMessagesTE);
    } else {
        // Log an error if the dereferenced handle is NULL (memory corruption?)
        if (gLogFile != NULL) { fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL!\n"); fflush(gLogFile); }
    }

    HSetState((Handle)gMessagesTE, teState); // Restore handle state
}

/**
 * @brief Handles activation and deactivation of the dialog's TextEdit fields.
 */
void ActivateDialogTE(Boolean activating) {
    // No need to check gDialogTEInitialized here, as this is called from HandleEvent
    // which only runs after initialization.

    // log_message("ActivateDialogTE: %s", activating ? "Activating" : "Deactivating");

    // Activate/Deactivate the messages TE field
    if (gMessagesTE != NULL) {
        if (activating) {
            TEActivate(gMessagesTE);
        } else {
            TEDeactivate(gMessagesTE);
        }
        // TODO: Add code here to draw/erase focus borders if desired
        // Example: Get item rect, check if it has focus, FrameRect/EraseRect
    }

    // Activate/Deactivate the input TE field
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);
        } else {
            TEDeactivate(gInputTE);
        }
        // TODO: Add code here to draw/erase focus borders if desired
    }
}

/**
 * @brief Handles updating the content of the dialog's TextEdit fields.
 */
void UpdateDialogTE(void) {
    Rect itemRect;
    DialogItemType itemTypeIgnored; // Ignored, but needed for GetDialogItem
    Handle itemHandleIgnored;       // Ignored

    // No need to check gDialogTEInitialized here, as this is called from HandleEvent
    // which only runs after initialization.

    // log_message("UpdateDialogTE called.");

    // Update the messages TE field
    if (gMessagesTE != NULL) {
        // Get the current rectangle of the userItem associated with this TE field
        GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // EraseRect(&itemRect); // Optional: Erase before TEUpdate if DrawDialog doesn't clear userItems
        TEUpdate(&itemRect, gMessagesTE); // Tell TextEdit to redraw its content within the given rect
    }

    // Update the input TE field
    if (gInputTE != NULL) {
        // Get the current rectangle of the userItem associated with this TE field
        GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
        // EraseRect(&itemRect); // Optional: Erase before TEUpdate
        TEUpdate(&itemRect, gInputTE); // Tell TextEdit to redraw its content
    }
}