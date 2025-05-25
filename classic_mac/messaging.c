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

/* Change from StreamPtr to NetworkStreamRef */
static NetworkStreamRef gTCPStream = NULL;
static Ptr gTCPStreamRcvBuffer = NULL;
static unsigned long gTCPStreamRcvBufferSize = 0;
static TCPStreamState gTCPState = TCP_STATE_UNINITIALIZED;

/* Keep ASR event handling */
static volatile ASR_Event_Info gAsrEvent;
static wdsEntry gNoCopyRDS[MAX_RDS_ENTRIES + 1];
static Boolean gNoCopyRdsPendingReturn = false;

/* For async operations we still need the PB for polling */
static TCPiopb gAsyncPB;
static Boolean gAsyncOperationInProgress = false;

/* Message queue support */
static QueuedMessage gMessageQueue[MAX_QUEUED_MESSAGES];
static int gQueueHead = 0;
static int gQueueTail = 0;

volatile Boolean gGracefulActiveCloseTerminating = false;
volatile unsigned long gDuplicateSocketRetryDelayStartTicks = 0;
volatile unsigned long gPostAbortCooldownStartTicks = 0;

/* Timeout constants remain the same */
#define TCP_ULP_TIMEOUT_DEFAULT_S 20
#define TCP_CONNECT_ULP_TIMEOUT_S 10
#define TCP_SEND_ULP_TIMEOUT_S 10
#define TCP_CLOSE_ULP_TIMEOUT_S 5
#define TCP_PASSIVE_OPEN_CMD_TIMEOUT_S 0
#define TCP_RECEIVE_CMD_TIMEOUT_S 1
#define APP_POLL_TIMEOUT_TICKS 6
#define kErrorRetryDelayTicks 120
#define kDuplicateSocketRetryDelayTicks 60
#define kPostAbortCooldownDelayTicks 45

/* Forward declarations */
static void StartPassiveListen(void);
static void HandleASREvents(GiveTimePtr giveTime);
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

static void mac_tcp_display_text_message_callback(const char *username, const char *ip, 
                                                  const char *message_content, void *platform_context)
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
        return false; // Queue full
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
        return false; // Queue empty
    }
    
    *msg = gMessageQueue[gQueueHead];
    gMessageQueue[gQueueHead].inUse = false;
    gQueueHead = (gQueueHead + 1) % MAX_QUEUED_MESSAGES;
    return true;
}

