//====================================
// FILE: ./classic_mac/tcp.c
// Implements TCP listening and receiving using a single stream approach.
// Adds explicit TCPClose(Abort) before re-listening.
//====================================

#include "tcp.h"
#include "logging.h"      // For log_message, log_to_file_only
#include "protocol.h"     // For parse_message, MSG_ defines
#include "peer_mac.h"     // For AddOrUpdatePeer, MarkPeerInactive
#include "dialog.h"       // For AppendToMessagesTE
#include "dialog_peerlist.h" // For UpdatePeerDisplayList
#include "network.h"      // For gMyUsername, gMyLocalIPStr

#include <Devices.h>      // For PBControlSync, PBControlAsync, PBKillIO
#include <Errors.h>       // For OSErr codes (noErr, etc.)
#include <Memory.h>       // For NewPtrClear, DisposePtr
#include <string.h>       // For memset, strcmp, sprintf
#include <stdio.h>        // For sprintf (used in logging/display)

// MacTCP specific includes (already covered by MacTCP.h via tcp.h)
// #include <MacTCP.h>

/* --- Configuration --- */
#define kTCPRecvBufferSize 8192
#define kTCPListenInternalBufferSize 8192 // Keep this large
#define kTCPListenRetryDelayTicks 60 // Delay before retrying listen *if* -23007 persists

/* --- Constants from MacSocket.c / MacTCP.h --- */
#define AbortTrue 1 // For ulpTimeoutAction in TCPClose

/* --- Module Globals --- */
static StreamPtr gTCPListenStream = NULL;
static Ptr gTCPListenInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;

static TCPiopb gTCPListenPB;
static TCPiopb gTCPRecvPB;
static TCPiopb gTCPClosePB; // *** NEW: PB for TCPClose ***
static TCPiopb gTCPReleasePB;

static Boolean gTCPListenPending = false;
static Boolean gTCPRecvPending = false;
static Boolean gTCPClosePending = false; // *** NEW: Flag for TCPClose ***
static Boolean gTCPReleasePending = false;
static StreamPtr gStreamBeingReleased = NULL;

static Boolean gNeedToReListen = false;

/* --- Public Globals (Defined as extern in header) --- */
ip_addr gCurrentConnectionIP = 0;
tcp_port gCurrentConnectionPort = 0;
Boolean gIsConnectionActive = false;

/* --- Static Function Prototypes --- */
static OSErr StartAsyncTCPListen(short macTCPRefNum);
static OSErr StartAsyncTCPRecv(short macTCPRefNum);
static OSErr StartAsyncTCPClose(short macTCPRefNum, StreamPtr streamToClose, Boolean abortConnection); // *** NEW ***
static OSErr StartAsyncTCPRelease(short macTCPRefNum, StreamPtr streamToRelease);
static void ProcessTCPReceive(short macTCPRefNum);
static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult); // *** NEW ***
static void HandleReleaseCompletion(short macTCPRefNum, OSErr ioResult);

