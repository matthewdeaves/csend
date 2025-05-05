//====================================
// FILE: ./classic_mac/tcp.c
//====================================

#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog.h" // For updating UI if needed
#include "dialog_peerlist.h"
#include "network.h" // For gMacTCPRefNum, gMyUsername, gMyLocalIPStr, ParseIPv4, YieldTimeToSystem
#include "../shared/messaging_logic.h"
#include <Devices.h>
#include <Errors.h>
#include <Memory.h> // For NewPtrClear, DisposePtr
#include <string.h> // For memset, strcmp
#include <stdio.h>  // For sprintf
#include <stdlib.h> // For size_t if needed

// --- Constants ---
#define kTCPRecvBufferSize 8192           // Buffer for receiving data on the listener stream
#define kTCPListenInternalBufferSize 8192 // Internal buffer needed by MacTCP for the listener stream
#define kTCPSenderInternalBufferSize 2048 // Smaller internal buffer for temporary sender streams
#define kTCPListenRetryDelayTicks 60      // Delay before retrying listen after certain errors
#define kShutdownConnectTimeoutTicks 180  // Timeout for connecting when sending QUIT (3 sec)
#define kShutdownSendTimeoutTicks 120     // Timeout for sending QUIT data (2 sec)
#define kRuntimeConnectTimeoutTicks 300   // Timeout for connecting when sending TEXT (5 sec)
#define kRuntimeSendTimeoutTicks 180      // Timeout for sending TEXT data (3 sec)
#define AbortTrue 1                       // ULP Timeout Action: Abort connection

// --- Listener Stream Globals ---
static StreamPtr gTCPListenStream = NULL;         // The persistent stream for listening
static Ptr gTCPListenInternalBuffer = NULL; // MacTCP's internal buffer for the listener
static Ptr gTCPRecvBuffer = NULL;           // Our buffer for received data on the listener
static TCPiopb gTCPListenPB;                // PB for TCPPassiveOpen (listen)
static TCPiopb gTCPRecvPB;                  // PB for TCPRcv (receive data)
static TCPiopb gTCPClosePB;                 // PB for TCPClose (close accepted connection)

// --- Listener State ---
static Boolean gTCPListenPending = false;   // Is TCPPassiveOpen pending?
static Boolean gTCPRecvPending = false;     // Is TCPRcv pending?
static Boolean gTCPClosePending = false;    // Is TCPClose pending?
static Boolean gNeedToReListen = false;     // Flag to re-issue listen after connection ends/fails
ip_addr gCurrentConnectionIP = 0;           // IP of peer connected to listener stream
tcp_port gCurrentConnectionPort = 0;       // Port of peer connected to listener stream
Boolean gIsConnectionActive = false;        // Is listener stream currently handling an active connection?

// --- Forward Declarations ---
static OSErr StartAsyncTCPListen(short macTCPRefNum);
static OSErr StartAsyncTCPRecv(short macTCPRefNum);
static OSErr StartAsyncTCPClose(short macTCPRefNum, Boolean abortConnection);
static void ProcessTCPReceive(short macTCPRefNum);
static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult);

// --- Low-Level Synchronous TCP Helper Functions ---
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost, tcp_port remotePort, ip_addr *localHost, tcp_port *localPort, GiveTimePtr giveTime);
static OSErr LowTCPSendSync(StreamPtr streamPtr, SInt8 timeoutTicks, Boolean push, Boolean urgent, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPAbortSync(StreamPtr streamPtr, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);

// --- Shared Logic Callbacks ---
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context; // Unused context
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_message("Peer connected/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) {
            UpdatePeerDisplayList(true); // Update UI if a new peer is seen
        }
    } else if (addResult < 0) {
        log_message("Peer list full, could not add/update %s@%s from TCP connection", username, ip);
    }
    return addResult;
}

static void mac_tcp_display_text_message(const char* username, const char* ip, const char* message_content, void* platform_context) {
    (void)platform_context; // Unused context
    (void)ip; // IP address is available if needed, but username is primary display info
    char displayMsg[BUFFER_SIZE + 100]; // Ensure buffer is large enough

    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        // Format message for display
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r"); // Add newline
        log_message("Message from %s@%s: %s", username, ip, message_content);
    } else {
        // Log if UI isn't ready
        log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}

static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) {
    (void)platform_context; // Unused context
    if (!ip) return;
    log_message("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) {
        // Update UI if peer is successfully marked inactive
        if (gMainWindow != NULL && gPeerListHandle != NULL) {
            UpdatePeerDisplayList(true);
        }
    }
}

// --- Public Functions ---

