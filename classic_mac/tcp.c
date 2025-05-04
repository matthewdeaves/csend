#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog_peerlist.h"
#include "network.h"
#include <Devices.h>
#include <Errors.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <MacTCP.h>

// Define the recommended buffer size
#define kTCPRecommendedBufSize 8192

StreamPtr gTCPListenStream = NULL;      // Pointer for the stream LISTENING for connections
Ptr gTCPListenRecvBuffer = NULL;    // Buffer specifically for the listener stream
TCPiopb gTCPListenPB;               // PB for TCPPassiveOpen calls
Boolean gTCPListenPending = false;

StreamPtr gTCPConnectionStream = NULL;  // Pointer for the CURRENT ACTIVE connection
Ptr gTCPRecvBuffer = NULL;          // Buffer for receiving data on the active connection
TCPiopb gTCPRecvPB;                 // PB for TCPRcv calls on the active connection
TCPiopb gTCPReleasePB;              // PB for TCPRelease calls
Boolean gTCPRecvPending = false;
Boolean gTCPReleasePending = false; // Tracks release state for EITHER listener or connection
ip_addr gCurrentConnectionIP = 0;   // IP of the current active connection
tcp_port gCurrentConnectionPort = 0; // Port of the current active connection

// Static function prototypes
static OSErr StartAsyncTCPListen(short macTCPRefNum);
static OSErr StartAsyncTCPRecv(short macTCPRefNum);
static OSErr StartAsyncTCPRelease(short macTCPRefNum, StreamPtr streamToRelease, Boolean isConnectionStream);
static void ProcessTCPReceive(short macTCPRefNum, ip_addr myLocalIP);

