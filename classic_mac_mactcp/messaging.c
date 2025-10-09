//====================================
// FILE: ./classic_mac/messaging.c
//====================================

#include "messaging.h"
#include "tcp_state_handlers.h"
#include "mactcp_impl.h"
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

/* Listen stream for incoming connections */
StreamPtr gTCPListenStream = kInvalidStreamPtr;
static Ptr gTCPListenRcvBuffer = NULL;
static unsigned long gTCPStreamRcvBufferSize = 0;
TCPStreamState gTCPListenState = TCP_STATE_UNINITIALIZED;

/* ASR event handling for listen stream */
static volatile ASR_Event_Info gListenAsrEvent;

/* Connection pool for outgoing connections
 * Replaces single send stream to enable concurrent message sending
 * Reference: MacTCP Programmer's Guide Chapter 4 - async connection patterns
 */
static TCPSendStreamPoolEntry gSendStreamPool[TCP_SEND_STREAM_POOL_SIZE];
static int gPoolInitialized = 0;

/* Receive data structures */
wdsEntry gListenNoCopyRDS[MAX_RDS_ENTRIES + 1];
Boolean gListenNoCopyRdsPendingReturn = false;

/* For async operations we need handles */
MacTCPAsyncHandle gListenAsyncHandle = NULL;
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
static void HandlePoolEntryASREvents(int poolIndex, GiveTimePtr giveTime);
void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status);
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content);
static Boolean DequeueMessage(QueuedMessage *msg);
static int AllocatePoolEntry(void);
static void ProcessMessageQueue(GiveTimePtr giveTime);
static OSErr StartAsyncSendOnPoolEntry(int poolIndex, const char *peerIPStr, const char *message_content,
                                       const char *msg_type);
static void ProcessPoolEntryStateMachine(int poolIndex, GiveTimePtr giveTime);
static void CheckPoolEntryTimeout(int poolIndex);

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

static void mac_tcp_display_text_message_callback(const char *username, const char *ip, const char *message_content,
        void *platform_context)
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

/* Find an IDLE pool entry for sending a message
 * Returns pool index if found, -1 if no IDLE entries available
 */
static int AllocatePoolEntry(void)
{
    int i;

    if (!gPoolInitialized) return -1;

    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].state == TCP_STATE_IDLE) {
            return i;
        }
    }

    return -1; /* No IDLE entries available */
}

static void ProcessMessageQueue(GiveTimePtr giveTime)
{
    QueuedMessage msg;
    int poolIndex;

    (void)giveTime; /* Unused parameter */

    if (!gPoolInitialized) return;

    /* Try to process one queued message per call */
    poolIndex = AllocatePoolEntry();
    if (poolIndex >= 0) {
        if (DequeueMessage(&msg)) {
            log_debug_cat(LOG_CAT_MESSAGING, "ProcessMessageQueue: Pool[%d] processing queued message to %s",
                          poolIndex, msg.peerIP);
            StartAsyncSendOnPoolEntry(poolIndex, msg.peerIP, msg.content, msg.messageType);
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
    int poolIndex;
    int queuedCount;

    if (peerIPStr == NULL || msg_type == NULL) {
        return paramErr;
    }

    if (!gPoolInitialized) {
        log_error_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Pool not initialized");
        return notOpenErr;
    }

    /* Try to allocate a pool entry for immediate send */
    poolIndex = AllocatePoolEntry();
    if (poolIndex >= 0) {
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Pool[%d] available, attempting immediate send to %s",
                      poolIndex, peerIPStr);
        return StartAsyncSendOnPoolEntry(poolIndex, peerIPStr, message_content, msg_type);
    }

    /* No pool entry available - queue the message */
    queuedCount = GetQueuedMessageCount();
    log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: All pool entries busy (%d queued), queueing message to %s",
                  queuedCount, peerIPStr);

    if (EnqueueMessage(peerIPStr, msg_type, message_content)) {
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Message queued (queue: %d/%d)",
                      queuedCount + 1, MAX_QUEUED_MESSAGES);
        return noErr;
    } else {
        log_error_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Failed to queue message - queue full");
        return memFullErr;
    }
}

pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                   unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    (void)userDataPtr; /* Unused parameter */

    if (gTCPListenStream == kInvalidStreamPtr || tcpStream != (StreamPtr)gTCPListenStream) {
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

/* Pool-aware Send ASR Handler
 * Identifies which pool entry triggered the event and stores event in that entry
 * Reference: MacTCP Programmer's Guide Section 4-3 "Using Asynchronous Routines"
 */
pascal void TCP_Send_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                 unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    int i;
    (void)userDataPtr; /* Unused parameter */

    if (!gPoolInitialized || tcpStream == NULL) {
        return;
    }

    /* Find which pool entry this stream belongs to */
    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].stream == (StreamPtr)tcpStream) {
            /* Found the pool entry - check if event already pending */
            if (gSendStreamPool[i].asrEvent.eventPending) {
                /* Event already pending, drop this one to avoid overwriting */
                log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: ASR event dropped (event pending)", i);
                return;
            }

            /* Store event in pool entry */
            gSendStreamPool[i].asrEvent.eventCode = (TCPEventCode)eventCode;
            gSendStreamPool[i].asrEvent.termReason = terminReason;
            if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
                BlockMoveData(icmpMsg, (void *)&gSendStreamPool[i].asrEvent.icmpReport, sizeof(ICMPReport));
            } else {
                memset((void *)&gSendStreamPool[i].asrEvent.icmpReport, 0, sizeof(ICMPReport));
            }
            gSendStreamPool[i].asrEvent.eventPending = true;
            return;
        }
    }

    /* Stream not found in pool - this shouldn't happen */
    log_warning_cat(LOG_CAT_MESSAGING, "TCP_Send_ASR_Handler: Unknown stream 0x%lX", (unsigned long)tcpStream);
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

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP listenAsrUPP,
              TCPNotifyUPP sendAsrUPP)
{
    OSErr err;
    int i, j;

    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP Messaging Subsystem with connection pool...");

    if (gTCPListenState != TCP_STATE_UNINITIALIZED || gPoolInitialized) {
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

    log_debug_cat(LOG_CAT_MESSAGING, "Allocated TCP listen stream receive buffer: %lu bytes", gTCPStreamRcvBufferSize);

    /* Create listen stream */
    err = MacTCPImpl_TCPCreate(macTCPRefNum, &gTCPListenStream, gTCPStreamRcvBufferSize,
                                 gTCPListenRcvBuffer, ListenNotifyWrapper);
    if (err != noErr || gTCPListenStream == kInvalidStreamPtr) {
        log_app_event("Error: Failed to create TCP Listen Stream: %d", err);
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
        return err;
    }

    /* Initialize connection pool for outgoing messages
     * Based on MacTCP Programmer's Guide Chapter 4: pool of streams for concurrent sends
     */
    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP send stream pool (%d streams)...", TCP_SEND_STREAM_POOL_SIZE);

    memset(gSendStreamPool, 0, sizeof(gSendStreamPool));

    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        /* Allocate receive buffer for this pool entry */
        gSendStreamPool[i].rcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);
        if (gSendStreamPool[i].rcvBuffer == NULL) {
            log_app_event("Fatal Error: Could not allocate pool[%d] receive buffer (%lu bytes).", i, gTCPStreamRcvBufferSize);

            /* Clean up previously allocated buffers */
            for (j = 0; j < i; j++) {
                if (gSendStreamPool[j].stream) {
                    MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[j].stream);
                }
                if (gSendStreamPool[j].rcvBuffer) {
                    DisposePtr(gSendStreamPool[j].rcvBuffer);
                }
            }
            MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
            DisposePtr(gTCPListenRcvBuffer);
            gTCPListenStream = kInvalidStreamPtr;
            gTCPListenRcvBuffer = NULL;
            return memFullErr;
        }

        /* Create TCP stream for this pool entry */
        err = MacTCPImpl_TCPCreate(macTCPRefNum, &gSendStreamPool[i].stream,
                                     gTCPStreamRcvBufferSize, gSendStreamPool[i].rcvBuffer,
                                     SendNotifyWrapper);
        if (err != noErr || gSendStreamPool[i].stream == kInvalidStreamPtr) {
            log_app_event("Error: Failed to create pool[%d] TCP stream: %d", i, err);

            /* Clean up this entry's buffer */
            DisposePtr(gSendStreamPool[i].rcvBuffer);

            /* Clean up previously created entries */
            for (j = 0; j < i; j++) {
                if (gSendStreamPool[j].stream) {
                    MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[j].stream);
                }
                if (gSendStreamPool[j].rcvBuffer) {
                    DisposePtr(gSendStreamPool[j].rcvBuffer);
                }
            }
            MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
            DisposePtr(gTCPListenRcvBuffer);
            gTCPListenStream = kInvalidStreamPtr;
            gTCPListenRcvBuffer = NULL;
            return err;
        }

        /* Initialize pool entry state */
        gSendStreamPool[i].state = TCP_STATE_IDLE;
        gSendStreamPool[i].targetIP = 0;
        gSendStreamPool[i].targetPort = 0;
        gSendStreamPool[i].peerIPStr[0] = '\0';
        gSendStreamPool[i].message[0] = '\0';
        gSendStreamPool[i].msgType[0] = '\0';
        gSendStreamPool[i].connectStartTime = 0;
        gSendStreamPool[i].sendStartTime = 0;
        gSendStreamPool[i].connectHandle = NULL;
        gSendStreamPool[i].sendHandle = NULL;
        gSendStreamPool[i].poolIndex = i;
        memset((void *)&gSendStreamPool[i].asrEvent, 0, sizeof(ASR_Event_Info));

        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Stream created at 0x%lX", i, (unsigned long)gSendStreamPool[i].stream);
    }

    gPoolInitialized = 1;
    log_info_cat(LOG_CAT_MESSAGING, "TCP send stream pool initialized (%d streams)", TCP_SEND_STREAM_POOL_SIZE);

    /* Initialize message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Initialize states */
    gTCPListenState = TCP_STATE_IDLE;
    gListenAsyncOperationInProgress = false;
    gListenNoCopyRdsPendingReturn = false;
    gListenStreamNeedsReset = false;
    gListenStreamResetTime = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));

    /* Start listening */
    StartPassiveListen();

    log_info_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem initialized with dual streams.");
    return noErr;
}

