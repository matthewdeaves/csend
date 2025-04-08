/**
 * @file main.c (Classic Mac)
 * @brief Main application file for the Classic Macintosh P2P Chat Client.
 */

 #include <MacTypes.h>
 #include <Quickdraw.h>
 #include <Fonts.h>
 #include <Events.h>
 #include <Windows.h>
 #include <TextEdit.h>
 #include <Dialogs.h>
 #include <Menus.h>
 #include <Devices.h>     // For PBControl, SystemTask
 #include <Resources.h>   // Needed for GetResource, ReleaseResource
 #include <stdlib.h>      // For exit()
 #include <string.h>      // For memcpy, strlen etc.

 // --- MacTCP Includes ---
#include <MacTCP.h>          // Include this first. It defines the necessary types.

#define __MACTCPCOMMONTYPES__ // This prevents AddressXlation.h from including
                              // the conflicting MacTCPCommonTypes.h file.

#include <AddressXlation.h>  // Needs types defined in MacTCP.h
                              // but will skip including MacTCPCommonTypes.h because
                              // the guard is already defined.

// --- Shared Code Includes ---
#include "common_defs.h" // Shared constant definitions
#include "protocol.h"    // Shared protocol formatting/parsing
#include "utils.h"       // Shared logging utility

 // --- Constants ---
 #define kBaseResID 128      // Matches the DLOG resource ID created in ResEdit
 
 // Dialog Item IDs (Matching ResEdit layout from screen share)
 #define kPeerListUserItem   1   // User Item placeholder for the List Manager list
 #define kMessagesTextEdit   2   // EditText for displaying received messages
 #define kInputTextEdit      3   // EditText for typing messages
 #define kSendButton         4   // Button to send the message
 #define kBroadcastCheckbox  5   // CheckBox for broadcast option
 
 // MacTCP Driver Name
 #define kTCPDriverName "\p.IPP" // Pascal string for the IP driver
 
 // --- Global Variables ---
 DialogPtr   gMainWindow = NULL; // Pointer to our main dialog window
 Boolean     gDone = false;      // Controls the main event loop termination
 short       gMacTCPRefNum = 0;  // Driver reference number for MacTCP
 ip_addr     gMyLocalIP = 0;     // Our local IP address (network byte order)
 char        gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Our local IP as string
 char        gMyUsername[32] = "MacUser"; // Default username
 
 // --- Function Prototypes ---
 void InitializeToolbox(void);
 OSErr InitializeNetworking(void); // Added function for networking init
 void MainEventLoop(void);
 void HandleEvent(EventRecord *event);
 void HandleDialogClick(DialogPtr dialog, short itemHit); // Renamed for clarity
 void DoSendAction(DialogPtr dialog); // Specific action for Send button
 
 // --- Main Application ---
 
 int main(void) {
     OSErr networkErr;
 
     InitializeToolbox();
 
     networkErr = InitializeNetworking();
     if (networkErr != noErr) {
         // Display an alert here in a real app
         log_message("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
         ExitToShell();
         return 1;
     }
     log_message("Network initialized successfully. Local IP: %s", gMyLocalIPStr);
 
 
     // Load the main chat dialog (after network init)
     gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
     if (gMainWindow == NULL) {
         log_message("Fatal: Failed to load dialog resource ID %d. Exiting.", kBaseResID);
         ExitToShell();
         return 1;
     }
 
     // Set the current port to the dialog window
     SetPort((GrafPtr)gMainWindow); // Cast DialogPtr to GrafPtr for SetPort
 
     // --- TODO: Initial setup for controls (List Manager, TextEdit handles) ---
 
     // Enter the main event loop
     MainEventLoop();
 
     // Cleanup (happens after loop terminates)
     if (gMainWindow != NULL) {
         DisposeDialog(gMainWindow);
         gMainWindow = NULL;
     }
     // Note: We don't typically PBClose the MacTCP driver [2]
 
     log_message("Application terminated.");
     return 0;
 }
 
 // --- Initialization Functions ---
 
 void InitializeToolbox(void) {
     InitGraf(&qd.thePort);
     InitFonts();
     InitWindows();
     InitMenus();
     TEInit();
     InitDialogs(NULL);
     InitCursor();
     log_message("Toolbox initialized."); // Use shared logger
 }
 
 /**
  * @brief Initializes MacTCP driver and retrieves local IP address.
  * @return OSErr noErr on success, or an error code on failure.
  */
 OSErr InitializeNetworking(void) {
     OSErr err;
     ParamBlockRec pb; // Use a generic ParamBlockRec
 
     log_message("Initializing Networking...");
 
     // 1. Open the MacTCP Driver (".IPP")
     // PBOpen requires ioParam.ioNamePtr to point to the driver name (Pascal string)
     // and returns the refNum in ioParam.ioRefNum.
     pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
     pb.ioParam.ioPermssn = fsCurPerm; // Usually ignored for drivers, use current permission
     err = PBOpenSync(&pb); // Synchronous open call [2]
 
     if (err != noErr) {
         log_message("Error: Failed to open MacTCP driver '.IPP'. Error: %d", err);
         gMacTCPRefNum = 0;
         return err;
     }
     gMacTCPRefNum = pb.ioParam.ioRefNum; // Store the driver reference number
     log_message("MacTCP driver opened successfully (RefNum: %d).", gMacTCPRefNum);
 
     // 2. Get the Local IP Address using ipctlGetAddr (csCode 15)
     // We need to use a specific structure overlay for this control call.
     // The IPParamBlock structure is defined in MacTCP.h (or similar).
     // We use memset to zero the block first.
     memset(&pb, 0, sizeof(ParamBlockRec)); // Clear the block for the next call
     pb.cntrlParam.ioCRefNum = gMacTCPRefNum; // Driver reference number
     pb.cntrlParam.csCode = ipctlGetAddr;     // Control code to get address [6]
 
     err = PBControlSync((ParmBlkPtr)&pb); // Synchronous control call [2]
 
     if (err != noErr) {
         log_message("Error: Failed to get local IP address. Error: %d", err);
         // Consider closing the driver? Probably not necessary as app will exit.
         return err;
     }
 
     // The IP address and netmask are returned directly in the parameter block
     // fields defined by the IPParamBlock structure overlay.
     // We need to cast the generic pb to the specific type to access them.
     // IMPORTANT: Check your MacTCP.h for the exact structure name and field names.
     // Assuming structure is IPParamBlock and fields are ourAddress / ourNetMask.
     // If your header uses a different structure (e.g., AddrParam), adjust accordingly.
     // Let's assume a structure `struct IPParamBlock { ...; ip_addr ourAddress; long ourNetMask; };` exists.
     // We need to access the fields *at the correct offsets* within the ParamBlockRec.
     // Typically, these specific fields start after the standard header + csCode.
     // **This part is highly dependent on your exact MacTCP.h header.**
     // A common way is to cast the parameter block pointer:

     // Access the IP address directly via the csParam array offset,
     // as the specific 'ourAddress' member isn't found in the included header's
     // IPParamBlock struct definition, despite documentation suggesting it.
     // csParam starts at offset 28, and the ip_addr is the first value returned.
     gMyLocalIP = *((ip_addr *)(&pb.cntrlParam.csParam[0])); // Get IP (network byte order)
 
     // Convert IP address to string format for logging/display
     AddrToStr(gMyLocalIP, gMyLocalIPStr); // Use DNR function [5]
 
     // Log the retrieved IP address
     log_message("Local IP address: %s (Network byte order: %lu)", gMyLocalIPStr, gMyLocalIP);
 
     // --- TODO: Initialize Domain Name Resolver (DNR) ---
     // err = OpenResolver(NULL); // Open DNR using default Hosts file [5]
     // if (err != noErr) {
     //    log_message("Warning: Failed to open Domain Name Resolver. Error: %d", err);
     //    // Continue, but name resolution might fail
     // } else {
     //    log_message("Domain Name Resolver initialized.");
     // }
 
     // --- TODO: Initialize UDP Socket for Discovery ---
     // Need to call UDPCreate here later [3]
 
     return noErr; // Success
 }
 
 
 // --- Event Handling ---
 
 void MainEventLoop(void) {
     EventRecord event;
     Boolean     gotEvent;
     long        sleepTime = 60L; // Ticks to sleep (60 = 1 second)
 
     while (!gDone) {
         // Use WaitNextEvent for better background processing
         gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);
 
         if (gotEvent) {
             if (IsDialogEvent(&event)) {
                 DialogPtr whichDialog;
                 short itemHit;
                 if (DialogSelect(&event, &whichDialog, &itemHit)) {
                     if (whichDialog == gMainWindow) {
                         HandleDialogClick(whichDialog, itemHit);
                     }
                 }
             } else {
                 HandleEvent(&event);
             }
         } else {
             // Idle time: Check for network activity
             // MacTCPIdle(); // Placeholder for MacTCP background processing
         }
     }
 }
 
 void HandleEvent(EventRecord *event) {
     short     windowPart;
     WindowPtr whichWindow;
 
     switch (event->what) {
         case mouseDown:
             windowPart = FindWindow(event->where, &whichWindow);
             switch (windowPart) {
                 case inMenuBar:
                     // HandleMenuClick(MenuSelect(event->where)); // Add later
                     break;
                 case inSysWindow:
                     SystemClick(event, whichWindow);
                     break;
                 case inDrag:
                      if (whichWindow == (WindowPtr)gMainWindow) { // Only drag our window
                         DragWindow(whichWindow, event->where, &qd.screenBits.bounds);
                      }
                      break;
                 case inGoAway:
                     if (whichWindow == (WindowPtr)gMainWindow) {
                         if (TrackGoAway(whichWindow, event->where)) {
                             gDone = true;
                         }
                     }
                     break;
                 default:
                     break;
             }
             break;
 
         // Keep other cases like keyDown, updateEvt, activateEvt from previous example
         case keyDown: case autoKey: break; // Basic handling for now
         case updateEvt:
             whichWindow = (WindowPtr)event->message;
              if (whichWindow == (WindowPtr)gMainWindow) { // Only update our window
                 BeginUpdate(whichWindow);
                 // UserItem drawing would go here later
                 DrawDialog(whichWindow); // Redraw standard dialog items
                 EndUpdate(whichWindow);
             }
             break;
         case activateEvt: break; // Basic handling for now
 
 
         default:
             break;
     }
 }
 
 /**
  * @brief Processes clicks on items within the chat dialog.
  * @param dialog Pointer to the chat dialog.
  * @param itemHit The item number that the user clicked.
  */
 void HandleDialogClick(DialogPtr dialog, short itemHit) {
     ControlHandle   checkboxHandle;
     DialogItemType  itemType;
     Handle          itemHandle;
     Rect            itemRect;
     short           currentValue;
 
     switch (itemHit) {
         case kSendButton:
             DoSendAction(dialog); // Call dedicated function for send action
             break;
 
         case kBroadcastCheckbox:
             // Get the handle to the checkbox control
             GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
             checkboxHandle = (ControlHandle)itemHandle;
             // Toggle its value
             currentValue = GetControlValue(checkboxHandle);
             SetControlValue(checkboxHandle, !currentValue);
             log_message("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
             break;
 
         // Add cases for other interactive items if needed (e.g., list clicks)
         default:
             break;
     }
 }
 
 /**
  * @brief Handles the action when the Send button is clicked.
  * @param dialog Pointer to the chat dialog.
  */
 void DoSendAction(DialogPtr dialog) {
     Str255          inputText;      // Pascal string to hold input text
     char            inputCStr[256]; // C string buffer
     char            formattedMsg[BUFFER_SIZE]; // Buffer for formatted protocol message
     ControlHandle   checkboxHandle;
     DialogItemType  itemType;
     Handle          itemHandle;
     Rect            itemRect;
     Boolean         isBroadcast;
 
     log_message("Send button clicked.");
 
     // 1. Get text from the input EditText (Item #3)
     GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &itemRect);
     GetDialogItemText(itemHandle, inputText);
 
     // Convert Pascal string to C string for shared code
     memcpy(inputCStr, &inputText[1], inputText[0]); // Copy bytes after length byte
     inputCStr[inputText[0]] = '\0'; // Null-terminate
 
     if (strlen(inputCStr) == 0) {
         log_message("Input field is empty, not sending.");
         return; // Don't send empty messages
     }
     log_message("Input text: '%s'", inputCStr);
 
     // 2. Check state of the Broadcast CheckBox (Item #5)
     GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
     checkboxHandle = (ControlHandle)itemHandle;
     isBroadcast = (GetControlValue(checkboxHandle) == 1); // 1=checked, 0=unchecked
     log_message("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
 
     // 3. Format the message using shared protocol code
     // Ensure gMyUsername and gMyLocalIPStr are populated correctly
     int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                       gMyUsername, gMyLocalIPStr, inputCStr);
 
     if (formatResult == 0) {
         log_message("Formatted message: %s", formattedMsg);
 
         // --- TODO: Send the message ---
         if (isBroadcast) {
             // Send formattedMsg via UDP broadcast
             log_message("Broadcasting message (UDP send not implemented yet)...");
             // Call UDPSend function here later [3]
         } else {
             // Send formattedMsg via TCP to selected peer
             log_message("Sending message to selected peer (TCP send not implemented yet)...");
             // Call TCPSend function here later [4] (need selected peer IP)
         }
 
         // --- TODO: Append sent message to the display area (Item #2) ---
         // Get handle for kMessagesTextEdit
         // Get current text
         // Append new text (e.g., "You: message content")
         // Set new text
         // Scroll display area
 
     } else {
         log_message("Error: Failed to format message for sending.");
         // Show an alert?
     }
 
     // 4. Clear the input EditText (Item #3)
     GetDialogItem(dialog, kInputTextEdit, &itemType, &itemHandle, &itemRect); // Get handle again
     SetDialogItemText(itemHandle, "\p"); // Set to empty Pascal string
 
     // Optional: Set focus back to input field?
     // SelectDialogItemText(dialog, kInputTextEdit, 0, 0);
 }