OSErr InitTCPListener(short macTCPRefNum) {
    OSErr err;
    StreamPtr tempListenStream = NULL;

    log_message("Initializing TCP Listener Stream...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPListenStream != NULL) {
        log_message("Error (InitTCPListener): Already initialized?");
        return streamAlreadyOpen; // Already initialized
    }

    // Allocate buffers
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

    // Create the persistent listener stream
    log_message("Creating Listener Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &tempListenStream, gTCPListenInternalBuffer, kTCPListenInternalBufferSize);
    if (err != noErr || tempListenStream == NULL) {
        log_message("Error: Failed to create Listener Stream: %d", err);
        CleanupTCPListener(macTCPRefNum); // Clean up allocated buffers
        return err;
    }
    gTCPListenStream = tempListenStream;
    log_message("Listener Stream created successfully (StreamPtr: 0x%lX).", (unsigned long)gTCPListenStream);

    // Initialize state variables
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;

    // Start the first asynchronous listen
    err = StartAsyncTCPListen(macTCPRefNum);
    if (err != noErr && err != 1) { // 1 means already pending (shouldn't happen here)
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

    StreamPtr listenStreamToRelease = gTCPListenStream; // Capture current stream pointer
    gTCPListenStream = NULL; // Prevent further use

    // Kill any pending operations on the listener stream
    if (gTCPListenPending && gTCPListenPB.tcpStream == listenStreamToRelease) {
        PBKillIO((ParmBlkPtr)&gTCPListenPB, false); // Async kill, ignore error
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

    // Reset state flags
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;

    // Release the stream synchronously if it exists and driver is open
    if (listenStreamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting synchronous release of listener stream 0x%lX...", (unsigned long)listenStreamToRelease);
        relErr = LowTCPReleaseSync(macTCPRefNum, listenStreamToRelease);
        if (relErr != noErr) {
             // Log warning, but proceed with buffer cleanup
             log_message("Warning (CleanupTCPListener): Synchronous TCPRelease failed: %d", relErr);
        } else {
             log_to_file_only("CleanupTCPListener: Synchronous TCPRelease successful.");
        }
    } else if (listenStreamToRelease != NULL) {
        log_message("Warning (CleanupTCPListener): Cannot release stream, MacTCP refnum is 0.");
    }

    // Dispose buffers
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

    // Check for completed Listen operation
    if (gTCPListenPending) {
        ioResult = gTCPListenPB.ioResult;
        if (ioResult <= 0) { // Completed (success or error)
            gTCPListenPending = false; // Clear flag *before* handling
            HandleListenerCompletion(macTCPRefNum, ioResult);
        }
    }

    // Check for completed Receive operation (only if connection is active)
    if (gTCPRecvPending && gIsConnectionActive) {
        // Defensive check: Ensure the completion is for the correct stream
        if (gTCPRecvPB.tcpStream == gTCPListenStream && gTCPListenStream != NULL) {
            ioResult = gTCPRecvPB.ioResult;
            if (ioResult <= 0) { // Completed
                gTCPRecvPending = false; // Clear flag *before* handling
                HandleReceiveCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPRecvPending) {
             // Should not happen if state is managed correctly, but log defensively
             log_message("Warning (PollTCPListener): Receive pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPRecvPending = false;
        }
    }

    // Check for completed Close operation
    if (gTCPClosePending) {
        // Defensive check: Ensure the completion is for the correct stream
        if (gTCPClosePB.tcpStream == gTCPListenStream && gTCPListenStream != NULL) {
            ioResult = gTCPClosePB.ioResult;
            if (ioResult <= 0) { // Completed
                gTCPClosePending = false; // Clear flag *before* handling
                HandleCloseCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPClosePending) {
             log_message("Warning (PollTCPListener): Async Close pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPClosePending = false;
        }
    }

    // Check if we need to re-issue the listen command
    if (gNeedToReListen && gTCPListenStream != NULL &&
        !gTCPListenPending && !gTCPRecvPending && !gTCPClosePending && !gIsConnectionActive)
    {
        log_to_file_only("PollTCPListener: Conditions met to attempt re-issuing listen.");
        gNeedToReListen = false; // Clear flag *before* attempting
        OSErr err = StartAsyncTCPListen(macTCPRefNum);
        if (err != noErr && err != 1) { // 1 = already pending (shouldn't happen here)
            log_message("Error (PollTCPListener): Attempt to re-issue listen failed immediately. Error: %d. Will retry later.", err);
            gNeedToReListen = true; // Set flag again to retry on next poll
        } else {
            log_to_file_only("PollTCPListener: Re-issued Async TCPPassiveOpen successfully (or already pending).");
        }
    } else if (gNeedToReListen && gTCPListenStream == NULL) {
        // This indicates a serious problem, listener stream was likely released due to error
        log_message("Warning (PollTCPListener): Need to re-listen but stream is NULL. Cannot proceed.");
        gNeedToReListen = false; // Stop trying if stream is gone
    }
}

// Internal function to perform a complete synchronous send operation using a temporary stream
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

    // 1. Allocate temporary buffer for the temporary stream
    tempConnBuffer = NewPtrClear(kTCPSenderInternalBufferSize);
    if (tempConnBuffer == NULL) {
        log_message("Error (InternalSyncTCPSend): Failed to allocate temporary connection buffer.");
        return memFullErr;
    }

    // 2. Create temporary stream
    err = LowTCPCreateSync(gMacTCPRefNum, &tempStream, tempConnBuffer, kTCPSenderInternalBufferSize);
    if (err != noErr || tempStream == NULL) {
        log_message("Error (InternalSyncTCPSend): LowTCPCreateSync failed: %d", err);
        DisposePtr(tempConnBuffer); // Clean up buffer
        return err;
    }
    log_to_file_only("InternalSyncTCPSend: Temp stream 0x%lX created.", (unsigned long)tempStream);

    // 3. Connect using the temporary stream
    log_to_file_only("InternalSyncTCPSend: Connecting temp stream 0x%lX...", (unsigned long)tempStream);
    err = LowTCPOpenConnectionSync(tempStream, connectTimeoutTicks, targetIP, targetPort, NULL, NULL, giveTime);
    if (err == noErr) {
        log_to_file_only("InternalSyncTCPSend: Connected successfully.");

        // 4. Send data
        sendWDS[0].length = messageLen;
        sendWDS[0].ptr = (Ptr)messageBuffer; // Cast away const, WDS expects Ptr
        sendWDS[1].length = 0; // Terminator for WDS array
        sendWDS[1].ptr = NULL;

        log_to_file_only("InternalSyncTCPSend: Sending data...");
        err = LowTCPSendSync(tempStream, sendTimeoutTicks, true, false, (Ptr)sendWDS, giveTime);
        if (err == noErr) {
            log_to_file_only("InternalSyncTCPSend: Send successful.");
        } else {
            log_message("Error (InternalSyncTCPSend): LowTCPSendSync failed: %d", err);
            finalErr = err; // Record send error
        }

        // 5. Abort connection (no need for graceful close for one-shot send)
        log_to_file_only("InternalSyncTCPSend: Aborting connection...");
        OSErr abortErr = LowTCPAbortSync(tempStream, giveTime);
        if (abortErr != noErr) {
            log_message("Warning (InternalSyncTCPSend): LowTCPAbortSync failed: %d", abortErr);
            if (finalErr == noErr) finalErr = abortErr; // Record abort error if no prior error
        }

    } else {
        log_message("Error (InternalSyncTCPSend): LowTCPOpenConnectionSync failed: %d", err);
        finalErr = err; // Record connect error
    }

    // 6. Release the temporary stream (regardless of errors)
    log_to_file_only("InternalSyncTCPSend: Releasing temp stream 0x%lX...", (unsigned long)tempStream);
    OSErr releaseErr = LowTCPReleaseSync(gMacTCPRefNum, tempStream);
    if (releaseErr != noErr) {
        log_message("Warning (InternalSyncTCPSend): LowTCPReleaseSync failed: %d", releaseErr);
        if (finalErr == noErr) finalErr = releaseErr; // Record release error if no prior error
    }

    // 7. Dispose the temporary connection buffer
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

    // Parse the destination IP address
    err = ParseIPv4(peerIP, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (TCP_SendTextMessageSync): Could not parse peer IP string '%s'.", peerIP);
        return paramErr;
    }

    // Format the message using the shared protocol function
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT,
                                  gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) {
        log_message("Error (TCP_SendTextMessageSync): Failed to format TEXT message for %s.", peerIP);
        return paramErr; // Or a more specific error
    }

    // Use the internal synchronous send function
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
    OSErr lastErr = noErr; // Track the last error encountered
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

    // Format the QUIT message once
    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT,
                                  gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) {
        log_message("Error (TCP_SendQuitMessagesSync): Failed to format QUIT message.");
        return paramErr; // Or a more specific error
    }

    // Count active peers first (for logging)
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            activePeerCount++;
        }
    }
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);

    // Iterate through peers and send QUIT using temporary streams
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr targetIP = 0;
            OSErr parseErr, sendErr;

            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s",
                        gPeerManager.peers[i].username, gPeerManager.peers[i].ip);

            // Parse IP for this peer
            parseErr = ParseIPv4(gPeerManager.peers[i].ip, &targetIP);
            if (parseErr != noErr || targetIP == 0) {
                log_message("Error: Could not parse peer IP string '%s'. Skipping.", gPeerManager.peers[i].ip);
                lastErr = parseErr; // Record the error
                continue; // Skip this peer
            }

            // Send using the internal synchronous function
            sendErr = InternalSyncTCPSend(targetIP, PORT_TCP, quitMessageBuffer, formattedLen,
                                          kShutdownConnectTimeoutTicks, kShutdownSendTimeoutTicks, giveTime);

            if (sendErr == noErr) {
                log_message("Successfully sent QUIT to %s.", gPeerManager.peers[i].ip);
                sentCount++;
            } else {
                log_message("Error sending QUIT to %s: %d", gPeerManager.peers[i].ip, sendErr);
                lastErr = sendErr; // Record the error
                // Continue trying other peers even if one fails
            }

            // Yield time between attempts to be cooperative
            giveTime();
        }
    }

    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers.", sentCount, activePeerCount);

    // Return the last error encountered (or noErr if all succeeded)
    return lastErr;
}


