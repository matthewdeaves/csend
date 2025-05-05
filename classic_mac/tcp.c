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
#include <Memory.h> // For NewPtrClear, DisposePtr, BlockMoveData
#include <string.h> // For memset, strcmp, strlen
#include <stdio.h>  // For sprintf
#include <stdlib.h> // For size_t if needed
#include <Events.h> // For TickCount, Delay
#include <OSUtils.h> // For Delay

// --- Constants ---
#define kTCPRecvBufferSize 8192           // Buffer for receiving data on listener stream
#define kTCPListenInternalBufferSize 8192 // Internal buffer for the listener stream
#define kTCPSendInternalBufferSize 2048   // Internal buffer for temporary sender streams
#define kTCPListenRetryDelayTicks 60      // Delay before retrying listen after certain errors
#define kConnectTimeoutTicks 300          // Timeout for TCPActiveOpen (5 sec)
#define kSendTimeoutTicks 180             // Timeout for TCPSend (3 sec)
#define kCloseTimeoutTicks 120            // Timeout for TCPClose (2 sec)
#define kQuitLoopDelayTicks 15            // Delay+Yield between QUIT sends (approx 1/4 sec)
#define kMacTCPTimeoutErr (-23014)        // MacTCP ULP timeout error code
#define AbortTrue 1                       // ULP Timeout Action: Abort connection

// --- Globals ---

// Listener Stream (for incoming connections)
static StreamPtr gTCPListenStream = NULL;
static Ptr gTCPListenInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;           // Receive buffer for listener stream
static TCPiopb gListenPB;                   // For TCPPassiveOpen
static TCPiopb gRecvPB;                     // For TCPRcv
static TCPiopb gClosePB;                    // For TCPClose (incoming connection)
// static TCPiopb gListenReleasePB;         // Release is synchronous, no PB needed

// Listener State Machine
static TCPListenerState gListenerState = TCP_LSTATE_UNINITIALIZED;
static TCPiopb *gListenPendingOpPB = NULL; // PB for the listener's current async operation
static Boolean gNeedToListen = false;      // Flag: should start listening when idle?

// Incoming connection details
static ip_addr gPeerIP = 0;
static tcp_port gPeerPort = 0;


// --- Forward Declarations ---
static OSErr StartListening(void);
static OSErr StartRecv(void);
static OSErr StartCloseIncoming(void); // Close the accepted incoming connection gracefully

static void HandleListenComplete(OSErr ioResult);
static void HandleRecvComplete(OSErr ioResult);
static void HandleCloseIncomingComplete(OSErr ioResult);

static void ProcessTCPReceive(void);
static OSErr KillListenerPendingOperation(const char* callerContext);

// Low-level sync helpers
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSync(StreamPtr streamPtr, SInt8 timeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPAbortSync(StreamPtr streamPtr, GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);

// Internal synchronous send helper using temporary stream
static OSErr InternalSyncTCPSend(ip_addr targetIP, tcp_port targetPort,
                                 const char *messageBuffer, int messageLen,
                                 SInt8 connectTimeoutTicks, SInt8 sendTimeoutTicks,
                                 GiveTimePtr giveTime);


// --- Shared Logic Callbacks (implementation unchanged) ---
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
    (void)platform_context; (void)ip;
    char displayMsg[BUFFER_SIZE + 100];
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r");
        log_message("Message from %s@%s: %s", username, ip, message_content);
    } else {
        log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready.");
    }
}
static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) {
    (void)platform_context; if (!ip) return;
    log_message("Peer %s has sent QUIT notification via TCP.", ip);
    if (MarkPeerInactive(ip)) {
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    }
}

// --- Public Functions ---

