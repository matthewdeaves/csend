//====================================
// FILE: ./classic_mac/discovery.c
//====================================

#include "discovery.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "network.h"
#include "dialog.h" // For gMyUsername, gMyLocalIPStr
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h>
#include <MixedMode.h> // For NewUDPIOCompletionProc

// --- Global State for Asynchronous UDP ---

StreamPtr gUDPStream = NULL;            // UDP Stream identifier
Ptr gUDPRecvBuffer = NULL;          // Buffer allocated for receiving data
UDPiopb gUDPReadPB;            // Persistent PB for udpRead calls
// UDPiopb gUDPWritePB;        // No longer needed for sync writes
UDPiopb gUDPBfrReturnPB;       // Persistent PB for udpBfrReturn calls

Boolean gUDPReadPending = false;       // Is an asynchronous udpRead currently active?
// Boolean gUDPWritePending = false; // No longer needed
Boolean gUDPBfrReturnPending = false;  // Is an asynchronous udpBfrReturn currently active?

// Flags/Data set by completion routines for the main loop to process (NOT USED)
// volatile Boolean gUDPDataAvailable = false;
// volatile OSErr gUDPReadResult = 0;
// volatile Boolean gUDPSendComplete = false;
// volatile OSErr gUDPSendResult = 0;
// volatile Boolean gUDPBfrReturnComplete = false;
// volatile OSErr gUDPBfrReturnResult = 0;

// --- Original Timer ---
unsigned long gLastBroadcastTimeTicks = 0; // For periodic broadcast timing

// --- Static Buffers for Sync Writes ---
static char gBroadcastBuffer[BUFFER_SIZE];
static struct wdsEntry gBroadcastWDS[2];
static char gResponseBuffer[BUFFER_SIZE];
static struct wdsEntry gResponseWDS[2];


// --- Initialization and Cleanup ---

OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    // (Content mostly unchanged, just log message)
    OSErr err;
    UDPiopb pbCreate;
    const unsigned short specificPort = PORT_UDP;

    log_message("Initializing UDP Discovery Endpoint (Async Read Poll / Sync Write)..."); // Updated log

    if (macTCPRefNum == 0) { /* log error */ return paramErr; }

    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) { /* log error */ return memFullErr; }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    memset(&pbCreate, 0, sizeof(UDPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = udpCreate;
    pbCreate.udpStream = 0L;
    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer;
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.localPort = specificPort;

    log_message("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    StreamPtr returnedStreamPtr = *((StreamPtr *) &pbCreate.csParam);
    log_message("DEBUG: After PBControlSync(udpCreate): err=%d, StreamPtr at offset 28 = 0x%lX, pbCreate.csParam.create.localPort=%u",
        err, (unsigned long)returnedStreamPtr, pbCreate.csParam.create.localPort);

    if (err != noErr) { /* log error, dispose buffer */ return err; }
    gUDPStream = returnedStreamPtr;
    if (gUDPStream == NULL) { /* log error, dispose buffer */ return ioErr; }

    unsigned short assignedPort = pbCreate.csParam.create.localPort;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);

    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;

    err = StartAsyncUDPRead(); // Will use nil completion
    if (err != noErr && err != 1) {
        log_message("Error (InitUDP): Failed to start initial async UDP read (polling). Error: %d", err);
    } else {
        log_message("Initial asynchronous UDP read (polling) STARTING.");
    }

    return noErr;
}

void CleanupUDPDiscoveryEndpoint(short macTCPRefNum) {
    // (Content unchanged)
    log_message("Cleaning up UDP Discovery Endpoint (Async)...");
    if (gUDPStream != NULL) {
        log_message("UDP Stream 0x%lX was open.", (unsigned long)gUDPStream);
        gUDPStream = NULL;
    } else {
        log_message("UDP Stream was not open or already cleaned up.");
    }
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    log_message("UDP Discovery Endpoint cleanup finished.");
}


// --- Initiating Operations ---

// --- StartAsyncUDPRead Definition (NO COMPLETION) ---
OSErr StartAsyncUDPRead(void) {
    OSErr err;

    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPReadPending) return 1;
    if (gUDPBfrReturnPending) return 1; // Still need to wait for buffer return
    if (gUDPRecvBuffer == NULL) return invalidBufPtr;

    memset(&gUDPReadPB, 0, sizeof(UDPiopb));
    gUDPReadPB.ioCompletion = nil; // <<< No completion routine
    gUDPReadPB.ioCRefNum = gMacTCPRefNum;
    gUDPReadPB.csCode = udpRead;
    gUDPReadPB.udpStream = gUDPStream;
    gUDPReadPB.csParam.receive.rcvBuff = gUDPRecvBuffer;
    gUDPReadPB.csParam.receive.rcvBuffLen = kMinUDPBufSize;
    gUDPReadPB.csParam.receive.timeOut = 0;

    gUDPReadPending = true; // Set pending flag

    err = PBControlAsync((ParmBlkPtr)&gUDPReadPB);

    if (err != noErr) {
        log_message("Error (StartAsyncUDPRead): PBControlAsync(udpRead - polling) failed immediately. Error: %d", err);
        gUDPReadPending = false; // Reset flag if failed
        return err;
    }
    // log_message("DEBUG: Asynchronous udpRead (polling) initiated.");
    return err;
}
// --- END ---