// --- Private Helper Functions ---

static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult) {
    // Ensure this completion is for the current listener stream
    StreamPtr completedListenerStream = gTCPListenPB.tcpStream;
    if (completedListenerStream != gTCPListenStream || gTCPListenStream == NULL) {
        log_message("Warning (HandleListenerCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                    (unsigned long)completedListenerStream, (unsigned long)gTCPListenStream);
        return;
    }

    if (ioResult == noErr) {
        // Connection indication received
        ip_addr acceptedIP = gTCPListenPB.csParam.open.remoteHost;
        tcp_port acceptedPort = gTCPListenPB.csParam.open.remotePort;
        char senderIPStr[INET_ADDRSTRLEN];
        AddrToStr(acceptedIP, senderIPStr); // Use DNR to get string representation
        log_message("TCP Connection Indication from %s:%u on listener stream 0x%lX.",
                    senderIPStr, acceptedPort, (unsigned long)completedListenerStream);

        // Check if we are already handling a connection or have other pending ops
        if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) {
             log_message("Warning: New connection indication while listener stream busy (Active: %d, RecvPending: %d, ClosePending: %d). Setting flag to re-listen later.",
                         gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
             gNeedToReListen = true; // We missed this connection, need to listen again later
        } else {
            // Accept the connection state
            gCurrentConnectionIP = acceptedIP;
            gCurrentConnectionPort = acceptedPort;
            gIsConnectionActive = true;

            // Start the first receive operation for this new connection
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
            if (err != noErr && err != 1) { // 1 = already pending (shouldn't happen)
                log_message("Error (HandleListenerCompletion): Failed to start first TCPRcv on listener stream. Error: %d. Closing connection state.", err);
                gIsConnectionActive = false; // Reset state
                gCurrentConnectionIP = 0;
                gCurrentConnectionPort = 0;
                // Don't start Close here, just set flag to re-listen
                gNeedToReListen = true;
            } else {
                 log_to_file_only("HandleListenerCompletion: First TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
            }
        }
    } else {
        // Handle errors from TCPPassiveOpen
        const char *errMsg = "Unknown Error";
        Boolean isConnectionExists = (ioResult == connectionExists); // Transient, try again
        Boolean isInvalidStream = (ioResult == invalidStreamPtr);   // Fatal for this stream
        Boolean isReqAborted = (ioResult == reqAborted);           // Usually from PBKillIO
        // Add other common errors if needed
        if (isConnectionExists) errMsg = "connectionExists (-23007)";
        else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isReqAborted) errMsg = "reqAborted (-23015)";

        if (isConnectionExists) {
             // This can happen if MacTCP's internal state is temporarily busy. Retry after a short delay.
             log_to_file_only("HandleListenerCompletion: TCPPassiveOpen completed with transient error: %d (%s). Will retry after delay.", ioResult, errMsg);
             unsigned long dummyTimer;
             Delay(kTCPListenRetryDelayTicks, &dummyTimer); // Simple delay
             gNeedToReListen = true; // Flag to retry
        } else if (isReqAborted) {
             // This is expected if we killed the listen (e.g., during cleanup)
             log_to_file_only("HandleListenerCompletion: TCPPassiveOpen completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
             // Don't automatically set gNeedToReListen here, cleanup handles it
        } else {
            // Other errors
            log_message("Error (HandleListenerCompletion): TCPPassiveOpen completed with error: %d (%s)", ioResult, errMsg);
            if (isInvalidStream) {
                 // This stream is unusable. Log and prevent further use.
                 log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid. It will be cleaned up.", (unsigned long)gTCPListenStream);
                 // CleanupNetworking will handle the release. Don't try to use it further.
                 gTCPListenStream = NULL; // Mark as unusable
                 gNeedToReListen = false; // Cannot re-listen on a NULL stream
            } else {
                 // For other errors, attempt to re-listen later
                 gNeedToReListen = true;
            }
        }
    }
}

static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr completedRecvStream = gTCPRecvPB.tcpStream;

    // Ensure completion is for the correct, non-NULL listener stream
    if (completedRecvStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleReceiveCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)completedRecvStream, (unsigned long)gTCPListenStream);
         return;
    }

    // If connection is no longer marked active (e.g., closed during processing), handle differently
     if (!gIsConnectionActive) {
         if (ioResult == noErr) {
             // Received data but connection state changed. Process data but don't continue.
             log_to_file_only("Warning (HandleReceiveCompletion): Receive completed successfully but connection no longer marked active. Data processed, not re-issuing receive.");
             ProcessTCPReceive(macTCPRefNum); // Process any final data
         } else {
             log_to_file_only("Warning (HandleReceiveCompletion): Receive completed with error %d, connection already inactive.", ioResult);
         }
         // If the error wasn't just an abort, we likely need to re-listen eventually
         if (ioResult != reqAborted) {
            gNeedToReListen = true;
         }
         return; // Do not proceed further
     }

    // Handle completion results for an active connection
    if (ioResult == noErr) {
        // Data received successfully
        ProcessTCPReceive(macTCPRefNum);

        // If connection is still active after processing, issue the next receive
        if (gIsConnectionActive && gTCPListenStream != NULL) {
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
             if (err != noErr && err != 1) { // 1 = already pending (shouldn't happen)
                 log_message("Error (HandleReceiveCompletion): Failed to start next async TCP receive after successful receive. Error: %d. Closing connection.", err);
                 // Couldn't start next receive, close the connection abruptly
                 StartAsyncTCPClose(macTCPRefNum, true); // Abort = true
             } else {
                 log_to_file_only("HandleReceiveCompletion: Successfully re-issued TCPRcv on stream 0x%lX.", (unsigned long)gTCPListenStream);
             }
        } else {
             // Connection became inactive during processing (e.g., ProcessTCPReceive handled QUIT)
             // or stream became NULL (fatal error).
             log_to_file_only("HandleReceiveCompletion: Connection stream closed/inactive or stream became NULL during processing. Not starting new receive.");
             // If stream still exists and no close is pending, flag to re-listen
             if (gTCPListenStream != NULL && !gTCPClosePending) {
                 gNeedToReListen = true;
             }
        }
    } else if (ioResult == connectionClosing) {
         // Peer initiated graceful close
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         log_message("TCP Connection closing gracefully from %s (Stream: 0x%lX).", senderIPStr, (unsigned long)completedRecvStream);
         // Process any final data that might have been in the buffer
         ProcessTCPReceive(macTCPRefNum);
         // Clean up connection state and flag to re-listen
         gIsConnectionActive = false;
         gCurrentConnectionIP = 0;
         gCurrentConnectionPort = 0;
         gNeedToReListen = true;
    } else {
         // Handle receive errors
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr); // Get IP for logging
         const char *errMsg = "Unknown Error";
         Boolean isConnTerminated = (ioResult == connectionTerminated); // Abrupt close
         Boolean isInvalidStream = (ioResult == invalidStreamPtr);   // Fatal
         Boolean isReqAborted = (ioResult == reqAborted);           // From PBKillIO
         // Add other relevant errors
         if (isConnTerminated) errMsg = "connectionTerminated (-23012)";
         else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
         else if (isReqAborted) errMsg = "reqAborted (-23015)";

        if (isReqAborted) {
            log_to_file_only("HandleReceiveCompletion: Async TCPRcv completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
        } else {
            log_message("Error (HandleReceiveCompletion): Async TCPRcv completed with error: %d (%s) from %s (Stream: 0x%lX).", ioResult, errMsg, senderIPStr, (unsigned long)completedRecvStream);
        }

        // Clean up connection state
        gIsConnectionActive = false;
        gCurrentConnectionIP = 0;
        gCurrentConnectionPort = 0;

        // Handle fatal stream error
        if (isInvalidStream) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during receive. It will be cleaned up.", (unsigned long)gTCPListenStream);
             gTCPListenStream = NULL; // Mark as unusable
             gNeedToReListen = false; // Cannot re-listen
        } else if (!isReqAborted) {
             // For other errors (like connectionTerminated), flag to re-listen
             gNeedToReListen = true;
        }
    }
}

