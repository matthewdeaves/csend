//====================================
// FILE: ./classic_mac/messaging.c
//====================================

#include "messaging.h"
#include "network_abstraction.h"
#include "logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "peer.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_messages.h"
#include "network_init.h"
#include "../shared/messaging.h"
#include <Devices.h>
#include <Errors.h>
#include <MacTypes.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Events.h>
#include <OSUtils.h>
#include <MixedMode.h>

/* Separate streams for listening and sending */
static NetworkStreamRef gTCPListenStream = NULL;
static NetworkStreamRef gTCPSendStream = NULL;

/* Separate buffers for each stream */
static Ptr gTCPListenRcvBuffer = NULL;
static Ptr gTCPSendRcvBuffer = NULL;
static unsigned long gTCPStreamRcvBufferSize = 0;

/* Separate state tracking */
static TCPStreamState gTCPListenState = TCP_STATE_UNINITIALIZED;
static TCPStreamState gTCPSendState = TCP_STATE_UNINITIALIZED;

/* ASR event handling - separate for each stream */
static volatile ASR_Event_Info gListenAsrEvent;
static volatile ASR_Event_Info gSendAsrEvent;

/* Receive data structures */
static wdsEntry gListenNoCopyRDS[MAX_RDS_ENTRIES + 1];
static Boolean gListenNoCopyRdsPendingReturn = false;

/* For async operations we need PBs for polling */
static TCPiopb gListenAsyncPB;
static Boolean gListenAsyncOperationInProgress = false;

/* Connection cleanup tracking */
static Boolean gListenStreamNeedsReset = false;
static unsigned long gListenStreamResetTime = 0;

/* Message queue support */
static QueuedMessage gMessageQueue[MAX_QUEUED_MESSAGES];
static int gQueueHead = 0;
static int gQueueTail = 0;

/* Timeout constants */
#define TCP_ULP_TIMEOUT_DEFAULT_S 20
#define TCP_CONNECT_ULP_TIMEOUT_S 10
#define TCP_SEND_ULP_TIMEOUT_S 10
#define TCP_CLOSE_ULP_TIMEOUT_S 5
#define TCP_PASSIVE_OPEN_CMD_TIMEOUT_S 0
#define TCP_RECEIVE_CMD_TIMEOUT_S 1
#define TCP_STREAM_RESET_DELAY_TICKS 60  /* 1 second delay after connection close */

/* Forward declarations */
static void StartPassiveListen(void);
static void HandleListenASREvents(GiveTimePtr giveTime);
static void HandleSendASREvents(GiveTimePtr giveTime);
static void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status);
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content);
static Boolean DequeueMessage(QueuedMessage *msg);
static void ProcessMessageQueue(GiveTimePtr giveTime);

/* Platform callbacks */
static int mac_tcp_add_or_update_peer_callback(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_debug("Peer added/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult == 0) {
        log_debug("Peer updated via TCP: %s@%s", username, ip);
    } else {
        log_debug("Peer list full or error for %s@%s from TCP.", username, ip);
    }
    return addResult;
}

static void mac_tcp_display_text_message_callback(const char *username, const char *ip, const char *message_content, void *platform_context)
{
    (void)platform_context;
    (void)ip;
    char displayMsg[BUFFER_SIZE + 100];
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
    }
    log_debug("Message from %s@%s displayed: %s", username, ip, message_content);
}

