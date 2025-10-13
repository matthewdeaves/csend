//====================================
// FILE: ./classic_mac/messaging_mac.h
//====================================

#ifndef MESSAGING_MAC_H
#define MESSAGING_MAC_H
#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"
#include "mactcp_impl.h"  /* For MacTCPAsyncHandle and NetworkTCPInfo types */
#include "network_init.h" /* For TCP_SEND_STREAM_POOL_SIZE and USE_APPLICATION_HEAP */

typedef void (*GiveTimePtr)(void);

#define streamBusyErr (-23050)
#define kTCPDefaultTimeout 0
#define kInvalidStreamPtr 0  /* StreamPtr is unsigned long, not a pointer - use 0 instead of NULL */

typedef enum {
    TCP_STATE_UNINITIALIZED,
    TCP_STATE_IDLE,
    TCP_STATE_LISTENING,
    TCP_STATE_CONNECTING_OUT,
    TCP_STATE_CONNECTED_IN,
    TCP_STATE_CONNECTED_OUT,
    TCP_STATE_SENDING,
    TCP_STATE_CLOSING_GRACEFUL,
    TCP_STATE_ABORTING,
    TCP_STATE_RELEASING,
    TCP_STATE_ERROR
} TCPStreamState;

#define MAX_RDS_ENTRIES 10
#define MAX_QUEUED_MESSAGES 64  /* Increased from 10 to handle burst sends: 4 rounds × 12 msg/round = 48 messages */

/* MacTCP Connection Pool Configuration
 * TCP_SEND_STREAM_POOL_SIZE is defined in network_init.h (4 for standard, 2 for Mac SE)
 * Based on MacTCP Programmer's Guide Chapter 4: "Using Asynchronous Routines"
 */

/* Connection timeout for stale connections (30 seconds at 60Hz ticks) */
#define TCP_STREAM_CONNECTION_TIMEOUT_TICKS (30 * 60)

typedef struct {
    Boolean eventPending;
    TCPEventCode eventCode;
    unsigned short termReason;
    ICMPReport icmpReport;
} ASR_Event_Info;

/* TCP Send Stream Pool Entry
 * Each entry represents one reusable TCP stream that can handle one connection at a time
 * Pool allocation strategy: find first IDLE stream, or queue if none available
 * Reference: MacTCP Programmer's Guide Section 4-18 "TCPActiveOpen" async mode
 */
typedef struct {
    StreamPtr stream;                  /* TCP stream handle (unsigned long) */
    Ptr rcvBuffer;                     /* Dedicated receive buffer for this stream */
    TCPStreamState state;              /* Current state of this pool entry */
    ip_addr targetIP;                  /* Target IP address for active connection */
    tcp_port targetPort;               /* Target port for active connection */
    char peerIPStr[INET_ADDRSTRLEN];   /* String representation of target IP */
    char message[BUFFER_SIZE];         /* Message being sent */
    char msgType[32];                  /* Message type */
    unsigned long connectStartTime;    /* TickCount() when connection started */
    unsigned long sendStartTime;       /* TickCount() when send started */
    MacTCPAsyncHandle connectHandle;   /* Async handle for TCPActiveOpen */
    MacTCPAsyncHandle sendHandle;      /* Async handle for TCPSend */
    MacTCPAsyncHandle closeHandle;     /* Async handle for TCPClose */
    ASR_Event_Info asrEvent;           /* ASR event for this stream */
    int poolIndex;                     /* Index in pool array (for debugging) */
} TCPSendStreamPoolEntry;

typedef struct {
    char peerIP[INET_ADDRSTRLEN];
    char messageType[32];
    char content[BUFFER_SIZE];
    Boolean inUse;
} QueuedMessage;

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP listenAsrUPP,
              TCPNotifyUPP sendAsrUPP);
void CleanupTCP(short macTCPRefNum);
void ProcessTCPStateMachine(GiveTimePtr giveTime);
OSErr MacTCP_QueueMessage(const char *peerIPStr,
                          const char *message_content,
                          const char *msg_type);
TCPStreamState GetTCPListenStreamState(void);
TCPStreamState GetTCPSendStreamState(void);
int GetQueuedMessageCount(void);

/* ASR handlers - use StreamPtr as they're called directly by MacTCP */
pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                   unsigned short terminReason, struct ICMPReport *icmpMsg);
pascal void TCP_Send_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                 unsigned short terminReason, struct ICMPReport *icmpMsg);

#endif
