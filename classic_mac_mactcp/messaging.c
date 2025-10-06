//====================================
// FILE: ./classic_mac/messaging.c
//====================================

#include "messaging.h"
#include "tcp_state_handlers.h"
#include "network_abstraction.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/peer_wrapper.h"
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
NetworkStreamRef gTCPListenStream = NULL;
static NetworkStreamRef gTCPSendStream = NULL;

/* Separate buffers for each stream */
static Ptr gTCPListenRcvBuffer = NULL;
static Ptr gTCPSendRcvBuffer = NULL;
static unsigned long gTCPStreamRcvBufferSize = 0;

/* Separate state tracking */
TCPStreamState gTCPListenState = TCP_STATE_UNINITIALIZED;
static TCPStreamState gTCPSendState = TCP_STATE_UNINITIALIZED;

/* Async send operation tracking */
static NetworkAsyncHandle gSendConnectHandle = NULL;
static NetworkAsyncHandle gSendDataHandle = NULL;
static char gCurrentSendPeerIP[INET_ADDRSTRLEN];
static char gCurrentSendMessage[BUFFER_SIZE];
static char gCurrentSendMsgType[32];
static ip_addr gCurrentSendTargetIP = 0;

/* ASR event handling - separate for each stream */
static volatile ASR_Event_Info gListenAsrEvent;
static volatile ASR_Event_Info gSendAsrEvent;

/* Receive data structures */
wdsEntry gListenNoCopyRDS[MAX_RDS_ENTRIES + 1];
Boolean gListenNoCopyRdsPendingReturn = false;

/* For async operations we need handles */
NetworkAsyncHandle gListenAsyncHandle = NULL;
Boolean gListenAsyncOperationInProgress = false;

/* Connection cleanup tracking */
Boolean gListenStreamNeedsReset = false;
unsigned long gListenStreamResetTime = 0;

/* Message queue support */
static QueuedMessage gMessageQueue[MAX_QUEUED_MESSAGES];
static int gQueueHead = 0;
static int gQueueTail = 0;

/* Timeout constants */
#define TCP_CLOSE_ULP_TIMEOUT_S 5
#define TCP_RECEIVE_CMD_TIMEOUT_S 1

/* Forward declarations */
void StartPassiveListen(void);
static void HandleListenASREvents(GiveTimePtr giveTime);
static void HandleSendASREvents(GiveTimePtr giveTime);
void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status);
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content);
static Boolean DequeueMessage(QueuedMessage *msg);
static void ProcessMessageQueue(GiveTimePtr giveTime);
static OSErr StartAsyncSend(const char *peerIPStr, const char *message_content, const char *msg_type);
static void ProcessSendStateMachine(GiveTimePtr giveTime);

/* Platform callbacks */
static int mac_tcp_add_or_update_peer_callback(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_debug_cat(LOG_CAT_PEER_MGMT, "Peer added/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult == 0) {
        log_debug_cat(LOG_CAT_PEER_MGMT, "Peer updated via TCP: %s@%s", username, ip);
    } else {
        log_error_cat(LOG_CAT_PEER_MGMT, "Peer list full or error for %s@%s from TCP.", username, ip);
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
    log_debug_cat(LOG_CAT_MESSAGING, "Message from %s@%s displayed: %s", username, ip, message_content);
}

static void mac_tcp_mark_peer_inactive_callback(const char *ip, void *platform_context)
{
    (void)platform_context;
    if (!ip) return;
    log_info_cat(LOG_CAT_PEER_MGMT, "Peer %s has sent QUIT via TCP. Marking inactive.", ip);
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
        log_error_cat(LOG_CAT_MESSAGING, "EnqueueMessage: Queue full, cannot enqueue message to %s", peerIP);
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
    log_debug_cat(LOG_CAT_MESSAGING, "EnqueueMessage: Queued message to %s (type: %s)", peerIP, msgType);
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
    (void)giveTime; /* Unused parameter */

    if (gTCPSendState == TCP_STATE_IDLE) {
        if (DequeueMessage(&msg)) {
            log_debug_cat(LOG_CAT_MESSAGING, "ProcessMessageQueue: Processing queued message to %s", msg.peerIP);
            StartAsyncSend(msg.peerIP, msg.content, msg.messageType);
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
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Send stream idle, attempting immediate send to %s", peerIPStr);
        return StartAsyncSend(peerIPStr, message_content, msg_type);
    }

    /* Otherwise queue it */
    if (EnqueueMessage(peerIPStr, msg_type, message_content)) {
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Message queued for later delivery to %s", peerIPStr);
        return noErr;
    } else {
        log_error_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Failed to queue message - queue full");
        return memFullErr;
    }
}

pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    (void)userDataPtr; /* Unused parameter */

    if (gTCPListenStream == NULL || tcpStream != (StreamPtr)gTCPListenStream) {
        return;
    }

    if (gListenAsrEvent.eventPending) {
        return;
    }

    gListenAsrEvent.eventCode = (TCPEventCode)eventCode;
    gListenAsrEvent.termReason = terminReason;
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        BlockMoveData(icmpMsg, (void *)&gListenAsrEvent.icmpReport, sizeof(ICMPReport));
    } else {
        memset((void *)&gListenAsrEvent.icmpReport, 0, sizeof(ICMPReport));
    }
    gListenAsrEvent.eventPending = true;
}

