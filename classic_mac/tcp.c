//====================================
// FILE: ./classic_mac/tcp.c
//====================================

#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "network.h" // For gMacTCPRefNum, gMyLocalIPStr, gMyUsername, ParseIPv4
#include "../shared/messaging_logic.h" // For handle_received_tcp_message

#include <Devices.h>
#include <Errors.h>
#include <MacTypes.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Events.h>  // For Delay
#include <OSUtils.h> // For TickCount if needed, though Delay takes dummy timer

// MacTCP Constants (ensure these are defined or use MacTCP headers if available)
#define kTCPRecvBufferSize 8192
#define kTCPInternalBufferSize 8192 // For TCPCreate's rcvBuff

// Timeouts, chosen empirically or from documentation recommendations
#define kTCPPassiveOpenULPTimeoutSeconds 2 // ULP timeout for passive open (TCP default)
#define kTCPListenPollTimeoutTicks 150      // How long OUR app polls a single PassiveOpen call
#define kTCPRecvPollTimeoutTicks 1        // How long OUR app polls a single Rcv Call
#define kTCPStatusPollTimeoutTicks 1      // How long OUR app polls a single Status call

#define kConnectTimeoutTicks 300    // Approx 5 seconds for ActiveOpen ULP Timeout
#define kSendTimeoutTicks 180       // Approx 3 seconds for Send ULP Timeout
#define kAbortTimeoutTicks 60      // Approx 1 second for Abort ULP timeout
#define kQuitLoopDelayTicks 120    // Approx 2 seconds delay between sending QUIT to peers
#define kErrorRetryDelayTicks 120   // Approx 2 seconds delay after certain recoverable errors

// MacTCP Error Codes (from MacTCPCommontypes.h or equivalent documentation)
#define kMacTCPTimeoutErr (-23016)  // commandTimeout
#define kDuplicateSocketErr (-23017) // duplicateSocket
#define kConnectionExistsErr (-23007) // connectionExists
#define kConnectionClosingErr (-23005) // connectionClosing
#define kConnectionDoesntExistErr (-23008) // connectionDoesntExist
#define kInvalidStreamPtrErr (-23010) // invalidStreamPtr
#define kInvalidWDSErr (-23014)       // Use for invalidWDS checks
#define kInvalidBufPtrErr (-23013)    // Use for invalidBufPtr checks
#define AbortTrue 1 // For ULP Timeout Action

// --- TCP State Machine and Globals ---
static StreamPtr gTCPStream = NULL;         // Our single TCP stream
static Ptr gTCPInternalBuffer = NULL;   // Buffer for TCPCreate (manages connection state)
static Ptr gTCPRecvBuffer = NULL;       // Our application buffer for TCPRcv
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static Boolean gIsSending = false;          // Simple mutex for send operations
static ip_addr gPeerIP = 0;             // IP of the currently connected peer (for incoming)
static tcp_port gPeerPort = 0;           // Port of the currently connected peer (for incoming)

// Forward declarations for static helper functions
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


// --- Platform Callbacks for Shared Messaging Logic ---
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context; // Unused
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) { // New peer added
        log_message("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult < 0) { // List full or other error
        log_message("Peer list full, could not add/update %s@%s from TCP connection", username, ip);
    }
    return addResult;
}

static void mac_tcp_display_text_message(const char* username, const char* ip, const char* message_content, void* platform_context) {
    (void)platform_context; // Unused
    (void)ip; // IP is available if needed, but username is primary for display
    char displayMsg[BUFFER_SIZE + 100];

    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r"); // Newline for Mac TE
        log_message("Message from %s@%s: %s", username, ip, message_content);
    } else {
        log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}

