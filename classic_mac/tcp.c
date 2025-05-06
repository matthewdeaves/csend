//====================================
// FILE: ./classic_mac/tcp.c
//====================================

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
#include <OSUtils.h> // For Delay

// --- Constants ---
#define kTCPRecvBufferSize 8192
#define kTCPInternalBufferSize 8192
// *** Aggressively tuned listen poll timeout for UI responsiveness ***
#define kTCPListenPollTimeoutTicks 5      // Timeout for PassiveOpen poll (approx 1/12th sec)
#define kTCPRecvPollTimeoutTicks 1        // Short timeout for sync Rcv poll
#define kTCPStatusPollTimeoutTicks 1      // Short timeout for sync Status poll
#define kConnectTimeoutTicks 300          // ULP Timeout for TCPActiveOpen (5 sec)
#define kSendTimeoutTicks 180             // ULP Timeout for TCPSend (3 sec)
#define kAbortTimeoutTicks 60             // Timeout for abort poll (1 sec)
#define kQuitLoopDelayTicks 120           // Delay+Yield between QUIT sends (2 sec)
#define kErrorRetryDelayTicks 120         // Delay after certain recoverable errors (2 sec)

#define kMacTCPTimeoutErr (-23016)
#define AbortTrue 1

// --- Globals ---
static StreamPtr gTCPStream = NULL;
static Ptr gTCPInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static Boolean gIsSending = false;        // Lock for outgoing send operations
static ip_addr gPeerIP = 0;               // IP of current incoming connection
static tcp_port gPeerPort = 0;             // Port of current incoming connection

// --- Forward Declarations ---
static void ProcessTCPReceive(unsigned short dataLength);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPPassiveOpenSyncPoll(SInt8 timeoutTicks, GiveTimePtr giveTime);
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPRcvSyncPoll(SInt8 timeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime);
static OSErr LowTCPStatusSyncPoll(GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState);
static OSErr LowTCPAbortSyncPoll(GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);

// --- Shared Logic Callbacks (implementation unchanged) ---
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) { (void)platform_context; int addResult = AddOrUpdatePeer(ip, username); if (addResult > 0) { log_message("Peer connected/updated via TCP: %s@%s", username, ip); if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true); } else if (addResult < 0) { log_message("Peer list full, could not add/update %s@%s from TCP connection", username, ip); } return addResult; }
static void mac_tcp_display_text_message(const char* username, const char* ip, const char* message_content, void* platform_context) { (void)platform_context; (void)ip; char displayMsg[BUFFER_SIZE + 100]; if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) { sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : ""); AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r"); log_message("Message from %s@%s: %s", username, ip, message_content); } else { log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready."); } }
static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) { (void)platform_context; if (!ip) return; log_message("Peer %s has sent QUIT notification via TCP.", ip); if (MarkPeerInactive(ip)) { if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true); } }

