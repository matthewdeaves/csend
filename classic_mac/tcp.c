#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "network.h"
#include "../shared/messaging_logic.h"
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
#define kTCPListenPollTimeoutTicks 150
#define kTCPRecvPollTimeoutTicks 1
#define kTCPStatusPollTimeoutTicks 1
#define kConnectTimeoutTicks 300
#define kSendTimeoutTicks 180
#define kAbortTimeoutTicks 60
#define kQuitLoopDelayTicks 120
#define kErrorRetryDelayTicks 120
#define kMacTCPTimeoutErr (-23016)
#define kDuplicateSocketErr (-23017)
#define kConnectionExistsErr (-23007)
#define kConnectionClosingErr (-23005)
#define kConnectionDoesntExistErr (-23008)
#define kInvalidStreamPtrErr (-23010)
#define kInvalidWDSErr (-23014)
#define kInvalidBufPtrErr (-23013)
#define AbortTrue 1
static StreamPtr gTCPStream = NULL;
static Ptr gTCPInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static Boolean gIsSending = false;
static ip_addr gPeerIP = 0;
static tcp_port gPeerPort = 0;
static void ProcessTCPReceive(unsigned short dataLength);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt8 appPollTimeoutTicks);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPPassiveOpenSyncPoll(SInt8 pollTimeoutTicks, GiveTimePtr giveTime);
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPRcvSyncPoll(SInt8 pollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime);
static OSErr LowTCPStatusSyncPoll(SInt8 pollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState);
static OSErr LowTCPAbortSyncPoll(SInt8 ulpTimeoutTicks, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context;
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_message("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult < 0) {
        log_message("Peer list full, could not add/update %s@%s from TCP connection", username, ip);
    }
    return addResult;
}
static void mac_tcp_display_text_message(const char* username, const char* ip, const char* message_content, void* platform_context) {
    (void)platform_context;
    (void)ip;
    char displayMsg[BUFFER_SIZE + 100];
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
        log_message("Message from %s@%s: %s", username, ip, message_content);
    } else {
        log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}