static void ProcessMessageQueue(GiveTimePtr giveTime)
{
    QueuedMessage msg;
    
    if (gTCPState == TCP_STATE_IDLE && !gAsyncOperationInProgress) {
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

OSErr MacTCP_QueueMessage(const char *peerIPStr,
                          const char *message_content,
                          const char *msg_type)
{
    if (peerIPStr == NULL || msg_type == NULL) {
        return paramErr;
    }
    
    /* Try to send immediately if possible */
    if (gTCPState == TCP_STATE_IDLE || gTCPState == TCP_STATE_LISTENING) {
        log_debug("MacTCP_QueueMessage: Attempting immediate send to %s", peerIPStr);
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

pascal void TCP_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, 
                           unsigned short terminReason, struct ICMPReport *icmpMsg)
{
#pragma unused(userDataPtr)
    
    /* Compare the StreamPtr with our stored NetworkStreamRef cast to StreamPtr */
    if (gTCPStream == NULL || tcpStream != (StreamPtr)gTCPStream) {
        return;
    }
    
    if (gAsrEvent.eventPending) {
        /* Already have pending event - could log this but don't overwrite */
        return;
    }
    
    gAsrEvent.eventCode = (TCPEventCode)eventCode;
    gAsrEvent.termReason = terminReason;
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        BlockMoveData(icmpMsg, &gAsrEvent.icmpReport, sizeof(ICMPReport));
    } else {
        memset(&gAsrEvent.icmpReport, 0, sizeof(ICMPReport));
    }
    gAsrEvent.eventPending = true;
}

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP asrNotifyUPP)
{
    OSErr err;
    
    log_debug("Initializing TCP Messaging Subsystem using network abstraction...");
    
    if (!gNetworkOps) {
        log_app_event("Error: Network abstraction not initialized");
        return notOpenErr;
    }
    
    if (gTCPState != TCP_STATE_UNINITIALIZED) {
        log_debug("InitTCP: Already initialized or in invalid state: %d", gTCPState);
        return streamAlreadyOpen;
    }
    
    if (macTCPRefNum == 0) return paramErr;
    if (asrNotifyUPP == NULL) {
        log_debug("InitTCP: ASR UPP is NULL. Cannot proceed.");
        return paramErr;
    }
    
    gTCPStreamRcvBufferSize = streamReceiveBufferSize;
    gTCPStreamRcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);
    if (gTCPStreamRcvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate TCP stream receive buffer (%lu bytes).", 
                     gTCPStreamRcvBufferSize);
        gTCPStreamRcvBufferSize = 0;
        return memFullErr;
    }
    
    log_debug("Allocated TCP stream receive buffer: %lu bytes at 0x%lX", 
              gTCPStreamRcvBufferSize, (unsigned long)gTCPStreamRcvBuffer);
    
    /* Create TCP stream using abstraction - pass the ASR UPP directly */
    err = gNetworkOps->TCPCreate(macTCPRefNum, &gTCPStream, gTCPStreamRcvBufferSize, 
                                gTCPStreamRcvBuffer, (NetworkNotifyProcPtr)asrNotifyUPP);
    if (err != noErr || gTCPStream == NULL) {
        log_app_event("Error: Failed to create TCP Stream: %d", err);
        if (gTCPStreamRcvBuffer) DisposePtr(gTCPStreamRcvBuffer);
        gTCPStreamRcvBuffer = NULL;
        gTCPStreamRcvBufferSize = 0;
        gTCPState = TCP_STATE_ERROR;
        return err;
    }
    
    log_debug("TCP Stream created successfully using network abstraction.");
    
    /* Initialize message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;
    
    gTCPState = TCP_STATE_IDLE;
    gAsyncOperationInProgress = false;
    gNoCopyRdsPendingReturn = false;
    gGracefulActiveCloseTerminating = false;
    gDuplicateSocketRetryDelayStartTicks = 0;
    gPostAbortCooldownStartTicks = 0;
    memset((Ptr)&gAsrEvent, 0, sizeof(ASR_Event_Info));
    
    StartPassiveListen();
    
    log_debug("TCP Messaging Subsystem initialized. State: IDLE. Listening initiated.");
    return noErr;
}

void CleanupTCP(short macTCPRefNum)
{
    log_debug("Cleaning up TCP Messaging Subsystem (State: %d)...", gTCPState);
    
    if (!gNetworkOps) {
        log_debug("Network abstraction not available during cleanup");
        return;
    }
    
    gTCPState = TCP_STATE_RELEASING;
    
    /* Clear message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;
    
    if (gAsyncOperationInProgress && gTCPStream != NULL) {
        log_debug("Async operation was in progress. Attempting to abort connection.");
        if (gNetworkOps->TCPAbort) {
            gNetworkOps->TCPAbort(gTCPStream);
        }
        gAsyncOperationInProgress = false;
    }
    
    if (gNoCopyRdsPendingReturn && gTCPStream != NULL) {
        log_debug("RDS Buffers were pending return. Attempting return.");
        if (gNetworkOps->TCPReturnBuffer) {
            gNetworkOps->TCPReturnBuffer(gTCPStream, (Ptr)gNoCopyRDS, YieldTimeToSystem);
        }
        gNoCopyRdsPendingReturn = false;
    }
    
    if (gTCPStream != NULL) {
        log_debug("Releasing TCP Stream...");
        if (gNetworkOps->TCPRelease) {
            gNetworkOps->TCPRelease(macTCPRefNum, gTCPStream);
        }
        gTCPStream = NULL;
    }
    
    if (gTCPStreamRcvBuffer != NULL) {
        log_debug("Disposing TCP stream receive buffer.");
        DisposePtr(gTCPStreamRcvBuffer);
        gTCPStreamRcvBuffer = NULL;
        gTCPStreamRcvBufferSize = 0;
    }
    
    memset((Ptr)&gAsrEvent, 0, sizeof(ASR_Event_Info));
    gGracefulActiveCloseTerminating = false;
    gTCPState = TCP_STATE_UNINITIALIZED;
    
    log_debug("TCP Messaging Subsystem cleanup finished.");
}

static void StartPassiveListen(void)
{
    OSErr err;
    
    if (!gNetworkOps) return;
    
    if (gTCPState != TCP_STATE_IDLE) {
        log_debug("StartPassiveListen: Cannot listen, current state is %d (not IDLE).", gTCPState);
        return;
    }
    
    if (gTCPStream == NULL) {
        log_debug("CRITICAL (StartPassiveListen): Stream is NULL. Cannot listen.");
        gTCPState = TCP_STATE_ERROR;
        return;
    }
    
    if (gAsyncOperationInProgress) {
        log_debug("StartPassiveListen: Another async operation is already in progress.");
        return;
    }
    
    log_debug("Attempting asynchronous TCPPassiveOpen on port %u...", PORT_TCP);
    
    /* For async passive open, we need to use PB directly for now */
    /* TODO: Add better async support to abstraction layer */
    memset(&gAsyncPB, 0, sizeof(TCPiopb));
    gAsyncPB.ioCompletion = nil;
    gAsyncPB.ioCRefNum = gMacTCPRefNum;
    gAsyncPB.csCode = TCPPassiveOpen;
    gAsyncPB.tcpStream = (StreamPtr)gTCPStream;  /* Cast for now */
    gAsyncPB.csParam.open.ulpTimeoutValue = TCP_ULP_TIMEOUT_DEFAULT_S;
    gAsyncPB.csParam.open.ulpTimeoutAction = 1;
    gAsyncPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    gAsyncPB.csParam.open.commandTimeoutValue = TCP_PASSIVE_OPEN_CMD_TIMEOUT_S;
    gAsyncPB.csParam.open.localPort = PORT_TCP;
    gAsyncPB.csParam.open.localHost = 0L;
    gAsyncPB.csParam.open.remoteHost = 0L;
    gAsyncPB.csParam.open.remotePort = 0;
    gAsyncPB.ioResult = 1;
    
    err = PBControlAsync((ParmBlkPtr)&gAsyncPB);
    
    if (err == noErr) {
        log_debug("TCPPassiveOpenAsync successfully initiated.");
        gTCPState = TCP_STATE_LISTENING;
        gAsyncOperationInProgress = true;
    } else {
        log_app_event("Error: TCPPassiveOpenAsync failed to LAUNCH: %d.", err);
        gTCPState = TCP_STATE_IDLE;
    }
}