static void mac_tcp_mark_peer_inactive_callback(const char *ip, void *platform_context)
{
    (void)platform_context;
    if (!ip) return;
    log_debug("Peer %s has sent QUIT via TCP. Marking inactive.", ip);
    if (MarkPeerInactive(ip)) {
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}

static tcp_platform_callbacks_t g_mac_tcp_callbacks = {
    .add_or_update_peer = mac_tcp_add_or_update_peer_callback,
    .display_text_message = mac_tcp_display_text_message_callback,
    .mark_peer_inactive = mac_tcp_mark_peer_inactive_callback
};

/* Message queue management functions */
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content)
{
    int nextTail = (gQueueTail + 1) % MAX_QUEUED_MESSAGES;
    if (nextTail == gQueueHead) {
        log_debug("EnqueueMessage: Queue full, cannot enqueue message to %s", peerIP);
        return false;
    }

    QueuedMessage *msg = &gMessageQueue[gQueueTail];
    strncpy(msg->peerIP, peerIP, INET_ADDRSTRLEN - 1);
    msg->peerIP[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(msg->messageType, msgType, 31);
    msg->messageType[31] = '\0';
    strncpy(msg->content, content ? content : "", BUFFER_SIZE - 1);
    msg->content[BUFFER_SIZE - 1] = '\0';
    msg->inUse = true;

    gQueueTail = nextTail;
    log_debug("EnqueueMessage: Queued message to %s (type: %s)", peerIP, msgType);
    return true;
}

static Boolean DequeueMessage(QueuedMessage *msg)
{
    if (gQueueHead == gQueueTail) {
        return false;
    }

    *msg = gMessageQueue[gQueueHead];
    gMessageQueue[gQueueHead].inUse = false;
    gQueueHead = (gQueueHead + 1) % MAX_QUEUED_MESSAGES;
    return true;
}

static void ProcessMessageQueue(GiveTimePtr giveTime)
{
    QueuedMessage msg;

    if (gTCPSendState == TCP_STATE_IDLE) {
        if (DequeueMessage(&msg)) {
            log_debug("ProcessMessageQueue: Processing queued message to %s", msg.peerIP);
            MacTCP_SendMessageSync(msg.peerIP, msg.content, msg.messageType,
                                   gMyUsername, gMyLocalIPStr, giveTime);
        }
    }
}

int GetQueuedMessageCount(void)
{
    int count = 0;
    int idx = gQueueHead;
    while (idx != gQueueTail) {
        if (gMessageQueue[idx].inUse) {
            count++;
        }
        idx = (idx + 1) % MAX_QUEUED_MESSAGES;
    }
    return count;
}

OSErr MacTCP_QueueMessage(const char *peerIPStr, const char *message_content, const char *msg_type)
{
    if (peerIPStr == NULL || msg_type == NULL) {
        return paramErr;
    }

    /* Try to send immediately if send stream is idle */
    if (gTCPSendState == TCP_STATE_IDLE) {
        log_debug("MacTCP_QueueMessage: Send stream idle, attempting immediate send to %s", peerIPStr);
        return MacTCP_SendMessageSync(peerIPStr, message_content, msg_type,
                                      gMyUsername, gMyLocalIPStr, YieldTimeToSystem);
    }

    /* Otherwise queue it */
    if (EnqueueMessage(peerIPStr, msg_type, message_content)) {
        log_debug("MacTCP_QueueMessage: Message queued for later delivery to %s", peerIPStr);
        return noErr;
    } else {
        log_debug("MacTCP_QueueMessage: Failed to queue message - queue full");
        return memFullErr;
    }
}

pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg)
{
#pragma unused(userDataPtr)

    if (gTCPListenStream == NULL || tcpStream != (StreamPtr)gTCPListenStream) {
        return;
    }

    if (gListenAsrEvent.eventPending) {
        return;
    }

    gListenAsrEvent.eventCode = (TCPEventCode)eventCode;
    gListenAsrEvent.termReason = terminReason;
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        BlockMoveData(icmpMsg, &gListenAsrEvent.icmpReport, sizeof(ICMPReport));
    } else {
        memset(&gListenAsrEvent.icmpReport, 0, sizeof(ICMPReport));
    }
    gListenAsrEvent.eventPending = true;
}