static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) {
    (void)platform_context;
    if (!ip) return;
    log_message("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) {
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}
OSErr InitTCP(short macTCPRefNum) {
    OSErr err;
    log_message("Initializing Single TCP Stream (Sync Poll Strategy)...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPStream != NULL || gTCPState != TCP_STATE_UNINITIALIZED) {
        log_message("Error (InitTCP): Already initialized or in unexpected state (%d)?", gTCPState);
        return streamAlreadyOpen;
    }
    gTCPInternalBuffer = NewPtrClear(kTCPInternalBufferSize);
    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);
    if (gTCPInternalBuffer == NULL || gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP buffers.");
        if (gTCPInternalBuffer) DisposePtr(gTCPInternalBuffer);
        if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer);
        gTCPInternalBuffer = gTCPRecvBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated TCP buffers (Internal: %ld, Recv: %ld).", (long)kTCPInternalBufferSize, (long)kTCPRecvBufferSize);
    log_message("Creating Single Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &gTCPStream, gTCPInternalBuffer, kTCPInternalBufferSize);
    if (err != noErr || gTCPStream == NULL) {
        log_message("Error: Failed to create TCP Stream: %d", err);
        CleanupTCP(macTCPRefNum);
        return err;
    }
    log_message("Single TCP Stream created (0x%lX).", (unsigned long)gTCPStream);
    gTCPState = TCP_STATE_IDLE;
    gIsSending = false;
    gPeerIP = 0; gPeerPort = 0;
    log_message("TCP initialization complete. State: IDLE.");
    return noErr;
}
void CleanupTCP(short macTCPRefNum) {
    log_message("Cleaning up Single TCP Stream (Sync Poll Strategy)...");
    StreamPtr streamToRelease = gTCPStream;
    TCPState stateBeforeCleanup = gTCPState;
    gTCPState = TCP_STATE_RELEASING;
    gTCPStream = NULL;
    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN || stateBeforeCleanup == TCP_STATE_LISTENING_POLL) {
         log_message("Cleanup: Attempting synchronous abort (best effort)...");
         if (streamToRelease != NULL) {
             StreamPtr currentGlobalStream = gTCPStream;
             gTCPStream = streamToRelease;
             LowTCPAbortSyncPoll(kAbortTimeoutTicks, YieldTimeToSystem);
             gTCPStream = currentGlobalStream;
         }
    }
    if (streamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting sync release of stream 0x%lX...", (unsigned long)streamToRelease);
        OSErr relErr = LowTCPReleaseSync(macTCPRefNum, streamToRelease);
        if (relErr != noErr) log_message("Warning: Sync release failed: %d", relErr);
        else log_to_file_only("Sync release successful.");
    } else if (streamToRelease != NULL) {
        log_message("Warning: Cannot release stream, MacTCP refnum is 0 or stream already NULLed.");
    }
    gTCPState = TCP_STATE_UNINITIALIZED;
    gIsSending = false;
    gPeerIP = 0; gPeerPort = 0;
    if (gTCPRecvBuffer != NULL) { DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL; }
    if (gTCPInternalBuffer != NULL) { DisposePtr(gTCPInternalBuffer); gTCPInternalBuffer = NULL; }
    log_message("TCP cleanup finished.");
}
void PollTCP(GiveTimePtr giveTime) {
    OSErr err;
    unsigned short amountUnread = 0;
    Byte connectionState = 0;
    unsigned long dummyTimer;
    if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) {
        return;
    }
    if (gIsSending) { return; }
    switch (gTCPState) {
        case TCP_STATE_IDLE:
            log_to_file_only("PollTCP: State IDLE. Attempting Passive Open Poll (ULP: %ds, AppPoll: %d ticks)...", kTCPPassiveOpenULPTimeoutSeconds, kTCPListenPollTimeoutTicks);
            err = LowTCPPassiveOpenSyncPoll(kTCPListenPollTimeoutTicks, giveTime);
            if (err == noErr) {
                char senderIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, senderIPStr);
                log_message("PollTCP: Incoming connection from %s:%u.", senderIPStr, gPeerPort);
                gTCPState = TCP_STATE_CONNECTED_IN;
                goto CheckConnectedInData;
            } else if (err == commandTimeout) {
                log_to_file_only("PollTCP: Passive Open Poll window (%d ticks) timed out. No connection. Returning to IDLE.", kTCPListenPollTimeoutTicks);
                gTCPState = TCP_STATE_IDLE;
            } else if (err == kDuplicateSocketErr || err == kConnectionExistsErr) {
                log_message("PollTCP: Passive Open Poll failed with %d. Attempting to Abort stream to reset.", err);
                OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                if (abortErr == noErr) {
                    log_message("PollTCP: Abort successful after Passive Open failure. Will retry passive open.");
                } else {
                    log_message("PollTCP: CRITICAL - Abort FAILED (%d) after Passive Open failure. TCP might be stuck.", abortErr);
                }
                gTCPState = TCP_STATE_IDLE;
                log_message("PollTCP: Delaying %d ticks due to error %d before retrying passive open.", kErrorRetryDelayTicks, err);
                Delay(kErrorRetryDelayTicks, &dummyTimer);
            } else {
                log_message("PollTCP: Passive Open Poll failed with other error: %d. Returning to IDLE.", err);
                gTCPState = TCP_STATE_IDLE;
            }
            break;
        case TCP_STATE_CONNECTED_IN:
