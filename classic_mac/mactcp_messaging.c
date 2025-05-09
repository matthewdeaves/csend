//====================================
// FILE: ./classic_mac/mactcp_messaging.c
//====================================

#include "./mactcp_messaging.h"
#include "logging.h"
#include "protocol.h"
#include "peer.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "network.h"
#include "../shared/messaging.h" // For tcp_platform_callbacks_t and handle_received_tcp_message

#include <Devices.h>
#include <Errors.h>
#include <MacTypes.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Events.h> // For EventRecord, WaitNextEvent (used in YieldTimeToSystem)
#include <OSUtils.h> // For Delay

// TCP Buffer Sizes
#define kTCPRecvBufferSize 8192
#define kTCPInternalBufferSize 8192 // For TCPCreate connection buffer

// ULP Timeout values for various operations (in seconds)
#define kTCPPassiveOpenULPTimeoutSeconds 2
#define kConnectULPTimeoutSeconds 5
#define kSendULPTimeoutSeconds 3
#define kAbortULPTimeoutSeconds 1

// Application-level poll timeouts for synchronous operations (in ticks, 60 ticks = 1 second)
#define kTCPRecvPollTimeoutTicks 1     // Short timeout for quick poll if data expected
#define kTCPStatusPollTimeoutTicks 1   // Short timeout for status check
#define kConnectPollTimeoutTicks (kConnectULPTimeoutSeconds * 60 + 30) // App poll timeout for connect
#define kSendPollTimeoutTicks (kSendULPTimeoutSeconds * 60 + 30)       // App poll timeout for send
#define kAbortPollTimeoutTicks (kAbortULPTimeoutSeconds * 60 + 30)     // App poll timeout for abort

// Delay for retrying operations or between certain actions
#define kErrorRetryDelayTicks 120 // 2 seconds, for retrying after some errors
#define kQuitLoopDelayTicks 120   // Delay used in the original SendQuit loop, now for reference if main.c needs it

// MacTCP specific error codes (some are standard, some might be from MacTCP docs)
// streamBusyErr is defined in the header
#define kMacTCPTimeoutErr (-23016)        // General TCP timeout from MacTCP
#define kDuplicateSocketErr (-23017)      // Attempt to create duplicate socket
#define kConnectionExistsErr (-23007)     // Connection already exists (e.g. active open)
#define kConnectionClosingErr (-23005)    // Connection is closing
#define kConnectionDoesntExistErr (-23008)// Connection does not exist
#define kInvalidStreamPtrErr (-23010)     // Invalid TCP stream pointer
#define kInvalidWDSErr (-23014)           // Invalid Write Data Structure
#define kInvalidBufPtrErr (-23013)        // Invalid buffer pointer
#define kRequestAbortedErr (-23006)       // Request was aborted (e.g. by ULP timeout or another call)

#define AbortTrue 1 // Parameter for ULP timeout action

// Globals for the single TCP stream management
static StreamPtr gTCPStream = NULL;
static Ptr gTCPInternalBuffer = NULL; // Buffer for TCPCreate
static Ptr gTCPRecvBuffer = NULL;     // Buffer for TCPRcv
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static Boolean gIsSending = false;    // Flag to prevent concurrent send operations

// Information about the currently connected peer (for incoming connections)
static ip_addr gPeerIP = 0;
static tcp_port gPeerPort = 0;

// Parameter block for asynchronous TCPPassiveOpen
static TCPiopb gTCPPassiveOpenPB;
static Boolean gPassiveOpenPBInitialized = false;


