//====================================
// FILE: ./classic_mac/dialog.h
//====================================

#ifndef DIALOG_H
#define DIALOG_H

#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h> // <-- Include List Manager header
#include <Events.h> // <-- Include Events header for EventRecord

#define kBaseResID 128

// Dialog Item IDs (matching csend.r DITL)
#define kPeerListUserItem 1 // UserItem for List Manager
#define kMessagesTextEdit 2 // UserItem for Messages TE
#define kInputTextEdit    3 // UserItem for Input TE
#define kSendButton       4 // Button
#define kBroadcastCheckbox 5 // Checkbox

extern DialogPtr gMainWindow;
extern TEHandle gMessagesTE;
extern TEHandle gInputTE;
extern ListHandle gPeerListHandle; // <-- Add ListHandle global
extern Boolean gDialogTEInitialized; // Tracks TE fields init state
extern Boolean gDialogListInitialized; // <-- Tracks List Manager init state
extern char gMyUsername[32];
extern Cell gLastSelectedCell; // <-- To store the selected cell coordinates

Boolean InitDialog(void);
void CleanupDialog(void);
// Correct the declaration to accept EventRecord*
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent); // <-- Pass the event record
void DoSendAction(DialogPtr dialog);
void AppendToMessagesTE(const char *text);
void ActivateDialogTE(Boolean activating);
void UpdateDialogControls(void); // <-- Renamed from UpdateDialogTE
void UpdatePeerDisplayList(Boolean forceRedraw); // <-- Add declaration

#endif // DIALOG_H