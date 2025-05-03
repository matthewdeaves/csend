#ifndef DIALOG_INPUT_H
#define DIALOG_INPUT_H

#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h> // For DialogPtr
#include <Events.h>  // For EventRecord

/*----------------------------------------------------------*/
/* External Globals (Defined in dialog.c)                   */
/*----------------------------------------------------------*/

extern DialogPtr gMainWindow; // The main dialog window
extern TEHandle gInputTE;     // Handle to the input TextEdit record

/*----------------------------------------------------------*/
/* Function Prototypes                                      */
/*----------------------------------------------------------*/

/**
 * @brief Initializes the Input TextEdit field.
 *
 * Gets the dialog item, creates the TE record.
 *
 * @param dialog The parent dialog pointer.
 * @return Boolean True if initialization was successful, false otherwise.
 */
Boolean InitInputTE(DialogPtr dialog);

/**
 * @brief Cleans up resources used by the Input TE field.
 *
 * Disposes of the TE record.
 */
void CleanupInputTE(void);

/**
 * @brief Handles mouse clicks within the Input TE field's view rectangle.
 *
 * Calls TEClick if the click location is within the TE view rectangle.
 *
 * @param dialog The parent dialog pointer (used for coordinate conversion).
 * @param theEvent The mouse down event record.
 */
void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent);

/**
 * @brief Updates the display of the Input TE field during an update event.
 *
 * Calls TEUpdate for the input TE record.
 *
 * @param dialog The parent dialog pointer (used to get the port).
 */
void HandleInputTEUpdate(DialogPtr dialog);

/**
 * @brief Activates or deactivates the Input TE field.
 *
 * Calls TEActivate or TEDeactivate.
 *
 * @param activating True to activate, false to deactivate.
 */
void ActivateInputTE(Boolean activating);

/**
 * @brief Gets the current text content from the Input TE field.
 *
 * Copies the text into the provided buffer as a C string.
 * Handles potential truncation if the buffer is too small.
 *
 * @param buffer The character buffer to copy the text into.
 * @param bufferSize The size of the buffer.
 * @return Boolean True if text was retrieved successfully, false otherwise (e.g., TE handle invalid).
 */
Boolean GetInputText(char *buffer, short bufferSize);

/**
 * @brief Clears the text content of the Input TE field.
 */
void ClearInputText(void);


#endif /* DIALOG_INPUT_H */