// Forward declarations for internal static functions
static void ProcessTCPReceive(unsigned short dataLength);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt16 appPollTimeoutTicks);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPActiveOpenSyncPoll(Byte ulpTimeoutSeconds, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSyncPoll(Byte ulpTimeoutSeconds, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPRcvSyncPoll(SInt16 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime);
static OSErr LowTCPStatusSyncPoll(SInt16 appPollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState);
static OSErr LowTCPAbortSyncPoll(Byte ulpTimeoutSeconds, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamToRelease);

// Platform-specific callbacks for the shared TCP message handler
static int mac_tcp_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context; // Unused
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) { // New peer added
        log_message("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult == 0) { // Existing peer updated
        log_to_file_only("Peer updated via TCP: %s@%s", username, ip);
    } else { // Error or list full
        log_message("Peer list full or error, could not add/update %s@%s from TCP connection", username, ip);
    }
    return addResult;
}

static void mac_tcp_display_text_message(const char *username, const char *ip, const char *message_content, void *platform_context)
{
    (void)platform_context; // Unused
    (void)ip; // IP is available if needed, but typically username is primary identifier in chat
    char displayMsg[BUFFER_SIZE + 100]; // Ensure enough space for formatting

    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r"); // Newline for Mac TextEdit
        log_message("Message from %s@%s: %s", username, ip, message_content);
    } else {
        log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}

static void mac_tcp_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context; // Unused
    if (!ip) return;

    log_message("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) { // MarkPeerInactive returns true if status changed
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}


OSErr InitTCP(short macTCPRefNum)
{
    OSErr err;
    log_message("Initializing Single TCP Stream (Async Passive Open / Sync Poll Strategy)...");

    if (macTCPRefNum == 0) return paramErr;
    if (gTCPStream != NULL || gTCPState != TCP_STATE_UNINITIALIZED) {
        log_message("Error (InitTCP): Already initialized or in unexpected state (%d)?", gTCPState);
        return streamAlreadyOpen; // Or some other appropriate error
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

    // Create the TCP stream
    log_message("Creating Single Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &gTCPStream, gTCPInternalBuffer, kTCPInternalBufferSize);
    if (err != noErr || gTCPStream == NULL) {
        log_message("Error: Failed to create TCP Stream: %d", err);
        CleanupTCP(macTCPRefNum); // Clean up allocated buffers if any
        return err;
    }
    log_message("Single TCP Stream created (0x%lX).", (unsigned long)gTCPStream);

    // Initialize parameter block for passive open (listening)
    memset(&gTCPPassiveOpenPB, 0, sizeof(TCPiopb));
    gTCPPassiveOpenPB.ioCompletion = nil; // For async calls
    gTCPPassiveOpenPB.ioCRefNum = gMacTCPRefNum;
    gTCPPassiveOpenPB.tcpStream = gTCPStream;
    gPassiveOpenPBInitialized = true;

    gTCPState = TCP_STATE_IDLE;
    gIsSending = false;
    gPeerIP = 0;
    gPeerPort = 0;

    log_message("TCP initialization complete. State: IDLE.");
    return noErr;
}

void CleanupTCP(short macTCPRefNum)
{
    log_message("Cleaning up Single TCP Stream...");
    StreamPtr streamToRelease = gTCPStream; // Cache before setting global to NULL
    TCPState stateBeforeCleanup = gTCPState;

    gTCPState = TCP_STATE_RELEASING; // Mark as releasing to stop PollTCP actions
    gTCPStream = NULL; // Prevent further use by PollTCP or new Send calls

    // If a connection was active or pending, try to abort it cleanly
    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN || stateBeforeCleanup == TCP_STATE_PASSIVE_OPEN_PENDING) {
        log_message("Cleanup: Attempting synchronous abort (best effort)...");
        if (streamToRelease != NULL) {
            // Temporarily restore gTCPStream for LowTCPAbortSyncPoll if it expects it
            StreamPtr currentGlobalStream = gTCPStream; // Should be NULL here
            gTCPStream = streamToRelease; // Restore for the abort call
            LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, YieldTimeToSystem);
            gTCPStream = currentGlobalStream; // Set back to NULL
        }
    }

    // Release the stream
    if (streamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting sync release of stream 0x%lX...", (unsigned long)streamToRelease);
        OSErr relErr = LowTCPReleaseSync(macTCPRefNum, streamToRelease);
        if (relErr != noErr) log_message("Warning: Sync release failed: %d", relErr);
        else log_to_file_only("Sync release successful.");
    } else if (streamToRelease != NULL) {
        log_message("Warning: Cannot release stream, MacTCP refnum is 0.");
    }

    gTCPState = TCP_STATE_UNINITIALIZED;
    gIsSending = false;
    gPeerIP = 0;
    gPeerPort = 0;
    gPassiveOpenPBInitialized = false; // Reset PB init status

    // Dispose buffers
    if (gTCPRecvBuffer != NULL) {
        DisposePtr(gTCPRecvBuffer);
        gTCPRecvBuffer = NULL;
    }
    if (gTCPInternalBuffer != NULL) {
        DisposePtr(gTCPInternalBuffer);
        gTCPInternalBuffer = NULL;
    }
    log_message("TCP cleanup finished.");
}

void PollTCP(GiveTimePtr giveTime)
{
    OSErr err;
    unsigned short amountUnread = 0;
    Byte connectionState = 0;
    unsigned long dummyTimer; // For Delay

    if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) {
        return; // Not initialized, in error, or cleaning up
    }

    if (gIsSending) { // If a send operation is in progress, yield and return
        giveTime();
        return;
    }

    switch (gTCPState) {
        case TCP_STATE_IDLE:
            // Attempt to start listening for an incoming connection
            log_to_file_only("PollTCP: State IDLE. Attempting ASYNC Passive Open (ULP: %ds)...", kTCPPassiveOpenULPTimeoutSeconds);
            if (!gPassiveOpenPBInitialized) { // Should not happen if InitTCP was successful
                log_message("PollTCP CRITICAL: gTCPPassiveOpenPB not initialized!");
                gTCPState = TCP_STATE_ERROR;
                break;
            }

            gTCPPassiveOpenPB.csCode = TCPPassiveOpen;
            gTCPPassiveOpenPB.csParam.open.ulpTimeoutValue = kTCPPassiveOpenULPTimeoutSeconds;
            gTCPPassiveOpenPB.csParam.open.ulpTimeoutAction = AbortTrue; // Abort if timeout expires
            gTCPPassiveOpenPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
            gTCPPassiveOpenPB.csParam.open.localPort = PORT_TCP;
            gTCPPassiveOpenPB.csParam.open.localHost = 0L; // Any local IP
            // For passive open, remote host/port are usually 0 to accept from anyone
            gTCPPassiveOpenPB.csParam.open.remoteHost = 0L;
            gTCPPassiveOpenPB.csParam.open.remotePort = 0;
            // Other params (TOS, TTL, etc.) set to default/0
            gTCPPassiveOpenPB.csParam.open.tosFlags = 0;
            gTCPPassiveOpenPB.csParam.open.precedence = 0;
            gTCPPassiveOpenPB.csParam.open.dontFrag = false;
            gTCPPassiveOpenPB.csParam.open.timeToLive = 0;
            gTCPPassiveOpenPB.csParam.open.security = 0;
            gTCPPassiveOpenPB.csParam.open.optionCnt = 0;
            gTCPPassiveOpenPB.csParam.open.commandTimeoutValue = 0; // No command timeout for async
            gTCPPassiveOpenPB.ioResult = 1; // Mark as pending

            err = PBControlAsync((ParmBlkPtr)&gTCPPassiveOpenPB);
            if (err == noErr) {
                log_to_file_only("PollTCP: Async TCPPassiveOpen initiated.");
                gTCPState = TCP_STATE_PASSIVE_OPEN_PENDING;
            } else {
                log_message("PollTCP: PBControlAsync(TCPPassiveOpen) failed immediately: %d. Retrying after delay.", err);
                gTCPState = TCP_STATE_IDLE; // Remain in IDLE to retry
                Delay(kErrorRetryDelayTicks, &dummyTimer);
            }
            break;

        case TCP_STATE_PASSIVE_OPEN_PENDING:
            giveTime(); // Yield while waiting for async operation
            if (gTCPPassiveOpenPB.ioResult == 1) { // Still pending
                return;
            }

            // Async operation completed
            if (gTCPPassiveOpenPB.ioResult == noErr) {
                // Successful connection
                gPeerIP = gTCPPassiveOpenPB.csParam.open.remoteHost;
                gPeerPort = gTCPPassiveOpenPB.csParam.open.remotePort;
                char senderIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, senderIPStr); // Convert IP to string for logging
                log_message("PollTCP: Incoming connection from %s:%u.", senderIPStr, gPeerPort);
                gTCPState = TCP_STATE_CONNECTED_IN;
                goto CheckConnectedInData; // Process any immediately available data
            } else {
                // Passive open failed
                err = gTCPPassiveOpenPB.ioResult;
                if (err == kRequestAbortedErr) {
                     log_message("PollTCP: Async Passive Open was aborted (err %d), likely by a send operation. Returning to IDLE.", err);
                } else {
                    log_message("PollTCP: Async Passive Open failed: %d.", err);
                }

                // If failed due to duplicate socket or existing connection, try to abort to clear the stream
                if (err == kDuplicateSocketErr || err == kConnectionExistsErr) {
                    log_message("PollTCP: Attempting Abort to clear stream after Passive Open failure (%d).", err);
                    LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                }
                gTCPState = TCP_STATE_IDLE; // Return to IDLE to retry listening
                Delay(kErrorRetryDelayTicks, &dummyTimer);
            }
            break;

        case TCP_STATE_CONNECTED_IN:
        CheckConnectedInData: // Label for direct jump after successful passive open
            log_to_file_only("PollTCP: State CONNECTED_IN. Checking status...");
            err = LowTCPStatusSyncPoll(kTCPStatusPollTimeoutTicks, giveTime, &amountUnread, &connectionState);

            if (err != noErr) {
                log_message("PollTCP: Error getting status while CONNECTED_IN: %d. Aborting.", err);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                gTCPState = TCP_STATE_IDLE;
                break;
            }

            // Check MacTCP connection states: 8=Established, 10=FIN_WAIT_1, 12=FIN_WAIT_2, 14=CLOSE_WAIT
            // If not in one of these, something is wrong or it closed abruptly.
            if (connectionState != 8 && connectionState != 10 && connectionState != 12 && connectionState != 14 ) {
                char peerIPStr[INET_ADDRSTRLEN];
                AddrToStr(gPeerIP, peerIPStr);
                log_message("PollTCP: Connection state is %d (not Established/Closing) for %s. Aborting and returning to IDLE.", connectionState, peerIPStr);
                LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                gTCPState = TCP_STATE_IDLE;
                break;
            }

            log_to_file_only("PollTCP: Status OK (State %d). Unread data: %u bytes.", connectionState, amountUnread);

            if (amountUnread > 0) {
                unsigned short bytesToRead = kTCPRecvBufferSize; // Max to read
                Boolean markFlag = false, urgentFlag = false;

                log_to_file_only("PollTCP: Attempting synchronous Rcv poll...");
                err = LowTCPRcvSyncPoll(kTCPRecvPollTimeoutTicks, gTCPRecvBuffer, &bytesToRead, &markFlag, &urgentFlag, giveTime);

                if (err == noErr) {
                    log_to_file_only("PollTCP: Rcv poll got %u bytes.", bytesToRead);
                    ProcessTCPReceive(bytesToRead);
                } else if (err == kConnectionClosingErr) { // Connection closing, but data was received
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll indicated connection closing by peer %s. Processing final %u bytes.", peerIPStr, bytesToRead);
                    if (bytesToRead > 0) ProcessTCPReceive(bytesToRead);
                    // Connection is closing, so abort our end and go back to idle.
                    LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                    gTCPState = TCP_STATE_IDLE;
                } else if (err == commandTimeout) {
                    // This can happen if status showed data, but Rcv timed out (e.g. data arrived between calls)
                    log_to_file_only("PollTCP: Rcv poll timed out despite status showing data? Odd. Will retry status.");
                } else { // Other errors on Rcv
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Rcv poll failed for %s: %d. Aborting.", peerIPStr, err);
                    LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                    gTCPState = TCP_STATE_IDLE;
                }
            } else { // No data unread
                if (connectionState == 14 ) { // CLOSE_WAIT: peer has closed, we should close too.
                    char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                    log_message("PollTCP: Peer %s has closed (State: CLOSE_WAIT). Aborting to clean up. Returning to IDLE.", peerIPStr);
                    LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
                    gTCPState = TCP_STATE_IDLE;
                } else if (connectionState != 8) { // Not established, but also not CLOSE_WAIT (e.g. FIN_WAIT_1/2)
                     char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
                     log_to_file_only("PollTCP: Peer %s in closing state %d with no data. Waiting for MacTCP.", peerIPStr, connectionState);
                }
                // If state is 8 (Established) and no data, just continue polling.
            }
            break;

        default:
            log_message("PollTCP: In unexpected state %d.", gTCPState);
            gTCPState = TCP_STATE_IDLE; // Reset to a known state
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
    char messageBuffer[BUFFER_SIZE]; // Buffer for the formatted message
    int formattedLen;
    struct wdsEntry sendWDS[2]; // Write Data Structure for TCPSend

    log_to_file_only("MacTCP_SendMessageSync: Request to send '%s' to %s", msg_type, peerIPStr);

    if (gMacTCPRefNum == 0) return notOpenErr; // MacTCP driver not open
    if (gTCPStream == NULL && gTCPState != TCP_STATE_RELEASING && gTCPState != TCP_STATE_UNINITIALIZED) {
         // This case implies a logic error if gTCPStream is NULL but state isn't one of the deep cleanup/uninit ones.
         if (gTCPStream == NULL) { // Double check, as the outer condition might be complex
            log_message("Error (SendMessage): gTCPStream is NULL and not in deep cleanup state.");
            return kInvalidStreamPtrErr;
         }
    }
    if (peerIPStr == NULL || msg_type == NULL || local_username == NULL || local_ip_str == NULL || giveTime == NULL) {
        return paramErr;
    }
    // message_content can be NULL or empty for certain message types like QUIT

    // If we are currently listening (passive open pending), we must abort it to send.
    if (gTCPState == TCP_STATE_PASSIVE_OPEN_PENDING) {
        log_message("SendMessage: Stream was in PASSIVE_OPEN_PENDING. Aborting pending listen to allow send.");
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
        if (abortErr == noErr) {
            log_to_file_only("SendMessage: Abort of pending passive open successful.");
        } else {
            log_message("SendMessage: Abort of pending passive open FAILED: %d. Send may fail.", abortErr);
            // If abort timed out, stream might still be busy.
            return (abortErr == commandTimeout) ? streamBusyErr : abortErr;
        }
        gTCPState = TCP_STATE_IDLE; // After abort, stream should be idle
    }

    if (gIsSending) {
        log_message("Warning (SendMessage): Send already in progress.");
        return streamBusyErr;
    }

    // Stream must be idle to initiate an active open for sending
    if (gTCPState != TCP_STATE_IDLE) {
        log_message("Warning (SendMessage): Stream not IDLE (state %d) after attempting to clear. Cannot send now.", gTCPState);
        return streamBusyErr; // Or another appropriate error indicating non-idle state
    }

    gIsSending = true; // Set sending flag

    // Parse the destination IP string
    err = ParseIPv4(peerIPStr, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (SendMessage): Invalid peer IP '%s'.", peerIPStr);
        finalErr = paramErr;
        goto SendMessageCleanup;
    }

    // Format the message using the shared protocol function
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, msg_type, local_username, local_ip_str, message_content ? message_content : "");
    if (formattedLen <= 0) {
        log_message("Error (SendMessage): format_message failed for type '%s'.", msg_type);
        finalErr = paramErr; // Or a more specific error
        goto SendMessageCleanup;
    }

    // Perform TCP Active Open (Connect)
    log_to_file_only("SendMessage: Connecting to %s...", peerIPStr);
    err = LowTCPActiveOpenSyncPoll(kConnectULPTimeoutSeconds, targetIP, PORT_TCP, giveTime);
    if (err == noErr) {
        log_to_file_only("SendMessage: Connected successfully to %s.", peerIPStr);

        // Prepare WDS for sending
        sendWDS[0].length = formattedLen -1; // Do not send the null terminator from format_message
        sendWDS[0].ptr = (Ptr)messageBuffer;
        sendWDS[1].length = 0; // Terminator for WDS array
        sendWDS[1].ptr = NULL;

        // Send the data
        log_to_file_only("SendMessage: Sending data (%d bytes)...", sendWDS[0].length);
        err = LowTCPSendSyncPoll(kSendULPTimeoutSeconds, true /* push */, (Ptr)sendWDS, giveTime);
        if (err != noErr) {
            log_message("Error (SendMessage): Send failed to %s: %d", peerIPStr, err);
            finalErr = err;
        } else {
            log_to_file_only("SendMessage: Send successful to %s.", peerIPStr);
        }

        // Abort the connection (since we're doing connect-send-disconnect per message)
        log_to_file_only("SendMessage: Aborting connection to %s...", peerIPStr);
        OSErr abortErr = LowTCPAbortSyncPoll(kAbortULPTimeoutSeconds, giveTime);
        if (abortErr != noErr) {
            log_message("Warning (SendMessage): Abort failed for %s: %d", peerIPStr, abortErr);
            if (finalErr == noErr) finalErr = abortErr; // Report abort error if send was ok
        }
    } else {
        log_message("Error (SendMessage): Connect to %s failed: %d", peerIPStr, err);
        finalErr = err;
        // Specific handling for connectionExistsErr if it means the peer is in TIME_WAIT or similar
        if (err == kConnectionExistsErr && strcmp(msg_type, MSG_QUIT) == 0) {
             log_message("SendMessage: Connect for QUIT to %s failed with connectionExists. Peer might be in TIME_WAIT. Assuming QUIT effectively sent.", peerIPStr);
             finalErr = noErr; // Treat as non-fatal for QUIT in this specific case
        }
    }

