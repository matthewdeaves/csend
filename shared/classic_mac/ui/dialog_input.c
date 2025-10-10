#include "dialog_input.h"
#include "dialog.h"
#include "../shared/logging.h"
#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Windows.h>
#include <Memory.h>
#include <string.h>
#include <Quickdraw.h>
TEHandle gInputTE = NULL;
/*
 * Initialize TextEdit Handle for Input Field
 *
 * Classic Mac UI pattern: TextEdit in Dialog as User Item
 *
 * Per Inside Macintosh Volume I, Chapter 13 (Dialog Manager):
 * 1. User items provide custom drawing/handling within dialogs
 * 2. TextEdit requires manual management in dialog context
 * 3. Proper rect management prevents drawing artifacts
 * 4. Memory allocation must be checked (TENew can fail)
 *
 * This approach gives full control over text editing behavior
 * compared to editText items which have limited functionality.
 */
Boolean InitInputTE(DialogPtr dialog)
{
    DialogItemType itemType;
    Handle itemHandle;
    Rect itemRect;

    log_debug_cat(LOG_CAT_UI, "Initializing Input TE (as UserItem)...");

    /* Get dialog item information for text input area */
    GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &itemRect);
    if (itemType == userItem) {
        /* Set up TextEdit rectangles following Apple HIG guidelines
         *
         * teViewRect: Visible area for text (what user sees)
         * teDestRect: Formatting area (can be larger for scrolling)
         *
         * InsetRect by 1 pixel creates visual border and prevents
         * text from touching dialog item boundaries */
        Rect teViewRect = itemRect;
        Rect teDestRect = itemRect;
        InsetRect(&teViewRect, 1, 1);   /* Visual boundary inset */
        InsetRect(&teDestRect, 1, 1);   /* Formatting boundary inset */
        if (teViewRect.bottom <= teViewRect.top || teViewRect.right <= teViewRect.left) {
            log_debug_cat(LOG_CAT_UI, "ERROR: Input TE itemRect too small after insetting for border. Original: (%d,%d,%d,%d)",
                          itemRect.top, itemRect.left, itemRect.bottom, itemRect.right);
            gInputTE = NULL;
            return false;
        }
        /* Create TextEdit record
         * TENew allocates TERec and associated data structures
         * Critical: Check for NULL return (memory allocation failure) */
        gInputTE = TENew(&teDestRect, &teViewRect);
        if (gInputTE == NULL) {
            log_debug_cat(LOG_CAT_UI, "CRITICAL ERROR: TENew failed for Input TE! Out of memory?");
            return false;
        } else {
            log_debug_cat(LOG_CAT_UI, "TENew succeeded for Input TE. Handle: 0x%lX. ViewRect for TE: (%d,%d,%d,%d)",
                          (unsigned long)gInputTE, teViewRect.top, teViewRect.left, teViewRect.bottom, teViewRect.right);
            TESetText((Ptr)"", 0, gInputTE);
            TECalText(gInputTE);
            TESetSelect(0, 0, gInputTE);
            return true;
        }
    } else {
        log_debug_cat(LOG_CAT_UI, "ERROR: Item %d (kInputTextEdit) is Type: %d. Expected userItem. Cannot initialize Input TE.", kInputTextEdit, itemType);
        gInputTE = NULL;
        return false;
    }
}
/*
 * CLEANUP INPUT TEXTEDIT COMPONENT
 *
 * Properly disposes of the TextEdit handle and associated memory to prevent
 * resource leaks. This function must be called during dialog cleanup or
 * application termination.
 *
 * TEXTEDIT MEMORY MANAGEMENT:
 * TEDispose() frees all memory associated with the TextEdit record:
 * - The TERec structure itself
 * - Text storage handle (hText)
 * - Line height information
 * - Selection and formatting data
 * - Any associated style records
 *
 * DEFENSIVE PROGRAMMING:
 * - Checks for NULL handle before disposal
 * - Sets handle to NULL after disposal to prevent double-disposal
 * - Logs operations for debugging and verification
 *
 * CALL TIMING:
 * This function should be called:
 * - During dialog cleanup (CleanupDialog)
 * - On application quit
 * - Before reinitializing TextEdit components
 * - When switching between different input modes
 *
 * Per Inside Macintosh Volume I: "Always dispose of TextEdit records
 * when you're finished with them to avoid memory leaks."
 */