void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    OSErr err;
    
    if (!gNetworkOps) return;
    if (gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_RELEASING) {
        return;
    }
    
    HandleASREvents(giveTime);
    
    /* Process message queue when appropriate */
    ProcessMessageQueue(giveTime);
    
    switch (gTCPState) {
    case TCP_STATE_IDLE:
        StartPassiveListen();
        break;
        
    case TCP_STATE_LISTENING:
        /* For async operations, we still need to check the PB directly */
        if (gAsyncOperationInProgress && gAsyncPB.ioResult != 1) {
            gAsyncOperationInProgress = false;
            err = gAsyncPB.ioResult;
            
            if (err == noErr) {
                ip_addr remote_ip = gAsyncPB.csParam.open.remoteHost;
                tcp_port remote_port = gAsyncPB.csParam.open.remotePort;
                char ipStr[INET_ADDRSTRLEN];
                
                if (gNetworkOps->AddressToString) {
                    gNetworkOps->AddressToString(remote_ip, ipStr);
                } else {
                    sprintf(ipStr, "%lu.%lu.%lu.%lu",
                            (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                            (remote_ip >> 8) & 0xFF, remote_ip & 0xFF);
                }
                
                log_app_event("Incoming TCP connection established from %s:%u.", ipStr, remote_port);
                gTCPState = TCP_STATE_CONNECTED;
            } else {
                log_app_event("TCPPassiveOpenAsync FAILED: %d.", err);
                
                if (err == duplicateSocket || err == connectionExists) {
                    log_debug("Passive Open failed (%d). Will retry listen after delay.", err);
                    gTCPState = TCP_STATE_RETRY_LISTEN_DELAY;
                    gDuplicateSocketRetryDelayStartTicks = TickCount();
                } else {
                    log_debug("Passive Open failed with unhandled error %d.", err);
                    /* For other errors, abort and try to recover immediately */
                    if (gNetworkOps->TCPAbort) {
                        gNetworkOps->TCPAbort(gTCPStream);
                    }
                    gTCPState = TCP_STATE_IDLE;
                    /* Don't add delay for non-duplicate socket errors */
                }
            }
        }
        break;
        
    case TCP_STATE_RETRY_LISTEN_DELAY:
        if ((TickCount() - gDuplicateSocketRetryDelayStartTicks) >= kDuplicateSocketRetryDelayTicks) {
            log_debug("Retry delay elapsed. Setting state to IDLE.");
            gTCPState = TCP_STATE_IDLE;
            gDuplicateSocketRetryDelayStartTicks = 0;
            
            // Before retrying, ensure the stream is in a clean state
            if (gAsyncOperationInProgress) {
                log_debug("Clearing stale async operation flag before retry");
                gAsyncOperationInProgress = false;
            }
            
            // If we've been getting repeated -23007 errors, try a full stream reset
            static int consecutiveConnectionExistsErrors = 0;
            if (gAsyncPB.ioResult == connectionExists || gAsyncPB.ioResult == duplicateSocket) {
                consecutiveConnectionExistsErrors++;
                if (consecutiveConnectionExistsErrors >= 3) {
                    log_debug("Too many consecutive connection errors. Attempting stream reset.");
                    
                    // Abort the stream to force cleanup
                    if (gNetworkOps->TCPAbort) {
                        gNetworkOps->TCPAbort(gTCPStream);
                    }
                    
                    // Wait longer for cleanup
                    unsigned long resetDelay = TickCount();
                    while ((TickCount() - resetDelay) < 300) { // 5 seconds
                        if (giveTime) giveTime();
                    }
                    
                    consecutiveConnectionExistsErrors = 0;
                }
            } else {
                consecutiveConnectionExistsErrors = 0;
            }
        }
        break;
        
    case TCP_STATE_POST_ABORT_COOLDOWN:
        if ((TickCount() - gPostAbortCooldownStartTicks) >= kPostAbortCooldownDelayTicks) {
            log_debug("Post-abort cooldown elapsed. Setting state to IDLE.");
            gTCPState = TCP_STATE_IDLE;
            gPostAbortCooldownStartTicks = 0;
        }
        break;
        
    case TCP_STATE_CONNECTED:
        /* Connected state - data handling done via ASR */
        break;
        
    case TCP_STATE_ERROR:
        log_debug("ProcessTCPStateMachine: In TCP_STATE_ERROR.");
        break;
        
    default:
        if (gTCPState != TCP_STATE_CONNECTING_OUT &&
            gTCPState != TCP_STATE_SENDING &&
            gTCPState != TCP_STATE_CLOSING_GRACEFUL &&
            gTCPState != TCP_STATE_ABORTING) {
            log_debug("ProcessTCPStateMachine: In unexpected TCP state %d.", gTCPState);
            gTCPState = TCP_STATE_ERROR;
        }
        break;
    }
    
    if (giveTime) giveTime();
}

