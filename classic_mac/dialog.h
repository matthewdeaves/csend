#ifndef DIALOG_H
#define DIALOG_H
#include <MacTypes.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Lists.h>
#include <Events.h>
#include "dialog_messages.h"
#include "dialog_input.h"
#include "dialog_peerlist.h"
#define kBaseResID 128
#define kPeerListUserItem 1
#define kMessagesTextEdit 2
#define kInputTextEdit 3
#define kSendButton 4
#define kBroadcastCheckbox 5
#define kMessagesScrollbar 6
#define kDebugCheckbox 7
extern DialogPtr gMainWindow;
extern Boolean gDialogTEInitialized;
extern Boolean gDialogListInitialized;

/* Update tracking to reduce excessive redraws */
extern Boolean gInputTENeedsUpdate;
extern Boolean gMessagesTENeedsUpdate;
extern Boolean gPeerListNeedsUpdate;
Boolean InitDialog(void);
void CleanupDialog(void);
void HandleSendButtonClick(void);
void ActivateDialogTE(Boolean activating);
void UpdateDialogControls(void);

/* Invalidation functions to mark components for update */
void InvalidateInputTE(void);
void InvalidateMessagesTE(void);
void InvalidatePeerList(void);
#endif