static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) {
    (void)platform_context; // Unused
    if (!ip) return;
    log_message("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) {
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}

// --- TCP Initialization and Cleanup ---
OSErr InitTCP(short macTCPRefNum) {
    OSErr err;
    log_message("Initializing Single TCP Stream (Sync Poll Strategy)...");

    if (macTCPRefNum == 0) return paramErr;
    if (gTCPStream != NULL || gTCPState != TCP_STATE_UNINITIALIZED) {
        log_message("Error (InitTCP): Already initialized or in unexpected state (%d)?", gTCPState);
        return streamAlreadyOpen; // -23011
    }

    // Allocate buffers
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
        CleanupTCP(macTCPRefNum); // Cleanup partial allocations
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
    StreamPtr streamToRelease = gTCPStream; // Cache before modifying globals
    TCPState stateBeforeCleanup = gTCPState;

    // Tentatively set state to prevent re-entry or further operations
    gTCPState = TCP_STATE_RELEASING; 
    gTCPStream = NULL; // Mark stream as unusable *before* trying to release

    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN || stateBeforeCleanup == TCP_STATE_LISTENING_POLL) {
         log_message("Cleanup: Attempting synchronous abort (best effort)...");
         if (streamToRelease != NULL) {
             // Temporarily restore gTCPStream for LowTCPAbortSyncPoll
             StreamPtr currentGlobalStream = gTCPStream; 
             gTCPStream = streamToRelease; 
             LowTCPAbortSyncPoll(kAbortTimeoutTicks, YieldTimeToSystem);
             gTCPStream = currentGlobalStream; // Restore (now NULL)
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

    // Final state and resource cleanup
    gTCPState = TCP_STATE_UNINITIALIZED;
    gIsSending = false;
    gPeerIP = 0; gPeerPort = 0;

    if (gTCPRecvBuffer != NULL) { DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL; }
    if (gTCPInternalBuffer != NULL) { DisposePtr(gTCPInternalBuffer); gTCPInternalBuffer = NULL; }
    log_message("TCP cleanup finished.");
}


// --- Main TCP Polling Logic ---
void PollTCP(GiveTimePtr giveTime) {
    OSErr err;
    unsigned short amountUnread = 0;
    Byte connectionState = 0; // MacTCP connection state (0=closed, 2=listen, 8=established, etc.)
    unsigned long dummyTimer; // For Delay()

    if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) {
        return; // Not in a pollable state
    }
    if (gIsSending) { return; } // Don't interfere with an active send operation

    switch (gTCPState) {
        case TCP_STATE_IDLE:
            log_to_file_only("PollTCP: State IDLE. Attempting Passive Open Poll (ULP: %ds, AppPoll: %d ticks)...", kTCPPassiveOpenULPTimeoutSeconds, kTCPListenPollTimeoutTicks);
            err = LowTCPPassiveOpenSyncPoll(kTCPListenPollTimeoutTicks, giveTime);
            if (err == noErr) {
                char senderIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, senderIPStr); // gPeerIP set by LowTCPPassiveOpenSyncPoll
                log_message("PollTCP: Incoming connection from %s:%u.", senderIPStr, gPeerPort);
                gTCPState = TCP_STATE_CONNECTED_IN;
                goto CheckConnectedInData; // Immediately check this new connection for data
            } else if (err == commandTimeout) { // -23016
                log_to_file_only("PollTCP: Passive Open Poll window (%d ticks) timed out. No connection. Returning to IDLE.", kTCPListenPollTimeoutTicks);
                gTCPState = TCP_STATE_IDLE; // Remain idle
            } else if (err == kDuplicateSocketErr || err == kConnectionExistsErr) { // -23017 or -23007
                log_message("PollTCP: Passive Open Poll failed with %d. Attempting to Abort stream to reset.", err);
                // ** NEW LOGIC FOR Issue 2 **
                // Abort the stream to clear the problematic state before trying passive open again.
                OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                if (abortErr == noErr) {
                    log_message("PollTCP: Abort successful after Passive Open failure. Will retry passive open.");
                } else {
                    log_message("PollTCP: CRITICAL - Abort FAILED (%d) after Passive Open failure. TCP might be stuck.", abortErr);
                    // Consider moving to TCP_STATE_ERROR if abort repeatedly fails.
                }
                gTCPState = TCP_STATE_IDLE; // Remain idle, will retry passive open next cycle
                log_message("PollTCP: Delaying %d ticks due to error %d before retrying passive open.", kErrorRetryDelayTicks, err);
                Delay(kErrorRetryDelayTicks, &dummyTimer);
            } else {
                log_message("PollTCP: Passive Open Poll failed with other error: %d. Returning to IDLE.", err);
                gTCPState = TCP_STATE_IDLE; // Remain idle
            }
            break;

        case TCP_STATE_CONNECTED_IN:
CheckConnectedInData: // Label for goto from successful passive open
            log_to_file_only("PollTCP: State CONNECTED_IN. Checking status...");
            err = LowTCPStatusSyncPoll(kTCPStatusPollTimeoutTicks, giveTime, &amountUnread, &connectionState);
            if (err != noErr) {
                log_message("PollTCP: Error getting status while CONNECTED_IN: %d. Aborting.", err);
                LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime); // Attempt to clean up
                gTCPState = TCP_STATE_IDLE; // Reset state machine
                break;
            }

            // MacTCP States: 8 (Established), 10 (FIN_WAIT_1), 12 (FIN_WAIT_2), 14 (CLOSE_WAIT)
            // We are interested if it's NOT one of these active/closing states.
            if (connectionState != 8 && connectionState != 10 && connectionState != 12 && connectionState != 14) {
                 char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                 log_message("PollTCP: Connection state is %d (not Established/Closing) for %s. Assuming closed/aborted. Returning to IDLE.", connectionState, peerIPStr);
                 gTCPState = TCP_STATE_IDLE; // Connection is no longer valid
                 break;
            }
            
            log_to_file_only("PollTCP: Status OK (State %d). Unread data: %u bytes.", connectionState, amountUnread);
            if (amountUnread > 0) {
                unsigned short bytesToRead = kTCPRecvBufferSize; // Max to read in one go
                Boolean markFlag = false, urgentFlag = false;
                log_to_file_only("PollTCP: Attempting synchronous Rcv poll...");
                err = LowTCPRcvSyncPoll(kTCPRecvPollTimeoutTicks, gTCPRecvBuffer, &bytesToRead, &markFlag, &urgentFlag, giveTime);
                
                if (err == noErr) {
                    log_to_file_only("PollTCP: Rcv poll got %u bytes.", bytesToRead);
                    ProcessTCPReceive(bytesToRead);
                    // Remain in TCP_STATE_CONNECTED_IN to check for more data or state changes
                } else if (err == kConnectionClosingErr) { // -23005 "all data on this connection has already been delivered"
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll indicated connection closing by peer %s. Processing final %u bytes.", peerIPStr, bytesToRead);
                    if (bytesToRead > 0) ProcessTCPReceive(bytesToRead); // Process any last bytes
                    gTCPState = TCP_STATE_IDLE; // Connection is gracefully closing from peer end
                } else if (err == commandTimeout) { // -23016
                     log_to_file_only("PollTCP: Rcv poll timed out despite status showing data? Odd. Will retry status.");
                     // Remain in TCP_STATE_CONNECTED_IN
                } else { // Other errors
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll failed for %s: %d. Aborting.", peerIPStr, err);
                    LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime); // Attempt to clean up
                    gTCPState = TCP_STATE_IDLE; // Reset state machine
                }
            } else { // No unread data
                 if (connectionState == 14 ) { // CLOSE_WAIT: peer has closed, waiting for us to close
                     char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                     log_message("PollTCP: Peer %s has closed (State: CLOSE_WAIT). Returning to IDLE.", peerIPStr);
                     // MacTCP should transition out of this, or we might need to TCPClose then TCPAbort if it lingers.
                     // For now, returning to IDLE will attempt a Passive Open, which might clear this state
                     // or we might need a TCPClose here.
                     // Let's try aborting our end to fully close it.
                     LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                     gTCPState = TCP_STATE_IDLE;
                 }
                 // If state is 8, 10, 12 and no data, just continue polling status.
            }
            break;

        // Other states (LISTENING_POLL, ERROR, RELEASING, UNINITIALIZED) are handled by the initial checks or not directly polled here.
        default:
            log_message("PollTCP: In unexpected state %d.", gTCPState);
            gTCPState = TCP_STATE_IDLE; // Attempt to recover
            break;
    }
}