void CleanupTCP(short macTCPRefNum)
{
    int i;

    log_debug_cat(LOG_CAT_MESSAGING, "Cleaning up TCP Messaging Subsystem...");

    /* Clear message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Clean up listen stream */
    if (gListenAsyncOperationInProgress && gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen async operation was in progress. Aborting.");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gListenAsyncOperationInProgress = false;
    }

    if (gListenNoCopyRdsPendingReturn && gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen RDS Buffers were pending return. Attempting return.");
        MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, YieldTimeToSystem);
        gListenNoCopyRdsPendingReturn = false;
    }

    if (gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Releasing TCP Listen Stream...");
        MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
        gTCPListenStream = kInvalidStreamPtr;
    }

    /* Clean up send stream pool */
    if (gPoolInitialized) {
        log_debug_cat(LOG_CAT_MESSAGING, "Cleaning up TCP send stream pool (%d streams)...", TCP_SEND_STREAM_POOL_SIZE);

        for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
            if (gSendStreamPool[i].stream != kInvalidStreamPtr) {
                /* Abort any active connections */
                if (gSendStreamPool[i].state != TCP_STATE_IDLE &&
                        gSendStreamPool[i].state != TCP_STATE_UNINITIALIZED) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Aborting active connection", i);
                    MacTCPImpl_TCPAbort(gSendStreamPool[i].stream);
                }

                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Releasing TCP stream", i);
                MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[i].stream);
                gSendStreamPool[i].stream = kInvalidStreamPtr;
            }

            if (gSendStreamPool[i].rcvBuffer != NULL) {
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Disposing receive buffer", i);
                DisposePtr(gSendStreamPool[i].rcvBuffer);
                gSendStreamPool[i].rcvBuffer = NULL;
            }
        }

        memset(gSendStreamPool, 0, sizeof(gSendStreamPool));
        gPoolInitialized = 0;
        log_debug_cat(LOG_CAT_MESSAGING, "TCP send stream pool cleaned up");
    }

    /* Dispose listen buffer */
    if (gTCPListenRcvBuffer != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Disposing TCP listen stream receive buffer.");
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
    }

    gTCPStreamRcvBufferSize = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));
    gTCPListenState = TCP_STATE_UNINITIALIZED;

    log_debug_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem cleanup finished.");
}

