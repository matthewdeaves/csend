#ifndef DIALOG_H
#define DIALOG_H 
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#define kBaseResID 128
#define kPeerListUserItem 1
#define kMessagesTextEdit 2
#define kInputTextEdit 3
#define kSendButton 4
#define kBroadcastCheckbox 5
extern DialogPtr gMainWindow;
extern TEHandle gMessagesTE;
extern TEHandle gInputTE;
extern Boolean gDialogTEInitialized;
extern char gMyUsername[32];
Boolean InitDialog(void);
void CleanupDialog(void);
void HandleDialogClick(DialogPtr dialog, short itemHit);
void DoSendAction(DialogPtr dialog);
void AppendToMessagesTE(const char *text);
void ActivateDialogTE(Boolean activating);
void UpdateDialogTE(void);
#endif
