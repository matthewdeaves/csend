#ifndef DIALOG_INPUT_H
#define DIALOG_INPUT_H 
#include <MacTypes.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
extern DialogPtr gMainWindow;
extern TEHandle gInputTE;
Boolean InitInputTE(DialogPtr dialog);
void CleanupInputTE(void);
void HandleInputTEClick(DialogPtr dialog, EventRecord *theEvent);
void HandleInputTEUpdate(DialogPtr dialog);
void ActivateInputTE(Boolean activating);
Boolean GetInputText(char *buffer, short bufferSize);
void ClearInputText(void);
#endif