void StartPassiveListen(void)
{
    OSErr err;

    if (gTCPListenState != TCP_STATE_IDLE) {
        log_error_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Cannot listen, current state is %d (not IDLE).", gTCPListenState);
        return;
    }

    if (gTCPListenStream == kInvalidStreamPtr) {
        log_error_cat(LOG_CAT_MESSAGING, "CRITICAL (StartPassiveListen): Listen stream is NULL. Cannot listen.");
        gTCPListenState = TCP_STATE_ERROR;
        return;
    }

    if (gListenAsyncOperationInProgress) {
        log_debug_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Another async operation is already in progress.");
        return;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Attempting asynchronous TCPListenAsync on port %u...", PORT_TCP);

    err = MacTCPImpl_TCPListenAsync(gTCPListenStream, PORT_TCP, &gListenAsyncHandle);

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
    int i;

    /* Handle ASR events for listen stream */
    HandleListenASREvents(giveTime);

    /* Process send stream pool - handle ASR events and state machines for each entry */
    if (gPoolInitialized) {
        for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
            /* Handle ASR events for this pool entry */
            HandlePoolEntryASREvents(i, giveTime);

            /* Process state machine for this pool entry */
            ProcessPoolEntryStateMachine(i, giveTime);

            /* Check for stale connections and timeout */
            CheckPoolEntryTimeout(i);
        }
    }

    /* Process message queue - allocate pool entries for queued messages */
    ProcessMessageQueue(giveTime);

    /* Process listen stream state using the dispatcher */
    dispatch_listen_state_handler(gTCPListenState, giveTime);

    if (giveTime) giveTime();
}

static void HandleListenASREvents(GiveTimePtr giveTime)
{
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
                MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                gListenNoCopyRdsPendingReturn = false;
            }

            NetworkTCPInfo tcpInfo;
            if (MacTCPImpl_TCPStatus(gTCPListenStream, &tcpInfo) != noErr) {
                log_error_cat(LOG_CAT_MESSAGING, "Listen ASR: TCPDataArrival, but GetStatus failed.");
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
                break;
            }

            Boolean urgentFlag, markFlag;
            OSErr rcvErr = MacTCPImpl_TCPReceiveNoCopy(gTCPListenStream, (Ptr)gListenNoCopyRDS,
                           MAX_RDS_ENTRIES, TCP_RECEIVE_CMD_TIMEOUT_S,
                           &urgentFlag, &markFlag, giveTime);

            if (rcvErr == noErr) {
                log_debug_cat(LOG_CAT_MESSAGING, "Listen TCPNoCopyRcv successful. Processing data.");
                if (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL) {
                    ProcessIncomingTCPData(gListenNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
                    gListenNoCopyRdsPendingReturn = true;

                    OSErr bfrReturnErr = MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gListenNoCopyRdsPendingReturn = false;
                    } else {
                        log_app_event("CRITICAL: Listen TCPBfrReturn FAILED: %d", bfrReturnErr);
                        gTCPListenState = TCP_STATE_ERROR;
                        MacTCPImpl_TCPAbort(gTCPListenStream);
                    }
                }
            } else if (rcvErr == connectionClosing) {
                log_app_event("Listen connection closing by peer.");
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
            } else if (rcvErr != commandTimeout) {
                log_app_event("Error during Listen TCPNoCopyRcv: %d", rcvErr);
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                gListenStreamNeedsReset = true;
                gListenStreamResetTime = TickCount();
            }
        }
        break;

    case TCPTerminate:
        log_app_event("Listen ASR: TCPTerminate. Reason: %u.", currentEvent.termReason);
        if (gListenNoCopyRdsPendingReturn) {
            MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
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
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
        break;

    default:
        break;
    }
}

/* Handle ASR events for a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-3 - ASR handler patterns
 */
static void HandlePoolEntryASREvents(int poolIndex, GiveTimePtr giveTime)
{
    TCPSendStreamPoolEntry *entry;
    ASR_Event_Info currentEvent;

    (void)giveTime; /* Unused parameter */

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];

    if (!entry->asrEvent.eventPending) return;

    /* Copy and clear event */
    currentEvent = entry->asrEvent;
    entry->asrEvent.eventPending = false;

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: ASR Event: Code %u, Reason %u (State: %d)",
                  poolIndex, currentEvent.eventCode, currentEvent.termReason, entry->state);

    switch (currentEvent.eventCode) {
    case TCPTerminate:
        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: TCPTerminate. Reason: %u.", poolIndex, currentEvent.termReason);
        /* Only set to IDLE if we were expecting the termination */
        if (entry->state == TCP_STATE_CLOSING_GRACEFUL || entry->state == TCP_STATE_IDLE) {
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
        }
        break;

    default:
        break;
    }
}

/* Process the state machine for a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-18 - async operation completion
 */