CheckConnectedInData:
            log_to_file_only("PollTCP: State CONNECTED_IN. Checking status...");
            err = LowTCPStatusSyncPoll(kTCPStatusPollTimeoutTicks, giveTime, &amountUnread, &connectionState);
            if (err != noErr) {
                log_message("PollTCP: Error getting status while CONNECTED_IN: %d. Aborting.", err);
                LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                gTCPState = TCP_STATE_IDLE;
                break;
            }
            if (connectionState != 8 && connectionState != 10 && connectionState != 12 && connectionState != 14) {
                 char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                 log_message("PollTCP: Connection state is %d (not Established/Closing) for %s. Assuming closed/aborted. Returning to IDLE.", connectionState, peerIPStr);
                 gTCPState = TCP_STATE_IDLE;
                 break;
            }
            log_to_file_only("PollTCP: Status OK (State %d). Unread data: %u bytes.", connectionState, amountUnread);
            if (amountUnread > 0) {
                unsigned short bytesToRead = kTCPRecvBufferSize;
                Boolean markFlag = false, urgentFlag = false;
                log_to_file_only("PollTCP: Attempting synchronous Rcv poll...");
                err = LowTCPRcvSyncPoll(kTCPRecvPollTimeoutTicks, gTCPRecvBuffer, &bytesToRead, &markFlag, &urgentFlag, giveTime);
                if (err == noErr) {
                    log_to_file_only("PollTCP: Rcv poll got %u bytes.", bytesToRead);
                    ProcessTCPReceive(bytesToRead);
                } else if (err == kConnectionClosingErr) {
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll indicated connection closing by peer %s. Processing final %u bytes.", peerIPStr, bytesToRead);
                    if (bytesToRead > 0) ProcessTCPReceive(bytesToRead);
                    gTCPState = TCP_STATE_IDLE;
                } else if (err == commandTimeout) {
                     log_to_file_only("PollTCP: Rcv poll timed out despite status showing data? Odd. Will retry status.");
                } else {
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll failed for %s: %d. Aborting.", peerIPStr, err);
                    LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                    gTCPState = TCP_STATE_IDLE;
                }
            } else {
                 if (connectionState == 14 ) {
                     char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                     log_message("PollTCP: Peer %s has closed (State: CLOSE_WAIT). Returning to IDLE.", peerIPStr);
                     LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                     gTCPState = TCP_STATE_IDLE;
                 }
            }
            break;
        default:
            log_message("PollTCP: In unexpected state %d.", gTCPState);
            gTCPState = TCP_STATE_IDLE;
            break;
    }
}
TCPState GetTCPState(void) { return gTCPState; }
OSErr TCP_SendTextMessageSync(const char *peerIPStr, const char *message, GiveTimePtr giveTime) {
    OSErr err = noErr, finalErr = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;
    struct wdsEntry sendWDS[2];
    log_to_file_only("TCP_SendTextMessageSync: Request to send TEXT to %s", peerIPStr);
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (peerIPStr == NULL || message == NULL || giveTime == NULL) return paramErr;
    if (gIsSending) {
        log_message("Warning (SendText): Send already in progress.");
        return streamBusyErr;
    }
    if (gTCPState != TCP_STATE_IDLE) {
        log_message("Warning (SendText): Stream not IDLE (state %d), cannot send.", gTCPState);
        return streamBusyErr;
    }
    gIsSending = true;
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (SendText): Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendTextCleanup;
    }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT, gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) {
        log_message("Error (SendText): format_message failed.");
        finalErr = paramErr;
        goto SendTextCleanup;
    }
    log_to_file_only("SendText: Connecting to %s...", peerIPStr);
    err = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, targetIP, PORT_TCP, giveTime);
    if (err == noErr) {
        log_to_file_only("SendText: Connected successfully to %s.", peerIPStr);
        sendWDS[0].length = formattedLen;
        sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0;
        sendWDS[1].ptr = NULL;
        log_to_file_only("SendText: Sending data (%d bytes)...", formattedLen);
        err = LowTCPSendSyncPoll(kSendTimeoutTicks, true , (Ptr)sendWDS, giveTime);
        if (err != noErr) {
            log_message("Error (SendText): Send failed to %s: %d", peerIPStr, err);
            finalErr = err;
        } else {
            log_to_file_only("SendText: Send successful to %s.", peerIPStr);
        }
        log_to_file_only("SendText: Aborting connection to %s...", peerIPStr);
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
        if (abortErr != noErr) {
            log_message("Warning (SendText): Abort failed for %s: %d", peerIPStr, abortErr);
            if (finalErr == noErr) finalErr = abortErr;
        }
    } else {
        log_message("Error (SendText): Connect to %s failed: %d", peerIPStr, err);
        finalErr = err;
    }