void CleanupInputTE(void)
{
    log_debug_cat(LOG_CAT_UI, "Cleaning up Input TE...");

    if (gInputTE != NULL) {
        TEDispose(gInputTE);  /* Free all TextEdit-associated memory */
        gInputTE = NULL;      /* Prevent accidental reuse of disposed handle */
    }

    log_debug_cat(LOG_CAT_UI, "Input TE cleanup finished.");
}
/*
 * Handle Mouse Clicks in TextEdit Input Field
 *
 * Classic Mac event handling pattern:
 * 1. Convert global coordinates to local (window-relative)
 * 2. Set proper graphics port for coordinate system
 * 3. Validate click is within TextEdit bounds
 * 4. Call TEClick with shift key state for selection extension
 * 5. Restore previous graphics port
 *
 * Per Inside Macintosh Volume I: "All QuickDraw operations are
 * relative to the current graphics port. Always save and restore
 * the port when drawing in different windows."
 */
void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent)
{
    if (gInputTE != NULL) {
        Point localPt = theEvent->where;         /* Copy global coordinates */
        GrafPtr oldPort;                         /* Save current port */
        Rect teViewRect = (**gInputTE).viewRect; /* Get TE's clickable area */

        /* Establish proper coordinate system for this dialog */
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));
        GlobalToLocal(&localPt);  /* Convert to window-local coordinates */
        if (PtInRect(localPt, &teViewRect)) {
            TEClick(localPt, (theEvent->modifiers & shiftKey) != 0, gInputTE);
        }
        SetPort(oldPort);
    }
}
/*
 * HANDLE INPUT TEXTEDIT UPDATE/REDRAW
 *
 * This function redraws the input TextEdit field during update events.
 * It's called when the system determines that the TextEdit area needs
 * to be refreshed due to window exposure, scrolling, or content changes.
 *
 * CLASSIC MAC UPDATE PATTERN:
 * 1. Save and set proper graphics port for coordinate system
 * 2. Get dialog item rectangle for border drawing
 * 3. Draw border frame around TextEdit area
 * 4. Lock TextEdit handle for safe memory access
 * 5. Erase background and redraw TextEdit content
 * 6. Restore handle state and graphics port
 *
 * MEMORY SAFETY:
 * Uses HGetState/HSetState pattern to safely access relocatable memory:
 * - Save current handle state (locked/unlocked, purgeable flags)
 * - Lock handle to prevent movement during access
 * - Access handle contents safely
 * - Restore original handle state
 *
 * DRAWING SEQUENCE:
 * - FrameRect(): Draws border around TextEdit field
 * - EraseRect(): Clears background to prevent drawing artifacts
 * - TEUpdate(): Redraws text content with proper formatting
 *
 * ERROR HANDLING:
 * Validates handle and dereferenced pointer at each step to prevent
 * crashes from corrupted or disposed memory.
 *
 * This function is typically called from the main dialog update routine
 * when the input field needs refreshing.
 */