static void ProcessPoolEntryStateMachine(int poolIndex, GiveTimePtr giveTime)
{
    TCPSendStreamPoolEntry *entry;
    OSErr err;
    OSErr operationResult;
    void *resultData;
    int msgLen;

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];

    switch (entry->state) {
    case TCP_STATE_CONNECTING_OUT:
        if (entry->connectHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->connectHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                entry->connectHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_info_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connected to %s", poolIndex, entry->peerIPStr);
                    entry->state = TCP_STATE_CONNECTED_OUT;

                    /* Start async send */
                    msgLen = strlen(entry->message);
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Sending %d bytes...", poolIndex, msgLen);
                    entry->state = TCP_STATE_SENDING;
                    entry->sendStartTime = TickCount();

                    err = MacTCPImpl_TCPSendAsync(entry->stream, (Ptr)entry->message,
                                                    msgLen, true, &entry->sendHandle);
                    if (err != noErr) {
                        log_app_event("Pool[%d]: Async send to %s failed to start: %d",
                                      poolIndex, entry->peerIPStr, err);
                        MacTCPImpl_TCPAbort(entry->stream);
                        entry->state = TCP_STATE_IDLE;
                        entry->connectHandle = NULL;
                        entry->sendHandle = NULL;
                    }
                } else {
                    log_app_event("Pool[%d]: Connection to %s failed: %d",
                                  poolIndex, entry->peerIPStr, operationResult);
                    entry->state = TCP_STATE_IDLE;
                    entry->connectHandle = NULL;
                }
            }
        }
        break;

    case TCP_STATE_SENDING:
        if (entry->sendHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->sendHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                entry->sendHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Message sent successfully", poolIndex);

                    /* Close connection */
                    if (strcmp(entry->msgType, MSG_QUIT) == 0) {
                        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Sending QUIT - using abort for immediate close", poolIndex);
                        MacTCPImpl_TCPAbort(entry->stream);
                        entry->state = TCP_STATE_IDLE;
                        entry->connectHandle = NULL;
                        entry->sendHandle = NULL;
                    } else {
                        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Attempting graceful close...", poolIndex);
                        entry->state = TCP_STATE_CLOSING_GRACEFUL;
                        err = MacTCPImpl_TCPClose(entry->stream, TCP_CLOSE_ULP_TIMEOUT_S, giveTime);
                        if (err != noErr) {
                            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Graceful close failed (%d), using abort", poolIndex, err);
                            MacTCPImpl_TCPAbort(entry->stream);
                            entry->state = TCP_STATE_IDLE;
                            entry->connectHandle = NULL;
                            entry->sendHandle = NULL;
                        }
                        /* Don't set IDLE here - wait for close to complete or ASR event */
                    }
                } else {
                    log_app_event("Pool[%d]: Send to %s failed: %d",
                                  poolIndex, entry->peerIPStr, operationResult);
                    MacTCPImpl_TCPAbort(entry->stream);
                    entry->state = TCP_STATE_IDLE;
                    entry->connectHandle = NULL;
                    entry->sendHandle = NULL;
                }
            }
        }
        break;

    case TCP_STATE_CLOSING_GRACEFUL:
        /* Graceful close in progress - transition to IDLE
         * ASR handler will handle TCPTerminate event */
        entry->state = TCP_STATE_IDLE;
        entry->connectHandle = NULL;
        entry->sendHandle = NULL;
        break;

    case TCP_STATE_IDLE:
        /* Normal idle state - no action needed, available for new send request */
        break;

    default:
        /* Unexpected states */
        if (entry->state != TCP_STATE_UNINITIALIZED) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Unexpected state: %d", poolIndex, entry->state);
        }
        break;
    }
}

/* Check for stale connections and timeout them
 * Prevents pool entries from getting stuck in connecting/sending states
 */
