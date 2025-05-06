#ifndef TCP_H
#define TCP_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

typedef void (*GiveTimePtr)(void);

#define kTCPDefaultTimeout 0
#define streamBusyErr (-23050) // MacTCP specific error, not a general OS error

typedef enum {
    TCP_STATE_UNINITIALIZED,
    TCP_STATE_IDLE,
    TCP_STATE_PASSIVE_OPEN_PENDING, // New state for asynchronous passive open
    TCP_STATE_CONNECTED_IN,
    TCP_STATE_RELEASING,
    TCP_STATE_ERROR
} TCPState;

OSErr InitTCP(short macTCPRefNum);
void CleanupTCP(short macTCPRefNum);
void PollTCP(GiveTimePtr giveTime);
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);
TCPState GetTCPState(void);

#endif