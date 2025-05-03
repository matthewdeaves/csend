#ifndef DIALOG_MESSAGES_H
#define DIALOG_MESSAGES_H

#include <MacTypes.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Dialogs.h> // For DialogPtr

/*----------------------------------------------------------*/
/* External Globals (Defined in dialog.c)                   */
/*----------------------------------------------------------*/

extern DialogPtr gMainWindow;       // The main dialog window
extern TEHandle gMessagesTE;        // Handle to the messages TextEdit record
extern ControlHandle gMessagesScrollBar; // Handle to the messages scrollbar

/*----------------------------------------------------------*/
/* Function Prototypes                                      */
/*----------------------------------------------------------*/

/**
 * @brief Initializes the Messages TextEdit field and its associated scrollbar.
 *
 * Gets the dialog items, creates the TE record, gets the scrollbar handle,
 * and sets initial scrollbar state.
 *
 * @param dialog The parent dialog pointer.
 * @return Boolean True if initialization was successful, false otherwise.
 */
Boolean InitMessagesTEAndScrollbar(DialogPtr dialog);

/**
 * @brief Cleans up resources used by the Messages TE field and scrollbar.
 *
 * Disposes of the TE record. The scrollbar is disposed with the dialog.
 */
void CleanupMessagesTEAndScrollbar(void);

/**
 * @brief Appends text to the end of the Messages TE field.
 *
 * Handles scrolling to the end and adjusting the scrollbar.
 * Prevents appending if the TE field is full (approx 32k limit).
 *
 * @param text The C string text to append. Should include '\r' for newlines.
 */
void AppendToMessagesTE(const char *text);

/**
 * @brief Adjusts the messages scrollbar's range and value based on TE content.
 *
 * Calculates the total lines, visible lines, and sets the scrollbar max/value.
 * Hides the scrollbar if no scrolling is needed.
 */
void AdjustMessagesScrollbar(void);

/**
 * @brief Handles clicks within the messages scrollbar control.
 *
 * This function is called by the Pascal scroll action procedure (MyScrollAction).
 * It determines the scroll amount based on the part code and updates the TE display.
 *
 * @param theControl The scrollbar control handle that was clicked.
 * @param partCode The part of the scrollbar that was clicked (arrows, page, thumb).
 */
void HandleMessagesScrollClick(ControlHandle theControl, short partCode);

/**
 * @brief Updates the display of the Messages TE field during an update event.
 *
 * Calls TEUpdate for the messages TE record.
 *
 * @param dialog The parent dialog pointer (used to get the port).
 */
void HandleMessagesTEUpdate(DialogPtr dialog);

/**
 * @brief Activates or deactivates the Messages TE field and scrollbar.
 *
 * Calls TEDeactivate (deactivation only needed for TE) and HiliteControl.
 *
 * @param activating True to activate, false to deactivate.
 */
void ActivateMessagesTEAndScrollbar(Boolean activating);

/**
 * @brief Scrolls the Messages TextEdit field vertically.
 *
 * Helper function used by scrollbar handling logic.
 *
 * @param deltaPixels The number of pixels to scroll (positive scrolls up, negative down).
 */
void ScrollMessagesTE(short deltaPixels);


#endif /* DIALOG_MESSAGES_H */