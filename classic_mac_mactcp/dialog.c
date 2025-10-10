/*
 * Classic Mac Dialog Manager Implementation for P2P Messenger
 *
 * This module implements the main user interface using Apple's Dialog Manager,
 * which provides a resource-based approach to creating and managing dialog boxes
 * with various controls (TextEdit, List, buttons, checkboxes).
 *
 * CLASSIC MAC UI ARCHITECTURE:
 *
 * 1. RESOURCE-BASED UI DESIGN:
 *    - Dialog layout defined in resource files (DLOG resources)
 *    - Controls defined as DITL (Dialog Item List) resources
 *    - Strings, icons, and other UI elements stored as resources
 *    - Allows easy localization and UI modifications without recompiling
 *
 * 2. DIALOG MANAGER PATTERNS:
 *    - Single modal or modeless dialog as main window
 *    - Event-driven interaction model
 *    - Manual control management (no automatic layout)
 *    - Explicit graphics port management for drawing operations
 *
 * 3. TEXTEDIT INTEGRATION:
 *    - Custom TextEdit fields for message display and input
 *    - Manual scrollbar management for text areas
 *    - Proper activation/deactivation for focus management
 *    - Update region handling for efficient drawing
 *
 * 4. LIST MANAGER INTEGRATION:
 *    - List control for displaying discovered peers
 *    - Custom drawing for list items
 *    - Selection tracking and event handling
 *    - Dynamic list updates as peers join/leave
 *
 * KEY DESIGN PATTERNS:
 *
 * 1. MODULAR COMPONENT DESIGN:
 *    - Separate modules for each UI component (input, messages, peer list)
 *    - Clean initialization and cleanup for each component
 *    - Independent update tracking to minimize redraws
 *
 * 2. GRAPHICS PORT MANAGEMENT:
 *    - Save/restore graphics port around drawing operations
 *    - Proper port setup for each UI component
 *    - Coordinate system management for different controls
 *
 * 3. EVENT HANDLING:
 *    - Centralized event dispatching to appropriate components
 *    - Proper focus management between TextEdit fields
 *    - User feedback through sound alerts and visual cues
 *
 * 4. STATE MANAGEMENT:
 *    - Track initialization state of each UI component
 *    - Update flags to minimize unnecessary redraws
 *    - Graceful handling of partial initialization failures
 *
 * PERFORMANCE CONSIDERATIONS:
 *
 * - Minimize TextEdit updates to reduce flicker
 * - Use update flags to avoid redundant drawing operations
 * - Efficient peer list updates when network state changes
 * - Proper graphics port management to avoid drawing artifacts
 *
 * References:
 * - Inside Macintosh Volume I: Dialog Manager
 * - Inside Macintosh Volume I: TextEdit
 * - Inside Macintosh Volume IV: List Manager
 * - Macintosh Human Interface Guidelines
 */

#include "dialog.h"
#include "../shared/logging.h"
#include "network_init.h"
#include "../shared/peer_wrapper.h"
#include "messaging.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h>
#include <Events.h>
#include <Windows.h>
#include <Memory.h>
#include <Sound.h>
#include <Resources.h>
#include <Fonts.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <Errors.h>
/*
 * GLOBAL UI STATE MANAGEMENT
 *
 * These variables track the state of the main dialog and its components.
 * Classic Mac programming commonly uses globals due to the single-threaded
 * execution model and simplified state management.
 */
DialogPtr gMainWindow = NULL;              /* Main dialog window handle */
Boolean gDialogTEInitialized = false;      /* TextEdit components ready */
Boolean gDialogListInitialized = false;    /* List component ready */

/*
 * UPDATE TRACKING FOR PERFORMANCE OPTIMIZATION
 *
 * Classic Mac UI can be slow due to limited graphics hardware and
 * single-threaded execution. These flags implement a simple invalidation
 * system to minimize unnecessary redraws and improve perceived performance.
 *
 * UPDATE STRATEGY:
 * - Components mark themselves "dirty" when content changes
 * - Main update function only redraws dirty components
 * - Flags cleared after successful update
 * - Reduces flicker and improves responsiveness
 *
 * This pattern is commonly used in Classic Mac applications to maintain
 * smooth UI performance on 68k and early PowerPC systems.
 */