static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr closedStream = gTCPClosePB.tcpStream;

    // Ensure completion is for the correct, non-NULL listener stream
    if (closedStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleCloseCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)closedStream, (unsigned long)gTCPListenStream);
         return;
    }

    // Connection is definitely inactive now
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;

    if (ioResult == noErr) {
        log_to_file_only("HandleCloseCompletion: Async TCPClose completed successfully for stream 0x%lX.", (unsigned long)closedStream);
    } else {
        // Handle close errors
        const char *errMsg = "Unknown Error";
        Boolean isInvalidStream = (ioResult == invalidStreamPtr); // Fatal
        Boolean isReqAborted = (ioResult == reqAborted);         // From PBKillIO
        // Add other relevant errors (e.g., connectionDoesntExist)
        if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isReqAborted) errMsg = "reqAborted (-23015)";

        if (isReqAborted) {
             log_to_file_only("HandleCloseCompletion: Async TCPClose completed with %d (%s), likely due to PBKillIO.", ioResult, errMsg);
        } else {
            log_message("Error (HandleCloseCompletion): Async TCPClose for stream 0x%lX completed with error: %d (%s)", (unsigned long)closedStream, ioResult, errMsg);
        }

        // Handle fatal stream error
        if (isInvalidStream) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during close. It will be cleaned up.", (unsigned long)gTCPListenStream);
             gTCPListenStream = NULL; // Mark as unusable
             gNeedToReListen = false; // Cannot re-listen
             return; // Don't set flag below
        }
    }

    // If the close wasn't just an abort, we need to re-listen
    if (ioResult != reqAborted) {
        log_to_file_only("HandleCloseCompletion: Setting flag to re-listen.");
        gNeedToReListen = true;
    }
}