pascal void TCP_Send_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg)
{
#pragma unused(userDataPtr)

    if (gTCPSendStream == NULL || tcpStream != (StreamPtr)gTCPSendStream) {
        return;
    }

    if (gSendAsrEvent.eventPending) {
        return;
    }

    gSendAsrEvent.eventCode = (TCPEventCode)eventCode;
    gSendAsrEvent.termReason = terminReason;
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        BlockMoveData(icmpMsg, &gSendAsrEvent.icmpReport, sizeof(ICMPReport));
    } else {
        memset(&gSendAsrEvent.icmpReport, 0, sizeof(ICMPReport));
    }
    gSendAsrEvent.eventPending = true;
}

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP listenAsrUPP, TCPNotifyUPP sendAsrUPP)
{
    OSErr err;

    log_debug("Initializing TCP Messaging Subsystem with dual streams...");

    if (!gNetworkOps) {
        log_app_event("Error: Network abstraction not initialized");
        return notOpenErr;
    }

    if (gTCPListenState != TCP_STATE_UNINITIALIZED || gTCPSendState != TCP_STATE_UNINITIALIZED) {
        log_debug("InitTCP: Already initialized");
        return streamAlreadyOpen;
    }

    if (macTCPRefNum == 0) return paramErr;
    if (listenAsrUPP == NULL || sendAsrUPP == NULL) {
        log_debug("InitTCP: ASR UPPs are NULL. Cannot proceed.");
        return paramErr;
    }

    gTCPStreamRcvBufferSize = streamReceiveBufferSize;

    /* Allocate receive buffer for listen stream */
    gTCPListenRcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);
    if (gTCPListenRcvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate TCP listen stream receive buffer (%lu bytes).",
                      gTCPStreamRcvBufferSize);
        return memFullErr;
    }

    /* Allocate receive buffer for send stream */
    gTCPSendRcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);
    if (gTCPSendRcvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate TCP send stream receive buffer (%lu bytes).",
                      gTCPStreamRcvBufferSize);
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
        return memFullErr;
    }

    log_debug("Allocated TCP stream receive buffers: %lu bytes each", gTCPStreamRcvBufferSize);

    /* Create listen stream */
    err = gNetworkOps->TCPCreate(macTCPRefNum, &gTCPListenStream, gTCPStreamRcvBufferSize,
                                 gTCPListenRcvBuffer, (NetworkNotifyProcPtr)listenAsrUPP);
    if (err != noErr || gTCPListenStream == NULL) {
        log_app_event("Error: Failed to create TCP Listen Stream: %d", err);
        DisposePtr(gTCPListenRcvBuffer);
        DisposePtr(gTCPSendRcvBuffer);
        gTCPListenRcvBuffer = NULL;
        gTCPSendRcvBuffer = NULL;
        return err;
    }

    /* Create send stream */
    err = gNetworkOps->TCPCreate(macTCPRefNum, &gTCPSendStream, gTCPStreamRcvBufferSize,
                                 gTCPSendRcvBuffer, (NetworkNotifyProcPtr)sendAsrUPP);
    if (err != noErr || gTCPSendStream == NULL) {
        log_app_event("Error: Failed to create TCP Send Stream: %d", err);
        gNetworkOps->TCPRelease(macTCPRefNum, gTCPListenStream);
        DisposePtr(gTCPListenRcvBuffer);
        DisposePtr(gTCPSendRcvBuffer);
        gTCPListenStream = NULL;
        gTCPListenRcvBuffer = NULL;
        gTCPSendRcvBuffer = NULL;
        return err;
    }

    log_debug("TCP Streams created successfully using network abstraction.");

    /* Initialize message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Initialize states */
    gTCPListenState = TCP_STATE_IDLE;
    gTCPSendState = TCP_STATE_IDLE;
    gListenAsyncOperationInProgress = false;
    gListenNoCopyRdsPendingReturn = false;
    gListenStreamNeedsReset = false;
    gListenStreamResetTime = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));
    memset((Ptr)&gSendAsrEvent, 0, sizeof(ASR_Event_Info));

    /* Start listening */
    StartPassiveListen();

    log_debug("TCP Messaging Subsystem initialized with dual streams.");
    return noErr;
}