void HandleInputTEUpdate(DialogPtr dialog)
{
    if (gInputTE != NULL) {
        Rect userItemRect;                /* Dialog item boundary rectangle */
        DialogItemType itemTypeIgnored;   /* Dialog item type (not needed) */
        Handle itemHandleIgnored;         /* Dialog item handle (not needed) */
        GrafPtr oldPort;                  /* Previous graphics port */
        Rect teActualViewRect;            /* TextEdit's actual view rectangle */

        /* Set up proper graphics port for drawing operations */
        GetPort(&oldPort);
        SetPort((GrafPtr)GetWindowPort(dialog));

        /* Get dialog item rectangle and draw border frame */
        GetDialogItem(dialog, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &userItemRect);
        FrameRect(&userItemRect);  /* Draw border around TextEdit area */

        /*
         * SAFE HANDLE ACCESS PATTERN
         *
         * Save handle state, lock for access, restore state when done.
         * This prevents crashes from relocatable memory movement.
         */
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);

        if (*gInputTE != NULL) {
            /* Get TextEdit's view rectangle and redraw content */
            teActualViewRect = (**gInputTE).viewRect;
            EraseRect(&teActualViewRect);              /* Clear background */
            TEUpdate(&teActualViewRect, gInputTE);     /* Redraw text content */
        } else {
            log_debug_cat(LOG_CAT_UI, "HandleInputTEUpdate ERROR: gInputTE deref failed after HLock!");
        }

        /* Restore handle state and graphics port */
        HSetState((Handle)gInputTE, teState);
        SetPort(oldPort);
    } else {
        log_debug_cat(LOG_CAT_UI, "HandleInputTEUpdate: gInputTE is NULL, skipping update.");
    }
}
/*
 * ACTIVATE/DEACTIVATE INPUT TEXTEDIT
 *
 * Manages the activation state of the input TextEdit field, controlling
 * whether it shows a blinking cursor and accepts keyboard input. This is
 * essential for proper focus management in Classic Mac applications.
 *
 * TEXTEDIT ACTIVATION STATES:
 *
 * ACTIVATED (activating = true):
 * - Shows blinking insertion point cursor
 * - Accepts keyboard input via TEKey()
 * - Highlights selection ranges
 * - Responds to text editing operations
 * - Indicates field has keyboard focus
 *
 * DEACTIVATED (activating = false):
 * - Hides insertion point cursor
 * - Ignores keyboard input
 * - Dims selection highlights
 * - Indicates field does not have focus
 * - Reduces visual distractions
 *
 * FOCUS MANAGEMENT:
 * Classic Mac applications must explicitly manage focus between
 * multiple TextEdit fields. Only one TextEdit should be active
 * at a time to provide clear user feedback about where typing
 * will appear.
 *
 * TYPICAL USAGE:
 * - Activate when user clicks in field or tabs to field
 * - Deactivate when user clicks elsewhere or tabs away
 * - Deactivate all fields when window loses focus
 * - Activate default field when window gains focus
 *
 * Per Apple Human Interface Guidelines: "Always provide clear
 * visual indication of which text field has keyboard focus."
 */
void ActivateInputTE(Boolean activating)
{
    if (gInputTE != NULL) {
        if (activating) {
            TEActivate(gInputTE);  /* Show cursor, enable text entry */
            log_debug_cat(LOG_CAT_UI, "ActivateInputTE: Activating Input TE.");
        } else {
            TEDeactivate(gInputTE);  /* Hide cursor, disable text entry */
            log_debug_cat(LOG_CAT_UI, "ActivateInputTE: Deactivating Input TE.");
        }
    }
}
/*
 * EXTRACT TEXT FROM INPUT TEXTEDIT FIELD
 *
 * Safely retrieves the current text content from the TextEdit field and
 * copies it to a C string buffer. This function handles all the complex
 * memory management required to access TextEdit's internal text storage.
 *
 * TEXTEDIT TEXT STORAGE ARCHITECTURE:
 * TextEdit stores text in a relocatable handle (hText) within the TERec:
 * - Text is stored as raw bytes (not null-terminated)
 * - Length tracked separately in teLength field
 * - Handle can move in memory (requires locking for access)
 * - Multiple levels of indirection: gInputTE -> TERec -> hText -> text data
 *
 * SAFE MEMORY ACCESS PATTERN:
 * 1. Validate all pointers and parameters
 * 2. Lock TextEdit handle to prevent movement
 * 3. Access TERec to get text handle and length
 * 4. Lock text handle to prevent movement
 * 5. Copy text data to caller's buffer
 * 6. Restore all handle states
 * 7. Null-terminate C string
 *
 * BUFFER MANAGEMENT:
 * - Checks buffer size to prevent overflow
 * - Truncates text if buffer too small (with warning)
 * - Always null-terminates result for C string safety
 * - Ensures valid output even on error conditions
 *
 * ERROR HANDLING:
 * - Validates TextEdit handle and text handle
 * - Returns empty string on any error condition
 * - Logs detailed error information for debugging
 * - Uses defensive programming throughout
 *
 * PARAMETERS:
 * - buffer: Destination C string buffer
 * - bufferSize: Size of destination buffer (including null terminator)
 *
 * RETURNS:
 * - true: Text retrieved successfully
 * - false: Error occurred (buffer will contain empty string)
 */