pascal void TCP_Send_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    (void)userDataPtr; /* Unused parameter */

    if (gTCPSendStream == NULL || tcpStream != (StreamPtr)gTCPSendStream) {
        return;
    }

    if (gSendAsrEvent.eventPending) {
        return;
    }

    gSendAsrEvent.eventCode = (TCPEventCode)eventCode;
    gSendAsrEvent.termReason = terminReason;
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        BlockMoveData(icmpMsg, (void *)&gSendAsrEvent.icmpReport, sizeof(ICMPReport));
    } else {
        memset((void *)&gSendAsrEvent.icmpReport, 0, sizeof(ICMPReport));
    }
    gSendAsrEvent.eventPending = true;
}

/* Wrapper functions to bridge Pascal calling convention to C calling convention */
static void ListenNotifyWrapper(void *stream, unsigned short eventCode,
                                Ptr userDataPtr, unsigned short terminReason,
                                struct ICMPReport *icmpMsg)
{
    /* Call the actual Pascal UPP with proper casting */
    TCP_Listen_ASR_Handler((StreamPtr)stream, eventCode, userDataPtr, terminReason, icmpMsg);
}

static void SendNotifyWrapper(void *stream, unsigned short eventCode,
                              Ptr userDataPtr, unsigned short terminReason,
                              struct ICMPReport *icmpMsg)
{
    /* Call the actual Pascal UPP with proper casting */
    TCP_Send_ASR_Handler((StreamPtr)stream, eventCode, userDataPtr, terminReason, icmpMsg);
}

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP listenAsrUPP, TCPNotifyUPP sendAsrUPP)
{
    OSErr err;

    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP Messaging Subsystem with dual streams...");

    if (!gNetworkOps) {
        log_app_event("Error: Network abstraction not initialized");
        return notOpenErr;
    }

    if (gTCPListenState != TCP_STATE_UNINITIALIZED || gTCPSendState != TCP_STATE_UNINITIALIZED) {
        log_debug_cat(LOG_CAT_MESSAGING, "InitTCP: Already initialized");
        return streamAlreadyOpen;
    }

    if (macTCPRefNum == 0) return paramErr;
    if (listenAsrUPP == NULL || sendAsrUPP == NULL) {
        log_error_cat(LOG_CAT_MESSAGING, "InitTCP: ASR UPPs are NULL. Cannot proceed.");
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

    log_debug_cat(LOG_CAT_MESSAGING, "Allocated TCP stream receive buffers: %lu bytes each", gTCPStreamRcvBufferSize);

    /* Create listen stream */
    err = gNetworkOps->TCPCreate(macTCPRefNum, &gTCPListenStream, gTCPStreamRcvBufferSize,
                                 gTCPListenRcvBuffer, ListenNotifyWrapper);
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
                                 gTCPSendRcvBuffer, SendNotifyWrapper);
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

    log_info_cat(LOG_CAT_MESSAGING, "TCP Streams created successfully using network abstraction.");

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

    log_info_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem initialized with dual streams.");
    return noErr;
}

