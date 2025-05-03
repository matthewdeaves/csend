#ifndef DIALOG_H
#define DIALOG_H

#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h>
#include <Events.h>

// Include the new component headers
#include "dialog_messages.h"
#include "dialog_input.h"
#include "dialog_peerlist.h"

/*----------------------------------------------------------*/
/* Dialog Resource Constants                                */
/*----------------------------------------------------------*/

#define kBaseResID 128          // Resource ID of the DLOG/DITL

// Dialog Item Numbers (DITL IDs)
#define kPeerListUserItem 1     // UserItem for the List Manager peer list
#define kMessagesTextEdit 2     // UserItem for the messages TextEdit field
#define kInputTextEdit 3        // UserItem for the input TextEdit field
#define kSendButton 4           // Button control for sending messages
#define kBroadcastCheckbox 5    // Checkbox control for broadcast toggle
#define kMessagesScrollbar 6    // Scrollbar control for the messages TE field

/*----------------------------------------------------------*/
/* Global Variables (Defined in dialog.c)                   */
/*----------------------------------------------------------*/

// These are declared here without 'extern' because they are defined in dialog.c
// The component modules (.h/.c) declare them 'extern' to access them.

extern DialogPtr gMainWindow;           // Pointer to the main dialog window
extern TEHandle gMessagesTE;            // Handle for messages display
extern TEHandle gInputTE;               // Handle for user input
extern ListHandle gPeerListHandle;      // Handle for peer list display
extern ControlHandle gMessagesScrollBar; // Handle for messages scrollbar
extern Boolean gDialogTEInitialized;    // Flag: True if both TEs initialized
extern Boolean gDialogListInitialized;  // Flag: True if List Manager list initialized
extern char gMyUsername[32];            // User's chosen name
extern Cell gLastSelectedCell;          // Last clicked cell in the peer list

/*----------------------------------------------------------*/
/* Function Prototypes (Implemented in dialog.c)            */
/*----------------------------------------------------------*/

/**
 * @brief Initializes the main application dialog window and its components.
 *
 * Loads the dialog resource, creates component controls (TE, List, Scrollbar)
 * by calling component initialization functions. Shows the window.
 *
 * @return Boolean True if initialization was successful, false otherwise.
 */
Boolean InitDialog(void);

/**
 * @brief Cleans up the main dialog and its components.
 *
 * Calls component cleanup functions and disposes of the dialog window.
 */
void CleanupDialog(void);

/**
 * @brief Handles mouse clicks within the dialog window's content area.
 *
 * Determines which item or component was clicked and routes the event
 * to the appropriate component handler (e.g., HandlePeerListClick) or
 * handles button/checkbox clicks directly.
 *
 * @param dialog The dialog pointer where the click occurred.
 * @param itemHit The dialog item number hit (if identified by DialogSelect).
 * @param theEvent The original mouse down event record.
 */
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent);

/**
 * @brief Performs the action associated with the Send button.
 *
 * Retrieves text from the input field, determines the recipient (broadcast or selected peer),
 * formats the message, (conceptually) sends it, appends it to the messages field,
 * and clears the input field.
 *
 * @param dialog The dialog pointer.
 */
void DoSendAction(DialogPtr dialog);

/**
 * @brief Activates or deactivates the dialog's interactive components.
 *
 * Calls component activation/deactivation functions.
 *
 * @param activating True to activate, false to deactivate.
 */
void ActivateDialogTE(Boolean activating); // Renaming might be good later, e.g., ActivateDialogControls

/**
 * @brief Handles redrawing controls during an update event.
 *
 * Calls component update functions and DrawControls.
 */
void UpdateDialogControls(void);

/**
 * @brief Pascal callback function for the messages scrollbar action.
 *
 * This function is registered with TrackControl for scrollbar parts (arrows, page).
 * It calls the C handler HandleMessagesScrollClick.
 * IMPORTANT: Must remain a pascal function.
 *
 * @param theControl The scrollbar control handle.
 * @param partCode The part of the scrollbar activated.
 */
pascal void MyScrollAction(ControlHandle theControl, short partCode);


#endif /* DIALOG_H */