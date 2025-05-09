#include "./mactcp_messaging.h"
#include "logging.h"
#include "protocol.h"
#include "peer.h"
#include "dialog.h"
#include "dialog_peerlist.h"
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
#define kTCPRecvBufferSize 8192
#define kTCPInternalBufferSize 8192
#define kTCPPassiveOpenULPTimeoutSeconds 2
#define kConnectULPTimeoutSeconds 5
#define kSendULPTimeoutSeconds 3
#define kAbortULPTimeoutSeconds 1
#define kTCPRecvPollTimeoutTicks 1
#define kTCPStatusPollTimeoutTicks 1
#define kConnectPollTimeoutTicks (kConnectULPTimeoutSeconds * 60 + 30)
#define kSendPollTimeoutTicks (kSendULPTimeoutSeconds * 60 + 30)
#define kAbortPollTimeoutTicks (kAbortULPTimeoutSeconds * 60 + 30)
#define kErrorRetryDelayTicks 120
#define kQuitLoopDelayTicks 120
#define kMacTCPTimeoutErr (-23016)
#define kDuplicateSocketErr (-23017)
#define kConnectionExistsErr (-23007)
#define kConnectionClosingErr (-23005)
#define kConnectionDoesntExistErr (-23008)
#define kInvalidStreamPtrErr (-23010)
#define kInvalidWDSErr (-23014)
#define kInvalidBufPtrErr (-23013)
#define kRequestAbortedErr (-23006)
#define AbortTrue 1
static StreamPtr gTCPStream = NULL;
static Ptr gTCPInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static Boolean gIsSending = false;
static ip_addr gPeerIP = 0;
static tcp_port gPeerPort = 0;
static TCPiopb gTCPPassiveOpenPB;
static Boolean gPassiveOpenPBInitialized = false;
static void ProcessTCPReceive(unsigned short dataLength);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt16 appPollTimeoutTicks);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPActiveOpenSyncPoll(Byte ulpTimeoutSeconds, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSyncPoll(Byte ulpTimeoutSeconds, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPRcvSyncPoll(SInt16 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime);
static OSErr LowTCPStatusSyncPoll(SInt16 appPollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState);
static OSErr LowTCPAbortSyncPoll(Byte ulpTimeoutSeconds, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamToRelease);
static int mac_tcp_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_debug("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult == 0) {
        log_debug("Peer updated via TCP: %s@%s", username, ip);
    } else {
        log_debug("Peer list full or error, could not add/update %s@%s from TCP connection", username, ip);
    }
    return addResult;
}
static void mac_tcp_display_text_message(const char *username, const char *ip, const char *message_content, void *platform_context)
{
    (void)platform_context;
    (void)ip;
    char displayMsg[BUFFER_SIZE + 100];
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
        log_debug("Message from %s@%s: %s", username, ip, message_content);
    } else {
        log_debug("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}
static void mac_tcp_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context;
    if (!ip) return;
    log_debug("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) {
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}
OSErr InitTCP(short macTCPRefNum)
{
    OSErr err;
    log_debug("Initializing Single TCP Stream (Async Passive Open / Sync Poll Strategy)...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPStream != NULL || gTCPState != TCP_STATE_UNINITIALIZED) {
        log_debug("Error (InitTCP): Already initialized or in unexpected state (%d)?", gTCPState);
        return streamAlreadyOpen;
    }
    gTCPInternalBuffer = NewPtrClear(kTCPInternalBufferSize);
    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);
    if (gTCPInternalBuffer == NULL || gTCPRecvBuffer == NULL) {
        log_debug("Fatal Error: Could not allocate TCP buffers.");
        if (gTCPInternalBuffer) DisposePtr(gTCPInternalBuffer);
        if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer);
        gTCPInternalBuffer = gTCPRecvBuffer = NULL;
        return memFullErr;
    }
    log_debug("Allocated TCP buffers (Internal: %ld, Recv: %ld).", (long)kTCPInternalBufferSize, (long)kTCPRecvBufferSize);
    log_debug("Creating Single Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &gTCPStream, gTCPInternalBuffer, kTCPInternalBufferSize);
    if (err != noErr || gTCPStream == NULL) {
        log_debug("Error: Failed to create TCP Stream: %d", err);
        CleanupTCP(macTCPRefNum);
        return err;
    }
    log_debug("Single TCP Stream created (0x%lX).", (unsigned long)gTCPStream);
    memset(&gTCPPassiveOpenPB, 0, sizeof(TCPiopb));
    gTCPPassiveOpenPB.ioCompletion = nil;
    gTCPPassiveOpenPB.ioCRefNum = gMacTCPRefNum;
    gTCPPassiveOpenPB.tcpStream = gTCPStream;
    gPassiveOpenPBInitialized = true;
    gTCPState = TCP_STATE_IDLE;
    gIsSending = false;
    gPeerIP = 0;
    gPeerPort = 0;
    log_debug("TCP initialization complete. State: IDLE.");
    return noErr;
}
void CleanupTCP(short macTCPRefNum)
{
    log_debug("Cleaning up Single TCP Stream...");
    StreamPtr streamToRelease = gTCPStream;
    TCPState stateBeforeCleanup = gTCPState;
    gTCPState = TCP_STATE_RELEASING;
    gTCPStream = NULL;
    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN || stateBeforeCleanup == TCP_STATE_PASSIVE_OPEN_PENDING) {
        log_debug("Cleanup: Attempting synchronous abort (best effort)...");
        if (streamToRelease != NULL) {
            StreamPtr currentGlobalStream = gTCPStream;
            gTCPStream = streamToRelease;
            LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, YieldTimeToSystem);
            gTCPStream = currentGlobalStream;
        }
    }
    if (streamToRelease != NULL && macTCPRefNum != 0) {
        log_debug("Attempting sync release of stream 0x%lX...", (unsigned long)streamToRelease);
        OSErr relErr = LowTCPReleaseSync(macTCPRefNum, streamToRelease);
        if (relErr != noErr) log_debug("Warning: Sync release failed: %d", relErr);
        else log_debug("Sync release successful.");
    } else if (streamToRelease != NULL) {
        log_debug("Warning: Cannot release stream, MacTCP refnum is 0.");
    }
    gTCPState = TCP_STATE_UNINITIALIZED;
    gIsSending = false;
    gPeerIP = 0;
    gPeerPort = 0;
    gPassiveOpenPBInitialized = false;
    if (gTCPRecvBuffer != NULL) {
        DisposePtr(gTCPRecvBuffer);
        gTCPRecvBuffer = NULL;
    }
    if (gTCPInternalBuffer != NULL) {
        DisposePtr(gTCPInternalBuffer);
        gTCPInternalBuffer = NULL;
    }
    log_debug("TCP cleanup finished.");
}
void PollTCP(GiveTimePtr giveTime)
{
    OSErr err;
    unsigned short amountUnread = 0;
    Byte connectionState = 0;
    unsigned long dummyTimer;
    if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) {
        return;
    }
    if (gIsSending) {
        giveTime();
        return;
    }
    switch (gTCPState) {
    case TCP_STATE_IDLE:
        log_debug("PollTCP: State IDLE. Attempting ASYNC Passive Open (ULP: %ds)...", kTCPPassiveOpenULPTimeoutSeconds);
        if (!gPassiveOpenPBInitialized) {
            log_debug("PollTCP CRITICAL: gTCPPassiveOpenPB not initialized!");
            gTCPState = TCP_STATE_ERROR;
            break;
        }
        gTCPPassiveOpenPB.csCode = TCPPassiveOpen;
        gTCPPassiveOpenPB.csParam.open.ulpTimeoutValue = kTCPPassiveOpenULPTimeoutSeconds;
        gTCPPassiveOpenPB.csParam.open.ulpTimeoutAction = AbortTrue;
        gTCPPassiveOpenPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
        gTCPPassiveOpenPB.csParam.open.localPort = PORT_TCP;
        gTCPPassiveOpenPB.csParam.open.localHost = 0L;
        gTCPPassiveOpenPB.csParam.open.remoteHost = 0L;
        gTCPPassiveOpenPB.csParam.open.remotePort = 0;
        gTCPPassiveOpenPB.csParam.open.tosFlags = 0;
        gTCPPassiveOpenPB.csParam.open.precedence = 0;
        gTCPPassiveOpenPB.csParam.open.dontFrag = false;
        gTCPPassiveOpenPB.csParam.open.timeToLive = 0;
        gTCPPassiveOpenPB.csParam.open.security = 0;
        gTCPPassiveOpenPB.csParam.open.optionCnt = 0;
        gTCPPassiveOpenPB.csParam.open.commandTimeoutValue = 0;
        gTCPPassiveOpenPB.ioResult = 1;
        err = PBControlAsync((ParmBlkPtr)&gTCPPassiveOpenPB);
        if (err == noErr) {
            log_debug("PollTCP: Async TCPPassiveOpen initiated.");
            gTCPState = TCP_STATE_PASSIVE_OPEN_PENDING;
        } else {
            log_debug("PollTCP: PBControlAsync(TCPPassiveOpen) failed immediately: %d. Retrying after delay.", err);
            gTCPState = TCP_STATE_IDLE;
            Delay(kErrorRetryDelayTicks, &dummyTimer);
        }
        break;
    case TCP_STATE_PASSIVE_OPEN_PENDING:
        giveTime();
        if (gTCPPassiveOpenPB.ioResult == 1) {
            return;
        }
        if (gTCPPassiveOpenPB.ioResult == noErr) {
            gPeerIP = gTCPPassiveOpenPB.csParam.open.remoteHost;
            gPeerPort = gTCPPassiveOpenPB.csParam.open.remotePort;
            char senderIPStr[INET_ADDRSTRLEN];
            AddrToStr(gPeerIP, senderIPStr);
            log_debug("PollTCP: Incoming connection from %s:%u.", senderIPStr, gPeerPort);
            gTCPState = TCP_STATE_CONNECTED_IN;
            goto CheckConnectedInData;
        } else {
            err = gTCPPassiveOpenPB.ioResult;
            if (err == kRequestAbortedErr) {
                log_debug("PollTCP: Async Passive Open was aborted (err %d), likely by a send operation. Returning to IDLE.", err);
            } else {
                log_debug("PollTCP: Async Passive Open failed: %d.", err);
            }
            if (err == kDuplicateSocketErr || err == kConnectionExistsErr) {
                log_debug("PollTCP: Attempting Abort to clear stream after Passive Open failure (%d).", err);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
            }
            gTCPState = TCP_STATE_IDLE;
            Delay(kErrorRetryDelayTicks, &dummyTimer);
        }
        break;
    case TCP_STATE_CONNECTED_IN:
CheckConnectedInData:
        log_debug("PollTCP: State CONNECTED_IN. Checking status...");
        err = LowTCPStatusSyncPoll(kTCPStatusPollTimeoutTicks, giveTime, &amountUnread, &connectionState);
        if (err != noErr) {
            log_debug("PollTCP: Error getting status while CONNECTED_IN: %d. Aborting.", err);
            LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
            gTCPState = TCP_STATE_IDLE;
            break;
        }
        if (connectionState != 8 && connectionState != 10 && connectionState != 12 && connectionState != 14) {
            char peerIPStr[INET_ADDRSTRLEN];
            AddrToStr(gPeerIP, peerIPStr);
            log_debug("PollTCP: Connection state is %d (not Established/Closing) for %s. Aborting and returning to IDLE.", connectionState, peerIPStr);
            LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
            gTCPState = TCP_STATE_IDLE;
            break;
        }
        log_debug("PollTCP: Status OK (State %d). Unread data: %u bytes.", connectionState, amountUnread);
        if (amountUnread > 0) {
            unsigned short bytesToRead = kTCPRecvBufferSize;
            Boolean markFlag = false, urgentFlag = false;
            log_debug("PollTCP: Attempting synchronous Rcv poll...");
            err = LowTCPRcvSyncPoll(kTCPRecvPollTimeoutTicks, gTCPRecvBuffer, &bytesToRead, &markFlag, &urgentFlag, giveTime);
            if (err == noErr) {
                log_debug("PollTCP: Rcv poll got %u bytes.", bytesToRead);
                ProcessTCPReceive(bytesToRead);
            } else if (err == kConnectionClosingErr) {
                char peerIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, peerIPStr);
                log_debug("PollTCP: Rcv poll indicated connection closing by peer %s. Processing final %u bytes.", peerIPStr, bytesToRead);
                if (bytesToRead > 0) ProcessTCPReceive(bytesToRead);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                gTCPState = TCP_STATE_IDLE;
            } else if (err == commandTimeout) {
                log_debug("PollTCP: Rcv poll timed out despite status showing data? Odd. Will retry status.");
            } else {
                char peerIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, peerIPStr);
                log_debug("PollTCP: Rcv poll failed for %s: %d. Aborting.", peerIPStr, err);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                gTCPState = TCP_STATE_IDLE;
            }
        } else {
            if (connectionState == 14) {
                char peerIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, peerIPStr);
                log_debug("PollTCP: Peer %s has closed (State: CLOSE_WAIT). Aborting to clean up. Returning to IDLE.", peerIPStr);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                gTCPState = TCP_STATE_IDLE;
            } else if (connectionState != 8) {
                char peerIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, peerIPStr);
                log_debug("PollTCP: Peer %s in closing state %d with no data. Waiting for MacTCP.", peerIPStr, connectionState);
            }
        }
        break;
    default:
        log_debug("PollTCP: In unexpected state %d.", gTCPState);
        gTCPState = TCP_STATE_IDLE;
        break;
    }
}
TCPState GetTCPState(void)
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
    struct wdsEntry sendWDS[2];
    log_debug("MacTCP_SendMessageSync: Request to send '%s' to %s", msg_type, peerIPStr);
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL && gTCPState != TCP_STATE_RELEASING && gTCPState != TCP_STATE_UNINITIALIZED) {
        if (gTCPStream == NULL) {
            log_debug("Error (SendMessage): gTCPStream is NULL and not in deep cleanup state.");
            return kInvalidStreamPtrErr;
        }
    }
    if (peerIPStr == NULL || msg_type == NULL || local_username == NULL || local_ip_str == NULL || giveTime == NULL) {
        return paramErr;
    }
    if (gTCPState == TCP_STATE_PASSIVE_OPEN_PENDING) {
        log_debug("SendMessage: Stream was in PASSIVE_OPEN_PENDING. Aborting pending listen to allow send.");
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
        if (abortErr == noErr) {
            log_debug("SendMessage: Abort of pending passive open successful.");
        } else {
            log_debug("SendMessage: Abort of pending passive open FAILED: %d. Send may fail.", abortErr);
            return (abortErr == commandTimeout) ? streamBusyErr : abortErr;
        }
        gTCPState = TCP_STATE_IDLE;
    }
    if (gIsSending) {
        log_debug("Warning (SendMessage): Send already in progress.");
        return streamBusyErr;
    }
    if (gTCPState != TCP_STATE_IDLE) {
        log_debug("Warning (SendMessage): Stream not IDLE (state %d) after attempting to clear. Cannot send now.", gTCPState);
        return streamBusyErr;
    }
    gIsSending = true;
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_debug("Error (SendMessage): Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendMessageCleanup;
    }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, msg_type, local_username, local_ip_str, message_content ? message_content : "");
    if (formattedLen <= 0) {
        log_debug("Error (SendMessage): format_message failed for type '%s'.", msg_type);
        finalErr = paramErr;
        goto SendMessageCleanup;
    }
    log_debug("SendMessage: Connecting to %s...", peerIPStr);
    err = LowTCPActiveOpenSyncPoll(kConnectULPTimeoutSeconds, targetIP, PORT_TCP, giveTime);
    if (err == noErr) {
        log_debug("SendMessage: Connected successfully to %s.", peerIPStr);
        sendWDS[0].length = formattedLen - 1;
        sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0;
        sendWDS[1].ptr = NULL;
        log_debug("SendMessage: Sending data (%d bytes)...", sendWDS[0].length);
        err = LowTCPSendSyncPoll(kSendULPTimeoutSeconds, true, (Ptr)sendWDS, giveTime);
        if (err != noErr) {
            log_debug("Error (SendMessage): Send failed to %s: %d", peerIPStr, err);
            finalErr = err;
        } else {
            log_debug("SendMessage: Send successful to %s.", peerIPStr);
        }
        log_debug("SendMessage: Aborting connection to %s...", peerIPStr);
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
        if (abortErr != noErr) {
            log_debug("Warning (SendMessage): Abort failed for %s: %d", peerIPStr, abortErr);
            if (finalErr == noErr) finalErr = abortErr;
        }
    } else {
        log_debug("Error (SendMessage): Connect to %s failed: %d", peerIPStr, err);
        finalErr = err;
        if (err == kConnectionExistsErr && strcmp(msg_type, MSG_QUIT) == 0) {
            log_debug("SendMessage: Connect for QUIT to %s failed with connectionExists. Peer might be in TIME_WAIT. Assuming QUIT effectively sent.", peerIPStr);
            finalErr = noErr;
        }
    }