static void HandleASREvents(GiveTimePtr giveTime)
{
    if (!gNetworkOps) return;
    if (!gAsrEvent.eventPending) return;
    
    ASR_Event_Info currentEvent = gAsrEvent;
    gAsrEvent.eventPending = false;
    
    log_debug("ASR Event Received: Code %u, Reason %u (State: %d). gGracefulActiveCloseTerminating: %s",
              currentEvent.eventCode, currentEvent.termReason, gTCPState,
              gGracefulActiveCloseTerminating ? "true" : "false");
    
    switch (currentEvent.eventCode) {
    case TCPDataArrival:
        log_debug("ASR: TCPDataArrival on stream 0x%lX.", (unsigned long)gTCPStream);
        
        if (gTCPState == TCP_STATE_CONNECTED || gTCPState == TCP_STATE_LISTENING) {
            if (gNoCopyRdsPendingReturn) {
                log_app_event("ASR: TCPDataArrival while RDS buffers still pending return! Attempting forced return now.");
                gNetworkOps->TCPReturnBuffer(gTCPStream, (Ptr)gNoCopyRDS, giveTime);
                gNoCopyRdsPendingReturn = false;
            }
            
            /* Get connection info */
            NetworkTCPInfo tcpInfo;
            if (gNetworkOps->TCPStatus(gTCPStream, &tcpInfo) != noErr) {
                log_debug("ASR: TCPDataArrival, but GetStatus failed. Connection might be gone.");
                if (gTCPState == TCP_STATE_CONNECTED) {
                    gNetworkOps->TCPAbort(gTCPStream);
                    gTCPState = TCP_STATE_IDLE;
                }
                break;
            }
            
            /* Receive data */
            Boolean urgentFlag, markFlag;
            OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPStream, (Ptr)gNoCopyRDS, 
                                                         MAX_RDS_ENTRIES, TCP_RECEIVE_CMD_TIMEOUT_S,
                                                         &urgentFlag, &markFlag, giveTime);
            
            if (rcvErr == noErr) {
                log_debug("TCPNoCopyRcv successful. Processing data.");
                if (gNoCopyRDS[0].length > 0 || gNoCopyRDS[0].ptr != NULL) {
                    ProcessIncomingTCPData(gNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
                    gNoCopyRdsPendingReturn = true;
                    
                    OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPStream, (Ptr)gNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gNoCopyRdsPendingReturn = false;
                    } else {
                        log_app_event("CRITICAL: TCPBfrReturn FAILED: %d after NoCopyRcv. Stream integrity compromised.", bfrReturnErr);
                        gTCPState = TCP_STATE_ERROR;
                        gNetworkOps->TCPAbort(gTCPStream);
                    }
                } else {
                    log_debug("TCPNoCopyRcv returned noErr but no data in RDS[0] (or NULL ptr).");
                }
            } else if (rcvErr == commandTimeout) {
                log_debug("TCPNoCopyRcv timed out. No data read this cycle despite DataArrival ASR.");
            } else if (rcvErr == connectionClosing) {
                log_app_event("TCPNoCopyRcv: Connection is closing by peer (rcvErr %d). Current state %d. Aborting.", rcvErr, gTCPState);
                gNetworkOps->TCPAbort(gTCPStream);
                gTCPState = TCP_STATE_POST_ABORT_COOLDOWN;
                gPostAbortCooldownStartTicks = TickCount();
                if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
            } else {
                log_app_event("Error during TCPNoCopyRcv: %d. Aborting connection.", rcvErr);
                gNetworkOps->TCPAbort(gTCPStream);
                gTCPState = TCP_STATE_IDLE;
                if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
            }
        } else {
            log_debug("ASR: TCPDataArrival received in unexpected state %d. Ignoring.", gTCPState);
        }
        break;
        
    case TCPTerminate:
        {
            char ipStr[INET_ADDRSTRLEN] = "N/A";
            NetworkTCPInfo tcpInfo;
            
            // Try to get connection info, but don't fail if we can't
            if (gTCPStream != NULL && gNetworkOps->TCPStatus(gTCPStream, &tcpInfo) == noErr) {
                if (tcpInfo.remoteHost != 0 && gNetworkOps->AddressToString) {
                    gNetworkOps->AddressToString(tcpInfo.remoteHost, ipStr);
                }
            }
            
            log_app_event("ASR: TCPTerminate for peer %s. Reason: %u. State: %d.", 
                        ipStr, currentEvent.termReason, gTCPState);
            
            // Clean up any pending operations
            if (gNoCopyRdsPendingReturn) {
                log_debug("ASR: Returning pending RDS buffers");
                gNetworkOps->TCPReturnBuffer(gTCPStream, (Ptr)gNoCopyRDS, giveTime);
                gNoCopyRdsPendingReturn = false;
            }
            
            // Clear async operation flag
            if (gAsyncOperationInProgress) {
                gAsyncOperationInProgress = false;
            }
            
            // Handle expected vs unexpected termination
            Boolean isExpectedTermination = 
                (gGracefulActiveCloseTerminating && 
                (currentEvent.termReason == 7 || currentEvent.termReason == TCPULPClose));
            
            if (isExpectedTermination) {
                log_debug("ASR: Expected termination after active close");
                gGracefulActiveCloseTerminating = false;
            }
            
            // Set appropriate state
            switch (gTCPState) {
                case TCP_STATE_LISTENING:
                case TCP_STATE_RETRY_LISTEN_DELAY:
                case TCP_STATE_POST_ABORT_COOLDOWN:
                    // Keep current state
                    break;
                default:
                    gTCPState = TCP_STATE_IDLE;
                    break;
            }
        }
        break;
        
    case TCPClosing:
        log_app_event("ASR: TCPClosing - Remote peer closed its send side. Current state: %d", gTCPState);
        if (gTCPState == TCP_STATE_CONNECTED || 
            (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == noErr)) {
            log_debug("Remote peer initiated close. Aborting our side and entering cooldown.");
            gNetworkOps->TCPAbort(gTCPStream);
            if (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == noErr) {
                gAsyncOperationInProgress = false;
            }
            gTCPState = TCP_STATE_POST_ABORT_COOLDOWN;
            gPostAbortCooldownStartTicks = TickCount();
        } else if (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == 1) {
            log_app_event("ASR: TCPClosing while PassiveOpen still pending. Aborting and going to IDLE.");
            gNetworkOps->TCPAbort(gTCPStream);
            gAsyncOperationInProgress = false;
            gTCPState = TCP_STATE_IDLE;
        }
        break;
        
    case TCPULPTimeout:
        log_app_event("ASR: TCPULPTimeout. Current state: %d", gTCPState);
        gNetworkOps->TCPAbort(gTCPStream);
        gTCPState = TCP_STATE_IDLE;
        if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
        gGracefulActiveCloseTerminating = false;
        break;
        
    case TCPUrgent:
        log_app_event("ASR: TCPUrgent data notification. Current state: %d", gTCPState);
        break;
        
    case TCPICMPReceived:
        {
            char localHostStr[INET_ADDRSTRLEN], remoteHostStr[INET_ADDRSTRLEN];
            if (gNetworkOps->AddressToString) {
                gNetworkOps->AddressToString(currentEvent.icmpReport.localHost, localHostStr);
                gNetworkOps->AddressToString(currentEvent.icmpReport.remoteHost, remoteHostStr);
            } else {
                sprintf(localHostStr, "0x%lX", (unsigned long)currentEvent.icmpReport.localHost);
                sprintf(remoteHostStr, "0x%lX", (unsigned long)currentEvent.icmpReport.remoteHost);
            }
            log_app_event("ASR: TCPICMPRecvd. Type %u, Code %u. Stream L(%s:%u) R(%s:%u). MoreInfo 0x%lX",
                          (unsigned short)currentEvent.icmpReport.reportType,
                          currentEvent.icmpReport.optionalAddlInfo,
                          localHostStr, currentEvent.icmpReport.localPort,
                          remoteHostStr, currentEvent.icmpReport.remotePort,
                          (unsigned long)currentEvent.icmpReport.optionalAddlInfoPtr);
        }
        break;
        
    default:
        log_debug("ASR: Unhandled event code %u.", currentEvent.eventCode);
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
        log_debug("ProcessIncomingTCPData: remote_ip_from_status is 0!");
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
                log_app_event("QUIT message processed from %s. Connection will be terminated by ASR or explicit close.", 
                             remoteIPStrConnected);
            }
        } else {
            log_debug("Failed to parse TCP message chunk from %s (length %u). Discarding.", 
                     remoteIPStrConnected, rds[i].length);
        }
    }
}