Boolean GetInputText(char *buffer, short bufferSize)
{
    /* Parameter validation */
    if (gInputTE == NULL || buffer == NULL || bufferSize <= 0) {
        if (buffer && bufferSize > 0) buffer[0] = '\0';  /* Ensure empty string */
        log_debug_cat(LOG_CAT_UI, "Error: GetInputText called with NULL TE/buffer or zero size.");
        return false;
    }

    /*
     * SAFE TEXTEDIT HANDLE ACCESS
     *
     * TextEdit handles are relocatable and must be locked during access
     * to prevent crashes from memory movement.
     */
    SignedByte teState = HGetState((Handle)gInputTE);
    HLock((Handle)gInputTE);
    Boolean success = false;

    if (*gInputTE != NULL && (**gInputTE).hText != NULL) {
        Handle textH = (**gInputTE).hText;    /* Text storage handle */
        Size textLen = (**gInputTE).teLength; /* Current text length */
        Size copyLen = textLen;               /* Amount to copy */

        /*
         * BUFFER OVERFLOW PROTECTION
         *
         * Ensure we don't copy more text than the buffer can hold,
         * leaving room for null terminator.
         */
        if (copyLen >= bufferSize) {
            copyLen = bufferSize - 1;  /* Leave room for null terminator */
            log_debug_cat(LOG_CAT_UI, "Warning: Input text truncated during GetInputText (buffer size %d, needed %ld).",
                         bufferSize, (long)textLen + 1);
        }

        /*
         * SAFE TEXT HANDLE ACCESS
         *
         * The text handle itself is also relocatable and must be locked
         * before dereferencing to copy the actual text data.
         */
        SignedByte textHandleState = HGetState(textH);
        HLock(textH);
        BlockMoveData(*textH, buffer, copyLen);  /* Copy text data */
        HSetState(textH, textHandleState);       /* Restore text handle state */

        buffer[copyLen] = '\0';  /* Null-terminate C string */
        success = true;
    } else {
        log_debug_cat(LOG_CAT_UI, "Error: Cannot get text from Input TE (NULL TE record or hText).");
        buffer[0] = '\0';  /* Return empty string on error */
        success = false;
    }

    /* Restore TextEdit handle state */
    HSetState((Handle)gInputTE, teState);
    return success;
}
/*
 * CLEAR INPUT TEXTEDIT FIELD
 *
 * Removes all text from the input field and resets the cursor position.
 * This function is typically called after successfully sending a message
 * to provide immediate user feedback and prepare for the next input.
 *
 * TEXTEDIT CLEARING SEQUENCE:
 * 1. TESetText() - Replace all text with empty string
 * 2. TECalText() - Recalculate line breaks and formatting
 * 3. TESetSelect() - Position cursor at beginning of field
 * 4. Force visual update to show changes immediately
 *
 * TEXT REPLACEMENT BEHAVIOR:
 * TESetText() with empty string and zero length completely replaces
 * the current text content. This is more efficient than selecting
 * all text and deleting it, and ensures clean state reset.
 *
 * CURSOR POSITIONING:
 * TESetSelect(0, 0) positions the insertion point at the beginning
 * of the now-empty field, ready for new text input. This provides
 * consistent user experience after clearing.
 *
 * IMMEDIATE VISUAL FEEDBACK:
 * Forces an immediate redraw of the input field so the user sees
 * the cleared state without waiting for the next update event.
 * This provides responsive UI feedback for user actions.
 *
 * MEMORY SAFETY:
 * Uses the standard handle locking pattern to safely access the
 * relocatable TextEdit memory structures during text manipulation.
 *
 * TYPICAL USAGE:
 * - After successfully sending a message
 * - When user selects "Clear" or similar command
 * - When resetting form state
 * - During error recovery to known clean state
 */