OSErr InitTCPListener(short macTCPRefNum) {
    OSErr err;
    TCPiopb pbCreate; // Use a local variable for the create call

    log_message("Initializing TCP Listener...");
    if (macTCPRefNum == 0) return paramErr;

    // Allocate receive buffer (used for accepted connections later)
    gTCPRecvBuffer = NewPtrClear(kTCPRecommendedBufSize);
    if (gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP receive buffer (%ld bytes).", (long)kTCPRecommendedBufSize);
        return memFullErr;
    }
    log_message("Allocated %ld bytes for TCP receive buffer at 0x%lX.", (long)kTCPRecommendedBufSize, (unsigned long)gTCPRecvBuffer);

    // Allocate buffer for the listener stream itself - USE RECOMMENDED SIZE
    gTCPListenRecvBuffer = NewPtrClear(kTCPRecommendedBufSize); // Use 8192 bytes
     if (gTCPListenRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP listen buffer (%ld bytes).", (long)kTCPRecommendedBufSize);
        DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated %ld bytes for TCP listen buffer at 0x%lX.", (long)kTCPRecommendedBufSize, (unsigned long)gTCPListenRecvBuffer);

    // --- Prepare TCPCreate Parameter Block ---
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L; // MacTCP will fill this on success
    pbCreate.csParam.create.rcvBuff = gTCPListenRecvBuffer; // Buffer for the listener stream itself
    pbCreate.csParam.create.rcvBuffLen = kTCPRecommendedBufSize; // Use 8192 bytes
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.userDataPtr = nil;

    log_message("Calling PBControlSync (TCPCreate) for passive listener on port %d...", PORT_TCP);
    err = PBControlSync((ParmBlkPtr)&pbCreate); // Pass the address of the local pbCreate

    if (err != noErr) {
        log_message("Error (InitTCPListener): TCPCreate failed. Error: %d", err);
        DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
        DisposePtr(gTCPListenRecvBuffer); gTCPListenRecvBuffer = NULL;
        return err;
    }

    // --- Check and store the returned stream pointer ---
    gTCPListenStream = pbCreate.tcpStream; // Get the stream pointer from the PB
    if (gTCPListenStream == NULL) {
        log_message("Error (InitTCPListener): TCPCreate succeeded but returned NULL stream.");
        DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
        DisposePtr(gTCPListenRecvBuffer); gTCPListenRecvBuffer = NULL;
        // Attempt to release the potentially created (but NULL?) stream just in case
        if (pbCreate.tcpStream != NULL) { // Should be NULL based on check, but defensive
             TCPiopb pbRelease;
             memset(&pbRelease, 0, sizeof(TCPiopb));
             pbRelease.ioCompletion = nil;
             pbRelease.ioCRefNum = macTCPRefNum;
             pbRelease.csCode = TCPRelease;
             pbRelease.tcpStream = pbCreate.tcpStream; // Use the one from pbCreate
             PBControlSync((ParmBlkPtr)&pbRelease); // Ignore error during cleanup
        }
        return ioErr;
    }
    log_message("TCP Listener Stream created successfully (StreamPtr: 0x%lX).", (unsigned long)gTCPListenStream);

    // --- Start listening asynchronously ---
    err = StartAsyncTCPListen(macTCPRefNum); // This function uses the global gTCPListenStream
    if (err != noErr && err != 1 /* 1 means request queued */) {
         log_message("Error (InitTCPListener): Failed to start initial async TCP listen. Error: %d", err);
         CleanupTCPListener(macTCPRefNum); // Cleanup will release the stream
         return err;
    }

    log_message("Initial asynchronous TCP listen STARTING on port %d.", PORT_TCP);
    return noErr;
}

void CleanupTCPListener(short macTCPRefNum) {
    OSErr err;
    log_message("Cleaning up TCP Listener...");

    // --- Release Active Connection Stream (if any) ---
    // Use a temporary variable to avoid race conditions if release completes instantly
    StreamPtr connStreamToRelease = gTCPConnectionStream;
    if (connStreamToRelease != NULL) {
        log_message("Attempting async release of active TCP connection stream 0x%lX...", (unsigned long)connStreamToRelease);
        gTCPConnectionStream = NULL; // Mark as gone immediately
        gTCPRecvPending = false;     // Cancel any pending receive on this stream

        // Initiate release (ignore errors during cleanup)
        StartAsyncTCPRelease(macTCPRefNum, connStreamToRelease, true); // true indicates connection stream

        // Wait briefly for release to potentially complete (best effort)
        log_message("Waiting briefly for potential async release of connection stream...");
        Delay(60, &gLastBroadcastTimeTicks); // Wait 1 second
        if (gTCPReleasePending && gTCPReleasePB.tcpStream == connStreamToRelease) {
            log_message("Warning: Async release of connection stream still pending during cleanup.");
            // Consider TCPAbort for forceful cleanup if needed, but usually release is sufficient
        }
    }

    // --- Release Listening Stream (if any) ---
    StreamPtr listenStreamToRelease = gTCPListenStream;
    if (listenStreamToRelease != NULL) {
        log_message("Attempting async release of TCP listening stream 0x%lX...", (unsigned long)listenStreamToRelease);
        gTCPListenStream = NULL; // Mark as gone immediately
        gTCPListenPending = false; // Cancel any pending listen

        // Initiate release (ignore errors during cleanup)
        StartAsyncTCPRelease(macTCPRefNum, listenStreamToRelease, false); // false indicates listener stream

        // Wait briefly
        log_message("Waiting briefly for potential async release of listening stream...");
        Delay(60, &gLastBroadcastTimeTicks);
         if (gTCPReleasePending && gTCPReleasePB.tcpStream == listenStreamToRelease) {
            log_message("Warning: Async release of listening stream still pending during cleanup.");
        }
    }

    // --- Dispose Buffers ---
    if (gTCPRecvBuffer != NULL) {
        log_message("Disposing TCP receive buffer at 0x%lX.", (unsigned long)gTCPRecvBuffer);
        DisposePtr(gTCPRecvBuffer);
        gTCPRecvBuffer = NULL;
    }
     if (gTCPListenRecvBuffer != NULL) {
        log_message("Disposing TCP listen buffer at 0x%lX.", (unsigned long)gTCPListenRecvBuffer);
        DisposePtr(gTCPListenRecvBuffer);
        gTCPListenRecvBuffer = NULL;
    }

    // Reset flags just in case
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPReleasePending = false;

    log_message("TCP Listener cleanup finished.");
}


void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr ioResult;
    OSErr err;

    // --- Poll Listener (TCPPassiveOpen) ---
    if (gTCPListenPending) {
        ioResult = gTCPListenPB.ioResult;
        if (ioResult != 1) { // 1 means still pending
            gTCPListenPending = false; // Request completed (success or error)

            // --- IMPORTANT: Store the original listener stream before checking result ---
            // Because gTCPListenPB.tcpStream gets overwritten on success
            StreamPtr originalListenerStream = gTCPListenStream; // Capture before potential overwrite

            if (ioResult == noErr) {
                // --- Connection Accepted ---

                // ** FIX: Capture the NEW connection stream pointer **
                StreamPtr newConnectionStream = gTCPListenPB.tcpStream;

                // Check if we already have an active connection
                if (gTCPConnectionStream != NULL) {
                    log_message("Warning: New TCP connection accepted while another was active (Old: 0x%lX, New: 0x%lX). Releasing old one.",
                                (unsigned long)gTCPConnectionStream, (unsigned long)newConnectionStream);
                    StartAsyncTCPRelease(macTCPRefNum, gTCPConnectionStream, true); // Release the old one
                }

                // ** FIX: Store the NEW connection details **
                gTCPConnectionStream = newConnectionStream;
                gCurrentConnectionIP = gTCPListenPB.csParam.open.remoteHost;
                gCurrentConnectionPort = gTCPListenPB.csParam.open.remotePort;

                char senderIPStr[INET_ADDRSTRLEN];
                AddrToStr(gCurrentConnectionIP, senderIPStr); // Convert IP for logging
                log_message("TCP Connection accepted from %s:%u (New Stream: 0x%lX).", senderIPStr, gCurrentConnectionPort, (unsigned long)gTCPConnectionStream);

                // Start receiving on the NEW connection stream
                err = StartAsyncTCPRecv(macTCPRefNum); // Uses gTCPConnectionStream
                if (err != noErr && err != 1) { // 1 means queued
                     log_message("Error (PollTCPListener): Failed to start async TCP receive after accept. Error: %d", err);
                     // If receive fails, release the newly accepted connection
                     StartAsyncTCPRelease(macTCPRefNum, gTCPConnectionStream, true);
                     gTCPConnectionStream = NULL; // Forget the bad connection
                }

                // Re-issue the listen on the ORIGINAL listening stream
                // ** FIX: Use the captured original listener stream pointer **
                gTCPListenStream = originalListenerStream; // Restore original listener pointer
                if (gTCPListenStream != NULL) {
                    err = StartAsyncTCPListen(macTCPRefNum); // Uses gTCPListenStream
                     if (err != noErr && err != 1) { // 1 means queued
                         log_message("CRITICAL Error (PollTCPListener): Failed to re-issue async TCP listen after accept. Error: %d. No longer listening!", err);
                         // Application can no longer accept new connections!
                     } else {
                         log_to_file_only("PollTCPListener: Re-issued async TCP listen on stream 0x%lX.", (unsigned long)gTCPListenStream);
                     }
                } else {
                     log_message("CRITICAL Error (PollTCPListener): Original listener stream was NULL when trying to re-listen!");
                }

            } else {
                // --- Listener Error ---
                log_message("Error (PollTCPListener): Async TCPPassiveOpen completed with error: %d on listener stream 0x%lX", ioResult, (unsigned long)originalListenerStream);
                 // Attempt to re-issue the listen even after an error, using the original stream
                 gTCPListenStream = originalListenerStream; // Ensure we use the correct stream
                 if (gTCPListenStream != NULL) {
                     err = StartAsyncTCPListen(macTCPRefNum);
                     if (err != noErr && err != 1) { // 1 means queued
                         log_message("CRITICAL Error (PollTCPListener): Failed to re-issue async TCP listen after error. Error: %d. No longer listening!", err);
                     } else {
                          log_to_file_only("PollTCPListener: Re-issued async TCP listen after previous error on stream 0x%lX.", (unsigned long)gTCPListenStream);
                     }
                 } else {
                      log_message("CRITICAL Error (PollTCPListener): Listener stream was NULL when trying to re-listen after error!");
                 }
            }
        }
        // else: ioResult == 1, listen still pending, do nothing this cycle
    }

    // --- Poll Receiver (TCPRcv on connection stream) ---
    if (gTCPRecvPending) {
        // Ensure this poll corresponds to the current connection stream
        if (gTCPRecvPB.tcpStream == gTCPConnectionStream && gTCPConnectionStream != NULL) {
            ioResult = gTCPRecvPB.ioResult;
            if (ioResult != 1) { // 1 means still pending
                gTCPRecvPending = false; // Request completed

                // Capture stream pointer before potential release
                StreamPtr completedRecvStream = gTCPRecvPB.tcpStream;

                if (ioResult == noErr) {
                    // --- Data Received ---
                    ProcessTCPReceive(macTCPRefNum, myLocalIP);
                    // Start the next receive ONLY if the connection wasn't closed in ProcessTCPReceive
                    if (gTCPConnectionStream == completedRecvStream && gTCPConnectionStream != NULL) {
                        err = StartAsyncTCPRecv(macTCPRefNum);
                         if (err != noErr && err != 1) { // 1 means queued
                             log_message("Error (PollTCPListener): Failed to start next async TCP receive. Error: %d. Releasing connection.", err);
                             StartAsyncTCPRelease(macTCPRefNum, gTCPConnectionStream, true);
                             gTCPConnectionStream = NULL; // Forget the connection
                         }
                    } else {
                         log_to_file_only("PollTCPListener: Receive completed, but connection stream changed/closed during processing. Not starting new receive.");
                    }
                } else if (ioResult == connectionClosing) {
                     // --- Graceful Close Received ---
                     char senderIPStr[INET_ADDRSTRLEN];
                     AddrToStr(gCurrentConnectionIP, senderIPStr); // Use the stored IP
                     log_message("TCP Connection closing gracefully from %s (Stream: 0x%lX). Releasing.", senderIPStr, (unsigned long)completedRecvStream);
                     StartAsyncTCPRelease(macTCPRefNum, completedRecvStream, true);
                     // Clear connection state if this was the active connection
                     if (gTCPConnectionStream == completedRecvStream) {
                         gTCPConnectionStream = NULL;
                     }
                }
                 else {
                     // --- Receive Error ---
                     char senderIPStr[INET_ADDRSTRLEN];
                     AddrToStr(gCurrentConnectionIP, senderIPStr); // Use the stored IP
                    log_message("Error (PollTCPListener): Async TCPRcv completed with error: %d from %s (Stream: 0x%lX). Releasing.", ioResult, senderIPStr, (unsigned long)completedRecvStream);
                    StartAsyncTCPRelease(macTCPRefNum, completedRecvStream, true);
                     // Clear connection state if this was the active connection
                     if (gTCPConnectionStream == completedRecvStream) {
                         gTCPConnectionStream = NULL;
                     }
                }
            }
            // else: ioResult == 1, receive still pending, do nothing this cycle
        } else if (gTCPConnectionStream == NULL) {
            // Receive completed for a stream that is no longer the active connection
            log_to_file_only("PollTCPListener: Ignoring completed TCPRcv (ioResult=%d) because gTCPConnectionStream is NULL.", gTCPRecvPB.ioResult);
            gTCPRecvPending = false; // Mark as processed
        } else {
             // Receive completed for a stream different than the current gTCPConnectionStream
             log_to_file_only("PollTCPListener: Ignoring completed TCPRcv (ioResult=%d) on stream 0x%lX; current connection is 0x%lX.",
                              gTCPRecvPB.ioResult, (unsigned long)gTCPRecvPB.tcpStream, (unsigned long)gTCPConnectionStream);
             gTCPRecvPending = false; // Mark as processed
        }
    }


    // --- Poll Releaser (TCPRelease) ---
    if (gTCPReleasePending) {
        ioResult = gTCPReleasePB.ioResult;
        if (ioResult != 1) { // 1 means still pending
            StreamPtr releasedStream = gTCPReleasePB.tcpStream; // Identify which stream was released
            gTCPReleasePending = false; // Request completed

            if (ioResult == noErr) {
                log_to_file_only("PollTCPListener: Async TCPRelease completed successfully for stream 0x%lX.", (unsigned long)releasedStream);
                // If this was the connection stream, gTCPConnectionStream should already be NULL
                // If this was the listener stream, gTCPListenStream should already be NULL
            } else {
                log_message("Error (PollTCPListener): Async TCPRelease for stream 0x%lX completed with error: %d", (unsigned long)releasedStream, ioResult);
                // The stream might still be in an indeterminate state
            }
        }
        // else: ioResult == 1, release still pending, do nothing this cycle
    }
}