TCPStreamState GetTCPStreamState(void)
{
    return gTCPState;
}

OSErr MacTCP_SendMessageSync(const char *peerIPStr,
                             const char *message_content,
                             const char *msg_type,
                             const char *local_username,
                             const char *local_ip_str,
                             GiveTimePtr giveTime)
{
    OSErr err = noErr, finalErr = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;
    Boolean wasListeningAndAbortedForSend = false;
    unsigned long abortStartTime, currentTime;
    
    if (!gNetworkOps) return notOpenErr;
    
    log_debug("MacTCP_SendMessageSync: Request to send '%s' to %s (Current TCP State: %d)", 
              msg_type, peerIPStr, gTCPState);
    
    // Validate parameters
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL || local_username == NULL || 
        local_ip_str == NULL || giveTime == NULL) {
        return paramErr;
    }
    
    // Check if we're in a state that allows sending
    if (gTCPState != TCP_STATE_IDLE && gTCPState != TCP_STATE_LISTENING) {
        log_app_event("Error: Stream busy (state %d). Cannot send now.", gTCPState);
        return streamBusyErr;
    }
    
    // Handle listening state more gracefully
    if (gTCPState == TCP_STATE_LISTENING) {
        if (gAsyncOperationInProgress) {
            log_debug("Aborting passive open to allow outgoing connection...");
            
            // First, check if the async operation has completed
            if (gAsyncPB.ioResult != 1) {
                // Operation completed, update state
                gAsyncOperationInProgress = false;
                if (gAsyncPB.ioResult == noErr) {
                    // Connection accepted, can't send on this stream
                    log_app_event("Error: Incoming connection just established. Cannot send.");
                    return streamBusyErr;
                }
            }
            
            // Abort using network abstraction
            err = gNetworkOps->TCPAbort(gTCPStream);
            
            if (err == noErr || err == connectionDoesntExist) {
                log_debug("Passive open aborted successfully");
                gAsyncOperationInProgress = false;
                gTCPState = TCP_STATE_IDLE;
                wasListeningAndAbortedForSend = true;
                
                // Increase delay significantly to ensure MacTCP cleans up
                // MacTCP needs time to clean up connection state tables
                abortStartTime = TickCount();
                do {
                    giveTime();
                    currentTime = TickCount();
                } while ((currentTime - abortStartTime) < 120); // 2 seconds instead of 0.5
                
            } else {
                log_app_event("Failed to abort passive open: %d", err);
                return streamBusyErr;
            }
        } else {
            // Not async but in listening state - reset to idle
            gTCPState = TCP_STATE_IDLE;
            wasListeningAndAbortedForSend = true;
        }
    }
    
    // Ensure we're now in IDLE state
    if (gTCPState != TCP_STATE_IDLE) {
        log_app_event("Error: Failed to reach IDLE state. Cannot send.");
        return streamBusyErr;
    }
    
    // Parse target IP
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_app_event("Error: Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendMessageDone;
    }
    
    // Format message
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, msg_type, local_username, 
                                 local_ip_str, message_content ? message_content : "");
    if (formattedLen <= 0) {
        log_app_event("Error: format_message failed for type '%s'.", msg_type);
        finalErr = paramErr;
        goto SendMessageDone;
    }
    
    // Connect to peer
    log_debug("Attempting connection to %s:%u...", peerIPStr, PORT_TCP);
    gTCPState = TCP_STATE_CONNECTING_OUT;
    gGracefulActiveCloseTerminating = false;
    
    err = gNetworkOps->TCPConnect(gTCPStream, targetIP, PORT_TCP, TCP_CONNECT_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error: Connection to %s failed: %d", peerIPStr, err);
        finalErr = err;
        gTCPState = TCP_STATE_IDLE;
        goto SendMessageDone;
    }
    
    log_debug("Connected to %s", peerIPStr);
    gTCPState = TCP_STATE_CONNECTED;
    
    // Send message
    log_debug("Sending %u bytes...", formattedLen - 1);
    gTCPState = TCP_STATE_SENDING;
    
    err = gNetworkOps->TCPSend(gTCPStream, (Ptr)messageBuffer, formattedLen - 1, 
                               true, TCP_SEND_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error: Send to %s failed: %d", peerIPStr, err);
        finalErr = err;
        gNetworkOps->TCPAbort(gTCPStream);
        gTCPState = TCP_STATE_IDLE;
        goto SendMessageDone;
    }
    
    log_debug("Message sent successfully");
    
    // Close connection
    if (strcmp(msg_type, MSG_QUIT) == 0) {
        log_debug("Sending QUIT - using abort for immediate close");
        gNetworkOps->TCPAbort(gTCPStream);
    } else {
        log_debug("Attempting graceful close...");
        gTCPState = TCP_STATE_CLOSING_GRACEFUL;
        err = gNetworkOps->TCPClose(gTCPStream, TCP_CLOSE_ULP_TIMEOUT_S, giveTime);
        if (err != noErr) {
            log_debug("Graceful close failed (%d), using abort", err);
            gNetworkOps->TCPAbort(gTCPStream);
        } else {
            gGracefulActiveCloseTerminating = true;
        }
    }
    
    gTCPState = TCP_STATE_IDLE;
    
SendMessageDone:
    // Restart listening if appropriate
    if (gTCPState == TCP_STATE_IDLE && !gAsyncOperationInProgress) {
        // Add a longer delay before restarting listen, especially after errors
        if (wasListeningAndAbortedForSend || finalErr != noErr) {
            unsigned long delayStart = TickCount();
            unsigned long delayTicks = 60; // 1 second default
            
            // If we had a connection error, wait longer
            if (finalErr == connectionExists || finalErr == duplicateSocket) {
                delayTicks = 180; // 3 seconds for connection-related errors
                log_debug("Connection error %d - using extended delay before restarting listen", finalErr);
            }
            
            while ((TickCount() - delayStart) < delayTicks) {
                giveTime();
            }
        }
        
        log_debug("Restarting passive listen...");
        StartPassiveListen();
    }
    
    log_debug("MacTCP_SendMessageSync complete. Status: %d, State: %d", 
              finalErr, gTCPState);
    return finalErr;
}