SendMessageCleanup:
    gIsSending = false; // Clear sending flag
    gTCPState = TCP_STATE_IDLE; // Return stream to IDLE state for listening or next send
    log_to_file_only("MacTCP_SendMessageSync to %s for '%s': Released send lock. Final Status: %d.", peerIPStr, msg_type, finalErr);
    return finalErr;
}


static void ProcessTCPReceive(unsigned short dataLength)
{
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN]; // Parsed from message
    char senderUsername[32]; // Parsed from message
    char msgType[32];        // Parsed from message
    char content[BUFFER_SIZE]; // Parsed from message

    // Static struct for callbacks, initialized once
    static tcp_platform_callbacks_t mac_callbacks = {
        .add_or_update_peer = mac_tcp_add_or_update_peer,
        .display_text_message = mac_tcp_display_text_message,
        .mark_peer_inactive = mac_tcp_mark_peer_inactive
    };

    if (dataLength > 0 && gTCPRecvBuffer != NULL) {
        // Get sender IP from the connection parameters (gPeerIP is set by PassiveOpen)
        OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection);
        if (addrErr != noErr) {
            // Fallback if AddrToStr fails (e.g., DNR not working)
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF);
            log_to_file_only("ProcessTCPReceive: AddrToStr failed for gPeerIP %lu. Using manual format '%s'.", gPeerIP, senderIPStrFromConnection);
        }

        // Null-terminate the received data if it fits, for parse_message
        if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0';
        else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0'; // Ensure null termination if buffer is full

        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (payload IP: %s).",
                             msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);

            // Call the shared message handler
            handle_received_tcp_message(senderIPStrFromConnection, // IP from connection is more reliable
                                        senderUsername,
                                        msgType,
                                        content,
                                        &mac_callbacks,
                                        NULL /* platform_context, not used by these mac_callbacks */);

            // Specific post-processing for QUIT
            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_message("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection);
                // The PollTCP state machine will see the connection state change (e.g. to CLOSE_WAIT)
                // and then abort and return to IDLE.
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else if (dataLength == 0) {
        // This can happen if the connection is closing or a keep-alive with no data.
        log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing signal or KeepAlive).");
    } else {
        // Should not happen if dataLength > 0 implies gTCPRecvBuffer is valid.
        log_message("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL or other issue?");
    }
}

