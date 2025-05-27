#ifndef DIALOG_MESSAGES_H
#define DIALOG_MESSAGES_H
#include <MacTypes.h>
#include <TextEdit.h>
#include <Controls.h>
#include <Dialogs.h>
extern TEHandle gMessagesTE;
extern ControlHandle gMessagesScrollBar;
Boolean InitMessagesTEAndScrollbar(DialogPtr dialog);
void CleanupMessagesTEAndScrollbar(void);
void AppendToMessagesTE(const char *text);
void AdjustMessagesScrollbar(void);
void HandleMessagesScrollClick(ControlHandle theControl, short partCode);
void ScrollMessagesTEToValue(short newScrollValue);
void HandleMessagesTEUpdate(DialogPtr dialog);
void ActivateMessagesTEAndScrollbar(Boolean activating);
void ScrollMessagesTE(short deltaPixels);
pascal void MyScrollAction(ControlHandle theControl, short partCode);
#endif