// --- SendUDPSyncInternal Definition ---
OSErr SendUDPSyncInternal(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                           const char *msgType, const char *content,
                           ip_addr destIP, udp_port destPort,
                           char *staticBuffer, struct wdsEntry *staticWDS)
{
    // (Content unchanged - uses PBControlSync)
    OSErr err;
    int formatted_len;
    UDPiopb pbSync;

    if (gUDPStream == NULL) return invalidStreamPtr;
    if (macTCPRefNum == 0) return paramErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    formatted_len = format_message(staticBuffer, BUFFER_SIZE, msgType, myUsername, myLocalIPStr, content);
    if (formatted_len <= 0) { /* log error */ return paramErr; }

    staticWDS[0].length = formatted_len - 1;
    staticWDS[0].ptr = staticBuffer;
    staticWDS[1].length = 0;
    staticWDS[1].ptr = nil;

    memset(&pbSync, 0, sizeof(UDPiopb));
    pbSync.ioCompletion = nil;
    pbSync.ioCRefNum = macTCPRefNum;
    pbSync.csCode = udpWrite;
    pbSync.udpStream = gUDPStream;
    pbSync.csParam.send.remoteHost = destIP;
    pbSync.csParam.send.remotePort = destPort;
    pbSync.csParam.send.wdsPtr = (Ptr)staticWDS;
    pbSync.csParam.send.checkSum = true;

    err = PBControlSync((ParmBlkPtr)&pbSync);

    if (err != noErr) {
        log_message("Error (SendUDPSync): PBControlSync(udpWrite) for '%s' failed. Error: %d", msgType, err);
        return err;
    }
    return noErr;
}
// --- END ---


// Public function to send broadcast SYNCHRONOUSLY
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    // (Content unchanged)
    return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                                MSG_DISCOVERY, "",
                                BROADCAST_IP, PORT_UDP,
                                gBroadcastBuffer, gBroadcastWDS);
}

// Public function to send response SYNCHRONOUSLY
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort) {
    // (Content unchanged)
     return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                                MSG_DISCOVERY_RESPONSE, "",
                                destIP, destPort,
                                gResponseBuffer, gResponseWDS);
}

// --- ReturnUDPBufferAsync Definition (NO COMPLETION) ---
OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize) {
    OSErr err;

    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPBfrReturnPending) return 1;
    if (dataPtr == NULL) return invalidBufPtr;

    memset(&gUDPBfrReturnPB, 0, sizeof(UDPiopb));
    gUDPBfrReturnPB.ioCompletion = nil; // <<< No completion routine
    gUDPBfrReturnPB.ioCRefNum = gMacTCPRefNum;
    gUDPBfrReturnPB.csCode = udpBfrReturn;
    gUDPBfrReturnPB.udpStream = gUDPStream;
    gUDPBfrReturnPB.csParam.receive.rcvBuff = dataPtr;
    gUDPBfrReturnPB.csParam.receive.rcvBuffLen = bufferSize;

    gUDPBfrReturnPending = true; // Set pending flag

    err = PBControlAsync((ParmBlkPtr)&gUDPBfrReturnPB);

    if (err != noErr) {
        log_message("CRITICAL Error (ReturnUDPBufferAsync): PBControlAsync(udpBfrReturn - polling) failed immediately. Error: %d.", err);
        gUDPBfrReturnPending = false; // Reset flag if failed
        return err;
    }
    // log_message("DEBUG: Asynchronous udpBfrReturn (polling) initiated for buffer 0x%lX.", (unsigned long)dataPtr);
    return noErr;
}
// --- END ---


// --- Processing Results (Called from Main Loop after polling) ---