// Low-level MacTCP call wrappers using a synchronous polling model
// These functions block until the MacTCP operation completes or an app-level timeout occurs.

static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode, SInt16 appPollTimeoutTicks)
{
    OSErr err;
    unsigned long startTime = TickCount();

    if (pBlock == NULL || giveTime == NULL) return paramErr;
    if (gMacTCPRefNum == 0) return notOpenErr; // MacTCP driver not open

    // For most csCodes, gTCPStream must be valid. Exceptions: TCPCreate, TCPRelease (handled by caller)
    if (csCode != TCPCreate && csCode != TCPRelease) {
        if (gTCPStream == NULL) {
            log_message("Error (LowLevelSyncPoll %d): gTCPStream is NULL.", csCode);
            return kInvalidStreamPtrErr;
        }
        pBlock->tcpStream = gTCPStream; // Ensure PB uses the global stream
    } else if (csCode == TCPRelease && pBlock->tcpStream == NULL) {
        // For TCPRelease, the stream to release is passed in pBlock by the caller
        log_message("Error (LowLevelSyncPoll TCPRelease): pBlock->tcpStream is NULL.");
        return kInvalidStreamPtrErr;
    }


    pBlock->ioCompletion = nil; // Synchronous polling, no completion routine
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->ioResult = 1; // Mark as pending for the loop
    pBlock->csCode = csCode;

    err = PBControlAsync((ParmBlkPtr)pBlock); // Initiate async MacTCP call
    if (err != noErr) {
        log_message("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err);
        return err;
    }

    // Poll until ioResult is no longer 1 (pending)
    while (pBlock->ioResult > 0) {
        giveTime(); // Yield to other processes/event loop

        // Check for application-level timeout
        if (appPollTimeoutTicks > 0 && (TickCount() - startTime) >= (unsigned long)appPollTimeoutTicks) {
            log_to_file_only("LowLevelSyncPoll (%d): App-level poll timeout (%d ticks) reached.", csCode, appPollTimeoutTicks);
            // Note: MacTCP call might still be pending. Aborting it here could be complex.
            // This timeout is mostly a safeguard for the app not to hang indefinitely.
            // The ULP timeout in the PB should ideally handle MacTCP-level timeouts.
            return commandTimeout; // Return a generic timeout error
        }
    }
    return pBlock->ioResult; // Return the actual result from MacTCP
}


static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtrOut, Ptr rcvBuff, unsigned long rcvBuffLen) {
    OSErr err;
    TCPiopb pbCreate;

    if (streamPtrOut == NULL || rcvBuff == NULL) return paramErr;

    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L; // Will be filled by MacTCP
    pbCreate.csParam.create.rcvBuff = rcvBuff;
    pbCreate.csParam.create.rcvBuffLen = rcvBuffLen;
    pbCreate.csParam.create.notifyProc = nil; // No notifier routine

    err = PBControlSync((ParmBlkPtr)&pbCreate); // Use synchronous MacTCP call
    if (err == noErr) {
        *streamPtrOut = pbCreate.tcpStream;
        if (*streamPtrOut == NULL) {
            log_message("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream.");
            err = ioErr; // Or some other MacTCP error
        }
    } else {
        *streamPtrOut = NULL;
        log_message("Error (LowTCPCreateSync): PBControlSync failed: %d", err);
    }
    return err;
}