void CleanupTCP(short macTCPRefNum)
{
    log_debug_cat(LOG_CAT_MESSAGING, "Cleaning up TCP Messaging Subsystem...");

    if (!gNetworkOps) {
        log_debug_cat(LOG_CAT_MESSAGING, "Network abstraction not available during cleanup");
        return;
    }

    /* Clear message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Clean up listen stream */
    if (gListenAsyncOperationInProgress && gTCPListenStream != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen async operation was in progress. Aborting.");
        gNetworkOps->TCPAbort(gTCPListenStream);
        gListenAsyncOperationInProgress = false;
    }

    if (gListenNoCopyRdsPendingReturn && gTCPListenStream != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen RDS Buffers were pending return. Attempting return.");
        gNetworkOps->TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, YieldTimeToSystem);
        gListenNoCopyRdsPendingReturn = false;
    }

    if (gTCPListenStream != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Releasing TCP Listen Stream...");
        gNetworkOps->TCPRelease(macTCPRefNum, gTCPListenStream);
        gTCPListenStream = NULL;
    }

    /* Clean up send stream */
    if (gTCPSendStream != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Releasing TCP Send Stream...");
        gNetworkOps->TCPRelease(macTCPRefNum, gTCPSendStream);
        gTCPSendStream = NULL;
    }

    /* Dispose buffers */
    if (gTCPListenRcvBuffer != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Disposing TCP listen stream receive buffer.");
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
    }

    if (gTCPSendRcvBuffer != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Disposing TCP send stream receive buffer.");
        DisposePtr(gTCPSendRcvBuffer);
        gTCPSendRcvBuffer = NULL;
    }

    gTCPStreamRcvBufferSize = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));
    memset((Ptr)&gSendAsrEvent, 0, sizeof(ASR_Event_Info));
    gTCPListenState = TCP_STATE_UNINITIALIZED;
    gTCPSendState = TCP_STATE_UNINITIALIZED;

    log_debug_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem cleanup finished.");
}

void StartPassiveListen(void)
{
    OSErr err;

    if (!gNetworkOps) return;

    if (gTCPListenState != TCP_STATE_IDLE) {
        log_error_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Cannot listen, current state is %d (not IDLE).", gTCPListenState);
        return;
    }

    if (gTCPListenStream == NULL) {
        log_error_cat(LOG_CAT_MESSAGING, "CRITICAL (StartPassiveListen): Listen stream is NULL. Cannot listen.");
        gTCPListenState = TCP_STATE_ERROR;
        return;
    }

    if (gListenAsyncOperationInProgress) {
        log_debug_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Another async operation is already in progress.");
        return;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Attempting asynchronous TCPListenAsync on port %u...", PORT_TCP);

    err = gNetworkOps->TCPListenAsync(gTCPListenStream, PORT_TCP, &gListenAsyncHandle);

    if (err == noErr) {
        log_debug_cat(LOG_CAT_MESSAGING, "TCPListenAsync successfully initiated.");
        gTCPListenState = TCP_STATE_LISTENING;
        gListenAsyncOperationInProgress = true;
    } else {
        log_app_event("Error: TCPListenAsync failed: %d.", err);
        gTCPListenState = TCP_STATE_IDLE;
    }
}

void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    if (!gNetworkOps) return;

    /* Handle ASR events for both streams */
    HandleListenASREvents(giveTime);
    HandleSendASREvents(giveTime);

    /* Process send state machine */
    ProcessSendStateMachine(giveTime);

    /* Process message queue when send stream is available */
    ProcessMessageQueue(giveTime);

    /* Process listen stream state using the dispatcher */
    dispatch_listen_state_handler(gTCPListenState, giveTime);

    if (giveTime) giveTime();
}

