// FILE: ./classic_mac/dialog.h
#ifndef DIALOG_H
#define DIALOG_H

#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>

// --- Constants ---
#define kBaseResID 128      // Matches the DLOG resource ID created in ResEdit

// Dialog Item IDs (Matching ResEdit layout)
#define kPeerListUserItem   1   // User Item placeholder for the List Manager list (TODO)
#define kMessagesTextEdit   2   // UserItem for displaying received messages (TENew)
#define kInputTextEdit      3   // UserItem for typing messages (TENew)
#define kSendButton         4   // Button to send the message
#define kBroadcastCheckbox  5   // CheckBox for broadcast option

// --- Global Variables ---
extern DialogPtr   gMainWindow;          // Pointer to our main dialog window
extern TEHandle    gMessagesTE;          // Handle for the received messages TextEdit item
extern TEHandle    gInputTE;             // Handle for the input TextEdit item
extern Boolean     gDialogTEInitialized; // Flag indicating if TE fields are initialized
extern char        gMyUsername[32];      // Username for this client

// --- Function Prototypes ---

/**
 * @brief Initializes the main application dialog window and its controls.
 * @details Loads the DLOG resource, creates the TextEdit fields using TENew,
 *          shows the window, and sets the initial focus.
 * @return true if initialization was successful (dialog loaded, TEs created).
 * @return false if any critical step failed.
 */
Boolean InitDialog(void);

/**
 * @brief Cleans up dialog resources.
 * @details Disposes of the TextEdit handles and the main dialog window.
 */
void CleanupDialog(void);

/**
 * @brief Handles clicks on active dialog items (buttons, checkboxes).
 * @details Determines which item was clicked and calls the appropriate action function
 *          (e.g., DoSendAction for the send button, toggles checkbox state).
 * @param dialog The dialog where the click occurred.
 * @param itemHit The item number that was clicked.
 */
void HandleDialogClick(DialogPtr dialog, short itemHit);

/**
 * @brief Performs the action when the 'Send' button is clicked.
 * @details Retrieves text from the input field, checks the broadcast checkbox,
 *          formats the message, calls the appropriate network send function (TODO),
 *          appends the sent message to the display area, and clears the input field.
 * @param dialog The dialog containing the controls.
 */
void DoSendAction(DialogPtr dialog);

/**
 * @brief Appends text to the messages TextEdit field.
 * @details Safely appends the given C string to the end of the text in the
 *          gMessagesTE field, handling locking/unlocking of the handle.
 * @param text The C string to append.
 */
void AppendToMessagesTE(const char *text);

/**
 * @brief Handles activation and deactivation of the dialog's TextEdit fields.
 * @details Calls TEActivate or TEDeactivate on gMessagesTE and gInputTE based on
 *          whether the main window is becoming active or inactive.
 * @param activating true if the window is activating, false if deactivating.
 */
void ActivateDialogTE(Boolean activating);

/**
 * @brief Handles updating the content of the dialog's TextEdit fields.
 * @details Calls TEUpdate for both gMessagesTE and gInputTE within the context
 *          of a BeginUpdate/EndUpdate block for the main window.
 */
void UpdateDialogTE(void);


#endif // DIALOG_H