static OSErr LowTCPActiveOpenSyncPoll(Byte ulpTimeoutSeconds, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime) {
    TCPiopb pbOpen;
    SInt16 pollTimeout;

    memset(&pbOpen, 0, sizeof(TCPiopb));
    // Setup parameters for active open (connect)
    pbOpen.csParam.open.ulpTimeoutValue = ulpTimeoutSeconds;
    pbOpen.csParam.open.ulpTimeoutAction = AbortTrue; // Abort on ULP timeout
    pbOpen.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pbOpen.csParam.open.commandTimeoutValue = 0; // No command timeout for async base
    pbOpen.csParam.open.remoteHost = remoteHost;
    pbOpen.csParam.open.remotePort = remotePort;
    pbOpen.csParam.open.localPort = 0;  // Ephemeral local port
    pbOpen.csParam.open.localHost = 0L; // Any local IP

    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30; // App poll timeout slightly > ULP timeout
    return LowLevelSyncPoll(&pbOpen, giveTime, TCPActiveOpen, pollTimeout);
}

static OSErr LowTCPSendSyncPoll(Byte ulpTimeoutSeconds, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime) {
    TCPiopb pbSend;
    SInt16 pollTimeout;

    if (wdsPtr == NULL) return kInvalidWDSErr;

    memset(&pbSend, 0, sizeof(TCPiopb));
    pbSend.csParam.send.ulpTimeoutValue = ulpTimeoutSeconds;
    pbSend.csParam.send.ulpTimeoutAction = AbortTrue;
    pbSend.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pbSend.csParam.send.pushFlag = push;
    pbSend.csParam.send.urgentFlag = false; // Not using urgent data
    pbSend.csParam.send.wdsPtr = wdsPtr;

    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30;
    return LowLevelSyncPoll(&pbSend, giveTime, TCPSend, pollTimeout);
}