// --- ProcessUDPReceive Definition (calls SYNC response, initiates ASYNC buffer return with NO completion) ---
void ProcessUDPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    // (Modified to call ReturnUDPBufferAsync with nil completion)
    char senderIPStrFromHeader[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    OSErr returnErr;
    ip_addr local_gUDPSenderIP;
    udp_port local_gUDPSenderPort;
    unsigned short local_gUDPDataLength;
    Ptr local_gUDPDataPtr;

    // Result code is checked in HandleIdleTasks before calling this
    // gUDPDataAvailable flag is not used

    // Read results from the global PB
    local_gUDPSenderIP = gUDPReadPB.csParam.receive.remoteHost;
    local_gUDPSenderPort = gUDPReadPB.csParam.receive.remotePort;
    local_gUDPDataLength = gUDPReadPB.csParam.receive.rcvBuffLen;
    local_gUDPDataPtr = gUDPReadPB.csParam.receive.rcvBuff;

    if (local_gUDPDataLength > 0) {
        // log_message("DEBUG: Processing received UDP data: %u bytes from %lu:%u, DataPtr=0x%lX", ...);

        if (local_gUDPSenderIP != myLocalIP) {
             OSErr addrErr = AddrToStr(local_gUDPSenderIP, senderIPStrFromHeader);
             if (addrErr != noErr) {
                 sprintf(senderIPStrFromHeader, "%lu.%lu.%lu.%lu", (local_gUDPSenderIP >> 24) & 0xFF, (local_gUDPSenderIP >> 16) & 0xFF, (local_gUDPSenderIP >> 8) & 0xFF, local_gUDPSenderIP & 0xFF);
             }

             if (parse_message(local_gUDPDataPtr, local_gUDPDataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
                if (strcmp(msgType, MSG_DISCOVERY_RESPONSE) == 0) {
                    log_message("Received DISCOVERY_RESPONSE from %s@%s", senderUsername, senderIPStrFromHeader);
                    if (AddOrUpdatePeer(senderIPStrFromHeader, senderUsername) < 0) { /* log */ }
                } else if (strcmp(msgType, MSG_DISCOVERY) == 0) {
                     log_message("Received DISCOVERY from %s@%s", senderUsername, senderIPStrFromHeader);
                     // Send response SYNCHRONOUSLY
                     OSErr sendErr = SendDiscoveryResponseSync(macTCPRefNum, gMyUsername, gMyLocalIPStr, local_gUDPSenderIP, local_gUDPSenderPort);
                     if (sendErr == noErr) { /* log */ }
                     else { log_message("Error sending sync discovery response: %d", sendErr); }
                     if (AddOrUpdatePeer(senderIPStrFromHeader, senderUsername) < 0) { /* log */ }
                }
             } else {
                 log_message("Discarding invalid/unknown UDP msg from %s (%u bytes).", senderIPStrFromHeader, local_gUDPDataLength);
             }
        } else {
            // log_message("DEBUG: Ignored UDP packet from self.");
        }

        // Return buffer using pointer from completed read PB (ASYNC, NO COMPLETION)
        returnErr = ReturnUDPBufferAsync(local_gUDPDataPtr, kMinUDPBufSize);
        if (returnErr != noErr && returnErr != 1) {
            log_message("CRITICAL Error (ProcessUDPReceive): Failed to initiate async udpBfrReturn (polling) using pointer 0x%lX. Error: %d.", (unsigned long)local_gUDPDataPtr, returnErr);
            // If buffer return fails to start, we cannot safely start a new read!
        } else {
            // log_message("DEBUG: Async udpBfrReturn (polling) initiated for pointer 0x%lX", (unsigned long)local_gUDPDataPtr);
            // gUDPBfrReturnPending is set inside ReturnUDPBufferAsync
        }

    } else {
         log_message("DEBUG: Async udpRead (polling) returned 0 bytes.");
         // No buffer to return, try starting next read immediately
         if (!gUDPReadPending && !gUDPBfrReturnPending) { // Check BfrReturn just in case
             StartAsyncUDPRead();
         }
    }
}
// --- END ---


// --- Periodic Check (Called from Main Loop) ---

void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    // (Calls SYNC broadcast)
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;

    if (gUDPStream == NULL || macTCPRefNum == 0) return;

    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks;
    }

    // Send broadcast synchronously if it's the first time or the interval has passed.
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        // log_message("DEBUG: Attempting SYNCHRONOUS broadcast send...");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);

        if (sendErr == noErr) {
            // log_message("DEBUG: Sync broadcast succeeded.");
            gLastBroadcastTimeTicks = currentTimeTicks;
        } else {
            log_message("Sync broadcast initiation failed (Error: %d)", sendErr);
        }
    }
}


// --- Completion Routines (COMMENTED OUT / NOT USED) ---
/*
pascal void UDPReadComplete(UDPiopb *pb) { ... }
pascal void UDPWriteComplete(UDPiopb *pb) { ... }
pascal void UDPBfrReturnComplete(UDPiopb *pb) { ... }
*/
// --- END ---