void CleanupTCP(short macTCPRefNum)
{
    log_debug("Cleaning up TCP Messaging Subsystem...");

    if (!gNetworkOps) {
        log_debug("Network abstraction not available during cleanup");
        return;
    }

    /* Clear message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Clean up listen stream */
    if (gListenAsyncOperationInProgress && gTCPListenStream != NULL) {
        log_debug("Listen async operation was in progress. Aborting.");
        gNetworkOps->TCPAbort(gTCPListenStream);
        gListenAsyncOperationInProgress = false;
    }

    if (gListenNoCopyRdsPendingReturn && gTCPListenStream != NULL) {
        log_debug("Listen RDS Buffers were pending return. Attempting return.");
        gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, YieldTimeToSystem);
        gListenNoCopyRdsPendingReturn = false;
    }

    if (gTCPListenStream != NULL) {
        log_debug("Releasing TCP Listen Stream...");
        gNetworkOps->TCPRelease(macTCPRefNum, gTCPListenStream);
        gTCPListenStream = NULL;
    }

    /* Clean up send stream */
    if (gTCPSendStream != NULL) {
        log_debug("Releasing TCP Send Stream...");
        gNetworkOps->TCPRelease(macTCPRefNum, gTCPSendStream);
        gTCPSendStream = NULL;
    }

    /* Dispose buffers */
    if (gTCPListenRcvBuffer != NULL) {
        log_debug("Disposing TCP listen stream receive buffer.");
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
    }

    if (gTCPSendRcvBuffer != NULL) {
        log_debug("Disposing TCP send stream receive buffer.");
        DisposePtr(gTCPSendRcvBuffer);
        gTCPSendRcvBuffer = NULL;
    }

    gTCPStreamRcvBufferSize = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));
    memset((Ptr)&gSendAsrEvent, 0, sizeof(ASR_Event_Info));
    gTCPListenState = TCP_STATE_UNINITIALIZED;
    gTCPSendState = TCP_STATE_UNINITIALIZED;

    log_debug("TCP Messaging Subsystem cleanup finished.");
}

static void StartPassiveListen(void)
{
    OSErr err;

    if (!gNetworkOps) return;

    if (gTCPListenState != TCP_STATE_IDLE) {
        log_debug("StartPassiveListen: Cannot listen, current state is %d (not IDLE).", gTCPListenState);
        return;
    }

    if (gTCPListenStream == NULL) {
        log_debug("CRITICAL (StartPassiveListen): Listen stream is NULL. Cannot listen.");
        gTCPListenState = TCP_STATE_ERROR;
        return;
    }

    if (gListenAsyncOperationInProgress) {
        log_debug("StartPassiveListen: Another async operation is already in progress.");
        return;
    }

    log_debug("Attempting asynchronous TCPPassiveOpen on port %u...", PORT_TCP);

    memset(&gListenAsyncPB, 0, sizeof(TCPiopb));
    gListenAsyncPB.ioCompletion = nil;
    gListenAsyncPB.ioCRefNum = gMacTCPRefNum;
    gListenAsyncPB.csCode = TCPPassiveOpen;
    gListenAsyncPB.tcpStream = (StreamPtr)gTCPListenStream;
    gListenAsyncPB.csParam.open.ulpTimeoutValue = TCP_ULP_TIMEOUT_DEFAULT_S;
    gListenAsyncPB.csParam.open.ulpTimeoutAction = 1;
    gListenAsyncPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    gListenAsyncPB.csParam.open.commandTimeoutValue = TCP_PASSIVE_OPEN_CMD_TIMEOUT_S;
    gListenAsyncPB.csParam.open.localPort = PORT_TCP;
    gListenAsyncPB.csParam.open.localHost = 0L;
    gListenAsyncPB.csParam.open.remoteHost = 0L;
    gListenAsyncPB.csParam.open.remotePort = 0;
    gListenAsyncPB.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&gListenAsyncPB);

    if (err == noErr) {
        log_debug("TCPPassiveOpenAsync successfully initiated.");
        gTCPListenState = TCP_STATE_LISTENING;
        gListenAsyncOperationInProgress = true;
    } else {
        log_app_event("Error: TCPPassiveOpenAsync failed to LAUNCH: %d.", err);
        gTCPListenState = TCP_STATE_IDLE;
    }
}