OSErr InitTCP(short macTCPRefNum) {
    OSErr err;
    // Only initialize Listener stream here
    log_message("Initializing TCP Listener Stream...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPListenStream != NULL || gListenerState != TCP_LSTATE_UNINITIALIZED) {
        log_message("Error (InitTCP): Already initialized or in unexpected state (%d)?", gListenerState);
        return streamAlreadyOpen;
    }

    // Allocate Listener buffers
    gTCPListenInternalBuffer = NewPtrClear(kTCPListenInternalBufferSize);
    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);

    if (gTCPListenInternalBuffer == NULL || gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate Listener TCP buffers.");
        if (gTCPListenInternalBuffer) DisposePtr(gTCPListenInternalBuffer);
        if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer);
        gTCPListenInternalBuffer = gTCPRecvBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated Listener TCP buffers (Internal: %ld, Recv: %ld).",
                (long)kTCPListenInternalBufferSize, (long)kTCPRecvBufferSize);

    // Create Listener Stream
    log_message("Creating Listener Stream...");
    err = LowTCPCreateSync(macTCPRefNum, &gTCPListenStream, gTCPListenInternalBuffer, kTCPListenInternalBufferSize);
    if (err != noErr || gTCPListenStream == NULL) {
        log_message("Error: Failed to create Listener Stream: %d", err);
        CleanupTCP(macTCPRefNum); // Clean up allocated buffers
        return err;
    }
    log_message("Listener Stream created (0x%lX).", (unsigned long)gTCPListenStream);

    // Initialize state
    gListenerState = TCP_LSTATE_IDLE;
    gListenPendingOpPB = NULL;
    gNeedToListen = true; // Start by wanting to listen

    // Initiate the first listen
    err = StartListening();
    if (err != noErr && err != 1) { // 1 means pending
         log_message("Error: Failed to start initial async TCP listen: %d. Cleaning up.", err);
         CleanupTCP(macTCPRefNum);
         return err;
    }

    log_message("Initial asynchronous TCP listen STARTING on port %d.", PORT_TCP);
    return noErr;
}

void CleanupTCP(short macTCPRefNum) {
    OSErr killErr = noErr;
    log_message("Cleaning up TCP Listener Stream..."); // Only listener now

    // --- Cleanup Listener Stream ---
    StreamPtr listenStreamToRelease = gTCPListenStream;
    gTCPListenStream = NULL; // Prevent further use

    // Kill pending listener operation
    if (gListenPendingOpPB != NULL) {
        log_message("Cleanup: Killing pending listener operation (State: %d)...", gListenerState);
        killErr = PBKillIO((ParmBlkPtr)gListenPendingOpPB, false); // Async kill
        if (killErr != noErr) log_message("Warning: PBKillIO failed for listener: %d", killErr);
        gListenPendingOpPB = NULL;
    }

    // Release listener stream synchronously
    if (listenStreamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting sync release of listener stream 0x%lX...", (unsigned long)listenStreamToRelease);
        OSErr relErr = LowTCPReleaseSync(macTCPRefNum, listenStreamToRelease);
        if (relErr != noErr) log_message("Warning: Sync release failed for listener: %d", relErr);
        else log_to_file_only("Sync release successful for listener.");
    } else if (listenStreamToRelease != NULL) {
        log_message("Warning: Cannot release listener stream, MacTCP refnum is 0.");
    }

    // Reset listener state
    gListenerState = TCP_LSTATE_UNINITIALIZED;
    gListenPendingOpPB = NULL;
    gNeedToListen = false;
    gPeerIP = 0; gPeerPort = 0;

    // --- Dispose Buffers ---
    if (gTCPRecvBuffer != NULL) {
        log_message("Disposing TCP receive buffer.");
        DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
    }
     if (gTCPListenInternalBuffer != NULL) {
        log_message("Disposing TCP listener internal buffer.");
        DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
    }
    // No sender buffer/stream to clean up here

    log_message("TCP Stream cleanup finished.");
}