// Start listening for incoming connections on the persistent listener stream
static OSErr StartAsyncTCPListen(short macTCPRefNum) {
    OSErr err;

    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPListen): Cannot listen, gTCPListenStream is NULL.");
        return invalidStreamPtr;
    }
    if (gTCPListenPending) {
        log_to_file_only("StartAsyncTCPListen: Listen already pending on stream 0x%lX.", (unsigned long)gTCPListenStream);
        return 1; // Indicate already pending
    }
    // Cannot start listen if stream is busy with an active connection or other ops
    if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) {
        log_message("Error (StartAsyncTCPListen): Cannot listen, stream 0x%lX is busy (Active: %d, RecvPending: %d, ClosePending: %d).",
                    (unsigned long)gTCPListenStream, gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
        return inProgress; // Indicate busy
    }

    // Prepare the parameter block for TCPPassiveOpen
    memset(&gTCPListenPB, 0, sizeof(TCPiopb));
    gTCPListenPB.ioCompletion = nil; // Use polling
    gTCPListenPB.ioCRefNum = macTCPRefNum;
    gTCPListenPB.csCode = TCPPassiveOpen;
    gTCPListenPB.tcpStream = gTCPListenStream;
    // Set timeouts (optional, 0 usually means infinite for passive open)
    gTCPListenPB.csParam.open.validityFlags = timeoutValue | timeoutAction; // Enable ULP timeout
    gTCPListenPB.csParam.open.ulpTimeoutValue = kTCPDefaultTimeout; // Use default (0 = infinite)
    gTCPListenPB.csParam.open.ulpTimeoutAction = AbortTrue; // Action if timeout occurs (irrelevant if value is 0)
    gTCPListenPB.csParam.open.commandTimeoutValue = 0; // No timeout for the command itself
    // Specify local port and allow any remote host/port
    gTCPListenPB.csParam.open.localPort = PORT_TCP;
    gTCPListenPB.csParam.open.localHost = 0L; // Listen on all local interfaces
    gTCPListenPB.csParam.open.remoteHost = 0L; // Accept connection from any remote IP
    gTCPListenPB.csParam.open.remotePort = 0;  // Accept connection from any remote port
    gTCPListenPB.csParam.open.userDataPtr = nil; // No user data

    // Issue the asynchronous call
    gTCPListenPending = true; // Set flag *before* calling
    err = PBControlAsync((ParmBlkPtr)&gTCPListenPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPListen): PBControlAsync(TCPPassiveOpen) failed immediately. Error: %d", err);
        gTCPListenPending = false; // Clear flag on immediate failure
        return err;
    }

    log_to_file_only("StartAsyncTCPListen: Async TCPPassiveOpen initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1; // Indicate call initiated successfully (pending)
}