void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    OSErr err;

    if (!gNetworkOps) return;

    /* Handle ASR events for both streams */
    HandleListenASREvents(giveTime);
    HandleSendASREvents(giveTime);

    /* Process message queue when send stream is available */
    ProcessMessageQueue(giveTime);

    /* Process listen stream state */
    switch (gTCPListenState) {
    case TCP_STATE_IDLE:
        /* Check if we need to wait before restarting listen */
        if (gListenStreamNeedsReset) {
            unsigned long currentTime = TickCount();
            if ((currentTime - gListenStreamResetTime) < TCP_STREAM_RESET_DELAY_TICKS) {
                /* Still waiting for stream to reset */
                break;
            }
            /* Enough time has passed, clear the reset flag */
            gListenStreamNeedsReset = false;
        }
        StartPassiveListen();
        break;

    case TCP_STATE_LISTENING:
        if (gListenAsyncOperationInProgress && gListenAsyncPB.ioResult != 1) {
            gListenAsyncOperationInProgress = false;
            err = gListenAsyncPB.ioResult;

            if (err == noErr) {
                ip_addr remote_ip = gListenAsyncPB.csParam.open.remoteHost;
                tcp_port remote_port = gListenAsyncPB.csParam.open.remotePort;
                char ipStr[INET_ADDRSTRLEN];

                if (gNetworkOps->AddressToString) {
                    gNetworkOps->AddressToString(remote_ip, ipStr);
                } else {
                    sprintf(ipStr, "%lu.%lu.%lu.%lu",
                            (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                            (remote_ip >> 8) & 0xFF, remote_ip & 0xFF);
                }

                log_app_event("Incoming TCP connection established from %s:%u.", ipStr, remote_port);
                gTCPListenState = TCP_STATE_CONNECTED_IN;

                /* Issue a non-blocking receive to check for data */
                Boolean urgentFlag, markFlag;
                memset(gListenNoCopyRDS, 0, sizeof(gListenNoCopyRDS));

                OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPListenStream, (Ptr)gListenNoCopyRDS,
                               MAX_RDS_ENTRIES, 0, /* 0 timeout = non-blocking */
                               &urgentFlag, &markFlag, giveTime);

                log_debug("Initial receive probe after accept: err=%d", rcvErr);

                if (rcvErr == noErr && (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL)) {
                    /* Data already available! */
                    log_debug("Data already available on connection accept!");
                    ProcessIncomingTCPData(gListenNoCopyRDS, remote_ip, remote_port);
                    gListenNoCopyRdsPendingReturn = true;

                    /* Return the buffers */
                    OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gListenNoCopyRdsPendingReturn = false;
                    }
                }
            } else {
                log_app_event("TCPPassiveOpenAsync FAILED: %d.", err);
                gTCPListenState = TCP_STATE_IDLE;
                /* Mark that we need to wait before trying again */
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
            }
        }
        break;

    case TCP_STATE_CONNECTED_IN:
        /* Also periodically check for data in case we missed the ASR */
        if (!gListenNoCopyRdsPendingReturn && !gListenAsyncOperationInProgress) {
            static unsigned long lastCheckTime = 0;
            unsigned long currentTime = TickCount();

            /* Check every 30 ticks (0.5 seconds) */
            if (currentTime - lastCheckTime > 30) {
                lastCheckTime = currentTime;

                Boolean urgentFlag, markFlag;
                memset(gListenNoCopyRDS, 0, sizeof(gListenNoCopyRDS));

                OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPListenStream, (Ptr)gListenNoCopyRDS,
                               MAX_RDS_ENTRIES, 0, /* non-blocking */
                               &urgentFlag, &markFlag, giveTime);

                if (rcvErr == noErr && (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL)) {
                    NetworkTCPInfo tcpInfo;
                    if (gNetworkOps->TCPStatus(gTCPListenStream, &tcpInfo) == noErr) {
                        log_debug("Periodic check found data available");
                        ProcessIncomingTCPData(gListenNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
                        gListenNoCopyRdsPendingReturn = true;

                        OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                        if (bfrReturnErr == noErr) {
                            gListenNoCopyRdsPendingReturn = false;
                        }
                    }
                } else if (rcvErr == connectionClosing) {
                    log_app_event("Listen connection closed by peer (periodic check).");
                    gNetworkOps->TCPAbort(gTCPListenStream);
                    gTCPListenState = TCP_STATE_IDLE;
                    gListenStreamNeedsReset = true;
                    gListenStreamResetTime = TickCount();
                }
            }
        }
        break;

    default:
        break;
    }

    if (giveTime) giveTime();
}