Boolean gInputTENeedsUpdate = false;      /* Input field needs redraw */
Boolean gMessagesTENeedsUpdate = false;   /* Message area needs redraw */
Boolean gPeerListNeedsUpdate = false;     /* Peer list needs redraw */
/*
 * INITIALIZE MAIN DIALOG AND ALL UI COMPONENTS
 *
 * This function sets up the complete user interface by loading the dialog
 * from resources and initializing all embedded controls. It follows the
 * classic Mac pattern of resource-based UI construction.
 *
 * INITIALIZATION SEQUENCE:
 * 1. Load dialog from DLOG resource
 * 2. Set proper graphics port for drawing operations
 * 3. Initialize TextEdit components (messages and input)
 * 4. Initialize List control for peer display
 * 5. Configure checkboxes with initial states
 * 6. Set initial focus and update all components
 * 7. Restore previous graphics port
 *
 * RESOURCE DEPENDENCIES:
 * - DLOG resource for dialog layout and appearance
 * - DITL resource for dialog items (controls, text fields)
 * - Proper resource ID constants defined in header
 *
 * ERROR HANDLING:
 * If any component fails to initialize, all successfully initialized
 * components are cleaned up to prevent resource leaks and the function
 * returns false to indicate failure.
 *
 * GRAPHICS PORT MANAGEMENT:
 * Classic Mac requires explicit graphics port management. All drawing
 * operations must occur with the correct port set, and the previous
 * port must be restored when finished.
 *
 * RETURNS:
 * - true: All UI components initialized successfully
 * - false: One or more components failed, UI not ready
 */