// Start receiving data on the active connection (listener stream)
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
         return 1; // Indicate already pending
    }
    // Can only receive if a connection is active
    if (!gIsConnectionActive) {
         log_to_file_only("Warning (StartAsyncTCPRecv): Cannot receive, connection not marked active on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
         return connectionDoesntExist; // Or appropriate error
    }
    // Cannot receive if other operations are blocking the stream
    if (gTCPListenPending || gTCPClosePending) {
         log_message("Error (StartAsyncTCPRecv): Cannot receive, stream 0x%lX has other pending operations (Listen: %d, Close: %d).",
                     (unsigned long)gTCPListenStream, gTCPListenPending, gTCPClosePending);
         return inProgress; // Indicate busy
    }

    // Prepare the parameter block for TCPRcv
    memset(&gTCPRecvPB, 0, sizeof(TCPiopb));
    gTCPRecvPB.ioCompletion = nil; // Use polling
    gTCPRecvPB.ioCRefNum = macTCPRefNum;
    gTCPRecvPB.csCode = TCPRcv;
    gTCPRecvPB.tcpStream = gTCPListenStream;
    gTCPRecvPB.csParam.receive.rcvBuff = gTCPRecvBuffer;     // Our receive buffer
    gTCPRecvPB.csParam.receive.rcvBuffLen = kTCPRecvBufferSize; // Max bytes to receive
    gTCPRecvPB.csParam.receive.commandTimeoutValue = 0; // No timeout for the receive command itself
    gTCPRecvPB.csParam.receive.userDataPtr = nil; // No user data

    // Issue the asynchronous call
    gTCPRecvPending = true; // Set flag *before* calling
    err = PBControlAsync((ParmBlkPtr)&gTCPRecvPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPRecv): PBControlAsync(TCPRcv) failed immediately. Error: %d", err);
        gTCPRecvPending = false; // Clear flag on immediate failure
        // If receive fails immediately, the connection is likely broken. Close it.
        StartAsyncTCPClose(macTCPRefNum, true); // Abort = true
        return err;
    }

    log_to_file_only("StartAsyncTCPRecv: Async TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1; // Indicate call initiated successfully (pending)
}

// Start closing the active connection on the listener stream
static OSErr StartAsyncTCPClose(short macTCPRefNum, Boolean abortConnection) {
    OSErr err;

    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPClose): Cannot close NULL stream.");
        return invalidStreamPtr;
    }
    if (gTCPClosePending) {
        log_to_file_only("StartAsyncTCPClose: Close already pending for listener stream 0x%lX.", (unsigned long)gTCPListenStream);
        return 1; // Indicate already pending
    }

    // Mark connection as inactive *before* initiating close
    gIsConnectionActive = false;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;

    // Kill any pending receive on this stream before closing
    if (gTCPRecvPending && gTCPRecvPB.tcpStream == gTCPListenStream) {
        log_to_file_only("StartAsyncTCPClose: Killing pending receive before closing.");
        PBKillIO((ParmBlkPtr)&gTCPRecvPB, false); // Async kill, ignore error
        gTCPRecvPending = false; // Clear flag immediately
    }

    // Prepare the parameter block for TCPClose
    memset(&gTCPClosePB, 0, sizeof(TCPiopb));
    gTCPClosePB.ioCompletion = nil; // Use polling
    gTCPClosePB.ioCRefNum = macTCPRefNum;
    gTCPClosePB.csCode = TCPClose;
    gTCPClosePB.tcpStream = gTCPListenStream;
    // Set ULP timeout for close operation (optional, 0 = default/infinite)
    gTCPClosePB.csParam.close.validityFlags = timeoutValue | timeoutAction; // Enable ULP timeout
    gTCPClosePB.csParam.close.ulpTimeoutValue = kTCPDefaultTimeout; // Use default (0)
    if (abortConnection) {
        gTCPClosePB.csParam.close.ulpTimeoutAction = AbortTrue; // Abort immediately
        log_to_file_only("StartAsyncTCPClose: Using Abort action.");
    } else {
        gTCPClosePB.csParam.close.ulpTimeoutAction = 0; // Graceful close (default action)
        log_to_file_only("StartAsyncTCPClose: Using Graceful close action.");
    }
    gTCPClosePB.csParam.close.userDataPtr = nil; // No user data

    // Issue the asynchronous call
    gTCPClosePending = true; // Set flag *before* calling
    err = PBControlAsync((ParmBlkPtr)&gTCPClosePB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPClose): PBControlAsync(TCPClose) failed immediately for stream 0x%lX. Error: %d", (unsigned long)gTCPListenStream, err);
        gTCPClosePending = false; // Clear flag on immediate failure
        gNeedToReListen = true; // Flag to re-listen since close failed
        return err;
    }

    log_to_file_only("StartAsyncTCPClose: Initiated async TCPClose for listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1; // Indicate call initiated successfully (pending)
}