// --- Static Helper Functions ---

static OSErr StartAsyncTCPListen(short macTCPRefNum) {
    OSErr err;
    // ** FIX: Check gTCPListenStream, not gTCPConnectionStream **
    if (gTCPListenStream == NULL) {
        log_message("Error (StartAsyncTCPListen): Cannot listen, gTCPListenStream is NULL.");
        return invalidStreamPtr;
    }
    if (gTCPListenPending) return 1; // Already listening

    memset(&gTCPListenPB, 0, sizeof(TCPiopb));
    gTCPListenPB.ioCompletion = nil; // Use polling
    gTCPListenPB.ioCRefNum = macTCPRefNum;
    gTCPListenPB.csCode = TCPPassiveOpen;
    // ** FIX: Use gTCPListenStream **
    gTCPListenPB.tcpStream = gTCPListenStream; // The stream created initially for listening
    gTCPListenPB.csParam.open.localPort = PORT_TCP;
    gTCPListenPB.csParam.open.localHost = 0L; // INADDR_ANY equivalent for MacTCP PassiveOpen
    gTCPListenPB.csParam.open.commandTimeoutValue = 0; // Infinite timeout for listen
    gTCPListenPB.csParam.open.userDataPtr = nil;
    gTCPListenPB.csParam.open.ulpTimeoutValue = 0; // Use default ULP timeout
    gTCPListenPB.csParam.open.ulpTimeoutAction = 1; // Abort on ULP timeout (0=report)
    gTCPListenPB.csParam.open.validityFlags = 0; // Use defaults for optional params
    gTCPListenPB.csParam.open.remoteHost = 0L; // Accept from any host
    gTCPListenPB.csParam.open.remotePort = 0; // Accept from any port

    gTCPListenPending = true;
    err = PBControlAsync((ParmBlkPtr)&gTCPListenPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPListen): PBControlAsync(TCPPassiveOpen) failed immediately. Error: %d", err);
        gTCPListenPending = false; // Failed to start
        return err;
    }
    // Successfully queued
    log_to_file_only("StartAsyncTCPListen: Async TCPPassiveOpen initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return err; // Will be 1 (request queued) or noErr if completed instantly (unlikely)
}