Boolean InitDialog(void)
{
    Boolean messagesOk = false;   /* Messages TextEdit initialization status */
    Boolean inputOk = false;      /* Input TextEdit initialization status */
    Boolean listOk = false;       /* Peer list initialization status */
    ControlHandle ctrlHandle;     /* Handle for checkbox controls */
    DialogItemType itemType;      /* Type of dialog item */
    Handle itemHandle;            /* Generic handle for dialog items */
    Rect itemRect;                /* Rectangle for dialog item bounds */
    GrafPtr oldPort;              /* Previous graphics port for restoration */
    /*
     * DIALOG RESOURCE LOADING
     *
     * GetNewDialog() loads a dialog from a DLOG resource and creates
     * the corresponding window. This is the standard approach for
     * creating dialogs in Classic Mac applications.
     *
     * PARAMETERS:
     * - kBaseResID: Resource ID of DLOG resource
     * - NULL: No storage provided (use default)
     * - (WindowPtr)-1L: Put dialog in front of all other windows
     *
     * ERROR HANDLING:
     * If GetNewDialog() fails, ResError() provides the specific
     * Resource Manager error code for debugging.
     */
    log_debug_cat(LOG_CAT_UI, "Loading dialog resource ID %d...", kBaseResID);
    gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr) - 1L);
    if (gMainWindow == NULL) {
        log_error_cat(LOG_CAT_UI, "Fatal: GetNewDialog failed (Error: %d). Check DLOG resource ID %d.", ResError(), kBaseResID);
        return false;
    }
    log_info_cat(LOG_CAT_UI, "Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow);
    /*
     * GRAPHICS PORT SETUP FOR UI INITIALIZATION
     *
     * Classic Mac requires proper graphics port setup before any drawing
     * operations. The dialog's graphics port must be current for TextEdit
     * and other control initialization to work correctly.
     */
    GetPort(&oldPort);                               /* Save current port */
    SetPort((GrafPtr)GetWindowPort(gMainWindow));    /* Set dialog's port */

    /*
     * UI COMPONENT INITIALIZATION
     *
     * Initialize each major UI component independently. Each initialization
     * function returns a Boolean indicating success/failure, allowing for
     * granular error handling and cleanup.
     */
    messagesOk = InitMessagesTEAndScrollbar(gMainWindow);  /* Message display area */
    inputOk = InitInputTE(gMainWindow);                    /* Text input field */
    listOk = InitPeerListControl(gMainWindow);             /* Peer list display */
    /*
     * INITIALIZATION STATE TRACKING AND ERROR HANDLING
     *
     * Track which components initialized successfully and handle partial
     * failures gracefully. If any critical component fails, clean up
     * all successfully initialized components to prevent resource leaks.
     */
    gDialogTEInitialized = (messagesOk && inputOk);  /* Both TextEdit components required */
    gDialogListInitialized = listOk;                 /* List component status */

    if (!gDialogTEInitialized || !gDialogListInitialized) {
        /*
         * PARTIAL INITIALIZATION FAILURE CLEANUP
         *
         * Clean up in reverse order of initialization to handle any
         * interdependencies between components. Each cleanup function
         * is designed to handle being called even if initialization
         * was unsuccessful.
         */
        log_error_cat(LOG_CAT_UI, "Error: One or more dialog components (TEs, List) failed to initialize. Cleaning up.");
        if (listOk) CleanupPeerListControl();
        if (inputOk) CleanupInputTE();
        if (messagesOk) CleanupMessagesTEAndScrollbar();
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
        SetPort(oldPort);  /* Restore previous port before returning */
        return false;
    }
    /*
     * DEBUG CHECKBOX INITIALIZATION
     *
     * Configure the debug output checkbox to reflect the current logging
     * state. This provides user control over debug message visibility.
     *
     * DIALOG ITEM ACCESS PATTERN:
     * 1. GetDialogItem() retrieves item information by item number
     * 2. Verify item type matches expected control type
     * 3. Cast generic handle to specific control type
     * 4. Set initial state based on application state
     *
     * ERROR HANDLING:
     * Defensive programming checks for NULL handles and incorrect item
     * types, which can occur if the dialog resource is malformed or
     * if item numbers don't match the resource definition.
     */
    GetDialogItem(gMainWindow, kDebugCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        /* Verify this is actually a checkbox control */
        if (itemType == (ctrlItem + chkCtrl)) {
            ctrlHandle = (ControlHandle)itemHandle;
            /* Set checkbox state to match current debug output setting */
            SetControlValue(ctrlHandle, is_debug_output_enabled() ? 1 : 0);
            log_debug_cat(LOG_CAT_UI, "Debug checkbox (Item %d) initialized to: %s", kDebugCheckbox, is_debug_output_enabled() ? "ON" : "OFF");
        } else {
            log_warning_cat(LOG_CAT_UI, "Item %d (kDebugCheckbox) is not a checkbox (Type: %d)! Cannot set initial debug state.", kDebugCheckbox, itemType);
        }
    } else {
        log_warning_cat(LOG_CAT_UI, "Item %d (kDebugCheckbox) handle is NULL! Cannot set initial state.", kDebugCheckbox);
    }
    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    if (itemHandle != NULL) {
        if (itemType == (ctrlItem + chkCtrl)) {
            ctrlHandle = (ControlHandle)itemHandle;
            SetControlValue(ctrlHandle, 0);
            log_debug_cat(LOG_CAT_UI, "Broadcast checkbox (Item %d) initialized to: OFF", kBroadcastCheckbox);
        } else {
            log_warning_cat(LOG_CAT_UI, "Item %d (kBroadcastCheckbox) is not a checkbox (Type: %d)! Cannot set initial state.", kBroadcastCheckbox, itemType);
        }
    } else {
        log_warning_cat(LOG_CAT_UI, "Item %d (kBroadcastCheckbox) handle is NULL! Cannot set initial state.", kBroadcastCheckbox);
    }
    UpdatePeerDisplayList(true);
    log_debug_cat(LOG_CAT_UI, "Setting focus to input field (item %d)...", kInputTextEdit);
    ActivateInputTE(true);
    /* Mark all components for initial update */
    gInputTENeedsUpdate = true;
    gMessagesTENeedsUpdate = true;
    gPeerListNeedsUpdate = true;
    UpdateDialogControls();
    log_debug_cat(LOG_CAT_UI, "Initial UpdateDialogControls() called from InitDialog.");
    SetPort(oldPort);
    log_info_cat(LOG_CAT_UI, "InitDialog finished successfully.");
    return true;
}
void CleanupDialog(void)
{
    log_debug_cat(LOG_CAT_UI, "Cleaning up Dialog...");
    CleanupPeerListControl();
    CleanupInputTE();
    CleanupMessagesTEAndScrollbar();
    if (gMainWindow != NULL) {
        log_debug_cat(LOG_CAT_UI, "Disposing dialog window...");
        DisposeDialog(gMainWindow);
        gMainWindow = NULL;
    }
    gDialogTEInitialized = false;
    gDialogListInitialized = false;
    log_debug_cat(LOG_CAT_UI, "Dialog cleanup complete.");
}
/*
 * HANDLE SEND BUTTON CLICK EVENT
 *
 * This function processes user requests to send messages, handling both
 * direct messages to selected peers and broadcast messages to all peers.
 * It demonstrates classic Mac UI event handling patterns and P2P messaging.
 *
 * SEND MODES:
 * 1. DIRECT MESSAGE: Send to currently selected peer in peer list
 * 2. BROADCAST MESSAGE: Send to all active peers when broadcast checkbox checked
 *
 * PROCESSING SEQUENCE:
 * 1. Validate UI state and get input text
 * 2. Check broadcast checkbox state
 * 3. Execute appropriate send mode (direct or broadcast)
 * 4. Provide user feedback (success/error messages)
 * 5. Clear input field on success and restore focus
 *
 * ERROR HANDLING:
 * - Validates TextEdit initialization state
 * - Checks for empty input (no-op)
 * - Handles network errors gracefully
 * - Provides audio and visual feedback for errors
 *
 * USER FEEDBACK:
 * - Success: Clear input field, show confirmation
 * - Error: Keep input text, show error message, beep
 * - Network busy: Suggest retry with appropriate message
 *
 * This function exemplifies classic Mac user interaction patterns with
 * immediate feedback and clear indication of application state.
 */
