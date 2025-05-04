#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "network.h"
#include <Devices.h>
#include <Errors.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#define kTCPRecvBufferSize 8192
#define kTCPListenInternalBufferSize 8192
#define kTCPListenRetryDelayTicks 60
#define AbortTrue 1
static StreamPtr gTCPListenStream = NULL;
static Ptr gTCPListenInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPiopb gTCPListenPB;
static TCPiopb gTCPRecvPB;
static TCPiopb gTCPClosePB;
static TCPiopb gTCPReleasePB;
static Boolean gTCPListenPending = false;
static Boolean gTCPRecvPending = false;
static Boolean gTCPClosePending = false;
static Boolean gTCPReleasePending = false;
static StreamPtr gStreamBeingReleased = NULL;
static Boolean gNeedToReListen = false;
ip_addr gCurrentConnectionIP = 0;
tcp_port gCurrentConnectionPort = 0;
Boolean gIsConnectionActive = false;
static OSErr StartAsyncTCPListen(short macTCPRefNum);
static OSErr StartAsyncTCPRecv(short macTCPRefNum);
static OSErr StartAsyncTCPClose(short macTCPRefNum, StreamPtr streamToClose, Boolean abortConnection);
static OSErr StartAsyncTCPRelease(short macTCPRefNum, StreamPtr streamToRelease);
static void ProcessTCPReceive(short macTCPRefNum);
static void HandleListenerCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleCloseCompletion(short macTCPRefNum, OSErr ioResult);
static void HandleReleaseCompletion(short macTCPRefNum, OSErr ioResult);
OSErr InitTCPListener(short macTCPRefNum) {
    OSErr err;
    TCPiopb pbCreate;
    log_message("Initializing TCP Listener (Single Stream Approach)...");
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
    log_message("Allocated %ld bytes for TCP listen internal buffer at 0x%lX.", (long)kTCPListenInternalBufferSize, (unsigned long)gTCPListenInternalBuffer);
    gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize);
    if (gTCPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate TCP receive buffer (%ld bytes).", (long)kTCPRecvBufferSize);
        DisposePtr(gTCPListenInternalBuffer); gTCPListenInternalBuffer = NULL;
        return memFullErr;
    }
    log_message("Allocated %ld bytes for TCP receive buffer at 0x%lX.", (long)kTCPRecvBufferSize, (unsigned long)gTCPRecvBuffer);
    memset(&pbCreate, 0, sizeof(TCPiopb));
    pbCreate.ioCompletion = nil;
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
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;
    gTCPReleasePending = false;
    gStreamBeingReleased = NULL;
    err = StartAsyncTCPListen(macTCPRefNum);
    if (err != noErr && err != 1) {
         log_message("Error (InitTCPListener): Failed to start initial async TCP listen. Error: %d", err);
         CleanupTCPListener(macTCPRefNum);
         return err;
    }
    log_message("Initial asynchronous TCP listen STARTING on port %d.", PORT_TCP);
    return noErr;
}
void CleanupTCPListener(short macTCPRefNum) {
    OSErr relErr;
    TCPiopb pbReleaseSync;
    log_message("Cleaning up TCP Listener (Single Stream Approach)...");
    StreamPtr listenStreamToRelease = gTCPListenStream;
    gTCPListenStream = NULL;
    if (gTCPListenPending) PBKillIO((ParmBlkPtr)&gTCPListenPB, false);
    if (gTCPRecvPending) PBKillIO((ParmBlkPtr)&gTCPRecvPB, false);
    if (gTCPClosePending) PBKillIO((ParmBlkPtr)&gTCPClosePB, false);
    if (gTCPReleasePending) PBKillIO((ParmBlkPtr)&gTCPReleasePB, false);
    gTCPListenPending = false;
    gTCPRecvPending = false;
    gTCPClosePending = false;
    gTCPReleasePending = false;
    gNeedToReListen = false;
    gIsConnectionActive = false;
    gStreamBeingReleased = NULL;
    gCurrentConnectionIP = 0;
    gCurrentConnectionPort = 0;
    if (listenStreamToRelease != NULL && macTCPRefNum != 0) {
        log_message("Attempting synchronous release of listener stream 0x%lX...", (unsigned long)listenStreamToRelease);
        memset(&pbReleaseSync, 0, sizeof(TCPiopb));
        pbReleaseSync.ioCompletion = nil;
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
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr ioResult;
    if (gTCPListenPending) {
        ioResult = gTCPListenPB.ioResult;
        if (ioResult <= 0) {
            gTCPListenPending = false;
            HandleListenerCompletion(macTCPRefNum, ioResult);
        }
    }
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
    if (gNeedToReListen && gTCPListenStream != NULL &&
        !gTCPListenPending && !gTCPRecvPending && !gTCPClosePending && !gTCPReleasePending)
    {
        log_to_file_only("PollTCPListener: Conditions met to attempt re-issuing listen.");
        gNeedToReListen = false;
        gIsConnectionActive = false;
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
                log_message("Error (HandleListenerCompletion): Failed to start first TCPRcv on listener stream. Error: %d. Releasing connection state.", err);
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
        Boolean isBadBuf = (ioResult == invalidBufPtr);
        Boolean isInsufficientRes = (ioResult == insufficientResources);
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
static void HandleReceiveCompletion(short macTCPRefNum, OSErr ioResult) {
    StreamPtr completedRecvStream = gTCPRecvPB.tcpStream;
    if (completedRecvStream != gTCPListenStream || gTCPListenStream == NULL) {
         log_message("Warning (HandleReceiveCompletion): Ignoring completion for unexpected/NULL stream 0x%lX (Current: 0x%lX).",
                     (unsigned long)completedRecvStream, (unsigned long)gTCPListenStream);
         return;
    }
    if (!gIsConnectionActive && ioResult == noErr) {
         log_to_file_only("Warning (HandleReceiveCompletion): Receive completed successfully but connection no longer marked active. Data processed, not re-issuing receive.");
         return;
    }
     if (!gIsConnectionActive && ioResult != noErr) {
         log_to_file_only("Warning (HandleReceiveCompletion): Receive completed with error %d, connection already inactive.", ioResult);
         return;
     }
    if (ioResult == noErr) {
        ProcessTCPReceive(macTCPRefNum);
        if (gIsConnectionActive && gTCPListenStream != NULL) {
            OSErr err = StartAsyncTCPRecv(macTCPRefNum);
             if (err != noErr && err != 1) {
                 log_message("Error (HandleReceiveCompletion): Failed to start next async TCP receive after successful receive. Error: %d. Closing connection state.", err);
                 gIsConnectionActive = false;
                 gCurrentConnectionIP = 0;
                 gCurrentConnectionPort = 0;
                 StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true);
             } else {
                 log_to_file_only("HandleReceiveCompletion: Successfully re-issued TCPRcv on stream 0x%lX.", (unsigned long)gTCPListenStream);
             }
        } else {
             log_to_file_only("HandleReceiveCompletion: Connection stream closed/inactive during processing. Not starting new receive.");
        }
    } else if (ioResult == connectionClosing) {
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         log_message("TCP Connection closing gracefully from %s (Stream: 0x%lX).", senderIPStr, (unsigned long)completedRecvStream);
         gIsConnectionActive = false;
         gCurrentConnectionIP = 0;
         gCurrentConnectionPort = 0;
         gNeedToReListen = true;
    } else {
         char senderIPStr[INET_ADDRSTRLEN];
         AddrToStr(gCurrentConnectionIP, senderIPStr);
         const char *errMsg = "Unknown Error";
         Boolean isConnDoesntExist = (ioResult == connectionDoesntExist);
         Boolean isInvalidStream = (ioResult == invalidStreamPtr);
         Boolean isConnTerminated = (ioResult == connectionTerminated);
         Boolean isBadBuf = (ioResult == invalidBufPtr);
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
             log_to_file_only("HandleReceiveCompletion: Attempting abortive close due to receive error.");
             StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true);
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
    if (ioResult == noErr) {
        log_to_file_only("HandleCloseCompletion: Async TCPClose completed successfully for stream 0x%lX.", (unsigned long)closedStream);
    } else {
        const char *errMsg = "Unknown Error";
        if (ioResult == invalidStreamPtr) errMsg = "invalidStreamPtr (-23010)";
        else if (ioResult == connectionDoesntExist) errMsg = "connectionDoesntExist (-23008)";
        else if (ioResult == connectionClosing) errMsg = "connectionClosing (-23009)";
        log_message("Error (HandleCloseCompletion): Async TCPClose for stream 0x%lX completed with error: %d (%s)", (unsigned long)closedStream, ioResult, errMsg);
        if (ioResult == invalidStreamPtr) {
             log_message("CRITICAL Error: Listener stream 0x%lX reported as invalid during close. Attempting release.", (unsigned long)gTCPListenStream);
             StartAsyncTCPRelease(macTCPRefNum, gTCPListenStream);
             gNeedToReListen = false;
             return;
        }
    }
    log_to_file_only("HandleCloseCompletion: Setting flag to re-listen.");
    gNeedToReListen = true;
}
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
         log_message("Error (StartAsyncTCPRecv): Cannot receive, connection not active on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
         return connectionDoesntExist;
    }
    if (gTCPListenPending || gTCPClosePending || (gTCPReleasePending && gStreamBeingReleased == gTCPListenStream)) {
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
        StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true);
        return err;
    }
    log_to_file_only("StartAsyncTCPRecv: Async TCPRcv initiated on listener stream 0x%lX.", (unsigned long)gTCPListenStream);
    return 1;
}
static OSErr StartAsyncTCPClose(short macTCPRefNum, StreamPtr streamToClose, Boolean abortConnection) {
    OSErr err;
    if (streamToClose == NULL) {
        log_message("Error (StartAsyncTCPClose): Cannot close NULL stream.");
        return invalidStreamPtr;
    }
    if (streamToClose != gTCPListenStream) {
        log_message("Error (StartAsyncTCPClose): Attempt to close non-listener stream 0x%lX.", (unsigned long)streamToClose);
        return invalidStreamPtr;
    }
    if (gTCPClosePending) {
        log_to_file_only("StartAsyncTCPClose: Close already pending for listener stream 0x%lX.", (unsigned long)streamToClose);
        return 1;
    }
     if (gTCPReleasePending && gStreamBeingReleased == streamToClose) {
         log_message("Error (StartAsyncTCPClose): Cannot close, stream 0x%lX release is pending.", (unsigned long)streamToClose);
         return inProgress;
    }
    memset(&gTCPClosePB, 0, sizeof(TCPiopb));
    gTCPClosePB.ioCompletion = nil;
    gTCPClosePB.ioCRefNum = macTCPRefNum;
    gTCPClosePB.csCode = TCPClose;
    gTCPClosePB.tcpStream = streamToClose;
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
        log_message("Error (StartAsyncTCPClose): PBControlAsync(TCPClose) failed immediately for stream 0x%lX. Error: %d", (unsigned long)streamToClose, err);
        gTCPClosePending = false;
        gNeedToReListen = true;
        return err;
    }
    log_to_file_only("StartAsyncTCPClose: Initiated async TCPClose for listener stream 0x%lX.", (unsigned long)streamToClose);
    return 1;
}
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
    if (gTCPListenPending || gTCPRecvPending || gTCPClosePending) {
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
        StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true);
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
                gIsConnectionActive = false;
                gCurrentConnectionIP = 0;
                gCurrentConnectionPort = 0;
                StartAsyncTCPClose(macTCPRefNum, gTCPListenStream, true);
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
            } else {
                 log_message("Received unhandled TCP message type '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection);
            }
        } else {
            log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength);
        }
    } else {
        log_to_file_only("ProcessTCPReceive: Received 0 bytes. Connection likely closing.");
    }
}