SendTextCleanup:
    gIsSending = false;
    gTCPState = TCP_STATE_IDLE;
    log_to_file_only("TCP_SendTextMessageSync to %s: Released send lock. Final Status: %d.", peerIPStr, finalErr);
    return finalErr;
}
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime) {
    int i;
    OSErr lastErr = noErr, currentErr = noErr;
    char quitMessageBuffer[BUFFER_SIZE];
    int formattedLen, activePeerCount = 0, sentCount = 0;
    unsigned long dummyTimer;
    struct wdsEntry sendWDS[2];
    log_message("TCP_SendQuitMessagesSync: Starting...");
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (giveTime == NULL) return paramErr;
    if (gIsSending) {
        log_message("Warning (SendQuit): Send already in progress.");
        return streamBusyErr;
    }
    if (gTCPState != TCP_STATE_IDLE) {
        log_message("Warning (SendQuit): Stream not IDLE (state %d). Cannot send QUIT now. Peer might be connecting.", gTCPState);
        return streamBusyErr;
    }
    gIsSending = true;
    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT, gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) {
        log_message("Error (SendQuit): format_message for QUIT failed.");
        lastErr = paramErr;
        goto SendQuitCleanup;
    }
    for (i = 0; i < MAX_PEERS; ++i) if (gPeerManager.peers[i].active) activePeerCount++;
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);
    if (activePeerCount == 0) {
        lastErr = noErr;
        goto SendQuitCleanup;
    }
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr currentTargetIP = 0;
            currentErr = noErr;
            if (gTCPState != TCP_STATE_IDLE) {
                log_message("CRITICAL (SendQuit): State became non-IDLE (%d) during QUIT loop for peer %s. Aborting loop.", gTCPState, gPeerManager.peers[i].ip);
                if (lastErr == noErr) lastErr = ioErr;
                break;
            }
            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            currentErr = ParseIPv4(gPeerManager.peers[i].ip, &currentTargetIP);
            if (currentErr != noErr || currentTargetIP == 0) {
                log_message("Error (SendQuit): Could not parse IP '%s'. Skipping.", gPeerManager.peers[i].ip);
                if (lastErr == noErr) lastErr = currentErr;
                goto NextPeerDelay;
            }
            log_to_file_only("SendQuit: Connecting to %s...", gPeerManager.peers[i].ip);
            currentErr = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, currentTargetIP, PORT_TCP, giveTime);
            if (currentErr == noErr) {
                log_to_file_only("SendQuit: Connected successfully to %s.", gPeerManager.peers[i].ip);
                sendWDS[0].length = formattedLen;
                sendWDS[0].ptr = (Ptr)quitMessageBuffer;
                sendWDS[1].length = 0;
                sendWDS[1].ptr = NULL;
                log_to_file_only("SendQuit: Sending data to %s...", gPeerManager.peers[i].ip);
                currentErr = LowTCPSendSyncPoll(kSendTimeoutTicks, true , (Ptr)sendWDS, giveTime);
                if (currentErr == noErr) {
                    log_to_file_only("SendQuit: Send successful for %s.", gPeerManager.peers[i].ip);
                    sentCount++;
                } else {
                    log_message("Error (SendQuit): Send failed for %s: %d", gPeerManager.peers[i].ip, currentErr);
                    if (lastErr == noErr) lastErr = currentErr;
                }
                log_to_file_only("SendQuit: Aborting connection to %s...", gPeerManager.peers[i].ip);
                OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                if (abortErr != noErr) {
                    log_message("Warning (SendQuit): Abort failed for %s: %d", gPeerManager.peers[i].ip, abortErr);
                    if (lastErr == noErr) lastErr = abortErr;
                }
            } else {
                log_message("Error (SendQuit): Connect failed for %s: %d", gPeerManager.peers[i].ip, currentErr);
                if (lastErr == noErr) lastErr = currentErr;
                if (currentErr == kConnectionExistsErr) {
                     log_message("SendQuit: Connect to %s failed with -23007 (connectionExists). Peer likely just disconnected or in TIME_WAIT. Skipping QUIT.", gPeerManager.peers[i].ip);
                }
            }
        NextPeerDelay:
            log_to_file_only("SendQuit: Yielding/Delaying (%d ticks) after peer %s...", kQuitLoopDelayTicks, gPeerManager.peers[i].ip);
            giveTime();
            Delay(kQuitLoopDelayTicks, &dummyTimer);
        }
    }