void HandleSendButtonClick(void)
{
    char inputCStr[256];                    /* Buffer for input text */
    ControlHandle broadcastCheckboxHandle;  /* Handle to broadcast checkbox */
    DialogItemType itemType;                /* Type of dialog item */
    Handle itemHandle;                      /* Generic item handle */
    Rect itemRect;                         /* Item bounds rectangle */
    Boolean isBroadcast;                   /* Broadcast mode flag */
    char displayMsg[BUFFER_SIZE + 100];    /* Buffer for user feedback messages */
    OSErr sendErr = noErr;                 /* Network operation result */
    int i;                                 /* Loop counter for broadcast */

    /*
     * UI STATE VALIDATION
     *
     * Ensure that the TextEdit components are properly initialized before
     * attempting to access them. This prevents crashes if the send button
     * is somehow activated before full initialization completes.
     *
     * SysBeep() provides immediate audio feedback to indicate an error
     * condition to the user, following Mac HIG recommendations.
     */
    if (!gDialogTEInitialized || gInputTE == NULL) {
        log_error_cat(LOG_CAT_UI, "Error (HandleSendButtonClick): Input TE not initialized.");
        SysBeep(10);  /* Standard error beep */
        return;
    }

    if (!GetInputText(inputCStr, sizeof(inputCStr))) {
        log_error_cat(LOG_CAT_UI, "Error: Could not get text from input field for sending.");
        SysBeep(10);
        ActivateInputTE(true);
        return;
    }

    if (strlen(inputCStr) == 0) {
        log_debug_cat(LOG_CAT_UI, "Send Action: Input field is empty. No action taken.");
        ActivateInputTE(true);
        return;
    }

    GetDialogItem(gMainWindow, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
    isBroadcast = false;
    if (itemHandle != NULL && itemType == (ctrlItem + chkCtrl)) {
        broadcastCheckboxHandle = (ControlHandle)itemHandle;
        isBroadcast = (GetControlValue(broadcastCheckboxHandle) == 1);
        log_debug_cat(LOG_CAT_UI, "Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
    } else {
        log_warning_cat(LOG_CAT_UI, "Broadcast item %d is not a checkbox or handle is NULL! Assuming not broadcast.", kBroadcastCheckbox);
    }

    /*
     * BROADCAST MESSAGE HANDLING
     *
     * When broadcast mode is enabled, send the message to all active peers.
     * This requires iterating through the peer list and queueing individual
     * messages for each peer.
     *
     * BROADCAST STRATEGY:
     * - Get count of active peers for validation
     * - Check network state to ensure sends are possible
     * - Iterate through peer list and queue message for each peer
     * - Track success/failure counts for user feedback
     * - Display summary of broadcast results
     */
    if (isBroadcast) {
        int sent_count = 0;                           /* Successfully queued messages */
        int failed_count = 0;                         /* Failed message queue attempts */
        int total_active_peers = pw_get_active_peer_count(); /* Current peer count */
        TCPStreamState currentState;                  /* Network state for validation */

        log_debug_cat(LOG_CAT_MESSAGING, "Attempting broadcast of: '%s' to %d active peers", inputCStr, total_active_peers);

        /* Display broadcast message in chat area for user confirmation */
        sprintf(displayMsg, "You (Broadcast): %s", inputCStr);
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");  /* Mac uses \r for line endings */

        if (total_active_peers == 0) {
            log_debug_cat(LOG_CAT_MESSAGING, "No active peers to broadcast to");
            sprintf(displayMsg, "No active peers found. Waiting for peers to join...");
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r");
            ActivateInputTE(true);
            return;
        }

        /*
         * NETWORK STATE VALIDATION FOR BROADCAST
         *
         * Check if the TCP connection pool is available for new operations.
         * Broadcasting requires multiple concurrent connections, so we need
         * to ensure the network subsystem can handle the load.
         *
         * If the network is busy, provide clear feedback to the user and
         * preserve their input for retry rather than losing the message.
         */
        currentState = GetTCPSendStreamState();
        if (currentState != TCP_STATE_IDLE) {
            log_warning_cat(LOG_CAT_MESSAGING, "Cannot broadcast: TCP send stream is busy (state %d)", currentState);
            sprintf(displayMsg, "Network busy. Please try again in a moment.");
            AppendToMessagesTE(displayMsg);
            AppendToMessagesTE("\r");
            SysBeep(10);           /* Audio feedback for temporary failure */
            ActivateInputTE(true); /* Return focus to input field */
            return;
        }

        for (i = 0; i < total_active_peers; i++) {
            peer_t peer;
            pw_get_peer_by_index(i, &peer);
            sendErr = MacTCP_QueueMessage(peer.ip, inputCStr, MSG_TEXT);
            if (sendErr == noErr) {
                sent_count++;
                log_debug_cat(LOG_CAT_MESSAGING, "Broadcast queued for %s@%s", peer.username, peer.ip);
            } else {
                failed_count++;
                log_error_cat(LOG_CAT_MESSAGING, "Broadcast queue failed for %s@%s: %d", peer.username, peer.ip, sendErr);
            }
        }

        if (sent_count > 0) {
            sprintf(displayMsg, "Broadcast queued for %d peer(s).", sent_count);
        } else {
            sprintf(displayMsg, "Broadcast failed. Could not queue for any peers.");
        }
        if (failed_count > 0) {
            sprintf(displayMsg + strlen(displayMsg), " (%d failed)", failed_count);
        }
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");

        log_info_cat(LOG_CAT_MESSAGING, "Broadcast of '%s' completed. Queued for %d/%d peers, %d failed.",
                     inputCStr, sent_count, total_active_peers, failed_count);

        if (sent_count > 0) {
            ClearInputText();
        }
    } else {
        peer_t targetPeer;
        if (DialogPeerList_GetSelectedPeer(&targetPeer)) {
            log_debug_cat(LOG_CAT_MESSAGING, "Attempting to send to selected peer %s@%s: '%s'",
                          targetPeer.username, targetPeer.ip, inputCStr);

            sendErr = MacTCP_QueueMessage(targetPeer.ip, inputCStr, MSG_TEXT);

            if (sendErr == noErr) {
                sprintf(displayMsg, "You (to %s): %s", targetPeer.username, inputCStr);
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_debug_cat(LOG_CAT_MESSAGING, "Sync send completed successfully.");
                ClearInputText();
            } else {
                if (sendErr == streamBusyErr) {
                    sprintf(displayMsg, "Could not send to %s: network busy. Try again.", targetPeer.username);
                } else {
                    sprintf(displayMsg, "Error sending to %s: %d", targetPeer.username, sendErr);
                }
                AppendToMessagesTE(displayMsg);
                AppendToMessagesTE("\r");
                log_error_cat(LOG_CAT_MESSAGING, "Error sending message to %s: %d", targetPeer.ip, sendErr);
                SysBeep(10);
            }
        } else {
            log_error_cat(LOG_CAT_UI, "Error: Cannot send, no peer selected in the list or selection invalid.");
            AppendToMessagesTE("Please select a peer to send to, or check Broadcast.\r");
            SysBeep(10);
        }
    }

    ActivateInputTE(true);
}
void ActivateDialogTE(Boolean activating)
{
    ActivateMessagesTEAndScrollbar(activating);
    ActivateInputTE(activating);
}
void UpdateDialogControls(void)
{
    GrafPtr oldPort;
    GrafPtr windowPort = (GrafPtr)GetWindowPort(gMainWindow);
    if (windowPort == NULL) {
        log_error_cat(LOG_CAT_UI, "UpdateDialogControls Error: Window port is NULL for gMainWindow!");
        return;
    }
    GetPort(&oldPort);
    SetPort(windowPort);
    
    /* Only update components that have been marked as needing updates */
    if (gMessagesTENeedsUpdate) {
        HandleMessagesTEUpdate(gMainWindow);
        gMessagesTENeedsUpdate = false;
    }
    if (gInputTENeedsUpdate) {
        HandleInputTEUpdate(gMainWindow);
        gInputTENeedsUpdate = false;
    }
    if (gPeerListNeedsUpdate) {
        HandlePeerListUpdate(gMainWindow);
        gPeerListNeedsUpdate = false;
    }
    
    SetPort(oldPort);
}

/* Invalidation functions to mark components for update */
void InvalidateInputTE(void)
{
    gInputTENeedsUpdate = true;
}

void InvalidateMessagesTE(void)
{
    gMessagesTENeedsUpdate = true;
}

void InvalidatePeerList(void)
{
    gPeerListNeedsUpdate = true;
}
