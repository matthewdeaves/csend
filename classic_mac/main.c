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
 #include <stdarg.h>      // For va_list, va_start, va_end
 #include <stdio.h>       // For file I/O (fopen, fprintf, fclose, fflush, vsprintf)
 #include <Sound.h>       // For SysBeep
 #include <Memory.h>      // For HLock/HUnlock/HSetState/HGetState

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
 // #include "utils.h"    // No longer needed

 // --- Constants ---
 #define kBaseResID 128      // Matches the DLOG resource ID created in ResEdit
 #define kLogFileName "csend_log.txt" // Name for the log file

 // Dialog Item IDs (Matching ResEdit layout from screen share)
 #define kPeerListUserItem   1   // User Item placeholder for the List Manager list
 #define kMessagesTextEdit   2   // UserItem for displaying received messages (TENew)
 #define kInputTextEdit      3   // UserItem for typing messages (TENew)
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
 TEHandle    gMessagesTE = NULL; // Handle for the received messages TextEdit item
 TEHandle    gInputTE = NULL;    // Handle for the input TextEdit item
 FILE        *gLogFile = NULL;   // File pointer for logging
 Boolean     gDialogTEInitialized = false; // *** ADDED: Flag for TE init state ***

 // --- Function Prototypes ---
 void InitializeToolbox(void);
 OSErr InitializeNetworking(void);
 void MainEventLoop(void);
 void HandleEvent(EventRecord *event);
 void HandleDialogClick(DialogPtr dialog, short itemHit);
 void DoSendAction(DialogPtr dialog);
 void AppendToMessagesTE(const char *text);
 void LogToDialog(const char *format, ...);
 void InitLogFile(void);
 void CloseLogFile(void);

 // --- Logging Functions ---

 void InitLogFile(void) {
     gLogFile = fopen(kLogFileName, "w");
     if (gLogFile == NULL) {
         SysBeep(10);
     } else {
         fprintf(gLogFile, "--- Log Started ---\n");
         fflush(gLogFile);
     }
 }

 void CloseLogFile(void) {
     if (gLogFile != NULL) {
         fprintf(gLogFile, "--- Log Ended ---\n");
         fclose(gLogFile);
         gLogFile = NULL;
     }
 }

 void AppendToMessagesTE(const char *text) {
     // No need to check gDialogTEInitialized here, LogToDialog does it
     if (gMessagesTE == NULL) return;
     SignedByte teState = HGetState((Handle)gMessagesTE);
     HLock((Handle)gMessagesTE);
     if (*gMessagesTE != NULL) {
         TESetSelect((*gMessagesTE)->teLength, (*gMessagesTE)->teLength, gMessagesTE);
         TEInsert(text, strlen(text), gMessagesTE);
     } else {
         if (gLogFile != NULL) { fprintf(gLogFile, "ERROR in AppendToMessagesTE: *gMessagesTE is NULL!\n"); fflush(gLogFile); }
     }
     HSetState((Handle)gMessagesTE, teState);
 }

 void LogToDialog(const char *format, ...) {
     char buffer[512];
     va_list args;

     va_start(args, format);
     vsprintf(buffer, format, args);
     va_end(args);

     // Always log to file if available
     if (gLogFile != NULL) {
         fprintf(gLogFile, "%s\n", buffer);
         fflush(gLogFile);
     }

     // *** MODIFIED: Only append to dialog TE *after* initialization is complete ***
     if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
         AppendToMessagesTE(buffer);
         AppendToMessagesTE("\r");
     } else if (gLogFile == NULL) { // Only beep if file logging also failed AND dialog not ready
         // SysBeep(1); // Maybe disable beep during init
     }
 }


 // --- Main Application ---

 int main(void) {
     OSErr networkErr;

     InitLogFile();
     LogToDialog("Starting application..."); // Log file only initially

     MaxApplZone();
     LogToDialog("MaxApplZone called."); // Log file only

     InitializeToolbox();
     LogToDialog("Toolbox Initialized."); // Log file only

     networkErr = InitializeNetworking();
     if (networkErr != noErr) {
         LogToDialog("Fatal: Network initialization failed (Error: %d). Exiting.", networkErr);
         CloseLogFile();
         ExitToShell();
         return 1;
     }
     LogToDialog("InitializeNetworking finished. Local IP reported: %s", gMyLocalIPStr); // Log file only

     // Load the main chat dialog
     LogToDialog("Loading dialog resource ID %d...", kBaseResID); // Log file only
     gMainWindow = GetNewDialog(kBaseResID, NULL, (WindowPtr)-1L);
     if (gMainWindow == NULL) {
         LogToDialog("Fatal: GetNewDialog failed (Error: %d). Exiting.", ResError());
         CloseLogFile();
         ExitToShell();
         return 1;
     }
     LogToDialog("Dialog loaded successfully (gMainWindow: 0x%lX).", (unsigned long)gMainWindow); // Log file only

     LogToDialog("Showing window..."); // Log file only
     ShowWindow(gMainWindow);
     SelectWindow(gMainWindow);
     LogToDialog("Setting port..."); // Log file only
     SetPort((GrafPtr)gMainWindow);
     LogToDialog("Port set."); // Log file only

     // --- Initialize Dialog Controls ---
     DialogItemType itemType;
     Handle         itemHandle; // Still needed for GetDialogItem
     Rect           destRectMessages, viewRectMessages; // Rects for TENew
     Rect           destRectInput, viewRectInput;       // Rects for TENew

     // --- Get Rects for UserItems and Create TE Records with TENew ---

     // Messages TE (Item 2 - Now UserItem)
     LogToDialog("Getting item %d info (Messages UserItem)...", kMessagesTextEdit);
     GetDialogItem(gMainWindow, kMessagesTextEdit, &itemType, &itemHandle, &destRectMessages);
     // *** CORRECTED LOGIC: Check for userItem ***
     if (itemType == userItem) {
         LogToDialog("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kMessagesTextEdit,
                     destRectMessages.top, destRectMessages.left, destRectMessages.bottom, destRectMessages.right);
         viewRectMessages = destRectMessages; // Start with view = dest
         // InsetRect(&viewRectMessages, 1, 1); // Optional inset

         LogToDialog("Calling TENew for Messages TE...");
         gMessagesTE = TENew(&destRectMessages, &viewRectMessages);
         if (gMessagesTE == NULL) {
             LogToDialog("CRITICAL ERROR: TENew failed for Messages TE!");
         } else {
             LogToDialog("TENew succeeded for Messages TE. Handle: 0x%lX", (unsigned long)gMessagesTE);
             LogToDialog("Calling TEAutoView for Messages TE...");
             TEAutoView(true, gMessagesTE); // Should be safe now
             LogToDialog("TEAutoView finished for Messages TE.");
         }
     } else {
         // This case should ideally not happen if resource was changed correctly
         LogToDialog("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kMessagesTextEdit, itemType);
         gMessagesTE = NULL;
     }


     // Input TE (Item 3 - Now UserItem)
     LogToDialog("Getting item %d info (Input UserItem)...", kInputTextEdit);
     GetDialogItem(gMainWindow, kInputTextEdit, &itemType, &itemHandle, &destRectInput);
      // *** CORRECTED LOGIC: Check for userItem ***
      if (itemType == userItem) {
         LogToDialog("Item %d is UserItem. Rect: (%d,%d,%d,%d)", kInputTextEdit,
                     destRectInput.top, destRectInput.left, destRectInput.bottom, destRectInput.right);
         viewRectInput = destRectInput; // Start with view = dest
         // InsetRect(&viewRectInput, 1, 1); // Optional inset

         LogToDialog("Calling TENew for Input TE...");
         gInputTE = TENew(&destRectInput, &viewRectInput);
          if (gInputTE == NULL) {
             LogToDialog("CRITICAL ERROR: TENew failed for Input TE!");
         } else {
             LogToDialog("TENew succeeded for Input TE. Handle: 0x%lX", (unsigned long)gInputTE);
             LogToDialog("Calling TEAutoView for Input TE...");
             TEAutoView(true, gInputTE); // Should be safe now
             LogToDialog("TEAutoView finished for Input TE.");
         }
     } else {
         // This case should ideally not happen if resource was changed correctly
         LogToDialog("ERROR: Item %d is NOT a UserItem (Type: %d)! Expected UserItem for TENew.", kInputTextEdit, itemType);
         gInputTE = NULL;
     }

     // *** Set flag indicating TE fields are now initialized (or attempted) ***
     gDialogTEInitialized = true;
     LogToDialog("Dialog TE fields initialization complete. Enabling dialog logging.");

     // --- Set initial focus AFTER TE init ---
     if (gInputTE != NULL) {
         LogToDialog("Setting focus to input field (item %d)...", kInputTextEdit);
         // Use SetDialogDefaultItem maybe? Or just TEActivate might be enough
         // SelectDialogItemText(gMainWindow, kInputTextEdit, 0, 0); // Set focus
         TEActivate(gInputTE); // Explicitly activate the input TE
         LogToDialog("Input TE activated.");
     } else {
         LogToDialog("Cannot set focus, Input TE handle is NULL.");
     }

     // --- NOW log initial messages to the dialog window ---
     LogToDialog("Welcome to Classic Mac Chat!"); // This will now append
     LogToDialog("Your IP: %s", gMyLocalIPStr);
     LogToDialog("Username: %s", gMyUsername);

     LogToDialog("Entering main event loop...");
     MainEventLoop(); // Main loop uses LogToDialog which appends
     LogToDialog("Exited main event loop.");

     // *** ADDED: Dispose TE Handles ***
     if (gMessagesTE != NULL) {
         LogToDialog("Disposing Messages TE...");
         TEDispose(gMessagesTE);
         gMessagesTE = NULL;
     }
      if (gInputTE != NULL) {
         LogToDialog("Disposing Input TE...");
         TEDispose(gInputTE);
         gInputTE = NULL;
     }

     if (gMainWindow != NULL) {
         LogToDialog("Disposing dialog...");
         DisposeDialog(gMainWindow);
         gMainWindow = NULL;
     }

     LogToDialog("Application terminated normally.");
     CloseLogFile();
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
 }

 OSErr InitializeNetworking(void) {
     OSErr err;
     ParamBlockRec pb;

     LogToDialog("Initializing Networking...");

     pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
     pb.ioParam.ioPermssn = fsCurPerm;
     LogToDialog("Attempting PBOpenSync for .IPP driver...");
     err = PBOpenSync(&pb);
     if (err != noErr) {
         LogToDialog("Error: PBOpenSync failed. Error: %d", err);
         gMacTCPRefNum = 0;
         return err;
     }
     gMacTCPRefNum = pb.ioParam.ioRefNum;
     LogToDialog("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

     memset(&pb, 0, sizeof(ParamBlockRec));
     pb.cntrlParam.ioCRefNum = gMacTCPRefNum;
     pb.cntrlParam.csCode = ipctlGetAddr;
     LogToDialog("Attempting PBControlSync for ipctlGetAddr...");
     err = PBControlSync((ParmBlkPtr)&pb);
     if (err != noErr) {
         LogToDialog("Error: PBControlSync failed. Error: %d", err);
         // Consider closing the driver if GetAddr fails?
         // PBCloseSync(...)
         return err;
     }
     LogToDialog("PBControlSync succeeded.");

     gMyLocalIP = *((ip_addr *)(&pb.cntrlParam.csParam[0]));
     LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
     AddrToStr(gMyLocalIP, gMyLocalIPStr);
     LogToDialog("AddrToStr finished. Result string: '%s'", gMyLocalIPStr);

     // DNR and UDP init still TODO
     return noErr;
 }


 // --- Event Handling ---

 void MainEventLoop(void) {
     EventRecord event;
     Boolean     gotEvent;
     long        sleepTime = 5L; // Reduced sleep time for better responsiveness

     while (!gDone) {
         // *** ADDED: TEIdle call for blinking cursor in active TE ***
         if (gMessagesTE != NULL) {
             TEIdle(gMessagesTE);
         }
         if (gInputTE != NULL) {
             TEIdle(gInputTE);
         }

         gotEvent = WaitNextEvent(everyEvent, &event, sleepTime, NULL);

         if (gotEvent) {
             if (IsDialogEvent(&event)) {
                 DialogPtr whichDialog;
                 short itemHit;
                 if (DialogSelect(&event, &whichDialog, &itemHit)) {
                     // DialogSelect handles clicks in TE fields for userItems too,
                     // activating them and passing key events.
                     if (whichDialog == gMainWindow && itemHit > 0) {
                         // Handle clicks on our specific action items
                         if (itemHit == kSendButton || itemHit == kBroadcastCheckbox) {
                              HandleDialogClick(whichDialog, itemHit);
                         }
                     } else if (whichDialog != gMainWindow) {
                         // DA event handled by DialogSelect
                     }
                 }
             } else {
                 HandleEvent(&event);
             }
         } else {
            // Idle time
         }
     }
 }


 void HandleEvent(EventRecord *event) {
     short     windowPart;
     WindowPtr whichWindow;
     Rect      itemRect; // Needed for TEUpdate

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
                      if (whichWindow == (WindowPtr)gMainWindow) {
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
                 case inContent:
                     if (whichWindow == (WindowPtr)gMainWindow) {
                         if (whichWindow != FrontWindow()) {
                             SelectWindow(whichWindow);
                         } else {
                            // Click in content area outside items - maybe deselect TE?
                            // TEClick(event->where, event->modifiers & shiftKey, gInputTE); // Example? Needs care
                         }
                     }
                     break;
                 default:
                     break;
             }
             break;

         case keyDown: case autoKey:
             // DialogSelect should handle passing keys to the active TE field.
             // If it doesn't seem to, we might need manual TEKey here,
             // but usually DialogSelect is sufficient for userItems too.
             break;

         case updateEvt:
             whichWindow = (WindowPtr)event->message;
              if (whichWindow == (WindowPtr)gMainWindow) {
                 BeginUpdate(whichWindow);
                 // DrawDialog *might* draw userItem frames, or might not.
                 // If the TE borders disappear, we need manual FrameRect here.
                 DrawDialog(whichWindow);

                 // *** ADDED: Explicit TEUpdate for TE content ***
                 // Need to get the item rects again for TEUpdate
                 DialogItemType itemTypeIgnored;
                 Handle itemHandleIgnored;

                 if (gMessagesTE != NULL) {
                     GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
                     // EraseRect(&itemRect); // Optional: Erase before TEUpdate if DrawDialog doesn't
                     TEUpdate(&itemRect, gMessagesTE);
                 }
                 if (gInputTE != NULL) {
                     GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &itemRect);
                     // EraseRect(&itemRect); // Optional: Erase before TEUpdate if DrawDialog doesn't
                     TEUpdate(&itemRect, gInputTE);
                 }
                 EndUpdate(whichWindow);
             } else {
                 // Handle updates for other windows/DAs
             }
             break;

         case activateEvt:
             whichWindow = (WindowPtr)event->message;
             if (whichWindow == (WindowPtr)gMainWindow) {
                 // *** ADDED: Activate/Deactivate TE Handles ***
                 // Need to redraw items that change appearance on activation/deactivation
                 // For userItems, this might involve drawing focus borders manually.
                 // DrawControls(whichWindow); // Might redraw standard controls if needed

                 // Get item rects to potentially draw focus borders
                 DialogItemType itemTypeIgnored;
                 Handle itemHandleIgnored;
                 Rect messagesRect, inputRect;
                 Boolean messagesGot = false, inputGot = false;

                 if(gMessagesTE != NULL) {
                    GetDialogItem(gMainWindow, kMessagesTextEdit, &itemTypeIgnored, &itemHandleIgnored, &messagesRect);
                    messagesGot = true;
                 }
                 if(gInputTE != NULL) {
                    GetDialogItem(gMainWindow, kInputTextEdit, &itemTypeIgnored, &itemHandleIgnored, &inputRect);
                    inputGot = true;
                 }


                 if (event->modifiers & activeFlag) { // Window is activating
                     LogToDialog("ActivateEvt: Activating gMainWindow");
                     if (gMessagesTE != NULL) {
                        TEActivate(gMessagesTE);
                        // Draw focus border if needed (e.g., if gMessagesTE is active)
                        // Example: if (GetDialogKeyboardFocusItem(gMainWindow) == (Handle)gMessagesTE) PenPat(&qd.black); FrameRect(&messagesRect);
                     }
                     if (gInputTE != NULL) {
                        TEActivate(gInputTE);
                        // Draw focus border if needed (e.g., if gInputTE is active)
                        // Example: if (GetDialogKeyboardFocusItem(gMainWindow) == (Handle)gInputTE) PenPat(&qd.black); FrameRect(&inputRect);
                     }
                 } else { // Window is deactivating
                     LogToDialog("ActivateEvt: Deactivating gMainWindow");
                     if (gMessagesTE != NULL) {
                        TEDeactivate(gMessagesTE);
                        // Erase focus border if needed
                        // Example: PenPat(&qd.white); FrameRect(&messagesRect); PenNormal();
                     }
                     if (gInputTE != NULL) {
                        TEDeactivate(gInputTE);
                        // Erase focus border if needed
                        // Example: PenPat(&qd.white); FrameRect(&inputRect); PenNormal();
                     }
                 }
             }
             break;

         default:
             break;
     }
 }

 void HandleDialogClick(DialogPtr dialog, short itemHit) {
     ControlHandle   checkboxHandle;
     DialogItemType  itemType;
     Handle          itemHandle;
     Rect            itemRect;
     short           currentValue;

     switch (itemHit) {
         case kSendButton:
             DoSendAction(dialog);
             break;

         case kBroadcastCheckbox:
             GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
             if (itemType == ctrlItem + chkCtrl) {
                 checkboxHandle = (ControlHandle)itemHandle;
                 currentValue = GetControlValue(checkboxHandle);
                 SetControlValue(checkboxHandle, !currentValue);
                 LogToDialog("Broadcast checkbox toggled to: %s", !currentValue ? "ON" : "OFF");
             } else {
                 LogToDialog("Warning: Item %d clicked, but not a checkbox!", kBroadcastCheckbox);
             }
             break;

         default:
             // Clicks in userItems (now TE fields) are handled by DialogSelect/TEClick
             // LogToDialog("HandleDialogClick: Click on unhandled item %d", itemHit);
             break;
     }
 }

 void DoSendAction(DialogPtr dialog) {
     Str255          inputText;
     char            inputCStr[256];
     char            formattedMsg[BUFFER_SIZE];
     ControlHandle   checkboxHandle;
     DialogItemType  itemType;
     Handle          itemHandle;
     Rect            itemRect;
     Boolean         isBroadcast;
     char            displayMsg[BUFFER_SIZE + 100];

     // 1. Get text from the input EditText (Item #3)
     if (gInputTE == NULL) {
         LogToDialog("Error: Input TextEdit not initialized. Cannot send.");
         SysBeep(10);
         return;
     }
     // *** Get text directly from TEHandle ***
     HLock((Handle)gInputTE);
     if (*gInputTE != NULL && (*gInputTE)->hText != NULL) {
         Size textLen = (*gInputTE)->teLength;
         if (textLen > 255) textLen = 255; // Prevent overflow
         BlockMoveData(*((*gInputTE)->hText), inputCStr, textLen);
         inputCStr[textLen] = '\0';
     } else {
         LogToDialog("Error: Cannot get text from Input TE (NULL handle/hText).");
         HUnlock((Handle)gInputTE); // Ensure unlock on error
         SysBeep(10);
         return;
     }
     HUnlock((Handle)gInputTE);


     if (strlen(inputCStr) == 0) {
         LogToDialog("Send Action: Input field is empty.");
         SysBeep(5);
         return;
     }

     // 2. Check state of the Broadcast CheckBox (Item #5)
     GetDialogItem(dialog, kBroadcastCheckbox, &itemType, &itemHandle, &itemRect);
     if (itemType == ctrlItem + chkCtrl) {
         checkboxHandle = (ControlHandle)itemHandle;
         isBroadcast = (GetControlValue(checkboxHandle) == 1);
         LogToDialog("Broadcast checkbox state: %s", isBroadcast ? "Checked" : "Unchecked");
     } else {
         LogToDialog("Warning: Broadcast item %d is not a checkbox! Assuming not broadcast.", kBroadcastCheckbox);
         isBroadcast = false;
     }


     // 3. Format the message
     int formatResult = format_message(formattedMsg, BUFFER_SIZE, MSG_TEXT,
                                       gMyUsername, gMyLocalIPStr, inputCStr);

     if (formatResult == 0) {
         // --- TODO: Send the message ---
         if (isBroadcast) {
             LogToDialog("Broadcasting: %s (Not implemented)", inputCStr);
             // Call UDPSend function here later
         } else {
             LogToDialog("Sending: %s (Not implemented)", inputCStr);
             // Call TCPSend function here later (need selected peer IP)
         }

         // --- Append *sent* message to the display area (Item #2) ---
         sprintf(displayMsg, "You: %s", inputCStr);
         AppendToMessagesTE(displayMsg);
         AppendToMessagesTE("\r");

     } else {
         LogToDialog("Error: Failed to format message for sending.");
         SysBeep(20);
     }

     // 4. Clear the input EditText (Item #3)
     // *** Set text directly via TEHandle ***
      if (gInputTE != NULL) {
          HLock((Handle)gInputTE);
          if (*gInputTE != NULL) {
              TESetText("", 0, gInputTE); // Set TE text to empty
              TECalText(gInputTE); // Recalculate after changing text
          }
          HUnlock((Handle)gInputTE);
         LogToDialog("Input field cleared.");
     }

     // 5. Set focus back to input field (DialogSelect might handle this, but explicit doesn't hurt)
     if (gInputTE != NULL) {
        TEActivate(gInputTE); // Ensure it's active for typing
        LogToDialog("Input field activated.");
     }
 }