SendQuitCleanup:
    gIsSending = false;
    gTCPState = TCP_STATE_IDLE;
    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers. Last error: %d.", sentCount, activePeerCount, lastErr);
    return lastErr;
}
static void ProcessTCPReceive(unsigned short dataLength) {
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
            log_to_file_only("ProcessTCPReceive: AddrToStr failed for gPeerIP %lu. Using manual format '%s'.", gPeerIP, senderIPStrFromConnection);
        }
        if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0';
        else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0';
        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (payload IP: %s).",
                             msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);
            handle_received_tcp_message(senderIPStrFromConnection, senderUsername, msgType, content, &mac_callbacks, NULL);
            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_message("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection);
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else if (dataLength == 0) {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing signal or KeepAlive).");
    } else {
        log_message("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL or other issue?");
    }
}
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt8 appPollTimeoutTicks) {
    OSErr err;
    unsigned long startTime = TickCount();
    if (pBlock == NULL || giveTime == NULL) return paramErr;
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->tcpStream = gTCPStream;
    pBlock->ioResult = 1;
    pBlock->csCode = csCode;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err);
        return err;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
        if (appPollTimeoutTicks > 0 && (TickCount() - startTime) >= (unsigned long)appPollTimeoutTicks) {
            log_to_file_only("LowLevelSyncPoll (%d): App-level poll timeout (%d ticks) reached.", csCode, appPollTimeoutTicks);
            return commandTimeout;
        }
    }
    return pBlock->ioResult;
}
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr rcvBuff, unsigned long rcvBuffLen) {
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
            log_message("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream.");
            err = ioErr;
        }
    } else {
        *streamPtrOut = NULL;
        log_message("Error (LowTCPCreateSync): PBControlSync failed: %d", err);
    }
    return err;
}
static OSErr LowTCPPassiveOpenSyncPoll(SInt8 appPollTimeoutTicks, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbOpen;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    memset(&pbOpen, 0, sizeof(TCPiopb));
    pbOpen.csParam.open.ulpTimeoutValue = kTCPPassiveOpenULPTimeoutSeconds;
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue;
    pbOpen.csParam.open.commandTimeoutValue = 2;
    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pbOpen.csParam.open.localPort = PORT_TCP;
    pbOpen.csParam.open.localHost = 0L;
    pbOpen.csParam.open.remoteHost = 0L;
    pbOpen.csParam.open.remotePort = 0;
    pbOpen.csParam.open.tosFlags = 0;
    pbOpen.csParam.open.precedence = 0;
    pbOpen.csParam.open.dontFrag = false;
    pbOpen.csParam.open.timeToLive = 0;
    pbOpen.csParam.open.security = 0;
    pbOpen.csParam.open.optionCnt = 0;
    err = LowLevelSyncPoll(&pbOpen, giveTime, TCPPassiveOpen, appPollTimeoutTicks);
    if (err == noErr) {
        gPeerIP = pbOpen.csParam.open.remoteHost;
        gPeerPort = pbOpen.csParam.open.remotePort;
    } else {
        gPeerIP = 0; gPeerPort = 0;
    }
    return err;
}
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicksForCall, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime) {
    TCPiopb pbOpen;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    memset(&pbOpen, 0, sizeof(TCPiopb));
    pbOpen.csParam.open.ulpTimeoutValue = (Byte)(ulpTimeoutTicksForCall / 60);
    if (pbOpen.csParam.open.ulpTimeoutValue == 0) pbOpen.csParam.open.ulpTimeoutValue = 1;
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue;
    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pbOpen.csParam.open.commandTimeoutValue = 0;
    pbOpen.csParam.open.remoteHost = remoteHost;
    pbOpen.csParam.open.remotePort = remotePort;
    pbOpen.csParam.open.localPort = 0;
    pbOpen.csParam.open.localHost = 0L;
    pbOpen.csParam.open.tosFlags = 0;
    pbOpen.csParam.open.precedence = 0;
    pbOpen.csParam.open.dontFrag = false;
    pbOpen.csParam.open.timeToLive = 0;
    pbOpen.csParam.open.security = 0;
    pbOpen.csParam.open.optionCnt = 0;
    SInt8 appPollTimeout = ulpTimeoutTicksForCall + 60;
    return LowLevelSyncPoll(&pbOpen, giveTime, TCPActiveOpen, appPollTimeout);
}
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicksForCall, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime) {
    TCPiopb pbSend;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (wdsPtr == NULL) return kInvalidWDSErr;
    memset(&pbSend, 0, sizeof(TCPiopb));
    pbSend.csParam.send.ulpTimeoutValue = (Byte)(ulpTimeoutTicksForCall / 60);
    if (pbSend.csParam.send.ulpTimeoutValue == 0) pbSend.csParam.send.ulpTimeoutValue = 1;
    pbSend.csParam.send.ulpTimeoutAction = AbortTrue;
    pbSend.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pbSend.csParam.send.pushFlag = push;
    pbSend.csParam.send.urgentFlag = false;
    pbSend.csParam.send.wdsPtr = wdsPtr;
    SInt8 appPollTimeout = ulpTimeoutTicksForCall + 60;
    return LowLevelSyncPoll(&pbSend, giveTime, TCPSend, appPollTimeout);
}
static OSErr LowTCPRcvSyncPoll(SInt8 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbRcv;
    unsigned short initialBufferLen;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
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
static OSErr LowTCPStatusSyncPoll(SInt8 appPollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState) {
    OSErr err;
    TCPiopb pbStat;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (amtUnread == NULL || connState == NULL) return paramErr;
    memset(&pbStat, 0, sizeof(TCPiopb));
    err = LowLevelSyncPoll(&pbStat, giveTime, TCPStatus, appPollTimeoutTicks);
    if (err == noErr) {
        *amtUnread = pbStat.csParam.status.amtUnreadData;
        *connState = pbStat.csParam.status.connectionState;
    } else {
        *amtUnread = 0;
        *connState = 0;
        log_message("Warning (LowTCPStatusSyncPoll): Failed: %d", err);
        if (err == kInvalidStreamPtrErr) err = kConnectionDoesntExistErr;
    }
    return err;
}
static OSErr LowTCPAbortSyncPoll(SInt8 ulpTimeoutTicksForAbort, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbAbort;
    if (gTCPStream == NULL) {
        log_to_file_only("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort.");
        return noErr;
    }
    memset(&pbAbort, 0, sizeof(TCPiopb));
    err = LowLevelSyncPoll(&pbAbort, giveTime, TCPAbort, ulpTimeoutTicksForAbort);
    if (err == kConnectionDoesntExistErr || err == kInvalidStreamPtrErr) {
        log_to_file_only("LowTCPAbortSyncPoll: Abort completed (connection doesn't exist or stream invalid). Result: %d. Considered OK for reset.", err);
        err = noErr;
    } else if (err != noErr) {
        log_message("Warning (LowTCPAbortSyncPoll): Abort poll failed with error: %d", err);
    } else {
        log_to_file_only("LowTCPAbortSyncPoll: Abort poll successful.");
    }
    return err;
}
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamToRelease) {
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
        log_message("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err);
    } else if (err == kInvalidStreamPtrErr) {
        log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid or released. Error: %d", (unsigned long)streamToRelease, err);
        err = noErr;
    }
    return err;
}