TCPState GetTCPState(void) { return gTCPState; }

// --- TCP Send Operations ---
OSErr TCP_SendTextMessageSync(const char *peerIPStr, const char *message, GiveTimePtr giveTime) {
    OSErr err = noErr, finalErr = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;
    struct wdsEntry sendWDS[2]; // Simple WDS: one buffer for message, one terminator

    log_to_file_only("TCP_SendTextMessageSync: Request to send TEXT to %s", peerIPStr);

    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (peerIPStr == NULL || message == NULL || giveTime == NULL) return paramErr;

    if (gIsSending) { 
        log_message("Warning (SendText): Send already in progress."); 
        return streamBusyErr; // Or a specific "busy" error
    }
    if (gTCPState != TCP_STATE_IDLE) { 
        log_message("Warning (SendText): Stream not IDLE (state %d), cannot send.", gTCPState); 
        return streamBusyErr; // Or a specific "state" error
    }

    gIsSending = true; // Acquire "lock"

    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (SendText): Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendTextCleanup;
    }

    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT, gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) {
        log_message("Error (SendText): format_message failed.");
        finalErr = paramErr; // Or a specific formatting error
        goto SendTextCleanup;
    }

    log_to_file_only("SendText: Connecting to %s...", peerIPStr);
    err = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, targetIP, PORT_TCP, giveTime);
    if (err == noErr) {
        log_to_file_only("SendText: Connected successfully to %s.", peerIPStr);
        sendWDS[0].length = formattedLen; // MacTCP expects length of data, not including null terminator for strings
        sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0; // Terminator for WDS
        sendWDS[1].ptr = NULL;

        log_to_file_only("SendText: Sending data (%d bytes)...", formattedLen);
        err = LowTCPSendSyncPoll(kSendTimeoutTicks, true /*pushFlag*/, (Ptr)sendWDS, giveTime);
        if (err != noErr) {
            log_message("Error (SendText): Send failed to %s: %d", peerIPStr, err);
            finalErr = err;
        } else {
            log_to_file_only("SendText: Send successful to %s.", peerIPStr);
        }

        // Always abort after send for this simple client model
        log_to_file_only("SendText: Aborting connection to %s...", peerIPStr);
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime); // Use specific abort timeout
        if (abortErr != noErr) {
            log_message("Warning (SendText): Abort failed for %s: %d", peerIPStr, abortErr);
            if (finalErr == noErr) finalErr = abortErr; // Report abort error if send was okay
        }
    } else {
        log_message("Error (SendText): Connect to %s failed: %d", peerIPStr, err);
        finalErr = err; // Store the connect error
    }