void PollTCPListener(short macTCPRefNum) {
    OSErr ioResult;
    TCPListenerState previousState;

    // If listener stream is gone or in error state, do nothing
    if (gTCPListenStream == NULL || gListenerState == TCP_LSTATE_UNINITIALIZED || gListenerState == TCP_LSTATE_ERROR || gListenerState == TCP_LSTATE_RELEASING) {
        return;
    }

    // Check if we need to start listening
    if (gListenPendingOpPB == NULL && gListenerState == TCP_LSTATE_IDLE && gNeedToListen) {
        log_to_file_only("PollListener: Stream idle and needs to listen. Starting listen.");
        StartListening(); // Ignore error, will retry on next poll if fails
        return; // Don't check completion immediately
    }

    // Check if a listener operation is pending
    if (gListenPendingOpPB != NULL) {
        ioResult = gListenPendingOpPB->ioResult;

        if (ioResult > 0) {
            // Still pending, do nothing
            return;
        }

        // --- Listener Operation Completed (Success or Error) ---
        log_to_file_only("PollListener: Pending operation (State: %d) completed with result: %d", gListenerState, ioResult);

        // Capture state *before* clearing gListenPendingOpPB and calling handler
        previousState = gListenerState;
        gListenPendingOpPB = NULL; // Clear pending operation pointer

        // Call the appropriate handler based on the state *when the operation was started*
        switch (previousState) {
            case TCP_LSTATE_LISTENING:
                HandleListenComplete(ioResult);
                break;
            case TCP_LSTATE_RECEIVING:
                HandleRecvComplete(ioResult);
                break;
            case TCP_LSTATE_CLOSING:
                HandleCloseIncomingComplete(ioResult);
                break;
            // Should not have pending ops in these states
            case TCP_LSTATE_IDLE:
            case TCP_LSTATE_UNINITIALIZED:
            case TCP_LSTATE_ERROR:
            case TCP_LSTATE_RELEASING:
                 log_message("PollListener: CRITICAL - Operation completed but listener was in unexpected state %d!", previousState);
                 gListenerState = TCP_LSTATE_IDLE;
                 gNeedToListen = true;
                break;
        }
    }
}

// Public function to get listener state
TCPListenerState GetTCPListenerState(void) {
    return gListenerState;
}


// Sends a text message SYNCHRONOUSLY using a temporary stream.
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime) {
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE]; // Local buffer for formatted message
    int formattedLen;

    log_to_file_only("TCP_SendTextMessageSync: Attempting to send TEXT to %s", peerIP);

    if (gMacTCPRefNum == 0) return notOpenErr;
    if (peerIP == NULL || message == NULL || giveTime == NULL) return paramErr;

    // Parse IP
    err = ParseIPv4(peerIP, &targetIP);
    if (err != noErr || targetIP == 0) {
        log_message("Error (SendText): Could not parse peer IP '%s'.", peerIP);
        return paramErr;
    }

    // Format message into local buffer
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT,
                                  gMyUsername, gMyLocalIPStr, message);
    if (formattedLen <= 0) {
        log_message("Error (SendText): Failed to format TEXT message for %s.", peerIP);
        return paramErr;
    }

    // Use the internal helper that manages a temporary stream
    err = InternalSyncTCPSend(targetIP, PORT_TCP, messageBuffer, formattedLen,
                              kConnectTimeoutTicks, kSendTimeoutTicks, giveTime);

    if (err == noErr) log_message("Successfully sent TEXT message to %s.", peerIP);
    else log_message("Failed to send TEXT message to %s (Error: %d).", peerIP, err);

    return err;
}


// Sends QUIT messages SYNCHRONOUSLY using temporary streams with delay.
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime) {
    int i;
    OSErr lastErr = noErr;
    char quitMessageBuffer[BUFFER_SIZE]; // Local buffer
    int formattedLen;
    int activePeerCount = 0;
    int sentCount = 0;
    unsigned long dummyTimer; // For Delay

    log_message("TCP_SendQuitMessagesSync: Starting...");

    if (gMacTCPRefNum == 0) return notOpenErr;
    if (giveTime == NULL) return paramErr;

    // Format the QUIT message once
    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT,
                                  gMyUsername, gMyLocalIPStr, "");
    if (formattedLen <= 0) {
        log_message("Error (SendQuit): Failed to format QUIT message.");
        return paramErr;
    }

    // Count active peers
    for (i = 0; i < MAX_PEERS; ++i) if (gPeerManager.peers[i].active) activePeerCount++;
    log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount);
    if (activePeerCount == 0) return noErr;

    // Iterate through peers
    for (i = 0; i < MAX_PEERS; ++i) {
        if (gPeerManager.peers[i].active) {
            ip_addr currentTargetIP = 0;
            OSErr parseErr, sendErr;

            log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s",
                        gPeerManager.peers[i].username, gPeerManager.peers[i].ip);

            // Parse IP
            parseErr = ParseIPv4(gPeerManager.peers[i].ip, &currentTargetIP);
            if (parseErr != noErr || currentTargetIP == 0) {
                log_message("Error (SendQuit): Could not parse peer IP '%s'. Skipping.", gPeerManager.peers[i].ip);
                lastErr = parseErr;
                continue;
            }

            // Use the internal helper that manages a temporary stream
            sendErr = InternalSyncTCPSend(currentTargetIP, PORT_TCP, quitMessageBuffer, formattedLen,
                                          kConnectTimeoutTicks, kSendTimeoutTicks, giveTime); // Use standard timeouts

            if (sendErr == noErr) {
                log_message("Successfully sent QUIT to %s.", gPeerManager.peers[i].ip);
                sentCount++;
            } else {
                log_message("Error sending QUIT to %s: %d", gPeerManager.peers[i].ip, sendErr);
                lastErr = sendErr;
            }

            // *** ADD YIELD AND DELAY BETWEEN PEERS ***
            log_to_file_only("SendQuit: Yielding/Delaying (%d ticks) after peer %s...", kQuitLoopDelayTicks, gPeerManager.peers[i].ip);
            giveTime(); // Basic yield
            Delay(kQuitLoopDelayTicks, &dummyTimer); // Add specific delay

        } // end if peer active
    } // end for loop

    log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers. Last error: %d.", sentCount, activePeerCount, lastErr);
    return lastErr;
}