//--------------------------------------------------------------------------------
// InitTCPListener
// (No changes needed from previous version)
//--------------------------------------------------------------------------------
OSErr InitTCPListener(short macTCPRefNum) {
    OSErr err;
    TCPiopb pbCreate; // Local PB for synchronous create

    log_message("Initializing TCP Listener (Single Stream Approach)...");
    if (macTCPRefNum == 0) return paramErr;
    if (gTCPListenStream != NULL) {
        log_message("Error (InitTCPListener): Already initialized?");
        return streamAlreadyOpen; // -23001
    }

    // 1. Allocate Buffers
    gTCPListenInternalBuffer = NewPtrClear(kTCPListenInternalBufferSize);
     if (gTCPListenInternalBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP listen internal buffer (%ld bytes).", (long)kTCPListenInternalBufferSize);
        return memFullErr;
    }
    log_message("Allocated %ld bytes for TCP listen internal buffer at 0x%lX.", (long)kTCPListenInternalBufferSize, (unsigned long)gTCPListenInternalBuffer);

    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);
    if (gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP receive buffer (%ld bytes).", (long)kTCPRecvBufferSize);
        DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated %ld bytes for TCP receive buffer at 0x%lX.", (long)kTCPRecvBufferSize, (unsigned long)gTCPRecvBuffer);


    // 2. Create Listener Stream (Synchronously)
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil; // Synchronous call
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = TCPCreate;
    pbCreate.tcpStream = 0L;
    pbCreate.csParam.create.rcvBuff = gTCPListenInternalBuffer;
    pbCreate.csParam.create.rcvBuffLen = kTCPListenInternalBufferSize;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.userDataPtr = nil;

    log_message("Calling PBControlSync (TCPCreate) for listener stream...");
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    if (err != noErr) {
        log_message("Error (InitTCPListener): TCPCreate failed. Error: %d", err);
        if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
        if (gTCPListenInternalBuffer) DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
        return err;
    }

    gTCPListenStream = pbCreate.tcpStream;
    if (gTCPListenStream == NULL) {
        log_message("Error (InitTCPListener): TCPCreate succeeded but returned NULL stream.");
        if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL;
        if (gTCPListenInternalBuffer) DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
        return ioErr;
    }
    log_message("TCP Listener Stream created successfully (StreamPtr: 0x%lX).", (unsigned long)gTCPListenStream);

    // 3. Initialize State and Start Listening Asynchronously
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false; // Initialize new flag
    gTCPReleasePending = false;
    gStreamBeingReleased = NULL;

    err = StartAsyncTCPListen(macTCPRefNum);
    if (err != noErr && err != 1) { // 1 means request is queued (inProgress)
         log_message("Error (InitTCPListener): Failed to start initial async TCP listen. Error: %d", err);
         CleanupTCPListener(macTCPRefNum); // Full cleanup
         return err;
    }

    log_message("Initial asynchronous TCP listen STARTING on port %d.", PORT_TCP);
    return noErr;
}

//--------------------------------------------------------------------------------
// CleanupTCPListener
// (Added kill for gTCPClosePB)
//--------------------------------------------------------------------------------
void CleanupTCPListener(short macTCPRefNum) {
    OSErr relErr;
    TCPiopb pbReleaseSync;

    log_message("Cleaning up TCP Listener (Single Stream Approach)...");

    StreamPtr listenStreamToRelease = gTCPListenStream;
    gTCPListenStream = NULL;

    // Cancel any pending asynchronous operations
    if (gTCPListenPending) PBKillIO((ParmBlkPtr)&gTCPListenPB, false);
    if (gTCPRecvPending) PBKillIO((ParmBlkPtr)&gTCPRecvPB, false);
    if (gTCPClosePending) PBKillIO((ParmBlkPtr)&gTCPClosePB, false); // *** NEW ***
    if (gTCPReleasePending) PBKillIO((ParmBlkPtr)&gTCPReleasePB, false);

    // Reset all state flags
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false; // *** NEW ***
    gTCPReleasePending = false;
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gStreamBeingReleased = NULL;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;

    // Release the single stream (use synchronous release during final cleanup)
    if (listenStreamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting synchronous release of listener stream 0x%lX...", (unsigned long)listenStreamToRelease);
        memset(&pbReleaseSync, 0, sizeof(TCPiopb));
        pbReleaseSync.ioCompletion = nil; // Synchronous
        pbReleaseSync.ioCRefNum = macTCPRefNum;
        pbReleaseSync.csCode = TCPRelease;
        pbReleaseSync.tcpStream = listenStreamToRelease;

        relErr = PBControlSync((ParmBlkPtr)&pbReleaseSync);
        if (relErr != noErr) {
             log_message("Warning (CleanupTCPListener): Synchronous TCPRelease failed. Error: %d", relErr);
        } else {
             log_to_file_only("CleanupTCPListener: Synchronous TCPRelease successful.");
        }
    } else if (listenStreamToRelease != NULL) {
        log_message("Warning (CleanupTCPListener): Cannot release stream, MacTCP refnum is 0.");
    }

    // Dispose Buffers
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

    log_message("TCP Listener cleanup finished.");
}