static OSErr LowTCPRcvSyncPoll(SInt16 appPollTimeoutTicks, Ptr buffer, unsigned short *bufferLen, Boolean *markFlag, Boolean *urgentFlag, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbRcv;
    unsigned short initialBufferLen;

    if (buffer == NULL || bufferLen == NULL || *bufferLen == 0) return kInvalidBufPtrErr;
    if (markFlag == NULL || urgentFlag == NULL) return paramErr;

    initialBufferLen = *bufferLen; // Store requested buffer length
    memset(&pbRcv, 0, sizeof(TCPiopb));
    // Use a very short commandTimeout for TCPRcv if polling,
    // as we expect data if status said so.
    pbRcv.csParam.receive.commandTimeoutValue = 1; // Minimal command timeout
    pbRcv.csParam.receive.rcvBuff = buffer;
    pbRcv.csParam.receive.rcvBuffLen = initialBufferLen;

    err = LowLevelSyncPoll(&pbRcv, giveTime, TCPRcv, appPollTimeoutTicks);

    *bufferLen = pbRcv.csParam.receive.rcvBuffLen; // Actual bytes received
    *markFlag = pbRcv.csParam.receive.markFlag;   // TCP PUSH flag
    *urgentFlag = pbRcv.csParam.receive.urgentFlag;

    return err;
}