SendTextCleanup:
    gIsSending = false; // Release "lock"
    gTCPState = TCP_STATE_IDLE; // Ensure state is reset to IDLE after send attempt
    log_to_file_only("TCP_SendTextMessageSync to %s: Released send lock. Final Status: %d.", peerIPStr, finalErr);
    return finalErr;
}


OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime) {
    int i;
    OSErr lastErr = noErr, currentErr = noErr;
    char quitMessageBuffer[BUFFER_SIZE];
    int formattedLen, activePeerCount = 0, sentCount = 0;
    unsigned long dummyTimer; // For Delay()
    struct wdsEntry sendWDS[2];

    log_message("TCP_SendQuitMessagesSync: Starting...");

    if (gMacTCPRefNum == 0) return notOpenErr;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (giveTime == NULL) return paramErr;

    if (gIsSending) { 
        log_message("Warning (SendQuit): Send already in progress."); 
        return streamBusyErr; 
    }
    // Check if stream is IDLE. If not, it implies an incoming connection might be active,
    // which means we shouldn't interfere.
    if (gTCPState != TCP_STATE_IDLE) {
        log_message("Warning (SendQuit): Stream not IDLE (state %d). Cannot send QUIT now. Peer might be connecting.", gTCPState);
        return streamBusyErr;
    }
    
    gIsSending = true; // Acquire "lock"

    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT, gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) {
        log_message("Error (SendQuit): format_message for QUIT failed.");
        lastErr = paramErr;
        goto SendQuitCleanup;
    }

    for (i = 0; i < MAX_PEERS; ++i) if (gPeerManager.peers[i].active) activePeerCount++;
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);
    if (activePeerCount == 0) {
        lastErr = noErr; // No peers to notify is not an error
        goto SendQuitCleanup;
    }

    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr currentTargetIP = 0;
            currentErr = noErr; // Reset for this peer

            // Double check state before each peer, though gIsSending should protect this.
            if (gTCPState != TCP_STATE_IDLE) { 
                log_message("CRITICAL (SendQuit): State became non-IDLE (%d) during QUIT loop for peer %s. Aborting loop.", gTCPState, gPeerManager.peers[i].ip);
                if (lastErr == noErr) lastErr = ioErr; // Indicate a general I/O problem
                break; 
            }

            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            currentErr = ParseIPv4(gPeerManager.peers[i].ip, &currentTargetIP);
            if (currentErr != noErr || currentTargetIP == 0) {
                log_message("Error (SendQuit): Could not parse IP '%s'. Skipping.", gPeerManager.peers[i].ip);
                if (lastErr == noErr) lastErr = currentErr;
                goto NextPeerDelay; // Use goto for clarity to reach delay
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
                currentErr = LowTCPSendSyncPoll(kSendTimeoutTicks, true /*pushFlag*/, (Ptr)sendWDS, giveTime);
                if (currentErr == noErr) {
                    log_to_file_only("SendQuit: Send successful for %s.", gPeerManager.peers[i].ip);
                    sentCount++;
                } else {
                    log_message("Error (SendQuit): Send failed for %s: %d", gPeerManager.peers[i].ip, currentErr);
                    if (lastErr == noErr) lastErr = currentErr; // Capture first send error
                }
                
                log_to_file_only("SendQuit: Aborting connection to %s...", gPeerManager.peers[i].ip);
                OSErr abortErr = LowTCPAbortSyncPoll(kAbortTimeoutTicks, giveTime);
                if (abortErr != noErr) {
                    log_message("Warning (SendQuit): Abort failed for %s: %d", gPeerManager.peers[i].ip, abortErr);
                    if (lastErr == noErr) lastErr = abortErr; // Capture first abort error
                }
            } else { // ActiveOpen failed
                log_message("Error (SendQuit): Connect failed for %s: %d", gPeerManager.peers[i].ip, currentErr);
                // **NEW LOGIC FOR Issue 4**
                if (lastErr == noErr) lastErr = currentErr; // Capture the connect error
                // Special handling for connectionExists during QUIT send
                if (currentErr == kConnectionExistsErr) {
                     log_message("SendQuit: Connect to %s failed with -23007 (connectionExists). Peer likely just disconnected or in TIME_WAIT. Skipping QUIT.", gPeerManager.peers[i].ip);
                     // Don't treat this specific error as a failure that prevents other QUITs necessarily,
                     // but it should be reflected in lastErr if no other "harder" errors occurred.
                }
            }
        NextPeerDelay:
            log_to_file_only("SendQuit: Yielding/Delaying (%d ticks) after peer %s...", kQuitLoopDelayTicks, gPeerManager.peers[i].ip);
            giveTime(); // Yield first
            Delay(kQuitLoopDelayTicks, &dummyTimer); // Then delay
        }
    }