//--------------------------------------------------------------------------------
// PollTCPListener
// (Added check for Close completion and condition for re-listen)
//--------------------------------------------------------------------------------
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr ioResult;

    // --- Check Pending Operations ---

    // 1. Check Listen Completion
    if (gTCPListenPending) {
        ioResult = gTCPListenPB.ioResult;
        if (ioResult <= 0) {
            gTCPListenPending = false;
            HandleListenerCompletion(macTCPRefNum, ioResult);
        }
    }

    // 2. Check Receive Completion
    if (gTCPRecvPending) {
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

    // 3. Check Close Completion *** NEW ***
    if (gTCPClosePending) {
        if (gTCPClosePB.tcpStream == gTCPListenStream && gTCPListenStream != NULL) {
            ioResult = gTCPClosePB.ioResult;
            if (ioResult <= 0) {
                gTCPClosePending = false;
                HandleCloseCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPClosePending) {
             log_message("Warning (PollTCPListener): Close pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPClosePending = false;
        }
    }

    // 4. Check Release Completion
    if (gTCPReleasePending) {
        if (gTCPReleasePB.tcpStream == gStreamBeingReleased && gStreamBeingReleased != NULL) {
            ioResult = gTCPReleasePB.ioResult;
            if (ioResult <= 0) {
                gTCPReleasePending = false;
                HandleReleaseCompletion(macTCPRefNum, ioResult);
            }
        } else if (gTCPReleasePending) {
             log_message("Warning (PollTCPListener): Release pending but stream pointer mismatch or NULL. Clearing flag.");
             gTCPReleasePending = false;
             gStreamBeingReleased = NULL;
        }
    }

    // --- Re-issue Listen if Necessary and Possible ---
    // Can only re-listen if the stream exists, we need to, and NO operations are pending on it.
    if (gNeedToReListen && gTCPListenStream != NULL &&
        !gTCPListenPending && !gTCPRecvPending && !gTCPClosePending && !gTCPReleasePending) // *** Added !gTCPClosePending ***
    {
        log_to_file_only("PollTCPListener: Conditions met to attempt re-issuing listen.");
        gNeedToReListen = false;
        gIsConnectionActive = false; // Ensure connection state is reset

        OSErr err = StartAsyncTCPListen(macTCPRefNum);
        if (err != noErr && err != 1) {
            log_message("Error (PollTCPListener): Attempt to re-issue listen failed immediately. Error: %d. Will retry later.", err);
            gNeedToReListen = true; // Set flag again to retry next time
        } else {
            log_to_file_only("PollTCPListener: Re-issued Async TCPPassiveOpen successfully (or already pending).");
        }
    } else if (gNeedToReListen && gTCPListenStream == NULL) {
        log_message("Warning (PollTCPListener): Need to re-listen but stream is NULL. Cannot proceed.");
        gNeedToReListen = false;
    }
}

//--------------------------------------------------------------------------------
// HandleListenerCompletion
// (No changes needed from previous version)
//--------------------------------------------------------------------------------
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

        if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) { // Added gTCPClosePending check
             log_message("Warning: New connection indication while listener stream busy (Active: %d, RecvPending: %d, ClosePending: %d). Setting flag to re-listen later.",
                         gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
             gNeedToReListen = true;
        } else {
            gCurrentConnectionIP = acceptedIP;
            gCurrentConnectionPort = acceptedPort;
            gIsConnectionActive = true;

            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
            if (err != noErr && err != 1) {
                log_message("Error (HandleListenerCompletion): Failed to start first TCPRcv on listener stream. Error: %d. Releasing connection state.", err);
                gIsConnectionActive = false;
                gCurrentConnectionIP = 0;
                gCurrentConnectionPort = 0;
                // Attempt to close the connection immediately on error? Or just re-listen? Let's try re-listen first.
                gNeedToReListen = true;
            } else {
                 log_to_file_only("HandleListenerCompletion: First TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
            }
        }
    } else {
        const char *errMsg = "Unknown Error";
        Boolean isConnectionExists = (ioResult == connectionExists); // -23007
        Boolean isInvalidStream = (ioResult == invalidStreamPtr);   // -23010
        Boolean isBadBuf = (ioResult == invalidBufPtr);       // -23004
        Boolean isInsufficientRes = (ioResult == insufficientResources); // -23005

        if (isConnectionExists) errMsg = "connectionExists (-23007)";
        else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
        else if (isBadBuf) errMsg = "invalidBufPtr (-23004)";
        else if (isInsufficientRes) errMsg = "insufficientResources (-23005)";

        if (isConnectionExists) {
             log_to_file_only("HandleListenerCompletion: TCPPassiveOpen completed with transient error: %d (%s). Will retry after delay.", ioResult, errMsg);
             unsigned long dummyTimer;
             Delay(kTCPListenRetryDelayTicks, &dummyTimer);
             gNeedToReListen = true;
        } else {
            log_message("Error (HandleListenerCompletion): TCPPassiveOpen completed with error: %d (%s)", ioResult, errMsg);

            if (isInvalidStream) {
                 log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid. Attempting release.", (unsigned long)gTCPListenStream);
                 StartAsyncTCPRelease(macTCPRefNum, gTCPListenStream);
                 gNeedToReListen = false;
            } else {
                 gNeedToReListen = true;
            }
        }
    }
}


//--------------------------------------------------------------------------------
// HandleReceiveCompletion
// (No changes needed from previous version, ProcessTCPReceive handles state change)
//--------------------------------------------------------------------------------
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr completedRecvStream = gTCPRecvPB.tcpStream;

    if (completedRecvStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleReceiveCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)completedRecvStream, (unsigned long)gTCPListenStream);
         return;
    }
    // Check gIsConnectionActive *after* checking stream, as ProcessTCPReceive might change it
    if (!gIsConnectionActive && ioResult == noErr) {
         // We received data, but ProcessTCPReceive (called just before this check in theory)
         // might have already marked connection inactive (e.g. QUIT).
         log_to_file_only("Warning (HandleReceiveCompletion): Receive completed successfully but connection no longer marked active. Data processed, not re-issuing receive.");
         // gNeedToReListen should be true if QUIT was processed.
         return;
    }
     if (!gIsConnectionActive && ioResult != noErr) {
         // Receive completed with error, and connection already marked inactive. Just log.
         log_to_file_only("Warning (HandleReceiveCompletion): Receive completed with error %d, connection already inactive.", ioResult);
         // gNeedToReListen should be true from previous error handling or QUIT processing.
         return;
     }


    if (ioResult == noErr) {
        ProcessTCPReceive(macTCPRefNum);

        // Re-post receive ONLY if the connection is still active after processing
        if (gIsConnectionActive && gTCPListenStream != NULL) {
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
             if (err != noErr && err != 1) {
                 log_message("Error (HandleReceiveCompletion): Failed to start next async TCP receive after successful receive. Error: %d. Closing connection state.", err);
                 gIsConnectionActive = false;
                 gCurrentConnectionIP = 0;
                 gCurrentConnectionPort = 0;
                 // Don't just re-listen, try to close first
                 StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true); // Attempt abort
                 // gNeedToReListen will be set in HandleCloseCompletion
             } else {
                 log_to_file_only("HandleReceiveCompletion: Successfully re-issued TCPRcv on stream 0x%lX.", (unsigned long)gTCPListenStream);
             }
        } else {
             // Connection was closed during processing (QUIT) or stream became NULL
             log_to_file_only("HandleReceiveCompletion: Connection stream closed/inactive during processing. Not starting new receive.");
             // If QUIT was processed, StartAsyncTCPClose should have been called.
             // If stream became NULL, HandleReleaseCompletion will prevent re-listen.
        }
    } else if (ioResult == connectionClosing) { // -23009
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         log_message("TCP Connection closing gracefully from %s (Stream: 0x%lX).", senderIPStr, (unsigned long)completedRecvStream);
         gIsConnectionActive = false;
         gCurrentConnectionIP = 0;
         gCurrentConnectionPort = 0;
         // No need to explicitly close here, but set flag to re-listen
         gNeedToReListen = true;
    } else {
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         const char *errMsg = "Unknown Error";
         Boolean isConnDoesntExist = (ioResult == connectionDoesntExist); // -23008
         Boolean isInvalidStream = (ioResult == invalidStreamPtr);       // -23010
         Boolean isConnTerminated = (ioResult == connectionTerminated);   // -23012
         Boolean isBadBuf = (ioResult == invalidBufPtr);           // -23004

         if (isConnDoesntExist) errMsg = "connectionDoesntExist (-23008)";
         else if (isInvalidStream) errMsg = "invalidStreamPtr (-23010)";
         else if (isConnTerminated) errMsg = "connectionTerminated (-23012)";
         else if (isBadBuf) errMsg = "invalidBufPtr (-23004)";

        log_message("Error (HandleReceiveCompletion): Async TCPRcv completed with error: %d (%s) from %s (Stream: 0x%lX).", ioResult, errMsg, senderIPStr, (unsigned long)completedRecvStream);

        gIsConnectionActive = false;
        gCurrentConnectionIP = 0;
        gCurrentConnectionPort = 0;

        if (isInvalidStream) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during receive. Attempting release.", (unsigned long)gTCPListenStream);
             StartAsyncTCPRelease(macTCPRefNum, gTCPListenStream);
             gNeedToReListen = false;
        } else {
             // For other errors, attempt an abortive close before trying to re-listen
             log_to_file_only("HandleReceiveCompletion: Attempting abortive close due to receive error.");
             StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true); // Attempt abort
             // gNeedToReListen will be set in HandleCloseCompletion
        }
    }
}