static void HandleListenASREvents(GiveTimePtr giveTime)
{
    if (!gNetworkOps) return;
    if (!gListenAsrEvent.eventPending) return;

    ASR_Event_Info currentEvent = gListenAsrEvent;
    gListenAsrEvent.eventPending = false;

    log_debug("Listen ASR Event: Code %u, Reason %u (State: %d)",
              currentEvent.eventCode, currentEvent.termReason, gTCPListenState);

    switch (currentEvent.eventCode) {
    case TCPDataArrival:
        if (gTCPListenState == TCP_STATE_CONNECTED_IN) {
            if (gListenNoCopyRdsPendingReturn) {
                log_app_event("Listen ASR: TCPDataArrival while RDS buffers still pending return!");
                gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                gListenNoCopyRdsPendingReturn = false;
            }

            NetworkTCPInfo tcpInfo;
            if (gNetworkOps->TCPStatus(gTCPListenStream, &tcpInfo) != noErr) {
                log_debug("Listen ASR: TCPDataArrival, but GetStatus failed.");
                gNetworkOps->TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
                break;
            }

            Boolean urgentFlag, markFlag;
            OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPListenStream, (Ptr)gListenNoCopyRDS,
                           MAX_RDS_ENTRIES, TCP_RECEIVE_CMD_TIMEOUT_S,
                           &urgentFlag, &markFlag, giveTime);

            if (rcvErr == noErr) {
                log_debug("Listen TCPNoCopyRcv successful. Processing data.");
                if (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL) {
                    ProcessIncomingTCPData(gListenNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
                    gListenNoCopyRdsPendingReturn = true;

                    OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gListenNoCopyRdsPendingReturn = false;
                    } else {
                        log_app_event("CRITICAL: Listen TCPBfrReturn FAILED: %d", bfrReturnErr);
                        gTCPListenState = TCP_STATE_ERROR;
                        gNetworkOps->TCPAbort(gTCPListenStream);
                    }
                }
            } else if (rcvErr == connectionClosing) {
                log_app_event("Listen connection closing by peer.");
                gNetworkOps->TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
            } else if (rcvErr != commandTimeout) {
                log_app_event("Error during Listen TCPNoCopyRcv: %d", rcvErr);
                gNetworkOps->TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
            }
        }
        break;

    case TCPTerminate:
        log_app_event("Listen ASR: TCPTerminate. Reason: %u.", currentEvent.termReason);
        if (gListenNoCopyRdsPendingReturn) {
            gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
            gListenNoCopyRdsPendingReturn = false;
        }
        gListenAsyncOperationInProgress = false;
        gTCPListenState = TCP_STATE_IDLE;
        /* Always set reset flag when connection terminates */
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
        break;

    case TCPClosing:
        log_app_event("Listen ASR: Remote peer closed connection.");
        gNetworkOps->TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
        break;

    default:
        break;
    }
}

static void HandleSendASREvents(GiveTimePtr giveTime)
{
    if (!gNetworkOps) return;
    if (!gSendAsrEvent.eventPending) return;

    ASR_Event_Info currentEvent = gSendAsrEvent;
    gSendAsrEvent.eventPending = false;

    log_debug("Send ASR Event: Code %u, Reason %u (State: %d)",
              currentEvent.eventCode, currentEvent.termReason, gTCPSendState);

    switch (currentEvent.eventCode) {
    case TCPTerminate:
        log_debug("Send ASR: TCPTerminate. Reason: %u.", currentEvent.termReason);
        gTCPSendState = TCP_STATE_IDLE;
        break;

    default:
        break;
    }
}

static void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status)
{
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    char remoteIPStrConnected[INET_ADDRSTRLEN];

    if (remote_ip_from_status != 0) {
        if (gNetworkOps && gNetworkOps->AddressToString) {
            gNetworkOps->AddressToString(remote_ip_from_status, remoteIPStrConnected);
        } else {
            sprintf(remoteIPStrConnected, "%lu.%lu.%lu.%lu",
                    (remote_ip_from_status >> 24) & 0xFF,
                    (remote_ip_from_status >> 16) & 0xFF,
                    (remote_ip_from_status >> 8) & 0xFF,
                    remote_ip_from_status & 0xFF);
        }
    } else {
        strcpy(remoteIPStrConnected, "unknown_ip");
    }

    log_debug("ProcessIncomingTCPData from %s:%u", remoteIPStrConnected, remote_port_from_status);

    for (int i = 0; rds[i].length > 0 || rds[i].ptr != NULL; ++i) {
        if (rds[i].length == 0 || rds[i].ptr == NULL) break;

        log_debug("Processing RDS entry %d: Ptr 0x%lX, Len %u", i, (unsigned long)rds[i].ptr, rds[i].length);

        if (parse_message((const char *)rds[i].ptr, rds[i].length,
                          senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_debug("Parsed TCP message: Type '%s', FromUser '%s', FromIP(payload) '%s', Content(len %d) '%.30s...'",
                      msgType, senderUsername, senderIPStrFromPayload, (int)strlen(content), content);

            handle_received_tcp_message(remoteIPStrConnected,
                                        senderUsername,
                                        msgType,
                                        content,
                                        &g_mac_tcp_callbacks,
                                        NULL);

            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_app_event("QUIT message processed from %s.", remoteIPStrConnected);
            }
        } else {
            log_debug("Failed to parse TCP message chunk from %s (length %u).",
                      remoteIPStrConnected, rds[i].length);
        }
    }
}