SendQuitCleanup:
    gIsSending = false; // Release "lock"
    gTCPState = TCP_STATE_IDLE; // Ensure stream is IDLE after attempts
    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers. Last error: %d.", sentCount, activePeerCount, lastErr);
    return lastErr;
}


// --- TCP Data Processing and Low-Level Wrappers ---

static void ProcessTCPReceive(unsigned short dataLength) {
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN]; // Parsed from message
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];

    // Static struct for callbacks, initialized once
    static tcp_platform_callbacks_t mac_callbacks = {
        .add_or_update_peer = mac_tcp_add_or_update_peer,
        .display_text_message = mac_tcp_display_text_message,
        .mark_peer_inactive = mac_tcp_mark_peer_inactive
    };

    if (dataLength > 0 && gTCPRecvBuffer != NULL) {
        // Get sender's IP from the connection parameters (gPeerIP is set by PassiveOpen)
        OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection);
        if (addrErr != noErr) { // Fallback if AddrToStr fails
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF);
            log_to_file_only("ProcessTCPReceive: AddrToStr failed for gPeerIP %lu. Using manual format '%s'.", gPeerIP, senderIPStrFromConnection);
        }

        // Ensure received data is null-terminated for string operations (parse_message)
        // This is safe because gTCPRecvBuffer is kTCPRecvBufferSize.
        if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0';
        else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0'; // Max case

        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (payload IP: %s).",
                             msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);
            // Use senderIPStrFromConnection (actual source IP of TCP segment) for identification
            handle_received_tcp_message(senderIPStrFromConnection, senderUsername, msgType, content, &mac_callbacks, NULL);

            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_message("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection);
                // The PollTCP state machine will see the connection state change (e.g. to CLOSE_WAIT)
                // and should transition to IDLE.
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else if (dataLength == 0) {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing signal or KeepAlive).");
    } else { // Should not happen if dataLength > 0
        log_message("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL or other issue?");
    }
}