//--------------------------------------------------------------------------------
// HandleCloseCompletion *** NEW ***
//--------------------------------------------------------------------------------
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr closedStream = gTCPClosePB.tcpStream;

    // Ensure completion is for the current listener stream
    if (closedStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleCloseCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)closedStream, (unsigned long)gTCPListenStream);
         return;
    }

    if (ioResult == noErr) {
        log_to_file_only("HandleCloseCompletion: Async TCPClose completed successfully for stream 0x%lX.", (unsigned long)closedStream);
    } else {
        const char *errMsg = "Unknown Error";
        // Add specific error checks if needed (e.g., invalidStreamPtr)
        if (ioResult == invalidStreamPtr) errMsg = "invalidStreamPtr (-23010)";
        else if (ioResult == connectionDoesntExist) errMsg = "connectionDoesntExist (-23008)";
        else if (ioResult == connectionClosing) errMsg = "connectionClosing (-23009)"; // Already closing

        log_message("Error (HandleCloseCompletion): Async TCPClose for stream 0x%lX completed with error: %d (%s)", (unsigned long)closedStream, ioResult, errMsg);

        if (ioResult == invalidStreamPtr) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during close. Attempting release.", (unsigned long)gTCPListenStream);
             StartAsyncTCPRelease(macTCPRefNum, gTCPListenStream);
             gNeedToReListen = false; // Cannot re-listen
             return; // Don't set gNeedToReListen below
        }
    }

    // Regardless of close success/failure (unless stream invalid), we are now ready to try listening again.
    log_to_file_only("HandleCloseCompletion: Setting flag to re-listen.");
    gNeedToReListen = true;
}