// --- Private Listener State Machine Helpers ---

// Kill the currently pending listener operation, if any.
static OSErr KillListenerPendingOperation(const char* callerContext) {
    OSErr killErr = noErr;
    if (gListenPendingOpPB != NULL) {
        TCPListenerState stateBeforeKill = gListenerState;
        log_message("Info (%s): Attempting to kill pending listener operation (State: %d)...", callerContext, stateBeforeKill);

        killErr = PBKillIO((ParmBlkPtr)gListenPendingOpPB, false); // Async kill

        if (killErr == noErr) {
            log_to_file_only("Info (%s): PBKillIO initiated successfully for listener.", callerContext);
            // Assume kill works, clear state
            gListenPendingOpPB = NULL;
            gListenerState = TCP_LSTATE_IDLE;
            return noErr;
        } else {
            log_message("Error (%s): PBKillIO failed immediately for listener: %d. State left as %d.", callerContext, killErr, stateBeforeKill);
            return killErr;
        }
    }
    return noErr; // Nothing to kill
}


// Initiate TCPPassiveOpen on the listener stream
static OSErr StartListening(void) {
    OSErr err;
    if (gTCPListenStream == NULL || gListenerState == TCP_LSTATE_ERROR || gListenerState == TCP_LSTATE_RELEASING) return invalidStreamPtr;
    if (gListenPendingOpPB != NULL || gListenerState != TCP_LSTATE_IDLE) {
        log_to_file_only("StartListening: Cannot start, pending op exists or state (%d) not IDLE.", gListenerState);
        return inProgress;
    }

    log_to_file_only("StartListening: Initiating TCPPassiveOpen...");
    memset(&gListenPB, 0, sizeof(TCPiopb));
    gListenPB.ioCompletion = nil;
    gListenPB.ioCRefNum = gMacTCPRefNum;
    gListenPB.csCode = TCPPassiveOpen;
    gListenPB.tcpStream = gTCPListenStream;
    gListenPB.csParam.open.validityFlags = 0;
    gListenPB.csParam.open.localPort = PORT_TCP;
    gListenPB.csParam.open.localHost = 0L;
    gListenPB.csParam.open.remoteHost = 0L;
    gListenPB.csParam.open.remotePort = 0;

    gListenerState = TCP_LSTATE_LISTENING; // Set state *before* async call
    gListenPendingOpPB = &gListenPB;
    err = PBControlAsync((ParmBlkPtr)&gListenPB);
    if (err != noErr) {
        log_message("Error (StartListening): PBControlAsync failed immediately: %d", err);
        gListenPendingOpPB = NULL;
        gListenerState = TCP_LSTATE_IDLE; // Revert state
        gNeedToListen = true; // Ensure retry
        return err;
    }
    return 1; // Pending
}