// LowLevelSyncPoll: Generic wrapper for making asynchronous MacTCP calls
// and polling them to completion synchronously.
// Takes a 'pollTimeoutTicks' for how long *this wrapper* will poll, not the ULP timeout.
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt8 appPollTimeoutTicks) {
    OSErr err;
    unsigned long startTime = TickCount();

    if (pBlock == NULL || giveTime == NULL) return paramErr;

    pBlock->ioCompletion = nil; // Synchronous polling
    pBlock->ioCRefNum = gMacTCPRefNum; // Ensure this is always set
    pBlock->tcpStream = gTCPStream;   // Ensure this is always set for stream operations
    pBlock->ioResult = 1;       // Must be > 0 for async calls (InProgress equivalent for MacTCP)
    pBlock->csCode = csCode;    // The specific TCP command

    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err);
        return err;
    }

    // Poll for completion or app-defined timeout
    // Note: appPollTimeoutTicks = 0 means poll indefinitely until ioResult changes
    while (pBlock->ioResult > 0) { 
        giveTime(); // Critical to allow MacTCP to process
        if (appPollTimeoutTicks > 0 && (TickCount() - startTime) >= (unsigned long)appPollTimeoutTicks) {
            // Application-level polling timeout detected.
            // This is NOT a MacTCP ULP timeout. It means OUR polling loop timed out.
            // We need to tell MacTCP to cancel THIS specific operation if possible, or just return a timeout.
            // For many calls like Open/Send/Rcv, MacTCP has its own ULP timeout.
            // This wrapper's timeout is more of a fallback for calls that might not have one
            // or if we want to limit how long *we* wait for MacTCP's own timeout.
            log_to_file_only("LowLevelSyncPoll (%d): App-level poll timeout (%d ticks) reached.", csCode, appPollTimeoutTicks);
            // It's tricky to "cancel" a pending PBControlAsync without aborting the whole stream.
            // For now, we assume MacTCP's internal ULP timeouts are the primary mechanism.
            // This timeout here mostly protects OUR main loop from getting stuck if ioResult somehow
            // never changes from 1 for a call that should eventually complete or error out.
            // Let's return a generic timeout, MacTCP's ioResult might still be 1.
            // In practice, for TCP calls, MacTCP's ULP timeouts (set in csParams) should trigger first.
            return commandTimeout; // Indicate OUR polling loop timed out.
        }
    }
    return pBlock->ioResult; // Return MacTCP's final result code
}


static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr rcvBuff, unsigned long rcvBuffLen) {
    OSErr err;
    TCPiopb pbCreate;

    if (streamPtrOut == NULL || rcvBuff == NULL) return paramErr;

    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil; // Synchronous
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L; // Output from MacTCP

    pbCreate.csParam.create.rcvBuff = rcvBuff;
    pbCreate.csParam.create.rcvBuffLen = rcvBuffLen; // Typically kTCPInternalBufferSize
    pbCreate.csParam.create.notifyProc = nil; // No ASR for this simple client

    err = PBControlSync((ParmBlkPtr)&pbCreate); // TCPCreate is typically fast, direct sync is okay
    if (err == noErr) {
        *streamPtrOut = pbCreate.tcpStream;
        if (*streamPtrOut == NULL) {
            log_message("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream.");
            err = ioErr; // General I/O error
        }
    } else {
        *streamPtrOut = NULL;
        log_message("Error (LowTCPCreateSync): PBControlSync failed: %d", err);
    }
    return err;
}

