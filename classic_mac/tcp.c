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
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define kTCPRecvBufferSize 8192
#define kTCPListenInternalBufferSize 8192
#define kTCPSenderInternalBufferSize 2048
#define kTCPListenRetryDelayTicks 60
#define kShutdownConnectTimeoutTicks 180
#define kShutdownSendTimeoutTicks 120
#define kRuntimeConnectTimeoutTicks 300
#define kRuntimeSendTimeoutTicks 180
#define AbortTrue 1
static StreamPtr gTCPListenStream = NULL;
static Ptr gTCPListenInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPiopb gTCPListenPB;
static TCPiopb gTCPRecvPB;
static TCPiopb gTCPClosePB;
static Boolean gTCPListenPending = false;
static Boolean gTCPRecvPending = false;
static Boolean gTCPClosePending = false;
static Boolean gNeedToReListen = false;
ip_addr gCurrentConnectionIP = 0;
tcp_port gCurrentConnectionPort = 0;
Boolean gIsConnectionActive = false;
static OSErr StartAsyncTCPListen(short macTCPRefNum);
static OSErr StartAsyncTCPRecv(short macTCPRefNum);
static OSErr StartAsyncTCPClose(short macTCPRefNum, Boolean abortConnection);
static void ProcessTCPReceive(short macTCPRefNum);
static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost, tcp_port remotePort, ip_addr *localHost, tcp_port *localPort, GiveTimePtr giveTime);
static OSErr LowTCPSendSync(StreamPtr streamPtr, SInt8 timeoutTicks, Boolean push, Boolean urgent, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPAbortSync(StreamPtr streamPtr, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context;
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_message("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) {
            UpdatePeerDisplayList(true);
        }
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
        if (gMainWindow != NULL && gPeerListHandle != NULL) {
            UpdatePeerDisplayList(true);
        }
    }
}
OSErr InitTCPListener(short macTCPRefNum) {
    OSErr err;
    StreamPtr tempListenStream = NULL;
    log_message("Initializing TCP Listener Stream...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPListenStream != NULL) {
        log_message("Error (InitTCPListener): Already initialized?");
        return streamAlreadyOpen;
    }
    gTCPListenInternalBuffer = NewPtrClear(kTCPListenInternalBufferSize);
     if (gTCPListenInternalBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP listen internal buffer (%ld bytes).", (long)kTCPListenInternalBufferSize);
        return memFullErr;
    }
    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);
    if (gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP receive buffer (%ld bytes).", (long)kTCPRecvBufferSize);
        DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated TCP buffers (ListenInternal: %ld, Recv: %ld).",
                (long)kTCPListenInternalBufferSize, (long)kTCPRecvBufferSize);
    log_message("Creating Listener Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &tempListenStream, gTCPListenInternalBuffer, kTCPListenInternalBufferSize);
    if (err != noErr || tempListenStream == NULL) {
        log_message("Error: Failed to create Listener Stream: %d", err);
        CleanupTCPListener(macTCPRefNum);
        return err;
    }
    gTCPListenStream = tempListenStream;
    log_message("Listener Stream created successfully (StreamPtr: 0x%lX).", (unsigned long)gTCPListenStream);
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;
    err = StartAsyncTCPListen(macTCPRefNum);
    if (err != noErr && err != 1) {
         log_message("Error: Failed to start initial async TCP listen: %d. Cleaning up.", err);
         CleanupTCPListener(macTCPRefNum);
         return err;
    }
    log_message("Initial asynchronous TCP listen STARTING on port %d.", PORT_TCP);
    return noErr;
}
void CleanupTCPListener(short macTCPRefNum) {
    OSErr relErr;
    log_message("Cleaning up TCP Listener Stream...");
    StreamPtr listenStreamToRelease = gTCPListenStream;
    gTCPListenStream = NULL;
    if (gTCPListenPending && gTCPListenPB.tcpStream == listenStreamToRelease) {
        PBKillIO((ParmBlkPtr)&gTCPListenPB, false);
        log_to_file_only("CleanupTCPListener: Killed pending Listen IO.");
    }
    if (gTCPRecvPending && gTCPRecvPB.tcpStream == listenStreamToRelease) {
        PBKillIO((ParmBlkPtr)&gTCPRecvPB, false);
        log_to_file_only("CleanupTCPListener: Killed pending Receive IO.");
    }
    if (gTCPClosePending && gTCPClosePB.tcpStream == listenStreamToRelease) {
        PBKillIO((ParmBlkPtr)&gTCPClosePB, false);
        log_to_file_only("CleanupTCPListener: Killed pending Close IO.");
    }
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;
    if (listenStreamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting synchronous release of listener stream 0x%lX...", (unsigned long)listenStreamToRelease);
        relErr = LowTCPReleaseSync(macTCPRefNum, listenStreamToRelease);
        if (relErr != noErr) {
             log_message("Warning (CleanupTCPListener): Synchronous TCPRelease failed: %d", relErr);
        } else {
             log_to_file_only("CleanupTCPListener: Synchronous TCPRelease successful.");
        }
    } else if (listenStreamToRelease != NULL) {
        log_message("Warning (CleanupTCPListener): Cannot release stream, MacTCP refnum is 0.");
    }
    if (gTCPRecvBuffer != NULL) {
        log_message("Disposing TCP receive buffer at 0x%lX.", (unsigned long)gTCPRecvBuffer);
        DisposePtr(gTCPRecvBuffer);
        gTCPRecvBuffer = NULL;
    }
     if (gTCPListenInternalBuffer != NULL) {
        log_message("Disposing TCP listen internal buffer at 0x%lX.", (unsigned long)gTCPListenInternalBuffer);
        DisposePtr(gTCPListenInternalBuffer);
        gTCPListenInternalBuffer = NULL;
    }
    log_message("TCP Listener Stream cleanup finished.");
}
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr ioResult;
    if (gTCPListenPending) {
        ioResult = gTCPListenPB.ioResult;
        if (ioResult <= 0) {
            gTCPListenPending = false;
            HandleListenerCompletion(macTCPRefNum, ioResult);
        }
    }
    if (gTCPRecvPending && gIsConnectionActive) {
        if (gTCPRecvPB.tcpStream == gTCPListenStream && gTCPListenStream != NULL) {
            ioResult = gTCPRecvPB.ioResult;
            if (ioResult <= 0) {
                gTCPRecvPending = false;
                HandleReceiveCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPRecvPending) {
             log_message("Warning (PollTCPListener): Receive pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPRecvPending = false;
        }
    }
    if (gTCPClosePending) {
        if (gTCPClosePB.tcpStream == gTCPListenStream && gTCPListenStream != NULL) {
            ioResult = gTCPClosePB.ioResult;
            if (ioResult <= 0) {
                gTCPClosePending = false;
                HandleCloseCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPClosePending) {
             log_message("Warning (PollTCPListener): Async Close pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPClosePending = false;
        }
    }
    if (gNeedToReListen && gTCPListenStream != NULL &&
        !gTCPListenPending && !gTCPRecvPending && !gTCPClosePending && !gIsConnectionActive)
    {
        log_to_file_only("PollTCPListener: Conditions met to attempt re-issuing listen.");
        gNeedToReListen = false;
        OSErr err = StartAsyncTCPListen(macTCPRefNum);
        if (err != noErr && err != 1) {
            log_message("Error (PollTCPListener): Attempt to re-issue listen failed immediately. Error: %d. Will retry later.", err);
            gNeedToReListen = true;
        } else {
            log_to_file_only("PollTCPListener: Re-issued Async TCPPassiveOpen successfully (or already pending).");
        }
    } else if (gNeedToReListen && gTCPListenStream == NULL) {
        log_message("Warning (PollTCPListener): Need to re-listen but stream is NULL. Cannot proceed.");
        gNeedToReListen = false;
    }
}
static OSErr InternalSyncTCPSend(ip_addr targetIP, tcp_port targetPort,
                                 const char *messageBuffer, int messageLen,
                                 SInt8 connectTimeoutTicks, SInt8 sendTimeoutTicks,
                                 GiveTimePtr giveTime)
{
    OSErr err = noErr;
    OSErr finalErr = noErr;
    StreamPtr tempStream = NULL;
    Ptr tempConnBuffer = NULL;
    struct wdsEntry sendWDS[2];
    log_to_file_only("InternalSyncTCPSend: To IP %lu:%u (%d bytes)", (unsigned long)targetIP, targetPort, messageLen);
    tempConnBuffer = NewPtrClear(kTCPSenderInternalBufferSize);
    if (tempConnBuffer == NULL) {
        log_message("Error (InternalSyncTCPSend): Failed to allocate temporary connection buffer.");
        return memFullErr;
    }
    err = LowTCPCreateSync(gMacTCPRefNum, &tempStream, tempConnBuffer, kTCPSenderInternalBufferSize);
    if (err != noErr || tempStream == NULL) {
        log_message("Error (InternalSyncTCPSend): LowTCPCreateSync failed: %d", err);
        DisposePtr(tempConnBuffer);
        return err;
    }
    log_to_file_only("InternalSyncTCPSend: Temp stream 0x%lX created.", (unsigned long)tempStream);
    log_to_file_only("InternalSyncTCPSend: Connecting temp stream 0x%lX...", (unsigned long)tempStream);
    err = LowTCPOpenConnectionSync(tempStream, connectTimeoutTicks, targetIP, targetPort, NULL, NULL, giveTime);
    if (err == noErr) {
        log_to_file_only("InternalSyncTCPSend: Connected successfully.");
        sendWDS[0].length = messageLen;
        sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0;
        sendWDS[1].ptr = NULL;
        log_to_file_only("InternalSyncTCPSend: Sending data...");
        err = LowTCPSendSync(tempStream, sendTimeoutTicks, true, false, (Ptr)sendWDS, giveTime);
        if (err == noErr) {
            log_to_file_only("InternalSyncTCPSend: Send successful.");
        } else {
            log_message("Error (InternalSyncTCPSend): LowTCPSendSync failed: %d", err);
            finalErr = err;
        }
        log_to_file_only("InternalSyncTCPSend: Aborting connection...");
        OSErr abortErr = LowTCPAbortSync(tempStream, giveTime);
        if (abortErr != noErr) {
            log_message("Warning (InternalSyncTCPSend): LowTCPAbortSync failed: %d", abortErr);
            if (finalErr == noErr) finalErr = abortErr;
        }
    } else {
        log_message("Error (InternalSyncTCPSend): LowTCPOpenConnectionSync failed: %d", err);
        finalErr = err;
    }
    log_to_file_only("InternalSyncTCPSend: Releasing temp stream 0x%lX...", (unsigned long)tempStream);
    OSErr releaseErr = LowTCPReleaseSync(gMacTCPRefNum, tempStream);
    if (releaseErr != noErr) {
        log_message("Warning (InternalSyncTCPSend): LowTCPReleaseSync failed: %d", releaseErr);
        if (finalErr == noErr) finalErr = releaseErr;
    }
    DisposePtr(tempConnBuffer);
    log_to_file_only("InternalSyncTCPSend: Finished. Final status: %d", finalErr);
    return finalErr;
}
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime) {
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;
    log_to_file_only("TCP_SendTextMessageSync: Attempting to send TEXT to %s", peerIP);
    if (gMacTCPRefNum == 0) {
        log_message("Error (TCP_SendTextMessageSync): MacTCP not initialized.");
        return notOpenErr;
    }
    if (peerIP == NULL || message == NULL || giveTime == NULL) {
        return paramErr;
    }
    err = ParseIPv4(peerIP, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (TCP_SendTextMessageSync): Could not parse peer IP string '%s'.", peerIP);
        return paramErr;
    }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT,
                                  gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) {
        log_message("Error (TCP_SendTextMessageSync): Failed to format TEXT message for %s.", peerIP);
        return paramErr;
    }
    err = InternalSyncTCPSend(targetIP, PORT_TCP, messageBuffer, formattedLen,
                              kRuntimeConnectTimeoutTicks, kRuntimeSendTimeoutTicks, giveTime);
    if (err == noErr) {
        log_message("Successfully sent TEXT message to %s.", peerIP);
    } else {
        log_message("Failed to send TEXT message to %s (Error: %d).", peerIP, err);
    }
    return err;
}
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime) {
    int i;
    OSErr lastErr = noErr;
    char quitMessageBuffer[BUFFER_SIZE];
    int formattedLen;
    int activePeerCount = 0;
    int sentCount = 0;
    log_message("TCP_SendQuitMessagesSync: Starting...");
    if (gMacTCPRefNum == 0) {
        log_message("TCP_SendQuitMessagesSync: Cannot send, MacTCP not initialized.");
        return notOpenErr;
    }
     if (giveTime == NULL) {
        return paramErr;
    }
    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT,
                                  gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) {
        log_message("Error (TCP_SendQuitMessagesSync): Failed to format QUIT message.");
        return paramErr;
    }
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            activePeerCount++;
        }
    }
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr targetIP = 0;
            OSErr parseErr, sendErr;
            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s",
                        gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            parseErr = ParseIPv4(gPeerManager.peers[i].ip, &targetIP);
            if (parseErr != noErr || targetIP == 0) {
                log_message("Error: Could not parse peer IP string '%s'. Skipping.", gPeerManager.peers[i].ip);
                lastErr = parseErr;
                continue;
            }
            sendErr = InternalSyncTCPSend(targetIP, PORT_TCP, quitMessageBuffer, formattedLen,
                                          kShutdownConnectTimeoutTicks, kShutdownSendTimeoutTicks, giveTime);
            if (sendErr == noErr) {
                log_message("Successfully sent QUIT to %s.", gPeerManager.peers[i].ip);
                sentCount++;
            } else {
                log_message("Error sending QUIT to %s: %d", gPeerManager.peers[i].ip, sendErr);
                lastErr = sendErr;
            }
            giveTime();
        }
    }
    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers.", sentCount, activePeerCount);
    return lastErr;
}
static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr completedListenerStream = gTCPListenPB.tcpStream;
    if (completedListenerStream != gTCPListenStream || gTCPListenStream == NULL) {
        log_message("Warning (HandleListenerCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                    (unsigned long)completedListenerStream, (unsigned long)gTCPListenStream);
        return;
    }
    if (ioResult == noErr) {
        ip_addr acceptedIP = gTCPListenPB.csParam.open.remoteHost;
        tcp_port acceptedPort = gTCPListenPB.csParam.open.remotePort;
        char senderIPStr[INET_ADDRSTRLEN];
        AddrToStr(acceptedIP, senderIPStr);
        log_message("TCP Connection Indication from %s:%u on listener stream 0x%lX.",
                    senderIPStr, acceptedPort, (unsigned long)completedListenerStream);
        if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) {
             log_message("Warning: New connection indication while listener stream busy (Active: %d, RecvPending: %d, ClosePending: %d). Setting flag to re-listen later.",
                         gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
             gNeedToReListen = true;
        } else {
            gCurrentConnectionIP = acceptedIP;
            gCurrentConnectionPort = acceptedPort;
            gIsConnectionActive = true;
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
            if (err != noErr && err != 1) {
                log_message("Error (HandleListenerCompletion): Failed to start first TCPRcv on listener stream. Error: %d. Closing connection state.", err);
                gIsConnectionActive = false;
                gCurrentConnectionIP = 0;
                gCurrentConnectionPort = 0;
                gNeedToReListen = true;
            } else {
                 log_to_file_only("HandleListenerCompletion: First TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
            }
        }
    } else {
        const char *errMsg = "Unknown Error";
        Boolean isConnectionExists = (ioResult == connectionExists);
        Boolean isInvalidStream = (ioResult == invalidStreamPtr);
        Boolean isReqAborted = (ioResult == reqAborted);
        if (isConnectionExists) errMsg = "connectionExists (-23007)";
        else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isReqAborted) errMsg = "reqAborted (-23015)";
        if (isConnectionExists) {
             log_to_file_only("HandleListenerCompletion: TCPPassiveOpen completed with transient error: %d (%s). Will retry after delay.", ioResult, errMsg);
             unsigned long dummyTimer;
             Delay(kTCPListenRetryDelayTicks, &dummyTimer);
             gNeedToReListen = true;
        } else if (isReqAborted) {
             log_to_file_only("HandleListenerCompletion: TCPPassiveOpen completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
        } else {
            log_message("Error (HandleListenerCompletion): TCPPassiveOpen completed with error: %d (%s)", ioResult, errMsg);
            if (isInvalidStream) {
                 log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid. It will be cleaned up.", (unsigned long)gTCPListenStream);
                 gTCPListenStream = NULL;
                 gNeedToReListen = false;
            } else {
                 gNeedToReListen = true;
            }
        }
    }
}
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr completedRecvStream = gTCPRecvPB.tcpStream;
    if (completedRecvStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleReceiveCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)completedRecvStream, (unsigned long)gTCPListenStream);
         return;
    }
     if (!gIsConnectionActive) {
         if (ioResult == noErr) {
             log_to_file_only("Warning (HandleReceiveCompletion): Receive completed successfully but connection no longer marked active. Data processed, not re-issuing receive.");
             ProcessTCPReceive(macTCPRefNum);
         } else {
             log_to_file_only("Warning (HandleReceiveCompletion): Receive completed with error %d, connection already inactive.", ioResult);
         }
         if (ioResult != reqAborted) {
            gNeedToReListen = true;
         }
         return;
     }
    if (ioResult == noErr) {
        ProcessTCPReceive(macTCPRefNum);
        if (gIsConnectionActive && gTCPListenStream != NULL) {
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
             if (err != noErr && err != 1) {
                 log_message("Error (HandleReceiveCompletion): Failed to start next async TCP receive after successful receive. Error: %d. Closing connection.", err);
                 StartAsyncTCPClose(macTCPRefNum, true);
             } else {
                 log_to_file_only("HandleReceiveCompletion: Successfully re-issued TCPRcv on stream 0x%lX.", (unsigned long)gTCPListenStream);
             }
        } else {
             log_to_file_only("HandleReceiveCompletion: Connection stream closed/inactive or stream became NULL during processing. Not starting new receive.");
             if (gTCPListenStream != NULL && !gTCPClosePending) {
                 gNeedToReListen = true;
             }
        }
    } else if (ioResult == connectionClosing) {
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         log_message("TCP Connection closing gracefully from %s (Stream: 0x%lX).", senderIPStr, (unsigned long)completedRecvStream);
         ProcessTCPReceive(macTCPRefNum);
         gIsConnectionActive = false;
         gCurrentConnectionIP = 0;
         gCurrentConnectionPort = 0;
         gNeedToReListen = true;
    } else {
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         const char *errMsg = "Unknown Error";
         Boolean isConnTerminated = (ioResult == connectionTerminated);
         Boolean isInvalidStream = (ioResult == invalidStreamPtr);
         Boolean isReqAborted = (ioResult == reqAborted);
         if (isConnTerminated) errMsg = "connectionTerminated (-23012)";
         else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
         else if (isReqAborted) errMsg = "reqAborted (-23015)";
        if (isReqAborted) {
            log_to_file_only("HandleReceiveCompletion: Async TCPRcv completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
        } else {
            log_message("Error (HandleReceiveCompletion): Async TCPRcv completed with error: %d (%s) from %s (Stream: 0x%lX).", ioResult, errMsg, senderIPStr, (unsigned long)completedRecvStream);
        }
        gIsConnectionActive = false;
        gCurrentConnectionIP = 0;
        gCurrentConnectionPort = 0;
        if (isInvalidStream) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during receive. It will be cleaned up.", (unsigned long)gTCPListenStream);
             gTCPListenStream = NULL;
             gNeedToReListen = false;
        } else if (!isReqAborted) {
             gNeedToReListen = true;
        }
    }
}
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr closedStream = gTCPClosePB.tcpStream;
    if (closedStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleCloseCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)closedStream, (unsigned long)gTCPListenStream);
         return;
    }
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;
    if (ioResult == noErr) {
        log_to_file_only("HandleCloseCompletion: Async TCPClose completed successfully for stream 0x%lX.", (unsigned long)closedStream);
    } else {
        const char *errMsg = "Unknown Error";
        Boolean isInvalidStream = (ioResult == invalidStreamPtr);
        Boolean isReqAborted = (ioResult == reqAborted);
        if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isReqAborted) errMsg = "reqAborted (-23015)";
        if (isReqAborted) {
             log_to_file_only("HandleCloseCompletion: Async TCPClose completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
        } else {
            log_message("Error (HandleCloseCompletion): Async TCPClose for stream 0x%lX completed with error: %d (%s)", (unsigned long)closedStream, ioResult, errMsg);
        }
        if (isInvalidStream) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during close. It will be cleaned up.", (unsigned long)gTCPListenStream);
             gTCPListenStream = NULL;
             gNeedToReListen = false;
             return;
        }
    }
    if (ioResult != reqAborted) {
        log_to_file_only("HandleCloseCompletion: Setting flag to re-listen.");
        gNeedToReListen = true;
    }
}
static OSErr StartAsyncTCPListen(short macTCPRefNum) {
    OSErr err;
    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPListen): Cannot listen, gTCPListenStream is NULL.");
        return invalidStreamPtr;
    }
    if (gTCPListenPending) {
        log_to_file_only("StartAsyncTCPListen: Listen already pending on stream 0x%lX.", (unsigned long)gTCPListenStream);
        return 1;
    }
    if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) {
        log_message("Error (StartAsyncTCPListen): Cannot listen, stream 0x%lX is busy (Active: %d, RecvPending: %d, ClosePending: %d).",
                    (unsigned long)gTCPListenStream, gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
        return inProgress;
    }
    memset(&gTCPListenPB, 0, sizeof(TCPiopb));
    gTCPListenPB.ioCompletion = nil;
    gTCPListenPB.ioCRefNum = macTCPRefNum;
    gTCPListenPB.csCode = TCPPassiveOpen;
    gTCPListenPB.tcpStream = gTCPListenStream;
    gTCPListenPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    gTCPListenPB.csParam.open.ulpTimeoutValue = kTCPDefaultTimeout;
    gTCPListenPB.csParam.open.ulpTimeoutAction = AbortTrue;
    gTCPListenPB.csParam.open.commandTimeoutValue = 0;
    gTCPListenPB.csParam.open.localPort = PORT_TCP;
    gTCPListenPB.csParam.open.localHost = 0L;
    gTCPListenPB.csParam.open.remoteHost = 0L;
    gTCPListenPB.csParam.open.remotePort = 0;
    gTCPListenPB.csParam.open.userDataPtr = nil;
    gTCPListenPending = true;
    err = PBControlAsync((ParmBlkPtr)&gTCPListenPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPListen): PBControlAsync(TCPPassiveOpen) failed immediately. Error: %d", err);
        gTCPListenPending = false;
        return err;
    }
    log_to_file_only("StartAsyncTCPListen: Async TCPPassiveOpen initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1;
}
static OSErr StartAsyncTCPRecv(short macTCPRefNum) {
    OSErr err;
    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPRecv): Cannot receive, gTCPListenStream is NULL.");
        return invalidStreamPtr;
     }
    if (gTCPRecvBuffer == NULL) {
         log_message("Error (StartAsyncTCPRecv): Cannot receive, gTCPRecvBuffer is NULL.");
         return invalidBufPtr;
    }
    if (gTCPRecvPending) {
         log_to_file_only("StartAsyncTCPRecv: Receive already pending on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
         return 1;
    }
    if (!gIsConnectionActive) {
         log_to_file_only("Warning (StartAsyncTCPRecv): Cannot receive, connection not marked active on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
         return connectionDoesntExist;
    }
    if (gTCPListenPending || gTCPClosePending) {
         log_message("Error (StartAsyncTCPRecv): Cannot receive, stream 0x%lX has other pending operations (Listen: %d, Close: %d).",
                     (unsigned long)gTCPListenStream, gTCPListenPending, gTCPClosePending);
         return inProgress;
    }
    memset(&gTCPRecvPB, 0, sizeof(TCPiopb));
    gTCPRecvPB.ioCompletion = nil;
    gTCPRecvPB.ioCRefNum = macTCPRefNum;
    gTCPRecvPB.csCode = TCPRcv;
    gTCPRecvPB.tcpStream = gTCPListenStream;
    gTCPRecvPB.csParam.receive.rcvBuff = gTCPRecvBuffer;
    gTCPRecvPB.csParam.receive.rcvBuffLen = kTCPRecvBufferSize;
    gTCPRecvPB.csParam.receive.commandTimeoutValue = 0;
    gTCPRecvPB.csParam.receive.userDataPtr = nil;
    gTCPRecvPending = true;
    err = PBControlAsync((ParmBlkPtr)&gTCPRecvPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPRecv): PBControlAsync(TCPRcv) failed immediately. Error: %d", err);
        gTCPRecvPending = false;
        StartAsyncTCPClose(macTCPRefNum, true);
        return err;
    }
    log_to_file_only("StartAsyncTCPRecv: Async TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1;
}
static OSErr StartAsyncTCPClose(short macTCPRefNum, Boolean abortConnection) {
    OSErr err;
    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPClose): Cannot close NULL stream.");
        return invalidStreamPtr;
    }
    if (gTCPClosePending) {
        log_to_file_only("StartAsyncTCPClose: Close already pending for listener stream 0x%lX.", (unsigned long)gTCPListenStream);
        return 1;
    }
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;
    if (gTCPRecvPending && gTCPRecvPB.tcpStream == gTCPListenStream) {
        log_to_file_only("StartAsyncTCPClose: Killing pending receive before closing.");
        PBKillIO((ParmBlkPtr)&gTCPRecvPB, false);
        gTCPRecvPending = false;
    }
    memset(&gTCPClosePB, 0, sizeof(TCPiopb));
    gTCPClosePB.ioCompletion = nil;
    gTCPClosePB.ioCRefNum = macTCPRefNum;
    gTCPClosePB.csCode = TCPClose;
    gTCPClosePB.tcpStream = gTCPListenStream;
    gTCPClosePB.csParam.close.validityFlags = timeoutValue | timeoutAction;
    gTCPClosePB.csParam.close.ulpTimeoutValue = kTCPDefaultTimeout;
    if (abortConnection) {
        gTCPClosePB.csParam.close.ulpTimeoutAction = AbortTrue;
        log_to_file_only("StartAsyncTCPClose: Using Abort action.");
    } else {
        gTCPClosePB.csParam.close.ulpTimeoutAction = 0;
        log_to_file_only("StartAsyncTCPClose: Using Graceful close action.");
    }
    gTCPClosePB.csParam.close.userDataPtr = nil;
    gTCPClosePending = true;
    err = PBControlAsync((ParmBlkPtr)&gTCPClosePB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPClose): PBControlAsync(TCPClose) failed immediately for stream 0x%lX. Error: %d", (unsigned long)gTCPListenStream, err);
        gTCPClosePending = false;
        gNeedToReListen = true;
        return err;
    }
    log_to_file_only("StartAsyncTCPClose: Initiated async TCPClose for listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1;
}
static void ProcessTCPReceive(short macTCPRefNum) {
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    unsigned short dataLength;
    static tcp_platform_callbacks_t mac_callbacks = {
        .add_or_update_peer = mac_tcp_add_or_update_peer,
        .display_text_message = mac_tcp_display_text_message,
        .mark_peer_inactive = mac_tcp_mark_peer_inactive
    };
    if (!gIsConnectionActive || gTCPListenStream == NULL) {
        log_message("Warning (ProcessTCPReceive): Called when connection not active or listener stream is NULL.");
        return;
    }
    if (gTCPRecvPB.tcpStream != gTCPListenStream) {
        log_message("CRITICAL Warning (ProcessTCPReceive): Received data for unexpected stream 0x%lX, expected 0x%lX. Ignoring and closing.",
                    (unsigned long)gTCPRecvPB.tcpStream, (unsigned long)gTCPListenStream);
        StartAsyncTCPClose(macTCPRefNum, true);
        return;
    }
    dataLength = gTCPRecvPB.csParam.receive.rcvBuffLen;
    if (dataLength > 0) {
        OSErr addrErr = AddrToStr(gCurrentConnectionIP, senderIPStrFromConnection);
        if (addrErr != noErr) {
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu",
                    (gCurrentConnectionIP >> 24) & 0xFF, (gCurrentConnectionIP >> 16) & 0xFF,
                    (gCurrentConnectionIP >> 8) & 0xFF, gCurrentConnectionIP & 0xFF);
            log_to_file_only("ProcessTCPReceive: AddrToStr failed (%d) for sender IP %lu. Using fallback '%s'.",
                         addrErr, (unsigned long)gCurrentConnectionIP, senderIPStrFromConnection);
        }
        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (Payload IP: %s).",
                       msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);
            handle_received_tcp_message(senderIPStrFromConnection,
                                        senderUsername,
                                        msgType,
                                        content,
                                        &mac_callbacks,
                                        NULL);
            if (strcmp(msgType, MSG_QUIT) == 0) {
                 log_message("Connection to %s finishing due to received QUIT.", senderIPStrFromConnection);
                 StartAsyncTCPClose(macTCPRefNum, true);
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes. Connection likely closing.");
    }
}
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen)
{
    OSErr err;
    TCPiopb pbCreate;
    if (streamPtr == NULL || connectionBuffer == NULL) return paramErr;
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L;
    pbCreate.csParam.create.rcvBuff = connectionBuffer;
    pbCreate.csParam.create.rcvBuffLen = connBufferLen;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.userDataPtr = nil;
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    if (err == noErr) {
        *streamPtr = pbCreate.tcpStream;
        if (*streamPtr == NULL) {
            log_message("Error (LowTCPCreateSync): PBControlSync succeeded but returned NULL stream.");
            err = ioErr;
        }
    } else {
        *streamPtr = NULL;
    }
    return err;
}
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost,
                                      tcp_port remotePort, ip_addr *localHost, tcp_port *localPort,
                                      GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;
    if (giveTime == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;
    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) {
        log_message("Error (LowTCPOpenConnectionSync): Failed to allocate PB.");
        return memFullErr;
    }
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPActiveOpen;
    pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;
    pBlock->csParam.open.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.open.ulpTimeoutAction = AbortTrue;
    pBlock->csParam.open.validityFlags = timeoutValue | timeoutAction;
    pBlock->csParam.open.commandTimeoutValue = 0;
    pBlock->csParam.open.remoteHost = remoteHost;
    pBlock->csParam.open.remotePort = remotePort;
    pBlock->csParam.open.localPort = 0;
    pBlock->csParam.open.localHost = 0;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPOpenConnectionSync): PBControlAsync failed immediately: %d", err);
        DisposePtr((Ptr)pBlock);
        return err;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
    }
    err = pBlock->ioResult;
    if (err == noErr) {
        if (localHost) *localHost = pBlock->csParam.open.localHost;
        if (localPort) *localPort = pBlock->csParam.open.localPort;
    }
    DisposePtr((Ptr)pBlock);
    return err;
}
static OSErr LowTCPSendSync(StreamPtr streamPtr, SInt8 timeoutTicks, Boolean push, Boolean urgent,
                            Ptr wdsPtr, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;
    if (giveTime == NULL || wdsPtr == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;
    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) {
        log_message("Error (LowTCPSendSync): Failed to allocate PB.");
        return memFullErr;
    }
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPSend;
    pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;
    pBlock->csParam.send.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.send.ulpTimeoutAction = AbortTrue;
    pBlock->csParam.send.validityFlags = timeoutValue | timeoutAction;
    pBlock->csParam.send.pushFlag = push;
    pBlock->csParam.send.urgentFlag = urgent;
    pBlock->csParam.send.wdsPtr = wdsPtr;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPSendSync): PBControlAsync failed immediately: %d", err);
        DisposePtr((Ptr)pBlock);
        return err;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
    }
    err = pBlock->ioResult;
    DisposePtr((Ptr)pBlock);
    return err;
}
static OSErr LowTCPAbortSync(StreamPtr streamPtr, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;
    if (giveTime == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;
    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) {
        log_message("Error (LowTCPAbortSync): Failed to allocate PB.");
        return memFullErr;
    }
    pBlock->ioCompletion = nil;
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPAbort;
    pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_to_file_only("Info (LowTCPAbortSync): PBControlAsync failed immediately: %d", err);
        if (err != connectionDoesntExist && err != invalidStreamPtr) {
             log_message("Warning (LowTCPAbortSync): Unexpected immediate error: %d", err);
        }
        DisposePtr((Ptr)pBlock);
        return noErr;
    }
    while (pBlock->ioResult > 0) {
        giveTime();
    }
    err = pBlock->ioResult;
    if (err != noErr && err != connectionDoesntExist && err != invalidStreamPtr) {
         log_message("Warning (LowTCPAbortSync): Abort completed with error: %d", err);
    }
    DisposePtr((Ptr)pBlock);
    return noErr;
}
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr)
{
    OSErr err;
    TCPiopb pbRelease;
    if (streamPtr == NULL) return invalidStreamPtr;
    memset(&pbRelease, 0, sizeof(TCPiopb));
    pbRelease.ioCompletion = nil;
    pbRelease.ioCRefNum = macTCPRefNum;
    pbRelease.csCode = TCPRelease;
    pbRelease.tcpStream = streamPtr;
    err = PBControlSync((ParmBlkPtr)&pbRelease);
    if (err != noErr && err != invalidStreamPtr) {
         log_message("Warning (LowTCPReleaseSync): PBControlSync(TCPRelease) failed: %d", err);
    } else if (err == invalidStreamPtr) {
         log_to_file_only("Info (LowTCPReleaseSync): Attempted to release invalid stream 0x%lX.", (unsigned long)streamPtr);
         err = noErr;
    }
    return err;
}