// Initiate TCPRcv on the listener stream
static OSErr StartRecv(void) {
    OSErr err;
    if (gTCPListenStream == NULL || gListenerState == TCP_LSTATE_ERROR || gListenerState == TCP_LSTATE_RELEASING) return invalidStreamPtr;
    // Should be called when connection is established (state LISTENING after completion)
     if (gListenPendingOpPB != NULL || (gListenerState != TCP_LSTATE_LISTENING && gListenerState != TCP_LSTATE_RECEIVING)) {
        log_message("StartRecv: Cannot start, pending op exists or state (%d) invalid.", gListenerState);
        return (gListenPendingOpPB != NULL) ? inProgress : paramErr;
    }
     if (gTCPRecvBuffer == NULL) return invalidBufPtr;

    log_to_file_only("StartRecv: Initiating TCPRcv...");
    memset(&gRecvPB, 0, sizeof(TCPiopb));
    gRecvPB.ioCompletion = nil;
    gRecvPB.ioCRefNum = gMacTCPRefNum;
    gRecvPB.csCode = TCPRcv;
    gRecvPB.tcpStream = gTCPListenStream;
    gRecvPB.csParam.receive.rcvBuff = gTCPRecvBuffer;
    gRecvPB.csParam.receive.rcvBuffLen = kTCPRecvBufferSize;
    gRecvPB.csParam.receive.commandTimeoutValue = 0;

    gListenerState = TCP_LSTATE_RECEIVING; // Set state *before* async call
    gListenPendingOpPB = &gRecvPB;
    err = PBControlAsync((ParmBlkPtr)&gRecvPB);
    if (err != noErr) {
        log_message("Error (StartRecv): PBControlAsync failed immediately: %d", err);
        gListenPendingOpPB = NULL;
        gListenerState = TCP_LSTATE_IDLE; // Revert state? Or CLOSING? Go IDLE.
        gNeedToListen = true;
        return err;
    }
    return 1; // Pending
}

// Initiate TCPClose on the listener stream (for incoming connection)
static OSErr StartCloseIncoming(void) {
    OSErr err;
    if (gTCPListenStream == NULL || gListenerState == TCP_LSTATE_ERROR || gListenerState == TCP_LSTATE_RELEASING) return invalidStreamPtr;
    // Can be called after Recv error or processing QUIT

    // Kill pending Recv if any
    err = KillListenerPendingOperation("StartCloseIncoming");
    if (err != noErr) {
        log_message("Error (StartCloseIncoming): Failed to kill prior operation (%d). Cannot initiate close.", err);
        gListenerState = TCP_LSTATE_ERROR;
        return err;
    }
    // State should now be IDLE

    log_to_file_only("StartCloseIncoming: Initiating TCPClose (graceful)...");
    memset(&gClosePB, 0, sizeof(TCPiopb));
    gClosePB.ioCompletion = nil;
    gClosePB.ioCRefNum = gMacTCPRefNum;
    gClosePB.csCode = TCPClose;
    gClosePB.tcpStream = gTCPListenStream;
    gClosePB.csParam.close.validityFlags = timeoutValue | timeoutAction;
    gClosePB.csParam.close.ulpTimeoutValue = kCloseTimeoutTicks;
    gClosePB.csParam.close.ulpTimeoutAction = AbortTrue; // Abort if close times out

    gListenerState = TCP_LSTATE_CLOSING; // Set state *before* async call
    gListenPendingOpPB = &gClosePB;
    err = PBControlAsync((ParmBlkPtr)&gClosePB);
    if (err != noErr) {
        log_message("Error (StartCloseIncoming): PBControlAsync failed immediately: %d. Assuming closed.", err);
        gListenPendingOpPB = NULL;
        gListenerState = TCP_LSTATE_IDLE; // Revert state to IDLE
        gNeedToListen = true; // Try listening again
        return noErr; // Goal (closed) likely achieved
    }
    return 1; // Pending
}

// --- Listener State Completion Handlers ---

