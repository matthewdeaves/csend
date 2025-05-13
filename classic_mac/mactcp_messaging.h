#ifndef MACTCP_MESSAGING_H
#define MACTCP_MESSAGING_H
#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"
typedef void (*GiveTimePtr)(void);
#define streamBusyErr (-23050)
#define kTCPDefaultTimeout 0
typedef enum {
    TCP_STATE_UNINITIALIZED,
    TCP_STATE_IDLE,
    TCP_STATE_LISTENING,
    TCP_STATE_CONNECTING_OUT,
    TCP_STATE_CONNECTED,
    TCP_STATE_SENDING,
    TCP_STATE_CLOSING_GRACEFUL,
    TCP_STATE_ABORTING,
    TCP_STATE_RELEASING,
    TCP_STATE_ERROR,
    TCP_STATE_RETRY_LISTEN_DELAY,
    TCP_STATE_POST_ABORT_COOLDOWN
} TCPStreamState;
#define MAX_RDS_ENTRIES 10
typedef struct {
    Boolean eventPending;
    TCPEventCode eventCode;
    unsigned short termReason;
    ICMPReport icmpReport;
} ASR_Event_Info;
extern volatile Boolean gGracefulActiveCloseTerminating;
extern volatile unsigned long gDuplicateSocketRetryDelayStartTicks;
extern volatile unsigned long gPostAbortCooldownStartTicks;
OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP asrNotifyUPP);
void CleanupTCP(short macTCPRefNum);
void ProcessTCPStateMachine(GiveTimePtr giveTime);
OSErr MacTCP_SendMessageSync(const char *peerIPStr,
                             const char *message_content,
                             const char *msg_type,
                             const char *local_username,
                             const char *local_ip_str,
                             GiveTimePtr giveTime);
TCPStreamState GetTCPStreamState(void);
pascal void TCP_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg);
#endif