//--------------------------------------------------------------------------------
// HandleReleaseCompletion
// (No changes needed from previous version)
//--------------------------------------------------------------------------------
static void HandleReleaseCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr releasedStream = gTCPReleasePB.tcpStream;

    if (releasedStream != gStreamBeingReleased || releasedStream == NULL) {
        log_message("Warning (HandleReleaseCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Expected: 0x%lX).",
                    (unsigned long)releasedStream, (unsigned long)gStreamBeingReleased);
        gStreamBeingReleased = NULL;
        return;
    }

    gStreamBeingReleased = NULL;

    if (ioResult == noErr) {
        log_to_file_only("HandleReleaseCompletion: Async TCPRelease completed successfully for stream 0x%lX.", (unsigned long)releasedStream);
    } else {
         const char *errMsg = "Unknown Error";
         if (ioResult == invalidStreamPtr) errMsg = "invalidStreamPtr (-23010)";

        log_message("Error (HandleReleaseCompletion): Async TCPRelease for stream 0x%lX completed with error: %d (%s)", (unsigned long)releasedStream, ioResult, errMsg);
    }

    if (releasedStream == gTCPListenStream) {
         log_message("HandleReleaseCompletion: Listener stream 0x%lX was released. Setting global pointer to NULL.", (unsigned long)releasedStream);
         gTCPListenStream = NULL;
         gNeedToReListen = false;
         gIsConnectionActive = false;
         gCurrentConnectionIP = 0;
         gCurrentConnectionPort = 0;
    }
}