// Process data received in gTCPRecvBuffer
static void ProcessTCPReceive(short macTCPRefNum) {
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN]; // Parsed from message
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    unsigned short dataLength;

    // Define the callbacks structure for the shared logic
    static tcp_platform_callbacks_t mac_callbacks = {
        .add_or_update_peer = mac_tcp_add_or_update_peer,
        .display_text_message = mac_tcp_display_text_message,
        .mark_peer_inactive = mac_tcp_mark_peer_inactive
    };

    // Sanity checks
    if (!gIsConnectionActive || gTCPListenStream == NULL) {
        log_message("Warning (ProcessTCPReceive): Called when connection not active or listener stream is NULL.");
        return;
    }
    // Ensure the completed PB matches the listener stream
    if (gTCPRecvPB.tcpStream != gTCPListenStream) {
        log_message("CRITICAL Warning (ProcessTCPReceive): Received data for unexpected stream 0x%lX, expected 0x%lX. Ignoring and closing.",
                    (unsigned long)gTCPRecvPB.tcpStream, (unsigned long)gTCPListenStream);
        StartAsyncTCPClose(macTCPRefNum, true); // Abort = true
        return;
    }

    dataLength = gTCPRecvPB.csParam.receive.rcvBuffLen; // Get actual bytes received
    if (dataLength > 0) {
        // Get the sender's IP string from the connection info
        OSErr addrErr = AddrToStr(gCurrentConnectionIP, senderIPStrFromConnection);
        if (addrErr != noErr) {
            // Fallback formatting if AddrToStr fails
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu",
                    (gCurrentConnectionIP >> 24) & 0xFF, (gCurrentConnectionIP >> 16) & 0xFF,
                    (gCurrentConnectionIP >> 8) & 0xFF, gCurrentConnectionIP & 0xFF);
            log_to_file_only("ProcessTCPReceive: AddrToStr failed (%d) for sender IP %lu. Using fallback '%s'.",
                         addrErr, (unsigned long)gCurrentConnectionIP, senderIPStrFromConnection);
        }

        // Parse the received data using the shared protocol function
        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            // Successfully parsed, call the shared logic handler
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s (Payload IP: %s).",
                       msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload);

            handle_received_tcp_message(senderIPStrFromConnection, // Use IP from connection info
                                        senderUsername,
                                        msgType,
                                        content,
                                        &mac_callbacks,
                                        NULL); // No platform context needed here

            // Check if the message requires closing the connection (e.g., QUIT)
            if (strcmp(msgType, MSG_QUIT) == 0) {
                 log_message("Connection to %s finishing due to received QUIT.", senderIPStrFromConnection);
                 // Initiate close (abort=true since peer is quitting anyway)
                 StartAsyncTCPClose(macTCPRefNum, true);
                 // gIsConnectionActive is set to false inside StartAsyncTCPClose
            }
        } else {
            // Parsing failed
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
            // Consider closing connection if parsing fails repeatedly? For now, just log.
        }
    } else {
        // Received 0 bytes - often indicates graceful close initiation by peer
        log_to_file_only("ProcessTCPReceive: Received 0 bytes. Connection likely closing.");
        // Don't need to explicitly close here; HandleReceiveCompletion handles connectionClosing error
    }
}


// --- Low-Level Synchronous TCP Helpers (Copied & Adapted) ---

// Creates a TCP stream synchronously.
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen)
{
    OSErr err;
    TCPiopb pbCreate;

    if (streamPtr == NULL || connectionBuffer == NULL) return paramErr;

    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil; // Synchronous
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L; // Will be filled in by MacTCP
    pbCreate.csParam.create.rcvBuff = connectionBuffer; // Internal buffer for MacTCP
    pbCreate.csParam.create.rcvBuffLen = connBufferLen;
    pbCreate.csParam.create.notifyProc = nil; // No ASR needed for sync operations
    pbCreate.csParam.create.userDataPtr = nil;

    err = PBControlSync((ParmBlkPtr)&pbCreate);

    if (err == noErr) {
        *streamPtr = pbCreate.tcpStream;
        if (*streamPtr == NULL) {
            log_message("Error (LowTCPCreateSync): PBControlSync succeeded but returned NULL stream.");
            err = ioErr; // Indicate an unexpected issue
        }
    } else {
        *streamPtr = NULL;
    }
    return err;
}