static OSErr StartAsyncTCPRecv(short macTCPRefNum) {
    OSErr err;
    if (gTCPConnectionStream == NULL) {
        log_message("Error (StartAsyncTCPRecv): Cannot receive, gTCPConnectionStream is NULL.");
        return invalidStreamPtr;
     }
    if (gTCPRecvBuffer == NULL) {
         log_message("Error (StartAsyncTCPRecv): Cannot receive, gTCPRecvBuffer is NULL.");
         return invalidBufPtr;
    }
    if (gTCPRecvPending) return 1; // Already receiving

    memset(&gTCPRecvPB, 0, sizeof(TCPiopb));
    gTCPRecvPB.ioCompletion = nil; // Use polling
    gTCPRecvPB.ioCRefNum = macTCPRefNum;
    gTCPRecvPB.csCode = TCPRcv;
    gTCPRecvPB.tcpStream = gTCPConnectionStream; // Use the active connection stream
    gTCPRecvPB.csParam.receive.rcvBuff = gTCPRecvBuffer; // The buffer for incoming data
    gTCPRecvPB.csParam.receive.rcvBuffLen = kTCPRecommendedBufSize; // Size of the buffer
    gTCPRecvPB.csParam.receive.commandTimeoutValue = 0; // Infinite timeout for receive
    gTCPRecvPB.csParam.receive.userDataPtr = nil;

    gTCPRecvPending = true;
    err = PBControlAsync((ParmBlkPtr)&gTCPRecvPB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPRecv): PBControlAsync(TCPRcv) failed immediately. Error: %d", err);
        gTCPRecvPending = false; // Failed to start
        return err;
    }
    // Successfully queued
    log_to_file_only("StartAsyncTCPRecv: Async TCPRcv initiated on connection stream 0x%lX.", (unsigned long)gTCPConnectionStream);
    return err; // Will be 1 (request queued) or noErr
}

