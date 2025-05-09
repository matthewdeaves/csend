//====================================
// FILE: ./classic_mac/mactcp_messaging.h
//====================================

#ifndef TCP_H
#define TCP_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

typedef void (*GiveTimePtr)(void);

#define kTCPDefaultTimeout 0
#define streamBusyErr (-23050) // MacTCP specific error for busy stream

typedef enum {
    TCP_STATE_UNINITIALIZED,
    TCP_STATE_IDLE,
    TCP_STATE_PASSIVE_OPEN_PENDING,
    TCP_STATE_CONNECTED_IN,
    TCP_STATE_RELEASING,
    TCP_STATE_ERROR
} TCPState;

OSErr InitTCP(short macTCPRefNum);
void CleanupTCP(short macTCPRefNum);
void PollTCP(GiveTimePtr giveTime);

OSErr MacTCP_SendMessageSync(const char *peerIPStr,
                             const char *message_content,
                             const char *msg_type,
                             const char *local_username,
                             const char *local_ip_str,
                             GiveTimePtr giveTime);

TCPState GetTCPState(void);

#endif /* TCP_H */