// --- Static Helper Functions ---

//--------------------------------------------------------------------------------
// StartAsyncTCPListen
// (Added check for gTCPClosePending)
//--------------------------------------------------------------------------------
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
    if (gIsConnectionActive || gTCPRecvPending || gTCPClosePending) { // *** Added gTCPClosePending ***
        log_message("Error (StartAsyncTCPListen): Cannot listen, stream 0x%lX is busy (Active: %d, RecvPending: %d, ClosePending: %d).",
                    (unsigned long)gTCPListenStream, gIsConnectionActive, gTCPRecvPending, gTCPClosePending);
        return inProgress;
    }
    if (gTCPReleasePending && gStreamBeingReleased == gTCPListenStream) {
         log_message("Error (StartAsyncTCPListen): Cannot listen, stream 0x%lX release is pending.", (unsigned long)gTCPListenStream);
         return inProgress;
    }

    memset(&gTCPListenPB, 0, sizeof(TCPiopb));
    gTCPListenPB.ioCompletion = nil;
    gTCPListenPB.ioCRefNum = macTCPRefNum;
    gTCPListenPB.csCode = TCPPassiveOpen;
    gTCPListenPB.tcpStream = gTCPListenStream;

    gTCPListenPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    gTCPListenPB.csParam.open.ulpTimeoutValue = kTCPDefaultTimeout;
    gTCPListenPB.csParam.open.ulpTimeoutAction = AbortTrue; // Use constant
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


//--------------------------------------------------------------------------------
// StartAsyncTCPRecv
// (Added check for gTCPClosePending)
//--------------------------------------------------------------------------------
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
         log_message("Error (StartAsyncTCPRecv): Cannot receive, connection not active on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
         return connectionDoesntExist;
    }
    if (gTCPListenPending || gTCPClosePending || (gTCPReleasePending && gStreamBeingReleased == gTCPListenStream)) { // *** Added gTCPClosePending ***
         log_message("Error (StartAsyncTCPRecv): Cannot receive, stream 0x%lX has other pending operations (Listen: %d, Close: %d, Release: %d).",
                     (unsigned long)gTCPListenStream, gTCPListenPending, gTCPClosePending, gTCPReleasePending);
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
        gIsConnectionActive = false;
        gCurrentConnectionIP = 0;
        gCurrentConnectionPort = 0;
        // Attempt close before trying to re-listen on failure
        StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true); // Attempt abort
        // gNeedToReListen will be set in HandleCloseCompletion
        return err;
    }

    log_to_file_only("StartAsyncTCPRecv: Async TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1;
}