static void HandleListenASREvents(GiveTimePtr giveTime)
{
    if (!gNetworkOps) return;
    if (!gListenAsrEvent.eventPending) return;

    ASR_Event_Info currentEvent = gListenAsrEvent;
    gListenAsrEvent.eventPending = false;

    log_debug_cat(LOG_CAT_MESSAGING, "Listen ASR Event: Code %u, Reason %u (State: %d)",
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
                log_error_cat(LOG_CAT_MESSAGING, "Listen ASR: TCPDataArrival, but GetStatus failed.");
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
                log_debug_cat(LOG_CAT_MESSAGING, "Listen TCPNoCopyRcv successful. Processing data.");
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
    (void)giveTime; /* Unused parameter */

    if (!gNetworkOps) return;
    if (!gSendAsrEvent.eventPending) return;

    ASR_Event_Info currentEvent = gSendAsrEvent;
    gSendAsrEvent.eventPending = false;

    log_debug_cat(LOG_CAT_MESSAGING, "Send ASR Event: Code %u, Reason %u (State: %d)",
                  currentEvent.eventCode, currentEvent.termReason, gTCPSendState);

    switch (currentEvent.eventCode) {
    case TCPTerminate:
        log_debug_cat(LOG_CAT_MESSAGING, "Send ASR: TCPTerminate. Reason: %u.", currentEvent.termReason);
        /* Only set to IDLE if we were expecting the termination */
        if (gTCPSendState == TCP_STATE_CLOSING_GRACEFUL ||
                gTCPSendState == TCP_STATE_IDLE) {
            gTCPSendState = TCP_STATE_IDLE;
        }
        break;

    default:
        break;
    }
}

/* Process the send state machine for async operations */
static void ProcessSendStateMachine(GiveTimePtr giveTime)
{
    OSErr err;
    OSErr operationResult;
    void *resultData;

    if (!gNetworkOps) return;

    switch (gTCPSendState) {
    case TCP_STATE_CONNECTING_OUT:
        if (gSendConnectHandle != NULL) {
            err = gNetworkOps->TCPCheckAsyncStatus(gSendConnectHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                gSendConnectHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_info_cat(LOG_CAT_MESSAGING, "Connected to %s", gCurrentSendPeerIP);
                    gTCPSendState = TCP_STATE_CONNECTED_OUT;

                    /* Start async send */
                    int msgLen = strlen(gCurrentSendMessage);
                    log_debug_cat(LOG_CAT_MESSAGING, "Sending %d bytes...", msgLen);
                    gTCPSendState = TCP_STATE_SENDING;

                    err = gNetworkOps->TCPSendAsync(gTCPSendStream, (Ptr)gCurrentSendMessage,
                                                    msgLen, true, &gSendDataHandle);
                    if (err != noErr) {
                        log_app_event("Error: Async send to %s failed to start: %d",
                                      gCurrentSendPeerIP, err);
                        gNetworkOps->TCPAbort(gTCPSendStream);
                        gTCPSendState = TCP_STATE_IDLE;
                    }
                } else {
                    log_app_event("Error: Connection to %s failed: %d",
                                  gCurrentSendPeerIP, operationResult);
                    gTCPSendState = TCP_STATE_IDLE;
                }
            }
        }
        break;

    case TCP_STATE_SENDING:
        if (gSendDataHandle != NULL) {
            err = gNetworkOps->TCPCheckAsyncStatus(gSendDataHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                gSendDataHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Message sent successfully");

                    /* Close connection */
                    if (strcmp(gCurrentSendMsgType, MSG_QUIT) == 0) {
                        log_debug_cat(LOG_CAT_MESSAGING, "Sending QUIT - using abort for immediate close");
                        gNetworkOps->TCPAbort(gTCPSendStream);
                        gTCPSendState = TCP_STATE_IDLE;
                    } else {
                        log_debug_cat(LOG_CAT_MESSAGING, "Attempting graceful close...");
                        gTCPSendState = TCP_STATE_CLOSING_GRACEFUL;
                        err = gNetworkOps->TCPClose(gTCPSendStream, TCP_CLOSE_ULP_TIMEOUT_S, giveTime);
                        if (err != noErr) {
                            log_warning_cat(LOG_CAT_MESSAGING, "Graceful close failed (%d), using abort", err);
                            gNetworkOps->TCPAbort(gTCPSendStream);
                            gTCPSendState = TCP_STATE_IDLE;
                        }
                        /* Don't set IDLE here - wait for close to complete */
                    }
                } else {
                    log_app_event("Error: Send to %s failed: %d",
                                  gCurrentSendPeerIP, operationResult);
                    gNetworkOps->TCPAbort(gTCPSendStream);
                    gTCPSendState = TCP_STATE_IDLE;
                }
            }
        }
        break;

    case TCP_STATE_CLOSING_GRACEFUL:
        /* Check if close operation completed */
        /* For now, assume close completes quickly and set to IDLE */
        /* In a full implementation, we'd check async close status */
        gTCPSendState = TCP_STATE_IDLE;
        break;

    case TCP_STATE_UNINITIALIZED:
    case TCP_STATE_IDLE:
    case TCP_STATE_LISTENING:
    case TCP_STATE_CONNECTED_IN:
    case TCP_STATE_CONNECTED_OUT:
    case TCP_STATE_ABORTING:
    case TCP_STATE_RELEASING:
    case TCP_STATE_ERROR:
        /* These states are not expected for send stream operations */
        log_warning_cat(LOG_CAT_MESSAGING, "Send stream in unexpected state: %d", gTCPSendState);
        break;

    default:
        break;
    }
}

