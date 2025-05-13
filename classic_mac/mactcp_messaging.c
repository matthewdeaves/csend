#include "mactcp_messaging.h"
#include "logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "peer.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_messages.h"
#include "mactcp_network.h"
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
static StreamPtr gTCPStream = NULL;
static Ptr gTCPStreamRcvBuffer = NULL;
static unsigned long gTCPStreamRcvBufferSize = 0;
static TCPStreamState gTCPState = TCP_STATE_UNINITIALIZED;
static TCPNotifyUPP gStoredAsrUPP = NULL;
static volatile ASR_Event_Info gAsrEvent;
static wdsEntry gNoCopyRDS[MAX_RDS_ENTRIES + 1];
static Boolean gNoCopyRdsPendingReturn = false;
static TCPiopb gAsyncPB;
static Boolean gAsyncOperationInProgress = false;
volatile Boolean gGracefulActiveCloseTerminating = false;
volatile unsigned long gDuplicateSocketRetryDelayStartTicks = 0;
volatile unsigned long gPostAbortCooldownStartTicks = 0;
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
static OSErr LowLevelAsync(TCPiopb *pBlockTemplate, SInt16 csCode);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt16 appPollTimeoutTicks);
static OSErr MacTCP_CreateStream(short macTCPRefNum, unsigned long rcvBuffSize, Ptr rcvBuff, TCPNotifyUPP asrProc, StreamPtr *streamPtrOut);
static OSErr MacTCP_ReleaseStream(short macTCPRefNum, StreamPtr streamToRelease);
static OSErr MacTCP_PassiveOpenAsync(StreamPtr stream, tcp_port localPort, Byte commandTimeoutSec);
static OSErr MacTCP_ActiveOpenSync(StreamPtr stream, ip_addr remoteHost, tcp_port remotePort, Byte ulpTimeoutSec, GiveTimePtr giveTime);
static OSErr MacTCP_SendSync(StreamPtr stream, Ptr wdsPtr, Boolean pushFlag, Byte ulpTimeoutSec, GiveTimePtr giveTime);
static OSErr MacTCP_NoCopyRcvSync(StreamPtr stream, wdsEntry rds[], short maxRDSEntries, Byte commandTimeoutSec, Boolean *urgentFlag, Boolean *markFlag, GiveTimePtr giveTime);
static OSErr MacTCP_BfrReturnSync(StreamPtr stream, wdsEntry rds[], GiveTimePtr giveTime);
static OSErr MacTCP_CloseGracefulSync(StreamPtr stream, Byte ulpTimeoutSec, GiveTimePtr giveTime);
static OSErr MacTCP_AbortConnection(StreamPtr stream);
static OSErr MacTCP_GetStatus(StreamPtr stream, struct TCPStatusPB *statusPBOut, GiveTimePtr giveTime);
static void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip, tcp_port remote_port);
static void HandleASREvents(GiveTimePtr giveTime);
static void StartPassiveListen(void);
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
pascal void TCP_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr, unsigned short terminReason, struct ICMPReport *icmpMsg)
{
#pragma unused(userDataPtr)
    if (tcpStream != gTCPStream) {
        return;
    }
    if (gAsrEvent.eventPending) {
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
    log_debug("Initializing TCP Messaging Subsystem...");
    if (gTCPState != TCP_STATE_UNINITIALIZED) {
        log_debug("InitTCP: Already initialized or in invalid state: %d", gTCPState);
        return streamAlreadyOpen;
    }
    if (macTCPRefNum == 0) return paramErr;
    if (asrNotifyUPP == NULL) {
        log_debug("InitTCP: ASR UPP is NULL. Cannot proceed.");
        return paramErr;
    }
    gStoredAsrUPP = asrNotifyUPP;
    gTCPStreamRcvBufferSize = streamReceiveBufferSize;
    gTCPStreamRcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);
    if (gTCPStreamRcvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate TCP stream receive buffer (%lu bytes).", gTCPStreamRcvBufferSize);
        gTCPStreamRcvBufferSize = 0;
        return memFullErr;
    }
    log_debug("Allocated TCP stream receive buffer: %lu bytes at 0x%lX", gTCPStreamRcvBufferSize, (unsigned long)gTCPStreamRcvBuffer);
    err = MacTCP_CreateStream(macTCPRefNum, gTCPStreamRcvBufferSize, gTCPStreamRcvBuffer, gStoredAsrUPP, &gTCPStream);
    if (err != noErr || gTCPStream == NULL) {
        log_app_event("Error: Failed to create TCP Stream: %d", err);
        if (gTCPStreamRcvBuffer) DisposePtr(gTCPStreamRcvBuffer);
        gTCPStreamRcvBuffer = NULL;
        gTCPStreamRcvBufferSize = 0;
        gStoredAsrUPP = NULL;
        gTCPState = TCP_STATE_ERROR;
        return err;
    }
    log_debug("TCP Stream created successfully (0x%lX). Its pointer value might be same as buffer, check MacTCP specifics.", (unsigned long)gTCPStream);
    gTCPState = TCP_STATE_IDLE;
    gAsyncOperationInProgress = false;
    gNoCopyRdsPendingReturn = false;
    gGracefulActiveCloseTerminating = false;
    gDuplicateSocketRetryDelayStartTicks = 0;
    gPostAbortCooldownStartTicks = 0;
    memset((Ptr)&gAsrEvent, 0, sizeof(ASR_Event_Info));
    StartPassiveListen();
    log_debug("TCP Messaging Subsystem initialized. State: IDLE. Listening initiated (if successful).");
    return noErr;
}
void CleanupTCP(short macTCPRefNum)
{
    log_debug("Cleaning up TCP Messaging Subsystem (State: %d)...", gTCPState);
    gTCPState = TCP_STATE_RELEASING;
    if (gAsyncOperationInProgress && gTCPStream != NULL) {
        log_debug("Async operation was in progress. Attempting to abort connection on stream 0x%lX.", (unsigned long)gTCPStream);
        MacTCP_AbortConnection(gTCPStream);
        gAsyncOperationInProgress = false;
    }
    if (gNoCopyRdsPendingReturn && gTCPStream != NULL) {
        log_debug("RDS Buffers were pending return for stream 0x%lX. Attempting return.", (unsigned long)gTCPStream);
        MacTCP_BfrReturnSync(gTCPStream, gNoCopyRDS, YieldTimeToSystem);
        gNoCopyRdsPendingReturn = false;
    }
    if (gTCPStream != NULL) {
        log_debug("Releasing TCP Stream 0x%lX...", (unsigned long)gTCPStream);
        MacTCP_ReleaseStream(macTCPRefNum, gTCPStream);
        gTCPStream = NULL;
    }
    if (gTCPStreamRcvBuffer != NULL) {
        log_debug("Disposing TCP stream receive buffer at 0x%lX.", (unsigned long)gTCPStreamRcvBuffer);
        DisposePtr(gTCPStreamRcvBuffer);
        gTCPStreamRcvBuffer = NULL;
        gTCPStreamRcvBufferSize = 0;
    }
    if (gStoredAsrUPP != NULL) {
        gStoredAsrUPP = NULL;
    }
    memset((Ptr)&gAsrEvent, 0, sizeof(ASR_Event_Info));
    gGracefulActiveCloseTerminating = false;
    gTCPState = TCP_STATE_UNINITIALIZED;
    log_debug("TCP Messaging Subsystem cleanup finished.");
}
static void StartPassiveListen(void)
{
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
        log_debug("StartPassiveListen: Another async operation is already in progress. Listen attempt deferred.");
        return;
    }
    log_debug("Attempting asynchronous TCPPassiveOpen on port %u...", PORT_TCP);
    OSErr err = MacTCP_PassiveOpenAsync(gTCPStream, PORT_TCP, TCP_PASSIVE_OPEN_CMD_TIMEOUT_S);
    if (err == noErr) {
        log_debug("TCPPassiveOpenAsync successfully initiated.");
        gTCPState = TCP_STATE_LISTENING;
        gAsyncOperationInProgress = true;
    } else {
        log_app_event("Error: TCPPassiveOpenAsync failed to LAUNCH: %d. State returning to IDLE.", err);
        gTCPState = TCP_STATE_IDLE;
    }
}
void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    OSErr err;
    if (gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_RELEASING) {
        return;
    }
    HandleASREvents(giveTime);
    switch (gTCPState) {
    case TCP_STATE_IDLE:
        StartPassiveListen();
        break;
    case TCP_STATE_LISTENING:
        if (gAsyncOperationInProgress && gAsyncPB.ioResult != 1) {
            gAsyncOperationInProgress = false;
            err = gAsyncPB.ioResult;
            if (err == noErr) {
                ip_addr remote_ip = gAsyncPB.csParam.open.remoteHost;
                tcp_port remote_port = gAsyncPB.csParam.open.remotePort;
                char ipStr[INET_ADDRSTRLEN];
                AddrToStr(remote_ip, ipStr);
                log_app_event("Incoming TCP connection established from %s:%u.", ipStr, remote_port);
                gTCPState = TCP_STATE_CONNECTED;
            } else {
                log_app_event("TCPPassiveOpenAsync FAILED: %d.", err);
                if (err == duplicateSocket || err == connectionExists) {
                    log_debug("Passive Open failed (%d). Will retry listen after delay.", err);
                    gTCPState = TCP_STATE_RETRY_LISTEN_DELAY;
                    gDuplicateSocketRetryDelayStartTicks = TickCount();
                } else {
                    log_debug("Passive Open failed with unhandled error %d. Aborting stream and returning to IDLE.", err);
                    MacTCP_AbortConnection(gTCPStream);
                    gTCPState = TCP_STATE_IDLE;
                    unsigned long dummyTimer;
                    Delay(kErrorRetryDelayTicks, &dummyTimer);
                }
            }
        }
        break;
    case TCP_STATE_RETRY_LISTEN_DELAY:
        if ((TickCount() - gDuplicateSocketRetryDelayStartTicks) >= kDuplicateSocketRetryDelayTicks) {
            log_debug("Retry delay for duplicateSocketErr elapsed. Setting state to IDLE to re-attempt listen.");
            gTCPState = TCP_STATE_IDLE;
            gDuplicateSocketRetryDelayStartTicks = 0;
        }
        break;
    case TCP_STATE_POST_ABORT_COOLDOWN:
        if ((TickCount() - gPostAbortCooldownStartTicks) >= kPostAbortCooldownDelayTicks) {
            log_debug("Post-abort cooldown elapsed. Setting state to IDLE to allow re-listen.");
            gTCPState = TCP_STATE_IDLE;
            gPostAbortCooldownStartTicks = 0;
        }
        break;
    case TCP_STATE_CONNECTED:
        break;
    case TCP_STATE_ERROR:
        log_debug("ProcessTCPStateMachine: In TCP_STATE_ERROR. No automatic recovery implemented.");
        break;
    default:
        if (gTCPState != TCP_STATE_CONNECTING_OUT &&
                gTCPState != TCP_STATE_SENDING &&
                gTCPState != TCP_STATE_CLOSING_GRACEFUL &&
                gTCPState != TCP_STATE_ABORTING) {
            log_debug("ProcessTCPStateMachine: In unexpected TCP state %d. Forcing to ERROR.", gTCPState);
            gTCPState = TCP_STATE_ERROR;
        }
        break;
    }
    if (giveTime) giveTime();
}
static void HandleASREvents(GiveTimePtr giveTime)
{
    if (!gAsrEvent.eventPending) {
        return;
    }
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
                MacTCP_BfrReturnSync(gTCPStream, gNoCopyRDS, giveTime);
                gNoCopyRdsPendingReturn = false;
            }
            ip_addr peer_ip_from_status = 0;
            tcp_port peer_port_from_status = 0;
            struct TCPStatusPB statusPB;
            if (MacTCP_GetStatus(gTCPStream, &statusPB, giveTime) == noErr) {
                peer_ip_from_status = statusPB.remoteHost;
                peer_port_from_status = statusPB.remotePort;
            } else {
                log_debug("ASR: TCPDataArrival, but GetStatus failed. Connection might be gone.");
                if (gTCPState == TCP_STATE_CONNECTED) {
                    MacTCP_AbortConnection(gTCPStream);
                    gTCPState = TCP_STATE_IDLE;
                }
                break;
            }
            Boolean urgentFlag, markFlag;
            OSErr rcvErr = MacTCP_NoCopyRcvSync(gTCPStream, gNoCopyRDS, MAX_RDS_ENTRIES,
                                                TCP_RECEIVE_CMD_TIMEOUT_S,
                                                &urgentFlag, &markFlag, giveTime);
            if (rcvErr == noErr) {
                log_debug("TCPNoCopyRcv successful. Processing data.");
                if (gNoCopyRDS[0].length > 0 || gNoCopyRDS[0].ptr != NULL) {
                    ProcessIncomingTCPData(gNoCopyRDS, peer_ip_from_status, peer_port_from_status);
                    gNoCopyRdsPendingReturn = true;
                    OSErr bfrReturnErr = MacTCP_BfrReturnSync(gTCPStream, gNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gNoCopyRdsPendingReturn = false;
                    } else {
                        log_app_event("CRITICAL: TCPBfrReturn FAILED: %d after NoCopyRcv. Stream integrity compromised.", bfrReturnErr);
                        gTCPState = TCP_STATE_ERROR;
                        MacTCP_AbortConnection(gTCPStream);
                    }
                } else {
                    log_debug("TCPNoCopyRcv returned noErr but no data in RDS[0] (or NULL ptr).");
                }
            } else if (rcvErr == commandTimeout) {
                log_debug("TCPNoCopyRcv timed out. No data read this cycle despite DataArrival ASR.");
            } else if (rcvErr == connectionClosing) {
                log_app_event("TCPNoCopyRcv: Connection is closing by peer (rcvErr %d). Current state %d. Aborting.", rcvErr, gTCPState);
                MacTCP_AbortConnection(gTCPStream);
                gTCPState = TCP_STATE_POST_ABORT_COOLDOWN;
                gPostAbortCooldownStartTicks = TickCount();
                if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
            } else {
                log_app_event("Error during TCPNoCopyRcv: %d. Aborting connection.", rcvErr);
                MacTCP_AbortConnection(gTCPStream);
                gTCPState = TCP_STATE_IDLE;
                if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
            }
        } else {
            log_debug("ASR: TCPDataArrival received in unexpected state %d. Ignoring.", gTCPState);
        }
        break;
    case TCPTerminate: {
        char ipStr[INET_ADDRSTRLEN] = "N/A";
        struct TCPStatusPB statusPB;
        if (MacTCP_GetStatus(gTCPStream, &statusPB, giveTime) == noErr && statusPB.remoteHost != 0) {
            AddrToStr(statusPB.remoteHost, ipStr);
        }
        log_app_event("ASR: TCPTerminate for peer %s. Reason: %u. Current State: %d. gGracefulClose: %s",
                      ipStr, currentEvent.termReason, gTCPState, gGracefulActiveCloseTerminating ? "true" : "false");
        if (gNoCopyRdsPendingReturn) {
            log_debug("ASR (TCPTerminate): Returning pending RDS buffers.");
            MacTCP_BfrReturnSync(gTCPStream, gNoCopyRDS, giveTime);
            gNoCopyRdsPendingReturn = false;
        }
        Boolean isExpectedGracefulTermination = ((currentEvent.termReason == 7 || currentEvent.termReason == TCPULPClose) &&
                                                gGracefulActiveCloseTerminating);
        if (isExpectedGracefulTermination) {
            log_debug("ASR (TCPTerminate): Recognized as expected termination of a prior active connection.");
            gGracefulActiveCloseTerminating = false;
            if (gTCPState == TCP_STATE_LISTENING) {
                log_debug("ASR (TCPTerminate Graceful): Current state is LISTENING (asyncOp %s). No state change.", gAsyncOperationInProgress ? "true" : "false");
            } else {
                log_debug("ASR (TCPTerminate Graceful): Current state %d (not LISTENING). Setting to IDLE.", gTCPState);
                if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
                gTCPState = TCP_STATE_IDLE;
            }
        } else {
            log_debug("ASR (TCPTerminate): Unexpected termination. Previous state %d.", gTCPState);
            if (gAsyncOperationInProgress) {
                gAsyncOperationInProgress = false;
            }
            if (gTCPState != TCP_STATE_POST_ABORT_COOLDOWN) {
                gTCPState = TCP_STATE_IDLE;
            } else {
                log_debug("ASR (TCPTerminate): State is POST_ABORT_COOLDOWN. Letting state machine handle transition to IDLE.");
            }
        }
    }
    break;
    case TCPClosing:
        log_app_event("ASR: TCPClosing - Remote peer closed its send side. Current state: %d", gTCPState);
        if (gTCPState == TCP_STATE_CONNECTED || (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == noErr)) {
            log_debug("Remote peer initiated close. Aborting our side and entering cooldown.");
            MacTCP_AbortConnection(gTCPStream);
            if (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == noErr) {
                gAsyncOperationInProgress = false;
            }
            gTCPState = TCP_STATE_POST_ABORT_COOLDOWN;
            gPostAbortCooldownStartTicks = TickCount();
        } else if (gTCPState == TCP_STATE_LISTENING && gAsyncOperationInProgress && gAsyncPB.ioResult == 1) {
            log_app_event("ASR: TCPClosing while PassiveOpen still pending. Aborting and going to IDLE.");
            MacTCP_AbortConnection(gTCPStream);
            gAsyncOperationInProgress = false;
            gTCPState = TCP_STATE_IDLE;
        }
        break;
    case TCPULPTimeout:
        log_app_event("ASR: TCPULPTimeout. Current state: %d", gTCPState);
        MacTCP_AbortConnection(gTCPStream);
        gTCPState = TCP_STATE_IDLE;
        if (gAsyncOperationInProgress) gAsyncOperationInProgress = false;
        gGracefulActiveCloseTerminating = false;
        break;
    case TCPUrgent:
        log_app_event("ASR: TCPUrgent data notification. Current state: %d", gTCPState);
        break;
    case TCPICMPReceived: {
        char localHostStr[INET_ADDRSTRLEN], remoteHostStr[INET_ADDRSTRLEN];
        AddrToStr(currentEvent.icmpReport.localHost, localHostStr);
        AddrToStr(currentEvent.icmpReport.remoteHost, remoteHostStr);
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
        AddrToStr(remote_ip_from_status, remoteIPStrConnected);
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
                log_app_event("QUIT message processed from %s. Connection will be terminated by ASR or explicit close.", remoteIPStrConnected);
            }
        } else {
            log_debug("Failed to parse TCP message chunk from %s (length %u). Discarding.", remoteIPStrConnected, rds[i].length);
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
    wdsEntry sendWDS[2];
    Boolean wasListeningAndAbortedForSend = false;
    log_debug("MacTCP_SendMessageSync: Request to send '%s' to %s (Current TCP State: %d)", msg_type, peerIPStr, gTCPState);
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL || local_username == NULL || local_ip_str == NULL || giveTime == NULL) {
        return paramErr;
    }
    if (gTCPState != TCP_STATE_IDLE && gTCPState != TCP_STATE_LISTENING) {
        log_app_event("Error (SendMessage): Stream not IDLE or LISTENING (state %d) before connect. Cannot send now.", gTCPState);
        return streamBusyErr;
    }
    if (gTCPState == TCP_STATE_LISTENING) {
        if (gAsyncOperationInProgress) {
            log_debug("SendMessage: Aborting pending asynchronous PassiveOpen to allow send.");
            err = MacTCP_AbortConnection(gTCPStream);
            if (err == noErr || err == connectionDoesntExist || err == invalidStreamPtr) {
                log_debug("SendMessage: Abort of stream for pending passive open successful.");
                gAsyncOperationInProgress = false;
                gTCPState = TCP_STATE_IDLE;
                wasListeningAndAbortedForSend = true;
            } else {
                log_app_event("SendMessage: Abort of pending passive open FAILED: %d. Send cannot proceed.", err);
                gTCPState = TCP_STATE_ERROR;
                return (err == commandTimeout) ? streamBusyErr : err;
            }
        } else {
            log_debug("SendMessage: Was LISTENING but no async op. Resetting to IDLE.");
            gTCPState = TCP_STATE_IDLE;
            wasListeningAndAbortedForSend = true;
        }
    }
    if (gTCPState != TCP_STATE_IDLE) {
        log_app_event("Error (SendMessage): Stream failed to become IDLE (state %d) before connect. Cannot send now.", gTCPState);
        return streamBusyErr;
    }
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_app_event("Error (SendMessage): Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendMessageDone;
    }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, msg_type, local_username, local_ip_str, message_content ? message_content : "");
    if (formattedLen <= 0) {
        log_app_event("Error (SendMessage): format_message failed for type '%s'.", msg_type);
        finalErr = paramErr;
        goto SendMessageDone;
    }
    log_debug("SendMessage: Attempting TCPActiveOpen to %s:%u...", peerIPStr, PORT_TCP);
    gTCPState = TCP_STATE_CONNECTING_OUT;
    gGracefulActiveCloseTerminating = false;
    err = MacTCP_ActiveOpenSync(gTCPStream, targetIP, PORT_TCP, TCP_CONNECT_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error (SendMessage): TCPActiveOpen to %s failed: %d", peerIPStr, err);
        finalErr = err;
        gTCPState = TCP_STATE_IDLE;
        goto SendMessageDone;
    }
    log_debug("SendMessage: TCPActiveOpen successful to %s.", peerIPStr);
    gTCPState = TCP_STATE_CONNECTED;
    sendWDS[0].length = formattedLen - 1;
    sendWDS[0].ptr = (Ptr)messageBuffer;
    sendWDS[1].length = 0;
    sendWDS[1].ptr = NULL;
    log_debug("SendMessage: Attempting TCPSend (%u bytes, push=true)...", sendWDS[0].length);
    gTCPState = TCP_STATE_SENDING;
    err = MacTCP_SendSync(gTCPStream, (Ptr)sendWDS, true, TCP_SEND_ULP_TIMEOUT_S, giveTime);
    if (err != noErr) {
        log_app_event("Error (SendMessage): TCPSend to %s failed: %d", peerIPStr, err);
        finalErr = err;
        MacTCP_AbortConnection(gTCPStream);
        gTCPState = TCP_STATE_IDLE;
        goto SendMessageDone;
    }
    log_debug("SendMessage: TCPSend successful to %s.", peerIPStr);
    if (strcmp(msg_type, MSG_QUIT) == 0) {
        log_debug("SendMessage: Sending QUIT, using TCPAbort for immediate termination.");
        gTCPState = TCP_STATE_ABORTING;
        gGracefulActiveCloseTerminating = false;
        err = MacTCP_AbortConnection(gTCPStream);
        if (err != noErr && err != connectionDoesntExist && err != invalidStreamPtr) {
            log_app_event("Warning (SendMessage): TCPAbort after QUIT failed: %d", err);
            if (finalErr == noErr) finalErr = err;
        } else {
            log_debug("TCPAbort after QUIT successful or connection already gone.");
        }
    } else {
        log_debug("SendMessage: Attempting TCPCloseGraceful...");
        gTCPState = TCP_STATE_CLOSING_GRACEFUL;
        err = MacTCP_CloseGracefulSync(gTCPStream, TCP_CLOSE_ULP_TIMEOUT_S, giveTime);
        if (err != noErr) {
            log_app_event("Warning (SendMessage): TCPCloseGraceful to %s FAILED: %d. Aborting as fallback.", peerIPStr, err);
            if (finalErr == noErr) finalErr = err;
            gGracefulActiveCloseTerminating = false;
            MacTCP_AbortConnection(gTCPStream);
        } else {
            log_debug("SendMessage: TCPCloseGraceful successful. Expecting Terminate ASR.");
            gGracefulActiveCloseTerminating = true;
        }
    }
    gTCPState = TCP_STATE_IDLE;