// --- Public Functions ---
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
        CleanupTCP(macTCPRefNum); // Call full cleanup
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

    StreamPtr streamToRelease = gTCPStream; // Capture before nullifying
    TCPState stateBeforeCleanup = gTCPState;

    gTCPStream = NULL; // Prevent further use by other functions
    gTCPState = TCP_STATE_RELEASING;

    // Abort if connection might still exist (best effort sync poll)
    // Check state *before* cleanup started. Only abort if it was connected.
    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN) {
         log_message("Cleanup: Attempting synchronous abort (best effort)...");
         // Use the captured streamToRelease if gTCPStream was nulled for safety
         // but LowTCPAbortSyncPoll uses the global gTCPStream.
         // Temporarily restore gTCPStream for abort if it was nulled too early.
         // This is tricky. The design of LowTCPAbortSyncPoll relies on gTCPStream.
         // Let's ensure gTCPStream is valid for LowTCPAbortSyncPoll.
         if (streamToRelease != NULL) { // Only if there was a stream to begin with
             StreamPtr currentGlobalStream = gTCPStream; // Should be NULL now
             gTCPStream = streamToRelease; // Temporarily restore for abort
             LowTCPAbortSyncPoll(YieldTimeToSystem);
             gTCPStream = currentGlobalStream; // Set back to NULL (or whatever it was)
         }
    }

    // Release stream synchronously
    if (streamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting sync release of stream 0x%lX...", (unsigned long)streamToRelease);
        OSErr relErr = LowTCPReleaseSync(macTCPRefNum, streamToRelease); // Use captured stream
        if (relErr != noErr) log_message("Warning: Sync release failed: %d", relErr);
        else log_to_file_only("Sync release successful.");
    } else if (streamToRelease != NULL) {
        log_message("Warning: Cannot release stream, MacTCP refnum is 0.");
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
    Byte connectionState = 0; // MacTypes.h Byte
    unsigned long dummyTimer;

    if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) {
        return;
    }
    if (gIsSending) { // Don't interfere with outgoing send operations (which are synchronous)
        return;
    }

    switch (gTCPState) {
        case TCP_STATE_IDLE:
            log_to_file_only("PollTCP: State IDLE. Attempting Passive Open Poll...");
            gTCPState = TCP_STATE_LISTENING_POLL; // Tentative state
            err = LowTCPPassiveOpenSyncPoll(kTCPListenPollTimeoutTicks, giveTime);

            if (err == noErr) {
                char senderIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, senderIPStr);
                log_message("PollTCP: Incoming connection from %s:%u.", senderIPStr, gPeerPort);
                gTCPState = TCP_STATE_CONNECTED_IN;
                goto CheckConnectedInData; // Check for data in the same poll cycle
            } else if (err == commandTimeout) {
                log_to_file_only("PollTCP: Passive Open Poll timed out. Returning to IDLE.");
                gTCPState = TCP_STATE_IDLE;
            } else {
                log_message("PollTCP: Passive Open Poll failed: %d. Returning to IDLE.", err);
                gTCPState = TCP_STATE_IDLE;
                if (err == duplicateSocket || err == connectionExists) { // -23007 or -23008
                     log_message("PollTCP: Delaying %d ticks due to error %d.", kErrorRetryDelayTicks, err);
                     Delay(kErrorRetryDelayTicks, &dummyTimer);
                }
            }
            break;

        case TCP_STATE_LISTENING_POLL: // Should not be reached if LowTCPPassiveOpenSyncPoll is truly synchronous
             log_message("PollTCP: WARNING - Reached LISTENING_POLL state unexpectedly.");
             gTCPState = TCP_STATE_IDLE;
             break;

        case TCP_STATE_CONNECTED_IN:
CheckConnectedInData:
            log_to_file_only("PollTCP: State CONNECTED_IN. Checking status...");
            err = LowTCPStatusSyncPoll(giveTime, &amountUnread, &connectionState);

            if (err != noErr) {
                log_message("PollTCP: Error getting status while CONNECTED_IN: %d. Aborting.", err);
                LowTCPAbortSyncPoll(giveTime); // Best effort abort
                gTCPState = TCP_STATE_IDLE;
                break;
            }

            // TCP connection states: 8=Established, 10=FIN_WAIT_1, 12=FIN_WAIT_2, 14=CLOSE_WAIT
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
                    // Stay in CONNECTED_IN state
                } else if (err == connectionClosing) {
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll indicated connection closing by peer %s. Processing final %u bytes.", peerIPStr, bytesToRead);
                    ProcessTCPReceive(bytesToRead); // Process final data
                    gTCPState = TCP_STATE_IDLE;     // Return to idle
                } else if (err == commandTimeout) {
                     log_to_file_only("PollTCP: Rcv poll timed out despite status showing data?");
                } else { // e.g., connectionTerminated
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll failed for %s: %d. Aborting.", peerIPStr, err);
                    LowTCPAbortSyncPoll(giveTime);
                    gTCPState = TCP_STATE_IDLE;
                }
            } else { // No data to read
                 if (connectionState == 14 /*CloseWait*/) {
                     char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                     log_message("PollTCP: Peer %s has closed (State: CLOSE_WAIT). Returning to IDLE.", peerIPStr);
                     gTCPState = TCP_STATE_IDLE;
                 }
            }
            break;
        default:
            log_message("PollTCP: In unexpected state %d.", gTCPState);
            gTCPState = TCP_STATE_IDLE; // Try to recover
            break;
    }
}

TCPState GetTCPState(void) {
    return gTCPState;
}

OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime) {
    OSErr err = noErr, finalErr = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;
    struct wdsEntry sendWDS[2];

    log_to_file_only("TCP_SendTextMessageSync: Request to send TEXT to %s", peerIP);

    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return invalidStreamPtr;
    if (peerIP == NULL || message == NULL || giveTime == NULL) return paramErr;

    if (gIsSending) { log_message("Warning (SendText): Send already in progress."); return streamBusyErr; }
    if (gTCPState != TCP_STATE_IDLE) { log_message("Warning (SendText): Stream not IDLE (state %d), cannot send.", gTCPState); return streamBusyErr; }
    gIsSending = true;
    // No gTCPState change here, send is a sequence of sync calls within this function.

    err = ParseIPv4(peerIP, &targetIP);
    if (err != noErr || targetIP == 0) { finalErr = paramErr; goto SendTextCleanup; }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT, gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) { finalErr = paramErr; goto SendTextCleanup; }

    log_to_file_only("SendText: Connecting...");
    err = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, targetIP, PORT_TCP, giveTime);
    if (err == noErr) {
        log_to_file_only("SendText: Connected successfully.");
        sendWDS[0].length = formattedLen; sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0; sendWDS[1].ptr = NULL;
        log_to_file_only("SendText: Sending data...");
        err = LowTCPSendSyncPoll(kSendTimeoutTicks, true, (Ptr)sendWDS, giveTime);
        if (err != noErr) { log_message("Error (SendText): Send failed: %d", err); finalErr = err; }
        else { log_to_file_only("SendText: Send successful."); }
        log_to_file_only("SendText: Aborting connection...");
        OSErr abortErr = LowTCPAbortSyncPoll(giveTime);
        if (abortErr != noErr) { log_message("Warning (SendText): Abort failed: %d", abortErr); if (finalErr == noErr) finalErr = abortErr; }
    } else { log_message("Error (SendText): Connect failed: %d", err); finalErr = err; }

SendTextCleanup:
    // gTCPState should remain IDLE or be forced to IDLE by abort.
    gIsSending = false;
    log_to_file_only("TCP_SendTextMessageSync: Released send lock. Final Status: %d.", finalErr);
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
    if (gTCPStream == NULL) return invalidStreamPtr;
    if (giveTime == NULL) return paramErr;

    if (gIsSending) { log_message("Warning (SendQuit): Send already in progress."); return streamBusyErr; }
    if (gTCPState != TCP_STATE_IDLE) { log_message("Warning (SendQuit): Stream not IDLE (state %d), cannot send.", gTCPState); return streamBusyErr; }
    gIsSending = true;

    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT, gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) { lastErr = paramErr; goto SendQuitCleanup; }

    for (i = 0; i < MAX_PEERS; ++i) if (gPeerManager.peers[i].active) activePeerCount++;
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);
    if (activePeerCount == 0) { lastErr = noErr; goto SendQuitCleanup; }

    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr currentTargetIP = 0;
            currentErr = noErr;
            if (gTCPState != TCP_STATE_IDLE) { log_message("CRITICAL (SendQuit): State not IDLE (%d) before peer %s. Aborting loop.", gTCPState, gPeerManager.peers[i].ip); lastErr = ioErr; break; }

            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            currentErr = ParseIPv4(gPeerManager.peers[i].ip, &currentTargetIP);
            if (currentErr != noErr || currentTargetIP == 0) { log_message("Error (SendQuit): Could not parse IP '%s'. Skipping.", gPeerManager.peers[i].ip); if (lastErr == noErr) lastErr = currentErr; goto NextPeerDelay; }

            log_to_file_only("SendQuit: Connecting to %s...", gPeerManager.peers[i].ip);
            currentErr = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, currentTargetIP, PORT_TCP, giveTime);

            // *** MODIFIED: Handle -23007 gracefully ***
            if (currentErr == connectionExists) { // -23007
                log_message("SendQuit: Connect to %s failed with -23007 (connectionExists). Peer likely just disconnected or in TIME_WAIT. Skipping QUIT.", gPeerManager.peers[i].ip);
                // Do not set lastErr here, allow other peers to be tried.
            } else if (currentErr == noErr) {
                log_to_file_only("SendQuit: Connected successfully.");
                sendWDS[0].length = formattedLen; sendWDS[0].ptr = (Ptr)quitMessageBuffer;
                sendWDS[1].length = 0; sendWDS[1].ptr = NULL;
                log_to_file_only("SendQuit: Sending data...");
                currentErr = LowTCPSendSyncPoll(kSendTimeoutTicks, true, (Ptr)sendWDS, giveTime);
                if (currentErr == noErr) { log_to_file_only("SendQuit: Send successful for %s.", gPeerManager.peers[i].ip); sentCount++; }
                else { log_message("Error (SendQuit): Send failed for %s: %d", gPeerManager.peers[i].ip, currentErr); if (lastErr == noErr) lastErr = currentErr; }
                log_to_file_only("SendQuit: Aborting connection...");
                OSErr abortErr = LowTCPAbortSyncPoll(giveTime);
                if (abortErr != noErr) { log_message("Warning (SendQuit): Abort failed for %s: %d", gPeerManager.peers[i].ip, abortErr); if (lastErr == noErr) lastErr = abortErr; }
            } else {
                log_message("Error (SendQuit): Connect failed for %s: %d", gPeerManager.peers[i].ip, currentErr);
                if (lastErr == noErr) lastErr = currentErr;
            }
        NextPeerDelay:
            log_to_file_only("SendQuit: Yielding/Delaying (%d ticks) after peer %s...", kQuitLoopDelayTicks, gPeerManager.peers[i].ip);
            giveTime(); Delay(kQuitLoopDelayTicks, &dummyTimer);
        }
    }