void ClearInputText(void)
{
    if (gInputTE != NULL) {
        /*
         * SAFE HANDLE ACCESS FOR TEXT MANIPULATION
         *
         * Lock the TextEdit handle to prevent memory movement during
         * text operations that modify the internal data structures.
         */
        SignedByte teState = HGetState((Handle)gInputTE);
        HLock((Handle)gInputTE);

        if (*gInputTE != NULL) {
            /* Clear text content and reset formatting */
            TESetText((Ptr)"", 0, gInputTE);    /* Set to empty string */
            TECalText(gInputTE);                /* Recalculate line breaks */
            TESetSelect(0, 0, gInputTE);        /* Position cursor at start */
        } else {
            log_debug_cat(LOG_CAT_UI, "ClearInputText Error: gInputTE deref failed!");
        }

        /* Restore handle state */
        HSetState((Handle)gInputTE, teState);
        log_debug_cat(LOG_CAT_UI, "Input field cleared.");

        /*
         * IMMEDIATE VISUAL UPDATE
         *
         * Force the input field to redraw immediately so the user sees
         * the cleared state without waiting for the next update event.
         * This provides responsive feedback for user actions.
         */
        if (gMainWindow != NULL) {
            HandleInputTEUpdate(gMainWindow);
        }
    }
}
/*
 * IDLE PROCESSING FOR INPUT TEXTEDIT
 *
 * Provides regular "idle time" to the TextEdit component so it can
 * perform periodic tasks like cursor blinking. This function must be
 * called regularly from the main event loop to maintain proper
 * TextEdit behavior.
 *
 * TEXTEDIT IDLE TASKS:
 * TEIdle() handles several time-based TextEdit behaviors:
 * - Cursor blinking animation (on/off cycle)
 * - Selection highlighting updates
 * - Auto-scroll during selection drags
 * - Internal housekeeping and optimization
 *
 * CALL FREQUENCY:
 * Should be called during every pass through the main event loop,
 * typically during WaitNextEvent() processing. The frequency affects:
 * - Cursor blink rate (faster calls = smoother blinking)
 * - Selection responsiveness during mouse drags
 * - Overall perceived text editing smoothness
 *
 * PERFORMANCE IMPACT:
 * TEIdle() is designed to be lightweight and called frequently.
 * It only performs work when necessary (e.g., cursor blink timing)
 * and returns quickly when no idle processing is needed.
 *
 * COOPERATIVE MULTITASKING:
 * In Classic Mac's cooperative multitasking environment, regular
 * TEIdle() calls ensure text editing remains responsive even when
 * the application is busy with other tasks.
 *
 * Per Inside Macintosh Volume I: "Call TEIdle frequently to ensure
 * proper cursor blinking and selection behavior."
 */
void IdleInputTE(void)
{
    if (gInputTE != NULL) {
        TEIdle(gInputTE);  /* Perform TextEdit idle processing */
    }
}
/*
 * HANDLE KEYBOARD INPUT FOR TEXTEDIT FIELD
 *
 * Processes keyDown events and forwards appropriate characters to the
 * TextEdit component for text entry. This function implements the
 * standard Classic Mac text input filtering and routing patterns.
 *
 * EVENT FILTERING:
 * Only processes keyboard events when:
 * 1. TextEdit component is initialized and valid
 * 2. Main window exists and is the front window (has focus)
 * 3. Command key is not pressed (reserves Cmd+key for menu commands)
 *
 * WINDOW FOCUS VALIDATION:
 * FrontWindow() check ensures we only accept input when our window
 * has keyboard focus. This prevents stealing keystrokes from other
 * applications or dialogs that might be in front.
 *
 * COMMAND KEY HANDLING:
 * Command key combinations are reserved for menu commands (Cmd+C for
 * copy, Cmd+V for paste, etc.) and should not be sent to TextEdit
 * as regular character input. The modifiers check filters these out.
 *
 * CHARACTER EXTRACTION:
 * The message field contains both the character code and key code.
 * charCodeMask (0x000000FF) extracts just the ASCII character code
 * that should be inserted into the text.
 *
 * TEXTEDIT INTEGRATION:
 * TEKey() handles all text editing operations:
 * - Regular character insertion
 * - Backspace and delete
 * - Arrow key navigation
 * - Selection modification
 * - Auto-scrolling
 *
 * RETURN VALUE:
 * - true: Event was handled by TextEdit (don't process further)
 * - false: Event not handled (allow other processing)
 *
 * This return value enables proper event handling chains where
 * unhandled events can be passed to other components or default
 * system handling.
 */
Boolean HandleInputTEKeyDown(EventRecord *theEvent)
{
    char theChar;  /* Extracted character code */

    /*
     * INPUT VALIDATION AND FOCUS CHECKING
     *
     * Ensure all required components are valid and we have input focus
     * before attempting to process keyboard input.
     */
    if (gInputTE != NULL && gMainWindow != NULL && FrontWindow() == (WindowPtr)gMainWindow) {
        /*
         * COMMAND KEY FILTERING
         *
         * Command key combinations are reserved for menu commands and
         * should not be processed as text input. Let them pass through
         * to the menu system for handling.
         */
        if (!(theEvent->modifiers & cmdKey)) {
            /* Extract character code and send to TextEdit */
            theChar = theEvent->message & charCodeMask;
            TEKey(theChar, gInputTE);
            return true;  /* Event handled - don't process further */
        }
    }

    return false;  /* Event not handled - allow further processing */
}