static void CheckPoolEntryTimeout(int poolIndex)
{
    TCPSendStreamPoolEntry *entry;
    unsigned long currentTime;
    unsigned long elapsedTicks;

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];
    currentTime = TickCount();

    /* Check for stale connections */
    if (entry->state == TCP_STATE_CONNECTING_OUT && entry->connectStartTime > 0) {
        elapsedTicks = currentTime - entry->connectStartTime;
        if (elapsedTicks > TCP_STREAM_CONNECTION_TIMEOUT_TICKS) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connection timeout to %s (%lu ticks)",
                            poolIndex, entry->peerIPStr, elapsedTicks);
            MacTCPImpl_TCPAbort(entry->stream);
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
        }
    } else if (entry->state == TCP_STATE_SENDING && entry->sendStartTime > 0) {
        elapsedTicks = currentTime - entry->sendStartTime;
        if (elapsedTicks > TCP_STREAM_CONNECTION_TIMEOUT_TICKS) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Send timeout to %s (%lu ticks)",
                            poolIndex, entry->peerIPStr, elapsedTicks);
            MacTCPImpl_TCPAbort(entry->stream);
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
        }
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
        MacTCPImpl_AddressToString(remote_ip_from_status, remoteIPStrConnected);
    } else {
        strcpy(remoteIPStrConnected, "unknown_ip");
    }

    log_debug_cat(LOG_CAT_MESSAGING, "ProcessIncomingTCPData from %s:%u", remoteIPStrConnected, remote_port_from_status);

    for (int i = 0; rds[i].length > 0 || rds[i].ptr != NULL; ++i) {
        if (rds[i].length == 0 || rds[i].ptr == NULL) break;

        log_debug_cat(LOG_CAT_MESSAGING, "Processing RDS entry %d: Ptr 0x%lX, Len %u", i, (unsigned long)rds[i].ptr,
                      rds[i].length);

        csend_uint32_t msg_id;
        if (parse_message((const char *)rds[i].ptr, rds[i].length,
                          senderIPStrFromPayload, senderUsername, msgType, &msg_id, content) == 0) {
            log_debug_cat(LOG_CAT_MESSAGING,
                          "Parsed TCP message: ID %lu, Type '%s', FromUser '%s', FromIP(payload) '%s', Content(len %d) '%.30s...'",
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
    int i;
    int idleCount = 0;

    if (!gPoolInitialized) {
        return TCP_STATE_UNINITIALIZED;
    }

    /* Count IDLE entries - if all IDLE, return IDLE, otherwise return BUSY state */
    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].state == TCP_STATE_IDLE) {
            idleCount++;
        }
    }

    /* If all idle, report IDLE, otherwise report that we have active connections */
    if (idleCount == TCP_SEND_STREAM_POOL_SIZE) {
        return TCP_STATE_IDLE;
    } else {
        /* Return a generic "busy" state */
        return TCP_STATE_CONNECTING_OUT;
    }
}

/* Start an async send operation on a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-18 "TCPActiveOpen" - async connection
 */
static OSErr StartAsyncSendOnPoolEntry(int poolIndex, const char *peerIPStr,
                                       const char *message_content, const char *msg_type)
{
    TCPSendStreamPoolEntry *entry;
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;

    if (!gPoolInitialized) return notOpenErr;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return paramErr;

    entry = &gSendStreamPool[poolIndex];

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: StartAsyncSend: Request to send '%s' to %s",
                  poolIndex, msg_type, peerIPStr);

    /* Validate parameters */
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (entry->stream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL) return paramErr;

    /* Must be in IDLE state */
    if (entry->state != TCP_STATE_IDLE) {
        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Stream not idle (state %d)", poolIndex, entry->state);
        return connectionExists;
    }

    /* Parse target IP */
    if (strcmp(peerIPStr, gMyLocalIPStr) == 0) {
        targetIP = gMyLocalIP;
    } else {
        err = ParseIPv4(peerIPStr, &targetIP);
        if (err != noErr) {
            log_app_event("Pool[%d]: Invalid IP address %s", poolIndex, peerIPStr);
            return err;
        }
    }

    /* Format message */
    formattedLen = format_message(messageBuffer, sizeof(messageBuffer), msg_type,
                                  generate_message_id(), gMyUsername, gMyLocalIPStr, message_content);
    if (formattedLen < 0) {
        log_app_event("Pool[%d]: format_message failed for type '%s'", poolIndex, msg_type);
        return paramErr;
    }

    /* Store operation info in pool entry */
    strncpy(entry->peerIPStr, peerIPStr, INET_ADDRSTRLEN - 1);
    entry->peerIPStr[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(entry->message, messageBuffer, BUFFER_SIZE - 1);
    entry->message[BUFFER_SIZE - 1] = '\0';
    strncpy(entry->msgType, msg_type, 31);
    entry->msgType[31] = '\0';
    entry->targetIP = targetIP;
    entry->targetPort = PORT_TCP;
    entry->connectStartTime = TickCount();

    /* Start async connect */
    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Starting async connection to %s:%u...",
                  poolIndex, peerIPStr, PORT_TCP);
    entry->state = TCP_STATE_CONNECTING_OUT;

    err = MacTCPImpl_TCPConnectAsync(entry->stream, targetIP, PORT_TCP, &entry->connectHandle);
    if (err != noErr) {
        log_app_event("Pool[%d]: Async connection to %s failed to start: %d", poolIndex, peerIPStr, err);
        entry->state = TCP_STATE_IDLE;
        entry->connectHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Async connect initiated", poolIndex);
    return noErr;
}