static OSErr LowTCPStatusSyncPoll(SInt16 appPollTimeoutTicks, GiveTimePtr giveTime, unsigned short *amtUnread, Byte *connState) {
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
        *connState = 0; // Indicate unknown or error state
        log_message("Warning (LowTCPStatusSyncPoll): Failed: %d", err);
        // Map invalidStreamPtr to connectionDoesntExist, as status on a non-existent stream implies this.
        if (err == kInvalidStreamPtrErr) err = kConnectionDoesntExistErr;
    }
    return err;
}

static OSErr LowTCPAbortSyncPoll(Byte ulpTimeoutSeconds, GiveTimePtr giveTime) {
    OSErr err;
    TCPiopb pbAbort;
    SInt16 pollTimeout;

    if (gTCPStream == NULL) { // Aborting a NULL stream is a no-op or error depending on context
        log_to_file_only("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort.");
        return noErr; // Or kInvalidStreamPtrErr if strict
    }

    memset(&pbAbort, 0, sizeof(TCPiopb));
    // ULP timeout for abort is usually not critical, but set for consistency
    // pbAbort.csParam.abort.ulpTimeoutValue = ulpTimeoutSeconds; // Not standard for Abort
    // pbAbort.csParam.abort.ulpTimeoutAction = AbortTrue;
    // pbAbort.csParam.abort.validityFlags = 0;

    pollTimeout = (SInt16)ulpTimeoutSeconds * 60 + 30;
    err = LowLevelSyncPoll(&pbAbort, giveTime, TCPAbort, pollTimeout);

    // Certain errors from Abort might be considered "successful" in terms of resetting the stream
    if (err == kConnectionDoesntExistErr || err == kInvalidStreamPtrErr || err == kRequestAbortedErr) {
        log_to_file_only("LowTCPAbortSyncPoll: Abort completed (err %d). Considered OK for reset.", err);
        err = noErr; // Treat these as non-fatal for the purpose of aborting
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
    pbRelease.tcpStream = streamToRelease; // Specify the stream to release

    err = PBControlSync((ParmBlkPtr)&pbRelease);
    if (err != noErr && err != kInvalidStreamPtrErr) { // kInvalidStreamPtrErr means it was already gone
        log_message("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err);
    } else if (err == kInvalidStreamPtrErr) {
        log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid or released. Error: %d", (unsigned long)streamToRelease, err);
        err = noErr; // Treat as non-fatal if already released
    }
    return err;
}