//--------------------------------------------------------------------------------
// StartAsyncTCPClose *** NEW ***
//--------------------------------------------------------------------------------
static OSErr StartAsyncTCPClose(short macTCPRefNum, StreamPtr streamToClose, Boolean abortConnection) {
    OSErr err;

    // Preconditions
    if (streamToClose == NULL) {
        log_message("Error (StartAsyncTCPClose): Cannot close NULL stream.");
        return invalidStreamPtr;
    }
    // Should only be closing the listener stream in this model
    if (streamToClose != gTCPListenStream) {
        log_message("Error (StartAsyncTCPClose): Attempt to close non-listener stream 0x%lX.", (unsigned long)streamToClose);
        return invalidStreamPtr;
    }
    if (gTCPClosePending) {
        log_to_file_only("StartAsyncTCPClose: Close already pending for listener stream 0x%lX.", (unsigned long)streamToClose);
        return 1; // Already in progress
    }
     if (gTCPReleasePending && gStreamBeingReleased == streamToClose) {
         log_message("Error (StartAsyncTCPClose): Cannot close, stream 0x%lX release is pending.", (unsigned long)streamToClose);
         return inProgress;
    }
    // It's generally okay to close while listen or receive are pending (they should fail)

    // Setup Parameter Block for TCPClose
    memset(&gTCPClosePB, 0, sizeof(TCPiopb));
    gTCPClosePB.ioCompletion = nil; // Use polling
    gTCPClosePB.ioCRefNum = macTCPRefNum;
    gTCPClosePB.csCode = TCPClose;
    gTCPClosePB.tcpStream = streamToClose;

    // --- Parameters for Close ---
    // Use validity flags to specify which timeout fields are valid.
    gTCPClosePB.csParam.close.validityFlags = timeoutValue | timeoutAction;
    gTCPClosePB.csParam.close.ulpTimeoutValue = kTCPDefaultTimeout; // Timeout for graceful close (0 likely okay)
    if (abortConnection) {
        gTCPClosePB.csParam.close.ulpTimeoutAction = AbortTrue; // 1 = Abort connection immediately
        log_to_file_only("StartAsyncTCPClose: Using Abort action.");
    } else {
        gTCPClosePB.csParam.close.ulpTimeoutAction = 0; // 0 = Graceful close (default)
        log_to_file_only("StartAsyncTCPClose: Using Graceful close action.");
    }
    gTCPClosePB.csParam.close.userDataPtr = nil;

    // Initiate Asynchronous Call
    gTCPClosePending = true; // Set flag *before* calling
    err = PBControlAsync((ParmBlkPtr)&gTCPClosePB);

    if (err != noErr) {
        // Call failed immediately
        log_message("Error (StartAsyncTCPClose): PBControlAsync(TCPClose) failed immediately for stream 0x%lX. Error: %d", (unsigned long)streamToClose, err);
        gTCPClosePending = false; // Reset flag
        // If close fails, the stream state is uncertain. Set flag to re-listen anyway? Or release?
        // Let's try setting re-listen for now, HandleCloseCompletion won't run.
        gNeedToReListen = true;
        return err;
    }

    // If err == noErr, request was queued. ioResult will be > 0.
    log_to_file_only("StartAsyncTCPClose: Initiated async TCPClose for listener stream 0x%lX.", (unsigned long)streamToClose);
    return 1; // Return 1 to indicate request is pending/in progress
}


//--------------------------------------------------------------------------------
// StartAsyncTCPRelease
// (Added check for gTCPClosePending)
//--------------------------------------------------------------------------------
static OSErr StartAsyncTCPRelease(short macTCPRefNum, StreamPtr streamToRelease) {
    OSErr err;

    if (streamToRelease == NULL) {
        log_message("Error (StartAsyncTCPRelease): Cannot release NULL stream.");
        return invalidStreamPtr;
    }
    if (streamToRelease != gTCPListenStream) {
        log_message("Error (StartAsyncTCPRelease): Attempt to release non-listener stream 0x%lX.", (unsigned long)streamToRelease);
        return invalidStreamPtr;
    }
    if (gTCPReleasePending && gStreamBeingReleased == streamToRelease) {
         log_to_file_only("StartAsyncTCPRelease: Release already pending for listener stream 0x%lX.", (unsigned long)streamToRelease);
         return 1;
    }
    if (gTCPReleasePending) {
         log_message("Warning (StartAsyncTCPRelease): Overwriting pending release for stream 0x%lX with new release request for listener stream 0x%lX.",
                     (unsigned long)gStreamBeingReleased, (unsigned long)streamToRelease);
    }
    // Check for other pending operations on this stream
    if (gTCPListenPending || gTCPRecvPending || gTCPClosePending) { // *** Added gTCPClosePending ***
         log_message("Warning (StartAsyncTCPRelease): Releasing stream 0x%lX while other operations pending (Listen: %d, Recv: %d, Close: %d).",
                     (unsigned long)streamToRelease, gTCPListenPending, gTCPRecvPending, gTCPClosePending);
    }

    memset(&gTCPReleasePB, 0, sizeof(TCPiopb));
    gTCPReleasePB.ioCompletion = nil;
    gTCPReleasePB.ioCRefNum = macTCPRefNum;
    gTCPReleasePB.csCode = TCPRelease;
    gTCPReleasePB.tcpStream = streamToRelease;

    gTCPReleasePending = true;
    gStreamBeingReleased = streamToRelease;
    err = PBControlAsync((ParmBlkPtr)&gTCPReleasePB);

    if (err != noErr) {
        log_message("Error (StartAsyncTCPRelease): PBControlAsync(TCPRelease) failed immediately for listener stream 0x%lX. Error: %d", (unsigned long)streamToRelease, err);
        gTCPReleasePending = false;
        gStreamBeingReleased = NULL;
        return err;
    }

    log_to_file_only("StartAsyncTCPRelease: Initiated async release for listener stream 0x%lX.", (unsigned long)streamToRelease);
    return 1;
}