static void HandleListenComplete(OSErr ioResult) {
    if (ioResult == noErr) {
        // Connection indication received
        gPeerIP = gListenPB.csParam.open.remoteHost;
        gPeerPort = gListenPB.csParam.open.remotePort;
        char senderIPStr[INET_ADDRSTRLEN];
        AddrToStr(gPeerIP, senderIPStr);
        log_message("HandleListenComplete: Connection Indication from %s:%u.", senderIPStr, gPeerPort);

        // Start receiving data
        OSErr err = StartRecv();
        if (err != noErr && err != 1) { // 1=pending
            log_message("Error (HandleListenComplete): Failed to start receive: %d. Closing incoming.", err);
            StartCloseIncoming(); // Close gracefully
        }
        // State is now RECEIVING (set by StartRecv) or CLOSING
    } else {
        // Handle errors
        const char *errMsg = "Unknown Error";
        Boolean isConnectionExists = (ioResult == connectionExists);
        Boolean isInvalidStream = (ioResult == invalidStreamPtr);
        Boolean isReqAborted = (ioResult == reqAborted);
        if (isConnectionExists) errMsg = "connectionExists (-23007)";
        else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isReqAborted) errMsg = "reqAborted (-23015)";

        if (isReqAborted) {
             log_to_file_only("HandleListenComplete: Listen aborted (result %d).", ioResult);
             gListenerState = TCP_LSTATE_IDLE; // Ensure state is IDLE
        } else {
            log_message("Error (HandleListenComplete): TCPPassiveOpen failed: %d (%s)", ioResult, errMsg);
            gListenerState = TCP_LSTATE_IDLE; // Go back to idle
            if (isInvalidStream) {
                 log_message("CRITICAL: Listener Stream invalid. Setting state to ERROR.");
                 gListenerState = TCP_LSTATE_ERROR;
                 gNeedToListen = false;
            } else {
                 gNeedToListen = true; // Try listening again later
                 if (isConnectionExists) {
                     unsigned long dummyTimer;
                     Delay(kTCPListenRetryDelayTicks, &dummyTimer);
                 }
            }
        }
    }
}

static void HandleRecvComplete(OSErr ioResult) {
     if (ioResult == noErr) {
        // Data received successfully
        ProcessTCPReceive(); // Process data in gTCPRecvBuffer

        // If ProcessTCPReceive didn't trigger a close, start next receive
        if (gListenerState == TCP_LSTATE_RECEIVING) { // Check state hasn't changed
            OSErr err = StartRecv();
             if (err != noErr && err != 1) {
                 log_message("Error (HandleRecvComplete): Failed to start next receive: %d. Closing.", err);
                 StartCloseIncoming();
             }
             // State remains RECEIVING (set by StartRecv) or CLOSING
        } else {
             log_to_file_only("HandleRecvComplete: State changed during processing (now %d). Not starting new receive.", gListenerState);
        }
    } else if (ioResult == connectionClosing) {
         // Peer initiated graceful close
         char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
         log_message("HandleRecvComplete: Connection closing gracefully from %s.", peerIPStr);
         ProcessTCPReceive(); // Process any final data
         gListenerState = TCP_LSTATE_IDLE; // Go back to idle
         gNeedToListen = true; // Listen again
    } else {
         // Handle receive errors
         char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr);
         const char *errMsg = "Unknown Error";
         Boolean isConnTerminated = (ioResult == connectionTerminated);
         Boolean isInvalidStream = (ioResult == invalidStreamPtr);
         Boolean isReqAborted = (ioResult == reqAborted);
         if (isConnTerminated) errMsg = "connectionTerminated (-23012)";
         else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
         else if (isReqAborted) errMsg = "reqAborted (-23015)";

        if (isReqAborted) {
            log_to_file_only("HandleRecvComplete: Receive aborted (result %d).", ioResult);
            gListenerState = TCP_LSTATE_IDLE;
        } else {
            log_message("Error (HandleRecvComplete): Receive failed from %s: %d (%s).", peerIPStr, ioResult, errMsg);
            if (isInvalidStream) {
                 log_message("CRITICAL: Listener Stream invalid. Setting state to ERROR.");
                 gListenerState = TCP_LSTATE_ERROR;
                 gNeedToListen = false;
            } else {
                 // For other errors (like terminated), close our end gracefully
                 StartCloseIncoming();
                 // State is now CLOSING
            }
        }
    }
}

static void HandleCloseIncomingComplete(OSErr ioResult) {
    // Graceful close of incoming connection completed (or failed)
    if (ioResult == noErr) {
        log_to_file_only("HandleCloseIncomingComplete: Graceful close completed.");
    } else {
        log_message("Warning (HandleCloseIncomingComplete): Graceful close failed: %d", ioResult);
    }
    gListenerState = TCP_LSTATE_IDLE; // Connection is closed, return to idle
    gNeedToListen = true; // Try listening again
}