// Opens a TCP connection synchronously using TCPActiveOpen.
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost,
                                      tcp_port remotePort, ip_addr *localHost, tcp_port *localPort,
                                      GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL; // Allocate PB dynamically for sync calls using polling

    if (giveTime == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;

    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) {
        log_message("Error (LowTCPOpenConnectionSync): Failed to allocate PB.");
        return memFullErr;
    }

    // Prepare PB for TCPActiveOpen
    pBlock->ioCompletion = nil; // Use polling
    pBlock->ioCRefNum = gMacTCPRefNum; // Use global driver refnum
    pBlock->csCode = TCPActiveOpen;
    pBlock->ioResult = 1; // Initialize ioResult to indicate pending
    pBlock->tcpStream = streamPtr;
    // Set ULP timeout
    pBlock->csParam.open.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.open.ulpTimeoutAction = AbortTrue; // Abort if timeout occurs
    pBlock->csParam.open.validityFlags = timeoutValue | timeoutAction; // Enable ULP timeout
    pBlock->csParam.open.commandTimeoutValue = 0; // No timeout for the command itself
    // Set remote host and port
    pBlock->csParam.open.remoteHost = remoteHost;
    pBlock->csParam.open.remotePort = remotePort;
    // Let MacTCP choose local port and IP
    pBlock->csParam.open.localPort = 0;
    pBlock->csParam.open.localHost = 0;

    // Issue async call and poll for completion
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPOpenConnectionSync): PBControlAsync failed immediately: %d", err);
        DisposePtr((Ptr)pBlock);
        return err;
    }

    // Poll ioResult until it's <= 0 (completed or error)
    while (pBlock->ioResult > 0) {
        giveTime(); // Yield to other processes
    }

    err = pBlock->ioResult; // Get final result

    // If successful, optionally return assigned local host/port
    if (err == noErr) {
        if (localHost) *localHost = pBlock->csParam.open.localHost;
        if (localPort) *localPort = pBlock->csParam.open.localPort;
    }

    DisposePtr((Ptr)pBlock); // Clean up allocated PB
    return err;
}

// Sends data synchronously using TCPSend.
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

    // Prepare PB for TCPSend
    pBlock->ioCompletion = nil; // Use polling
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPSend;
    pBlock->ioResult = 1; // Indicate pending
    pBlock->tcpStream = streamPtr;
    // Set ULP timeout
    pBlock->csParam.send.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.send.ulpTimeoutAction = AbortTrue;
    pBlock->csParam.send.validityFlags = timeoutValue | timeoutAction; // Enable ULP timeout
    // Set send flags and data pointer
    pBlock->csParam.send.pushFlag = push;
    pBlock->csParam.send.urgentFlag = urgent;
    pBlock->csParam.send.wdsPtr = wdsPtr; // Pointer to Write Data Structure array

    // Issue async call and poll for completion
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPSendSync): PBControlAsync failed immediately: %d", err);
        DisposePtr((Ptr)pBlock);
        return err;
    }

    // Poll ioResult
    while (pBlock->ioResult > 0) {
        giveTime();
    }

    err = pBlock->ioResult; // Get final result
    DisposePtr((Ptr)pBlock); // Clean up PB
    return err;
}

// Aborts a TCP connection synchronously using TCPAbort.
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

    // Prepare PB for TCPAbort
    pBlock->ioCompletion = nil; // Use polling
    pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPAbort;
    pBlock->ioResult = 1; // Indicate pending
    pBlock->tcpStream = streamPtr;

    // Issue async call and poll for completion
    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        // Abort might fail if connection doesn't exist, often not critical
        log_to_file_only("Info (LowTCPAbortSync): PBControlAsync failed immediately: %d", err);
        // Don't necessarily return error here unless it's unexpected
        if (err != connectionDoesntExist && err != invalidStreamPtr) {
             log_message("Warning (LowTCPAbortSync): Unexpected immediate error: %d", err);
        }
        DisposePtr((Ptr)pBlock);
        // Return noErr generally, as abort is best-effort cleanup
        return noErr; // Or return err if strict error checking needed
    }

    // Poll ioResult
    while (pBlock->ioResult > 0) {
        giveTime();
    }

    err = pBlock->ioResult; // Get final result
    if (err != noErr && err != connectionDoesntExist && err != invalidStreamPtr) {
         log_message("Warning (LowTCPAbortSync): Abort completed with error: %d", err);
    }

    DisposePtr((Ptr)pBlock); // Clean up PB
    // Return noErr generally for abort
    return noErr; // Or return err if strict error checking needed
}

// Releases a TCP stream synchronously using TCPRelease.
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr)
{
    OSErr err;
    TCPiopb pbRelease;

    if (streamPtr == NULL) return invalidStreamPtr;

    memset(&pbRelease, 0, sizeof(TCPiopb));
    pbRelease.ioCompletion = nil; // Synchronous
    pbRelease.ioCRefNum = macTCPRefNum;
    pbRelease.csCode = TCPRelease;
    pbRelease.tcpStream = streamPtr;
    // No csParam needed for TCPRelease

    err = PBControlSync((ParmBlkPtr)&pbRelease);
    // Release might fail if stream is invalid, log but don't halt cleanup
    if (err != noErr && err != invalidStreamPtr) {
         log_message("Warning (LowTCPReleaseSync): PBControlSync(TCPRelease) failed: %d", err);
    } else if (err == invalidStreamPtr) {
         log_to_file_only("Info (LowTCPReleaseSync): Attempted to release invalid stream 0x%lX.", (unsigned long)streamPtr);
         err = noErr; // Treat invalid stream on release as success (it's already gone)
    }
    return err;
}