TCPStreamState GetTCPListenStreamState(void)
{
    return gTCPListenState;
}

TCPStreamState GetTCPSendStreamState(void)
{
    return gTCPSendState;
}

OSErr MacTCP_SendMessageSync(const char *peerIPStr, const char *message_content, const char *msg_type, const char *local_username, const char *local_ip_str, GiveTimePtr giveTime)
{
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;

    if (!gNetworkOps) return notOpenErr;

    log_debug("MacTCP_SendMessageSync: Request to send '%s' to %s (Send Stream State: %d)",
              msg_type, peerIPStr, gTCPSendState);

    /* Validate parameters */
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPSendStream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL || local_username == NULL ||
            local_ip_str == NULL || giveTime == NULL) {
        return paramErr;
    }

    /* Check if send stream is available */
    if (gTCPSendState != TCP_STATE_IDLE) {
        log_app_event("Error: Send stream busy (state %d). Cannot send now.", gTCPSendState);
        return streamBusyErr;
    }

    /* Parse target IP */
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_app_event("Error: Invalid peer IP '%s'.", peerIPStr);
        return paramErr;
    }

    /* Format message */
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, msg_type, local_username,
                                  local_ip_str, message_content ? message_content : "");
    if (formattedLen <= 0) {
        log_app_event("Error: format_message failed for type '%s'.", msg_type);
        return paramErr;
    }

    /* Connect to peer */
    log_debug("Attempting connection to %s:%u...", peerIPStr, PORT_TCP);
    gTCPSendState = TCP_STATE_CONNECTING_OUT;

    err = gNetworkOps->TCPConnect(gTCPSendStream, targetIP, PORT_TCP, TCP_CONNECT_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error: Connection to %s failed: %d", peerIPStr, err);
        gTCPSendState = TCP_STATE_IDLE;
        return err;
    }

    log_debug("Connected to %s", peerIPStr);
    gTCPSendState = TCP_STATE_CONNECTED_OUT;

    /* Send message */
    log_debug("Sending %u bytes...", formattedLen - 1);
    gTCPSendState = TCP_STATE_SENDING;

    err = gNetworkOps->TCPSend(gTCPSendStream, (Ptr)messageBuffer, formattedLen - 1,
                               true, TCP_SEND_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error: Send to %s failed: %d", peerIPStr, err);
        gNetworkOps->TCPAbort(gTCPSendStream);
        gTCPSendState = TCP_STATE_IDLE;
        return err;
    }

    log_debug("Message sent successfully");

    /* Close connection */
    if (strcmp(msg_type, MSG_QUIT) == 0) {
        log_debug("Sending QUIT - using abort for immediate close");
        gNetworkOps->TCPAbort(gTCPSendStream);
    } else {
        log_debug("Attempting graceful close...");
        gTCPSendState = TCP_STATE_CLOSING_GRACEFUL;
        err = gNetworkOps->TCPClose(gTCPSendStream, TCP_CLOSE_ULP_TIMEOUT_S, giveTime);
        if (err != noErr) {
            log_debug("Graceful close failed (%d), using abort", err);
            gNetworkOps->TCPAbort(gTCPSendStream);
        }
    }

    gTCPSendState = TCP_STATE_IDLE;

    log_debug("MacTCP_SendMessageSync complete. Send stream back to IDLE.");
    return noErr;
}