// Listens for an incoming connection. Sets gPeerIP and gPeerPort on success.
static OSErr LowTCPPassiveOpenSyncPoll(SInt8 appPollTimeoutTicks, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbOpen;

    if (gTCPStream == NULL) return kInvalidStreamPtrErr;

    memset(&pbOpen, 0, sizeof(TCPiopb));
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll
    
    // ULP Timeout for the connection attempt itself (after first SYN)
    pbOpen.csParam.open.ulpTimeoutValue = kTCPPassiveOpenULPTimeoutSeconds; 
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue; // Abort if ULP timeout expires
    // Command timeout for THIS PBControl call (waiting for initial SYN)
    // MacTCP PassiveOpen uses commandTimeoutValue field for the "no SYN received" timeout.
    // The docs for TCPPassiveOpen (page 41) say:
    // "command timeout in seconds; 0 = infinity" -> This is pbOpen.csParam.open.commandTimeoutValue (offset 35)
    // Let's make this explicit for MacTCP's internal timer.
    pbOpen.csParam.open.commandTimeoutValue = 2; // Wait 2 seconds for an incoming SYN

    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction; // Indicate ULP timeout fields are valid

    pbOpen.csParam.open.localPort = PORT_TCP; // Listen on our standard TCP port
    pbOpen.csParam.open.localHost = 0L;       // Listen on all local interfaces (gMyLocalIP)
    pbOpen.csParam.open.remoteHost = 0L;      // Accept from any remote host
    pbOpen.csParam.open.remotePort = 0;       // Accept from any remote port
    
    // IP options (TOS, precedence, etc.) - use defaults
    pbOpen.csParam.open.tosFlags = 0;    // Routine
    pbOpen.csParam.open.precedence = 0;  // Routine
    pbOpen.csParam.open.dontFrag = false;
    pbOpen.csParam.open.timeToLive = 0;  // Default (usually 60-64)
    pbOpen.csParam.open.security = 0;
    pbOpen.csParam.open.optionCnt = 0;
    // pbOpen.csParam.open.options would go here if optionCnt > 0

    err = LowLevelSyncPoll(&pbOpen, giveTime, TCPPassiveOpen, appPollTimeoutTicks);
    if (err == noErr) {
        gPeerIP = pbOpen.csParam.open.remoteHost;   // Capture peer's IP
        gPeerPort = pbOpen.csParam.open.remotePort; // Capture peer's port
    } else {
        gPeerIP = 0; gPeerPort = 0; // Clear if open failed
    }
    return err;
}

static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicksForCall, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime) {
    TCPiopb pbOpen;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;

    memset(&pbOpen, 0, sizeof(TCPiopb));
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll

    pbOpen.csParam.open.ulpTimeoutValue = (Byte)(ulpTimeoutTicksForCall / 60); // Convert ticks to seconds for ULP timeout
    if (pbOpen.csParam.open.ulpTimeoutValue == 0) pbOpen.csParam.open.ulpTimeoutValue = 1; // Minimum 1 second ULP
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue;
    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction;
    
    pbOpen.csParam.open.commandTimeoutValue = 0; // Not used for ActiveOpen ULP timeout handles it

    pbOpen.csParam.open.remoteHost = remoteHost;
    pbOpen.csParam.open.remotePort = remotePort;
    pbOpen.csParam.open.localPort = 0;  // Ephemeral local port
    pbOpen.csParam.open.localHost = 0L; // OS chooses local IP

    // Default IP options
    pbOpen.csParam.open.tosFlags = 0;
    pbOpen.csParam.open.precedence = 0;
    pbOpen.csParam.open.dontFrag = false;
    pbOpen.csParam.open.timeToLive = 0; // Default
    pbOpen.csParam.open.security = 0;
    pbOpen.csParam.open.optionCnt = 0;

    // The LowLevelSyncPoll's appPollTimeoutTicks should be longer than ULP timeout for this active open.
    // Let ULP timeout be primary. Poll for slightly longer than ULP.
    SInt8 appPollTimeout = ulpTimeoutTicksForCall + 60; // Poll for ULP + 1 second cushion

    return LowLevelSyncPoll(&pbOpen, giveTime, TCPActiveOpen, appPollTimeout);
}

static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicksForCall, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime) {
    TCPiopb pbSend;
    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (wdsPtr == NULL) return kInvalidWDSErr; 

    memset(&pbSend, 0, sizeof(TCPiopb));
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll
    
    pbSend.csParam.send.ulpTimeoutValue = (Byte)(ulpTimeoutTicksForCall / 60); // Convert ULP timeout to seconds
    if (pbSend.csParam.send.ulpTimeoutValue == 0) pbSend.csParam.send.ulpTimeoutValue = 1; // Min 1 sec
    pbSend.csParam.send.ulpTimeoutAction = AbortTrue;
    pbSend.csParam.send.validityFlags = timeoutValue | timeoutAction;
    
    pbSend.csParam.send.pushFlag = push;
    pbSend.csParam.send.urgentFlag = false; // Not using urgent data
    pbSend.csParam.send.wdsPtr = wdsPtr;
    // sendLength and sendFree are output params from TCPSend, not input.

    SInt8 appPollTimeout = ulpTimeoutTicksForCall + 60; // Poll for ULP + 1 second cushion
    return LowLevelSyncPoll(&pbSend, giveTime, TCPSend, appPollTimeout);
}