// Added boolean to distinguish which stream type is being released for logging/debugging
static OSErr StartAsyncTCPRelease(short macTCPRefNum, StreamPtr streamToRelease, Boolean isConnectionStream) {
    OSErr err;
    const char *streamTypeStr = isConnectionStream ? "connection" : "listener";

    if (streamToRelease == NULL) {
        log_message("Error (StartAsyncTCPRelease): Cannot release NULL %s stream.", streamTypeStr);
        return invalidStreamPtr;
    }

    // Allow initiating release even if another is pending, but log it.
    if (gTCPReleasePending) {
         log_message("Warning (StartAsyncTCPRelease): Initiating release for %s stream 0x%lX while another release is pending.", streamTypeStr, (unsigned long)streamToRelease);
         // Potentially problematic if the pending release is for the *same* stream.
         // If the PBs are different (gTCPReleasePB vs a local one), it might be okay,
         // but using a single global gTCPReleasePB means this new call will overwrite the pending one.
         // For cleanup, this might be acceptable, but in active use, needs care.
    }

    memset(&gTCPReleasePB, 0, sizeof(TCPiopb)); // Use the global PB
    gTCPReleasePB.ioCompletion = nil; // Use polling
    gTCPReleasePB.ioCRefNum = macTCPRefNum;
    gTCPReleasePB.csCode = TCPRelease;
    gTCPReleasePB.tcpStream = streamToRelease; // The stream to close

    gTCPReleasePending = true; // Mark that *a* release is pending using the global PB
    err = PBControlAsync((ParmBlkPtr)&gTCPReleasePB);
    if (err != noErr) {
        log_message("Error (StartAsyncTCPRelease): PBControlAsync(TCPRelease) failed immediately for %s stream 0x%lX. Error: %d", streamTypeStr, (unsigned long)streamToRelease, err);
        gTCPReleasePending = false; // Failed to start
        return err;
    }
    // Successfully queued
    log_to_file_only("StartAsyncTCPRelease: Initiated async release for %s stream 0x%lX.", streamTypeStr, (unsigned long)streamToRelease);
    return err; // Will be 1 (request queued) or noErr
}