SendMessageCleanup:
    gIsSending = false;
    gTCPState = TCP_STATE_IDLE;
    log_debug("MacTCP_SendMessageSync to %s for '%s': Released send lock. Final Status: %d.", peerIPStr, msg_type, finalErr);
    return finalErr;
}
static void ProcessTCPReceive(unsigned short dataLength)
{
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    static tcp_platform_callbacks_t mac_callbacks = {
        .add_or_update_peer = mac_tcp_add_or_update_peer,
        .display_text_message = mac_tcp_display_text_message,
        .mark_peer_inactive = mac_tcp_mark_peer_inactive
    };
    if (dataLength > 0 && gTCPRecvBuffer != NULL) {
        OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection);
        if (addrErr != noErr) {
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF);
            log_debug("ProcessTCPReceive: AddrToStr failed for gPeerIP %lu. Using manual format '%s'.", gPeerIP, senderIPStrFromConnection);
        }
        if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0';
        else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0';
        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_debug("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (payload IP: %s).",
                             msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);
            handle_received_tcp_message(senderIPStrFromConnection,
                                        senderUsername,
                                        msgType,
                                        content,
                                        &mac_callbacks,
                                        NULL);
            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_debug("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection);
            }
        } else {
            log_debug("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else if (dataLength == 0) {
        log_debug("ProcessTCPReceive: Received 0 bytes (likely connection closing signal or KeepAlive).");
    } else {
        log_debug("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL or other issue?");
    }
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
            return kInvalidStreamPtrErr;
        }
        pBlock->tcpStream = gTCPStream;
    } else if (csCode == TCPRelease && pBlock->tcpStream == NULL) {
        log_debug("Error (LowLevelSyncPoll TCPRelease): pBlock->tcpStream is NULL.");
        return kInvalidStreamPtrErr;
    }
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->ioResult = 1;
    pBlock->csCode = csCode;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_debug("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err);
        return err;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
        if (appPollTimeoutTicks > 0 && (TickCount() - startTime) >= (unsigned long)appPollTimeoutTicks) {
            log_debug("LowLevelSyncPoll (%d): App-level poll timeout (%d ticks) reached.", csCode, appPollTimeoutTicks);
            return commandTimeout;
        }
    }
    return pBlock->ioResult;
}
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr rcvBuff, unsigned long rcvBuffLen)
{
    OSErr err;
    TCPiopb pbCreate;
    if (streamPtrOut == NULL || rcvBuff == NULL) return paramErr;
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L;
    pbCreate.csParam.create.rcvBuff = rcvBuff;
    pbCreate.csParam.create.rcvBuffLen = rcvBuffLen;
    pbCreate.csParam.create.notifyProc = nil;
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    if (err == noErr) {
        *streamPtrOut = pbCreate.tcpStream;
        if (*streamPtrOut == NULL) {
            log_debug("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream.");
            err = ioErr;
        }
    } else {
        *streamPtrOut = NULL;
        log_debug("Error (LowTCPCreateSync): PBControlSync failed: %d", err);
    }
    return err;
}
static OSErr LowTCPActiveOpenSyncPoll(Byte ulpTimeoutSeconds, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime)
{
    TCPiopb pbOpen;
    SInt16 pollTimeout;
    memset(&pbOpen, 0, sizeof(TCPiopb));
    pbOpen.csParam.open.ulpTimeoutValue = ulpTimeoutSeconds;
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue;
    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pbOpen.csParam.open.commandTimeoutValue = 0;
    pbOpen.csParam.open.remoteHost = remoteHost;
    pbOpen.csParam.open.remotePort = remotePort;
    pbOpen.csParam.open.localPort = 0;
    pbOpen.csParam.open.localHost = 0L;
    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30;
    return LowLevelSyncPoll(&pbOpen, giveTime, TCPActiveOpen, pollTimeout);
}
static OSErr LowTCPSendSyncPoll(Byte ulpTimeoutSeconds, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime)
{
    TCPiopb pbSend;
    SInt16 pollTimeout;
    if (wdsPtr == NULL) return kInvalidWDSErr;
    memset(&pbSend, 0, sizeof(TCPiopb));
    pbSend.csParam.send.ulpTimeoutValue = ulpTimeoutSeconds;
    pbSend.csParam.send.ulpTimeoutAction = AbortTrue;
    pbSend.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pbSend.csParam.send.pushFlag = push;
    pbSend.csParam.send.urgentFlag = false;
    pbSend.csParam.send.wdsPtr = wdsPtr;
    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30;
    return LowLevelSyncPoll(&pbSend, giveTime, TCPSend, pollTimeout);
}
static OSErr LowTCPRcvSyncPoll(SInt16 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb pbRcv;
    unsigned short initialBufferLen;
    if (buffer == NULL || bufferLen == NULL || *bufferLen == 0) return kInvalidBufPtrErr;
    if (markFlag == NULL || urgentFlag == NULL) return paramErr;
    initialBufferLen = *bufferLen;
    memset(&pbRcv, 0, sizeof(TCPiopb));
    pbRcv.csParam.receive.commandTimeoutValue = 1;
    pbRcv.csParam.receive.rcvBuff = buffer;
    pbRcv.csParam.receive.rcvBuffLen = initialBufferLen;
    err = LowLevelSyncPoll(&pbRcv, giveTime, TCPRcv, appPollTimeoutTicks);
    *bufferLen = pbRcv.csParam.receive.rcvBuffLen;
    *markFlag = pbRcv.csParam.receive.markFlag;
    *urgentFlag = pbRcv.csParam.receive.urgentFlag;
    return err;
}
static OSErr LowTCPStatusSyncPoll(SInt16 appPollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState)
{
    OSErr err;
    TCPiopb pbStat;
    if (amtUnread == NULL || connState == NULL) return paramErr;
    memset(&pbStat, 0, sizeof(TCPiopb));
    err = LowLevelSyncPoll(&pbStat, giveTime, TCPStatus, appPollTimeoutTicks);
    if (err == noErr) {
        *amtUnread = pbStat.csParam.status.amtUnreadData;
        *connState = pbStat.csParam.status.connectionState;
    } else {
        *amtUnread = 0;
        *connState = 0;
        log_debug("Warning (LowTCPStatusSyncPoll): Failed: %d", err);
        if (err == kInvalidStreamPtrErr) err = kConnectionDoesntExistErr;
    }
    return err;
}
static OSErr LowTCPAbortSyncPoll(Byte ulpTimeoutSeconds, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb pbAbort;
    SInt16 pollTimeout;
    if (gTCPStream == NULL) {
        log_debug("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort.");
        return noErr;
    }
    memset(&pbAbort, 0, sizeof(TCPiopb));
    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30;
    err = LowLevelSyncPoll(&pbAbort, giveTime, TCPAbort, pollTimeout);
    if (err == kConnectionDoesntExistErr || err == kInvalidStreamPtrErr || err == kRequestAbortedErr) {
        log_debug("LowTCPAbortSyncPoll: Abort completed (err %d). Considered OK for reset.", err);
        err = noErr;
    } else if (err != noErr) {
        log_debug("Warning (LowTCPAbortSyncPoll): Abort poll failed with error: %d", err);
    } else {
        log_debug("LowTCPAbortSyncPoll: Abort poll successful.");
    }
    return err;
}
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamToRelease)
{
    OSErr err;
    TCPiopb pbRelease;
    if (streamToRelease == NULL) return kInvalidStreamPtrErr;
    memset(&pbRelease, 0, sizeof(TCPiopb));
    pbRelease.ioCompletion = nil;
    pbRelease.ioCRefNum = macTCPRefNum;
    pbRelease.csCode = TCPRelease;
    pbRelease.tcpStream = streamToRelease;
    err = PBControlSync((ParmBlkPtr)&pbRelease);
    if (err != noErr && err != kInvalidStreamPtrErr) {
        log_debug("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err);
    } else if (err == kInvalidStreamPtrErr) {
        log_debug("Info (LowTCPReleaseSync): Stream 0x%lX already invalid or released. Error: %d", (unsigned long)streamToRelease, err);
        err = noErr;
    }
    return err;
}