// Process data received in gTCPRecvBuffer
static void ProcessTCPReceive(void) {
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

    // Use the static gRecvPB directly here.
    dataLength = gRecvPB.csParam.receive.rcvBuffLen;

    if (dataLength > 0) {
        OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection);
        if (addrErr != noErr) {
            sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF);
            log_to_file_only("ProcessTCPReceive: AddrToStr failed (%d). Using fallback '%s'.", addrErr, senderIPStrFromConnection);
        }

        if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection);
            handle_received_tcp_message(senderIPStrFromConnection, senderUsername, msgType, content, &mac_callbacks, NULL);

            // If QUIT received, initiate close immediately
            if (strcmp(msgType, MSG_QUIT) == 0) {
                 log_message("ProcessTCPReceive: QUIT received from %s. Initiating close.", senderIPStrFromConnection);
                 StartCloseIncoming(); // Close gracefully
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing).");
    }
}


// --- Low-Level Synchronous TCP Helpers ---

// Creates a TCP stream synchronously. (Unchanged)
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen)
{
    OSErr err; TCPiopb pbCreate;
    if (streamPtr == NULL || connectionBuffer == NULL) return paramErr;
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil; pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate; pbCreate.tcpStream = 0L;
    pbCreate.csParam.create.rcvBuff = connectionBuffer;
    pbCreate.csParam.create.rcvBuffLen = connBufferLen;
    pbCreate.csParam.create.notifyProc = nil;
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    if (err == noErr) {
        *streamPtr = pbCreate.tcpStream;
        if (*streamPtr == NULL) { log_message("Error (LowTCPCreateSync): NULL stream."); err = ioErr; }
    } else { *streamPtr = NULL; }
    return err;
}

// Opens a TCP connection synchronously using TCPActiveOpen.
// Uses polling with giveTime.
static OSErr LowTCPOpenConnectionSync(StreamPtr streamPtr, SInt8 timeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;

    if (giveTime == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;

    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) { log_message("Error (LowTCPOpenConnectionSync): Failed alloc PB."); return memFullErr; }

    pBlock->ioCompletion = nil; pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPActiveOpen; pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;
    pBlock->csParam.open.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.open.ulpTimeoutAction = AbortTrue;
    pBlock->csParam.open.validityFlags = timeoutValue | timeoutAction;
    pBlock->csParam.open.commandTimeoutValue = 0;
    pBlock->csParam.open.remoteHost = remoteHost;
    pBlock->csParam.open.remotePort = remotePort;
    pBlock->csParam.open.localPort = 0; pBlock->csParam.open.localHost = 0;

    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPOpenConnectionSync): PBControlAsync failed: %d", err);
        DisposePtr((Ptr)pBlock); return err;
    }
    while (pBlock->ioResult > 0) { giveTime(); } // Poll
    err = pBlock->ioResult;
    DisposePtr((Ptr)pBlock);
    return err;
}

// Sends data synchronously using TCPSend.
// Uses polling with giveTime.
static OSErr LowTCPSendSync(StreamPtr streamPtr, SInt8 timeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;

    if (giveTime == NULL || wdsPtr == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;

    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) { log_message("Error (LowTCPSendSync): Failed alloc PB."); return memFullErr; }

    pBlock->ioCompletion = nil; pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPSend; pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;
    pBlock->csParam.send.ulpTimeoutValue = timeoutTicks;
    pBlock->csParam.send.ulpTimeoutAction = AbortTrue;
    pBlock->csParam.send.validityFlags = timeoutValue | timeoutAction;
    pBlock->csParam.send.pushFlag = push;
    pBlock->csParam.send.urgentFlag = false; // Assuming not urgent
    pBlock->csParam.send.wdsPtr = wdsPtr;

    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_message("Error (LowTCPSendSync): PBControlAsync failed: %d", err);
        DisposePtr((Ptr)pBlock); return err;
    }
    while (pBlock->ioResult > 0) { giveTime(); } // Poll
    err = pBlock->ioResult;
    DisposePtr((Ptr)pBlock);
    return err;
}