void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status)
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

    log_debug_cat(LOG_CAT_MESSAGING, "ProcessIncomingTCPData from %s:%u", remoteIPStrConnected, remote_port_from_status);

    for (int i = 0; rds[i].length > 0 || rds[i].ptr != NULL; ++i) {
        if (rds[i].length == 0 || rds[i].ptr == NULL) break;

        log_debug_cat(LOG_CAT_MESSAGING, "Processing RDS entry %d: Ptr 0x%lX, Len %u", i, (unsigned long)rds[i].ptr, rds[i].length);

        csend_uint32_t msg_id;
        if (parse_message((const char *)rds[i].ptr, rds[i].length,
                          senderIPStrFromPayload, senderUsername, msgType, &msg_id, content) == 0) {
            log_debug_cat(LOG_CAT_MESSAGING, "Parsed TCP message: ID %lu, Type '%s', FromUser '%s', FromIP(payload) '%s', Content(len %d) '%.30s...'",
                          (unsigned long)msg_id, msgType, senderUsername, senderIPStrFromPayload, (int)strlen(content), content);

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
            log_error_cat(LOG_CAT_MESSAGING, "Failed to parse TCP message chunk from %s (length %u).",
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

/* Start an async send operation */
static OSErr StartAsyncSend(const char *peerIPStr, const char *message_content, const char *msg_type)
{
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;

    if (!gNetworkOps) return notOpenErr;

    log_debug_cat(LOG_CAT_MESSAGING, "StartAsyncSend: Request to send '%s' to %s", msg_type, peerIPStr);

    /* Validate parameters */
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPSendStream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL) return paramErr;

    /* Must be in IDLE state */
    if (gTCPSendState != TCP_STATE_IDLE) {
        log_debug_cat(LOG_CAT_MESSAGING, "StartAsyncSend: Send stream not idle (state %d)", gTCPSendState);
        return connectionExists;
    }

    /* Parse target IP */
    if (strcmp(peerIPStr, gMyLocalIPStr) == 0) {
        targetIP = gMyLocalIP;
    } else {
        err = ParseIPv4(peerIPStr, &targetIP);
        if (err != noErr) {
            log_app_event("Error: Invalid IP address %s", peerIPStr);
            return err;
        }
    }

    /* Format message */
    formattedLen = format_message(messageBuffer, sizeof(messageBuffer), msg_type,
                                  generate_message_id(), gMyUsername, gMyLocalIPStr, message_content);
    if (formattedLen < 0) {
        log_app_event("Error: format_message failed for type '%s'.", msg_type);
        return paramErr;
    }

    /* Store current send operation info */
    strcpy(gCurrentSendPeerIP, peerIPStr);
    strcpy(gCurrentSendMessage, messageBuffer);
    strcpy(gCurrentSendMsgType, msg_type);
    gCurrentSendTargetIP = targetIP;

    /* Start async connect */
    log_debug_cat(LOG_CAT_MESSAGING, "Starting async connection to %s:%u...", peerIPStr, PORT_TCP);
    gTCPSendState = TCP_STATE_CONNECTING_OUT;

    err = gNetworkOps->TCPConnectAsync(gTCPSendStream, targetIP, PORT_TCP, &gSendConnectHandle);
    if (err != noErr) {
        log_app_event("Error: Async connection to %s failed to start: %d", peerIPStr, err);
        gTCPSendState = TCP_STATE_IDLE;
        return err;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Async connect initiated");
    return noErr;
}