static void ProcessTCPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE]; // Ensure this matches protocol.c needs
    unsigned short dataLength;

    // Get the length of received data from the completed PB
    // ** FIX: Ensure we are processing data for the correct stream **
    if (gTCPRecvPB.tcpStream != gTCPConnectionStream || gTCPConnectionStream == NULL) {
        log_message("Warning (ProcessTCPReceive): Received data for stream 0x%lX but current connection is 0x%lX. Ignoring.",
                    (unsigned long)gTCPRecvPB.tcpStream, (unsigned long)gTCPConnectionStream);
        return; // Don't process data for an old/invalid stream
    }

    dataLength = gTCPRecvPB.csParam.receive.rcvBuffLen;

    if (dataLength > 0) {
        // Get the sender's IP from the stored connection info
        AddrToStr(gCurrentConnectionIP, senderIPStrFromConnection);

        // Attempt to parse the message
        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Parsed '%s' from %s@%s (Payload IP: %s). Content: '%.30s...'",
                       msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload, content);

            // Update peer list based on the connection IP
            int addResult = AddOrUpdatePeer(senderIPStrFromConnection, senderUsername);
             if (addResult > 0) { // Peer was newly added
                 log_message("New peer connected/updated via TCP: %s@%s", senderUsername, senderIPStrFromConnection);
                 UpdatePeerDisplayList(true); // Force redraw if new peer
             } else if (addResult < 0) { // List was full
                 log_message("Peer list full, could not add %s@%s from TCP connection", senderUsername, senderIPStrFromConnection);
             }
             // else addResult == 0, peer was updated, no redraw needed unless forced elsewhere

            // Handle specific message types
            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_message("Peer %s@%s has sent QUIT notification via TCP.", senderUsername, senderIPStrFromConnection);
                if (MarkPeerInactive(senderIPStrFromConnection)) {
                     UpdatePeerDisplayList(true); // Force redraw if peer removed
                }
                // ** FIX: Close the connection after receiving QUIT **
                log_message("Closing connection to %s due to QUIT.", senderIPStrFromConnection);
                StartAsyncTCPRelease(macTCPRefNum, gTCPConnectionStream, true); // Release this connection
                gTCPConnectionStream = NULL; // Mark connection as gone immediately
                gTCPRecvPending = false; // Stop trying to receive on this closed stream

            } else if (strcmp(msgType, MSG_TEXT) == 0) {
                char displayMsg[BUFFER_SIZE + 100]; // Make sure this is large enough
                 sprintf(displayMsg, "%s: %s", senderUsername, content);
                 AppendToMessagesTE(displayMsg);
                 AppendToMessagesTE("\r"); // Add newline for display
                 log_message("Message from %s@%s: %s", senderUsername, senderIPStrFromConnection, content);
            } else {
                 // Handle other potential TCP message types if needed
                 log_message("Received unhandled TCP message type '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection);
            }
        } else {
            // Parsing failed
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else {
        // Received 0 bytes - often indicates graceful close initiated by remote
        log_to_file_only("ProcessTCPReceive: Received 0 bytes (graceful close initiated by remote?).");
        // The connectionClosing error in PollTCPListener will handle the release.
    }
    // Note: The next receive is started in PollTCPListener after this function returns,
    // *unless* we closed the connection here due to QUIT.
}