// Aborts a TCP connection synchronously using TCPAbort.
// Uses polling with giveTime.
static OSErr LowTCPAbortSync(StreamPtr streamPtr, GiveTimePtr giveTime)
{
    OSErr err;
    TCPiopb *pBlock = NULL;

    if (giveTime == NULL) return paramErr;
    if (streamPtr == NULL) return invalidStreamPtr;

    pBlock = (TCPiopb *)NewPtrClear(sizeof(TCPiopb));
    if (pBlock == NULL) { log_message("Error (LowTCPAbortSync): Failed alloc PB."); return memFullErr; }

    pBlock->ioCompletion = nil; pBlock->ioCRefNum = gMacTCPRefNum;
    pBlock->csCode = TCPAbort; pBlock->ioResult = 1;
    pBlock->tcpStream = streamPtr;

    err = PBControlAsync((ParmBlkPtr)pBlock);
    if (err != noErr) {
        log_to_file_only("Info (LowTCPAbortSync): PBControlAsync failed: %d", err);
        DisposePtr((Ptr)pBlock);
        // Return noErr generally, as abort is best-effort cleanup
        return (err == connectionDoesntExist || err == invalidStreamPtr) ? noErr : err;
    }
    while (pBlock->ioResult > 0) { giveTime(); } // Poll
    err = pBlock->ioResult;
    if (err != noErr && err != connectionDoesntExist && err != invalidStreamPtr) {
         log_message("Warning (LowTCPAbortSync): Abort completed with error: %d", err);
    }
    DisposePtr((Ptr)pBlock);
    // Return noErr generally for abort, unless unexpected error occurred
    return (err == connectionDoesntExist || err == invalidStreamPtr || err == noErr) ? noErr : err;
}

// Releases a TCP stream synchronously. (Unchanged)
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr)
{
    OSErr err; TCPiopb pbRelease;
    if (streamPtr == NULL) return invalidStreamPtr;
    memset(&pbRelease, 0, sizeof(TCPiopb));
    pbRelease.ioCompletion = nil; pbRelease.ioCRefNum = macTCPRefNum;
    pbRelease.csCode = TCPRelease; pbRelease.tcpStream = streamPtr;
    err = PBControlSync((ParmBlkPtr)&pbRelease);
    if (err != noErr && err != invalidStreamPtr) { log_message("Warning (LowTCPReleaseSync): Failed: %d", err); }
    else if (err == invalidStreamPtr) { log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid.", (unsigned long)streamPtr); err = noErr; }
    return err;
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
    tempConnBuffer = NewPtrClear(kTCPSendInternalBufferSize);
    if (tempConnBuffer == NULL) {
        log_message("Error (InternalSyncTCPSend): Failed to allocate temporary connection buffer.");
        return memFullErr;
    }

    // 2. Create temporary stream
    err = LowTCPCreateSync(gMacTCPRefNum, &tempStream, tempConnBuffer, kTCPSendInternalBufferSize);
    if (err != noErr || tempStream == NULL) {
        log_message("Error (InternalSyncTCPSend): LowTCPCreateSync failed: %d", err);
        DisposePtr(tempConnBuffer); // Clean up buffer
        return err;
    }
    log_to_file_only("InternalSyncTCPSend: Temp stream 0x%lX created.", (unsigned long)tempStream);

    // 3. Connect using the temporary stream
    log_to_file_only("InternalSyncTCPSend: Connecting temp stream 0x%lX...", (unsigned long)tempStream);
    err = LowTCPOpenConnectionSync(tempStream, connectTimeoutTicks, targetIP, targetPort, giveTime);
    if (err == noErr) {
        log_to_file_only("InternalSyncTCPSend: Connected successfully.");

        // 4. Send data
        sendWDS[0].length = messageLen;
        sendWDS[0].ptr = (Ptr)messageBuffer; // Cast away const, WDS expects Ptr
        sendWDS[1].length = 0; sendWDS[1].ptr = NULL; // Terminator for WDS array

        log_to_file_only("InternalSyncTCPSend: Sending data...");
        err = LowTCPSendSync(tempStream, sendTimeoutTicks, true, (Ptr)sendWDS, giveTime);
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