SendMessageDone:
    if (gTCPState == TCP_STATE_IDLE && !gAsyncOperationInProgress) {
        if (wasListeningAndAbortedForSend) {
            log_debug("SendMessage: Send sequence complete, was listening, restarting passive listen.");
        } else {
            log_debug("SendMessage: Send sequence ended, stream is IDLE, attempting to ensure passive listen is active.");
        }
        StartPassiveListen();
    } else if (gTCPState != TCP_STATE_LISTENING && gTCPState != TCP_STATE_IDLE &&
               gTCPState != TCP_STATE_RETRY_LISTEN_DELAY && gTCPState != TCP_STATE_POST_ABORT_COOLDOWN) {
        log_app_event("Warning (SendMessage): Send sequence ended in unexpected state %d. Forcing IDLE and attempting listen.", gTCPState);
        gTCPState = TCP_STATE_IDLE;
        if (!gAsyncOperationInProgress) StartPassiveListen();
    }
    log_debug("MacTCP_SendMessageSync to %s for '%s': Complete. Final Status: %d. New TCP State: %d", peerIPStr, msg_type, finalErr, gTCPState);
    return finalErr;
}
static OSErr LowLevelAsync(TCPiopb *pBlockTemplate, SInt16 csCode)
{
    if (gTCPStream == NULL && csCode != TCPCreate) {
        log_debug("LowLevelAsync Error: gTCPStream is NULL for csCode %d.", csCode);
        return invalidStreamPtr;
    }
    if (gAsyncOperationInProgress) {
        log_debug("LowLevelAsync Error: Another async operation is already in progress for csCode %d.", csCode);
        return streamBusyErr;
    }
    BlockMoveData(pBlockTemplate, &gAsyncPB, sizeof(TCPiopb));
    gAsyncPB.ioCompletion = nil;
    gAsyncPB.ioCRefNum = gMacTCPRefNum;
    gAsyncPB.tcpStream = gTCPStream;
    gAsyncPB.csCode = csCode;
    gAsyncPB.ioResult = 1;
    OSErr err = PBControlAsync((ParmBlkPtr)&gAsyncPB);
    if (err != noErr) {
        log_debug("Error (LowLevelAsync %d): PBControlAsync failed to LAUNCH: %d", csCode, err);
        return err;
    }
    return noErr;
}
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt16 appPollTimeoutTicks)
{
    OSErr err;
    unsigned long startTime = TickCount();
    if (pBlock == NULL || giveTime == NULL) return paramErr;
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (csCode != TCPCreate && csCode != TCPRelease) {
        if (gTCPStream == NULL) {
            log_debug("Error (LowLevelSyncPoll %d): gTCPStream is NULL.", csCode);
            return invalidStreamPtr;
        }
        pBlock->tcpStream = gTCPStream;
    } else if (csCode == TCPRelease && pBlock->tcpStream == NULL) {
        log_debug("Error (LowLevelSyncPoll TCPRelease): pBlock->tcpStream for release is NULL.");
        return invalidStreamPtr;
    }
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = csCode;
    pBlock->ioResult = 1;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_debug("Error (LowLevelSyncPoll %d): PBControlAsync failed to LAUNCH: %d", csCode, err);
        return err;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
        if (appPollTimeoutTicks > 0 && (TickCount() - startTime) >= (unsigned long)appPollTimeoutTicks) {
            log_debug("LowLevelSyncPoll (%d): App-level poll timeout (%d ticks) reached for PB 0x%lX.", csCode, appPollTimeoutTicks, (unsigned long)pBlock);
            return commandTimeout;
        }
    }
    return pBlock->ioResult;
}
static OSErr MacTCP_CreateStream(short macTCPRefNum, unsigned long rcvBuffSize, Ptr rcvBuff, TCPNotifyUPP asrProc, StreamPtr *streamPtrOut)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.create.rcvBuff = rcvBuff;
    pb.csParam.create.rcvBuffLen = rcvBuffSize;
    pb.csParam.create.notifyProc = (TCPNotifyProcPtr)asrProc;
    pb.ioCompletion = nil;
    pb.ioCRefNum = macTCPRefNum;
    pb.csCode = TCPCreate;
    OSErr err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr) {
        *streamPtrOut = pb.tcpStream;
        if (*streamPtrOut == NULL) {
            log_debug("Error (MacTCP_CreateStream): PBControlSync ok but returned NULL stream.");
            err = ioErr;
        }
    } else {
        *streamPtrOut = NULL;
        log_debug("Error (MacTCP_CreateStream): PBControlSync FAILED: %d", err);
    }
    return err;
}
static OSErr MacTCP_ReleaseStream(short macTCPRefNum, StreamPtr streamToRelease)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = streamToRelease;
    OSErr err = LowLevelSyncPoll(&pb, YieldTimeToSystem, TCPRelease, APP_POLL_TIMEOUT_TICKS * 4);
    if (err == invalidStreamPtr) {
        log_debug("MacTCP_ReleaseStream: Stream 0x%lX already invalid/released (err %d). Considered OK.", (unsigned long)streamToRelease, err);
        return noErr;
    }
    if (err != noErr) {
        log_debug("MacTCP_ReleaseStream: LowLevelSyncPoll for TCPRelease on stream 0x%lX returned error %d.", (unsigned long)streamToRelease, err);
    }
    return err;
}
static OSErr MacTCP_PassiveOpenAsync(StreamPtr stream, tcp_port localPort, Byte commandTimeoutSec)
{
    TCPiopb pbTemplate;
    memset(&pbTemplate, 0, sizeof(TCPiopb));
    pbTemplate.csParam.open.ulpTimeoutValue = TCP_ULP_TIMEOUT_DEFAULT_S;
    pbTemplate.csParam.open.ulpTimeoutAction = 1;
    pbTemplate.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pbTemplate.csParam.open.commandTimeoutValue = commandTimeoutSec;
    pbTemplate.csParam.open.localPort = localPort;
    pbTemplate.csParam.open.localHost = 0L;
    pbTemplate.csParam.open.remoteHost = 0L;
    pbTemplate.csParam.open.remotePort = 0;
    pbTemplate.tcpStream = stream;
    return LowLevelAsync(&pbTemplate, TCPPassiveOpen);
}
static OSErr MacTCP_ActiveOpenSync(StreamPtr stream, ip_addr remoteHost, tcp_port remotePort, Byte ulpTimeoutSec, GiveTimePtr giveTime)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.open.ulpTimeoutValue = ulpTimeoutSec;
    pb.csParam.open.ulpTimeoutAction = 1;
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.remoteHost = remoteHost;
    pb.csParam.open.remotePort = remotePort;
    pb.csParam.open.localPort = 0;
    pb.csParam.open.localHost = 0L;
    SInt16 pollTimeout = (SInt16)ulpTimeoutSec * 60 + 60;
    return LowLevelSyncPoll(&pb, giveTime, TCPActiveOpen, pollTimeout);
}
static OSErr MacTCP_SendSync(StreamPtr stream, Ptr wdsPtr, Boolean pushFlag, Byte ulpTimeoutSec, GiveTimePtr giveTime)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.send.ulpTimeoutValue = ulpTimeoutSec;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag = pushFlag;
    pb.csParam.send.urgentFlag = false;
    pb.csParam.send.wdsPtr = wdsPtr;
    SInt16 pollTimeout = (SInt16)ulpTimeoutSec * 60 + 60;
    return LowLevelSyncPoll(&pb, giveTime, TCPSend, pollTimeout);
}
static OSErr MacTCP_NoCopyRcvSync(StreamPtr stream, wdsEntry rds[], short maxRDSEntries, Byte commandTimeoutSec, Boolean *urgentFlag, Boolean *markFlag, GiveTimePtr giveTime)
{
    TCPiopb pb;
    OSErr err;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.receive.commandTimeoutValue = commandTimeoutSec;
    pb.csParam.receive.rdsPtr = (Ptr)rds;
    pb.csParam.receive.rdsLength = maxRDSEntries;
    SInt16 pollTimeout = (commandTimeoutSec == 0) ? (APP_POLL_TIMEOUT_TICKS * 10) : ((SInt16)commandTimeoutSec * 60 + 60);
    err = LowLevelSyncPoll(&pb, giveTime, TCPNoCopyRcv, pollTimeout);
    if (err == noErr) {
        *urgentFlag = pb.csParam.receive.urgentFlag;
        *markFlag = pb.csParam.receive.markFlag;
    } else {
        *urgentFlag = false;
        *markFlag = false;
    }
    return err;
}
static OSErr MacTCP_BfrReturnSync(StreamPtr stream, wdsEntry rds[], GiveTimePtr giveTime)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.receive.rdsPtr = (Ptr)rds;
    return LowLevelSyncPoll(&pb, giveTime, TCPRcvBfrReturn, APP_POLL_TIMEOUT_TICKS * 2);
}
static OSErr MacTCP_CloseGracefulSync(StreamPtr stream, Byte ulpTimeoutSec, GiveTimePtr giveTime)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    pb.csParam.close.ulpTimeoutValue = ulpTimeoutSec;
    pb.csParam.close.ulpTimeoutAction = 1;
    pb.csParam.close.validityFlags = timeoutValue | timeoutAction;
    SInt16 pollTimeout = (SInt16)ulpTimeoutSec * 60 + 60;
    return LowLevelSyncPoll(&pb, giveTime, TCPClose, pollTimeout);
}
static OSErr MacTCP_AbortConnection(StreamPtr stream)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof(TCPiopb));
    if (stream == NULL) {
        log_debug("MacTCP_AbortConnection: Stream is NULL, nothing to abort.");
        return noErr;
    }
    OSErr err = LowLevelSyncPoll(&pb, YieldTimeToSystem, TCPAbort, APP_POLL_TIMEOUT_TICKS * 5);
    if (err == connectionDoesntExist || err == invalidStreamPtr) {
        log_debug("MacTCP_AbortConnection: Connection did not exist or stream invalid (err %d). Considered OK for abort.", err);
        return noErr;
    }
    if (err != noErr) {
        log_debug("MacTCP_AbortConnection: LowLevelSyncPoll for TCPAbort returned error %d.", err);
    }
    return err;
}
static OSErr MacTCP_GetStatus(StreamPtr stream, struct TCPStatusPB *statusPBOut, GiveTimePtr giveTime)
{
    TCPiopb pb;
    OSErr err;
    if (stream == NULL || statusPBOut == NULL) return paramErr;
    memset(&pb, 0, sizeof(TCPiopb));
    err = LowLevelSyncPoll(&pb, giveTime, TCPStatus, APP_POLL_TIMEOUT_TICKS);
    if (err == noErr) {
        BlockMoveData((Ptr)&pb.csParam.status, (Ptr)statusPBOut, sizeof(struct TCPStatusPB));
    } else {
        log_debug("MacTCP_GetStatus: LowLevelSyncPoll for TCPStatus returned error %d.", err);
    }
    return err;
}