static OSErr LowTCPRcvSyncPoll(SInt8 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbRcv;
    unsigned short initialBufferLen;

    if (gTCPStream == NULL) return kInvalidStreamPtrErr;
    if (buffer == NULL || bufferLen == NULL || *bufferLen == 0) return kInvalidBufPtrErr;
    if (markFlag == NULL || urgentFlag == NULL) return paramErr;

    initialBufferLen = *bufferLen; // Save original buffer length
    memset(&pbRcv, 0, sizeof(TCPiopb));
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll

    // commandTimeoutValue for TCPRcv is how long TCP waits for data before completing the call.
    // 0 = infinite. We use our appPollTimeoutTicks in the wrapper.
    // Let's set a short MacTCP command timeout, and let our wrapper control overall poll.
    pbRcv.csParam.receive.commandTimeoutValue = 1; // 1 second MacTCP command timeout for data arrival
    pbRcv.csParam.receive.rcvBuff = buffer;
    pbRcv.csParam.receive.rcvBuffLen = initialBufferLen;
    
    err = LowLevelSyncPoll(&pbRcv, giveTime, TCPRcv, appPollTimeoutTicks);
    
    *bufferLen = pbRcv.csParam.receive.rcvBuffLen; // Actual bytes received
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
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll
    
    err = LowLevelSyncPoll(&pbStat, giveTime, TCPStatus, appPollTimeoutTicks);
    if (err == noErr) {
        *amtUnread = pbStat.csParam.status.amtUnreadData;
        *connState = pbStat.csParam.status.connectionState;
    } else {
        *amtUnread = 0;
        *connState = 0; // Indicate unknown/closed state on error
        log_message("Warning (LowTCPStatusSyncPoll): Failed: %d", err);
        // If status fails with invalid stream, the connection is effectively gone.
        if (err == kInvalidStreamPtrErr) err = kConnectionDoesntExistErr; // Map to more logical error for caller
    }
    return err;
}

static OSErr LowTCPAbortSyncPoll(SInt8 ulpTimeoutTicksForAbort, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbAbort;

    if (gTCPStream == NULL) {
        log_to_file_only("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort.");
        return noErr; // Or invalidStreamPtr, but for reset purposes, noErr if already gone is fine.
    }
    
    memset(&pbAbort, 0, sizeof(TCPiopb));
    // ioCRefNum and tcpStream will be set by LowLevelSyncPoll
    // Abort does not strictly use ULP timeout in csParams, but we poll for completion.
    // The ulpTimeoutTicksForAbort is for our polling loop.
    
    err = LowLevelSyncPoll(&pbAbort, giveTime, TCPAbort, ulpTimeoutTicksForAbort);
    
    // **MODIFIED LOGIC** Treat "already gone" errors as a successful abort for state reset.
    if (err == kConnectionDoesntExistErr || err == kInvalidStreamPtrErr) { // -23008 or -23010
        log_to_file_only("LowTCPAbortSyncPoll: Abort completed (connection doesn't exist or stream invalid). Result: %d. Considered OK for reset.", err);
        err = noErr; // THIS IS THE KEY CHANGE: Map these to success for our state machine
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
    pbRelease.ioCompletion = nil; // Synchronous
    pbRelease.ioCRefNum = macTCPRefNum;
    pbRelease.csCode = TCPRelease;
    pbRelease.tcpStream = streamToRelease; // Specify the stream to release

    err = PBControlSync((ParmBlkPtr)&pbRelease);
    // If stream is already invalid, MacTCP might return invalidStreamPtr. This is not a disaster.
    if (err != noErr && err != kInvalidStreamPtrErr) {
        log_message("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err);
    } else if (err == kInvalidStreamPtrErr) {
        log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid or released. Error: %d", (unsigned long)streamToRelease, err);
        err = noErr; // Treat as success for cleanup purposes
    }
    return err;
}