//--------------------------------------------------------------------------------
// ProcessTCPReceive
// (Modified QUIT handling to call StartAsyncTCPClose)
//--------------------------------------------------------------------------------
static void ProcessTCPReceive(short macTCPRefNum) {
    char senderIPStrFromConnection[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    unsigned short dataLength;

    if (!gIsConnectionActive || gTCPListenStream == NULL) {
        log_message("Warning (ProcessTCPReceive): Called when connection not active or listener stream is NULL.");
        return;
    }
    if (gTCPRecvPB.tcpStream != gTCPListenStream) {
        log_message("CRITICAL Warning (ProcessTCPReceive): Received data for unexpected stream 0x%lX, expected 0x%lX. Ignoring.",
                    (unsigned long)gTCPRecvPB.tcpStream, (unsigned long)gTCPListenStream);
        gIsConnectionActive = false;
        gCurrentConnectionIP = 0;
        gCurrentConnectionPort = 0;
        // Attempt close before trying to re-listen on logic error
        StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true); // Attempt abort
        // gNeedToReListen will be set in HandleCloseCompletion
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
            log_to_file_only("ProcessTCPReceive: Parsed '%s' from %s@%s (Payload IP: %s). Content: '%.30s...'",
                       msgType, senderUsername, senderIPStrFromConnection, senderIPStrFromPayload, content);

            int addResult = AddOrUpdatePeer(senderIPStrFromConnection, senderUsername);
             if (addResult > 0) {
                 log_message("Peer connected/updated via TCP: %s@%s", senderUsername, senderIPStrFromConnection);
                 UpdatePeerDisplayList(true);
             } else if (addResult < 0) {
                 log_message("Peer list full, could not add/update %s@%s from TCP connection", senderUsername, senderIPStrFromConnection);
             }

            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_message("Peer %s@%s has sent QUIT notification via TCP.", senderUsername, senderIPStrFromConnection);
                if (MarkPeerInactive(senderIPStrFromConnection)) {
                     UpdatePeerDisplayList(true);
                }
                log_message("Connection to %s finishing due to QUIT.", senderIPStrFromConnection);

                // Close the connection state
                gIsConnectionActive = false;
                gCurrentConnectionIP = 0;
                gCurrentConnectionPort = 0;
                // *** NEW: Initiate Abortive Close ***
                StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true); // true = Abort
                // *** DO NOT set gNeedToReListen here. Set it in HandleCloseCompletion. ***

                // Cancel any pending receive (safety check)
                if(gTCPRecvPending && gTCPRecvPB.tcpStream == gTCPListenStream) {
                    log_to_file_only("ProcessTCPReceive (QUIT): Cancelling pending receive.");
                    PBKillIO((ParmBlkPtr)&gTCPRecvPB, false);
                    gTCPRecvPending = false;
                }

            } else if (strcmp(msgType, MSG_TEXT) == 0) {
                char displayMsg[BUFFER_SIZE + 100];
                 sprintf(displayMsg, "%s: %s", senderUsername, content);
                 AppendToMessagesTE(displayMsg);
                 AppendToMessagesTE("\r");
                 log_message("Message from %s@%s: %s", senderUsername, senderIPStrFromConnection, content);
                 // Connection remains active, expect HandleReceiveCompletion to re-post receive
            } else {
                 log_message("Received unhandled TCP message type '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection);
                 // Connection remains active, expect HandleReceiveCompletion to re-post receive
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
            // Connection remains active, expect HandleReceiveCompletion to re-post receive
        }
    } else {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes. Connection likely closing.");
        // Let HandleReceiveCompletion deal with connectionClosing error
    }
}