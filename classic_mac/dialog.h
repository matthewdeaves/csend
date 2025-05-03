#ifndef DIALOG_H
#define DIALOG_H 
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h>
#include <Events.h>
#define kBaseResID 128
#define kPeerListUserItem 1
#define kMessagesTextEdit 2
#define kInputTextEdit 3
#define kSendButton 4
#define kBroadcastCheckbox 5
#define kMessagesScrollbar 6
extern DialogPtr gMainWindow;
extern TEHandle gMessagesTE;
extern TEHandle gInputTE;
extern ListHandle gPeerListHandle;
extern ControlHandle gMessagesScrollBar;
extern Boolean gDialogTEInitialized;
extern Boolean gDialogListInitialized;
extern char gMyUsername[32];
extern Cell gLastSelectedCell;
Boolean InitDialog(void);
void CleanupDialog(void);
void HandleDialogClick(DialogPtr dialog, short itemHit, EventRecord *theEvent);
void DoSendAction(DialogPtr dialog);
void AppendToMessagesTE(const char *text);
void ActivateDialogTE(Boolean activating);
void UpdateDialogControls(void);
void UpdatePeerDisplayList(Boolean forceRedraw);
void AdjustMessagesScrollbar(void);
pascal void MyScrollAction(ControlHandle theControl, short partCode);
#endif