SendQuitCleanup:
    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers. Last error: %d.", sentCount, activePeerCount, lastErr);
    gIsSending = false;
    return lastErr;
}

// --- Private Helpers ---
// ProcessTCPReceive remains unchanged
static void ProcessTCPReceive(unsigned short dataLength) { char senderIPStrFromConnection[INET_ADDRSTRLEN], senderIPStrFromPayload[INET_ADDRSTRLEN]; char senderUsername[32], msgType[32], content[BUFFER_SIZE]; static tcp_platform_callbacks_t mac_callbacks = { .add_or_update_peer = mac_tcp_add_or_update_peer, .display_text_message = mac_tcp_display_text_message, .mark_peer_inactive = mac_tcp_mark_peer_inactive }; if (dataLength > 0 && gTCPRecvBuffer != NULL) { OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection); if (addrErr != noErr) sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF); if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0'; else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0'; if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) { log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection); handle_received_tcp_message(senderIPStrFromConnection, senderUsername, msgType, content, &mac_callbacks, NULL); if (strcmp(msgType, MSG_QUIT) == 0) log_message("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection); } else log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength); } else if (dataLength == 0) log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing signal)."); else log_message("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL?"); }

// LowLevelSyncPoll remains unchanged
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode) { OSErr err; if (pBlock == NULL || giveTime == NULL) return paramErr; pBlock->ioCompletion = nil; pBlock->ioResult = 1; pBlock->csCode = csCode; err = PBControlAsync((ParmBlkPtr)pBlock); if (err != noErr) { log_message("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err); return err; } while (pBlock->ioResult > 0) { giveTime(); } return pBlock->ioResult; }
// LowTCPCreateSync remains unchanged
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen) { OSErr err; TCPiopb pbCreate; if (streamPtr == NULL || connectionBuffer == NULL) return paramErr; memset(&pbCreate, 0, sizeof(TCPiopb)); pbCreate.ioCompletion = nil; pbCreate.ioCRefNum = macTCPRefNum; pbCreate.csCode = TCPCreate; pbCreate.tcpStream = 0L; pbCreate.csParam.create.rcvBuff = connectionBuffer; pbCreate.csParam.create.rcvBuffLen = connBufferLen; pbCreate.csParam.create.notifyProc = nil; err = PBControlSync((ParmBlkPtr)&pbCreate); if (err == noErr) { *streamPtr = pbCreate.tcpStream; if (*streamPtr == NULL) { log_message("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream."); err = ioErr; } } else { *streamPtr = NULL; log_message("Error (LowTCPCreateSync): PBControlSync failed: %d", err); } return err; }

// LowTCPPassiveOpenSyncPoll - gPeerIP/Port set here now
static OSErr LowTCPPassiveOpenSyncPoll(SInt8 timeoutTicks, GiveTimePtr giveTime) {
    OSErr err; TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr;
    memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream;
    pb.csParam.open.commandTimeoutValue = timeoutTicks; pb.csParam.open.validityFlags = 0;
    pb.csParam.open.localPort = PORT_TCP; pb.csParam.open.localHost = 0L;
    pb.csParam.open.remoteHost = 0L; pb.csParam.open.remotePort = 0;
    err = LowLevelSyncPoll(&pb, giveTime, TCPPassiveOpen);
    if (err == noErr) { gPeerIP = pb.csParam.open.remoteHost; gPeerPort = pb.csParam.open.remotePort; } // Store peer info
    else { gPeerIP = 0; gPeerPort = 0; }
    return err;
}

// LowTCPActiveOpenSyncPoll remains unchanged
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime) { TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; pb.csParam.open.ulpTimeoutValue = ulpTimeoutTicks; pb.csParam.open.ulpTimeoutAction = AbortTrue; pb.csParam.open.validityFlags = timeoutValue | timeoutAction; pb.csParam.open.commandTimeoutValue = 0; pb.csParam.open.remoteHost = remoteHost; pb.csParam.open.remotePort = remotePort; pb.csParam.open.localPort = 0; pb.csParam.open.localHost = 0; return LowLevelSyncPoll(&pb, giveTime, TCPActiveOpen); }
// LowTCPSendSyncPoll remains unchanged
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime) { TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr; if (wdsPtr == NULL) return invalidWDS; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; pb.csParam.send.ulpTimeoutValue = ulpTimeoutTicks; pb.csParam.send.ulpTimeoutAction = AbortTrue; pb.csParam.send.validityFlags = timeoutValue | timeoutAction; pb.csParam.send.pushFlag = push; pb.csParam.send.urgentFlag = false; pb.csParam.send.wdsPtr = wdsPtr; return LowLevelSyncPoll(&pb, giveTime, TCPSend); }
// LowTCPRcvSyncPoll remains unchanged
static OSErr LowTCPRcvSyncPoll(SInt8 timeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime) { OSErr err; TCPiopb pb; unsigned short initialBufferLen; if (gTCPStream == NULL) return invalidStreamPtr; if (buffer == NULL || bufferLen == NULL || *bufferLen == 0) return invalidBufPtr; if (markFlag == NULL || urgentFlag == NULL) return paramErr; initialBufferLen = *bufferLen; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; pb.csParam.receive.commandTimeoutValue = timeoutTicks; pb.csParam.receive.rcvBuff = buffer; pb.csParam.receive.rcvBuffLen = initialBufferLen; err = LowLevelSyncPoll(&pb, giveTime, TCPRcv); *bufferLen = pb.csParam.receive.rcvBuffLen; *markFlag = pb.csParam.receive.markFlag; *urgentFlag = pb.csParam.receive.urgentFlag; return err; }
// LowTCPStatusSyncPoll remains unchanged
static OSErr LowTCPStatusSyncPoll(GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState) { OSErr err; TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr; if (amtUnread == NULL || connState == NULL) return paramErr; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; err = LowLevelSyncPoll(&pb, giveTime, TCPStatus); if (err == noErr) { *amtUnread = pb.csParam.status.amtUnreadData; *connState = pb.csParam.status.connectionState; } else { *amtUnread = 0; *connState = 0; log_message("Warning (LowTCPStatusSyncPoll): Failed: %d", err); if (err == invalidStreamPtr) err = connectionDoesntExist; } return err; }
// LowTCPAbortSyncPoll remains unchanged
static OSErr LowTCPAbortSyncPoll(GiveTimePtr giveTime) { OSErr err; TCPiopb pb; if (gTCPStream == NULL) { log_to_file_only("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort."); return noErr; } memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; err = LowLevelSyncPoll(&pb, giveTime, TCPAbort); if (err == connectionDoesntExist || err == invalidStreamPtr) { log_to_file_only("LowTCPAbortSyncPoll: Abort completed (conn not exist/invalid stream). Result: %d", err); err = noErr; } else if (err != noErr) { log_message("Warning (LowTCPAbortSyncPoll): Abort poll failed: %d", err); } else { log_to_file_only("LowTCPAbortSyncPoll: Abort poll successful."); } return err; }
// LowTCPReleaseSync remains unchanged
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr) { OSErr err; TCPiopb pbRelease; if (streamPtr == NULL) return invalidStreamPtr; memset(&pbRelease, 0, sizeof(TCPiopb)); pbRelease.ioCompletion = nil; pbRelease.ioCRefNum = macTCPRefNum; pbRelease.csCode = TCPRelease; pbRelease.tcpStream = streamPtr; err = PBControlSync((ParmBlkPtr)&pbRelease); if (err != noErr && err != invalidStreamPtr) { log_message("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err); } else if (err == invalidStreamPtr) { log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid.", (unsigned long)streamPtr); err = noErr; } return err; }