// FILE: ./classic_mac/discovery.c
#include "discovery.h" // Includes MyUDPiopb definition (still needed for size comparison log, though not used for calls)
#include "logging.h"   // For log_message
#include "protocol.h"  // For format_message, parse_message, MSG_DISCOVERY, MSG_DISCOVERY_RESPONSE, MSG_MAGIC_NUMBER
#include "peer_mac.h"  // For AddOrUpdatePeer
#include "network.h"   // For AddrToStr

#include <Devices.h>   // For PBControlSync, TickCount, CntrlParam, ParmBlkPtr
#include <Errors.h>    // Include for error codes (memFullErr, invalidStreamPtr, reqAborted, commandTimeout)
#include <string.h>    // For memset, strlen, strcmp, strncpy
#include <stdio.h>     // For sprintf (used in fallback AddrToStr), fprintf, fputc, fflush
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For NewPtrClear, DisposePtr

#include <stddef.h> // For offsetof debuging

// --- Global Variable Definitions ---
StreamPtr gUDPStream = NULL; // Pointer to our UDP stream for discovery (initialize to NULL)
Ptr     gUDPRecvBuffer = NULL; // Pointer to the buffer allocated for UDPCreate (initialize to NULL)
unsigned long gLastBroadcastTimeTicks = 0; // Time in Ticks


// Keep PrintUDPiopbLayout showing both sizes for reference
void PrintUDPiopbLayout() {
    // *** UPDATED Log Message ***
    log_message("--- Layout Log (Using standard UDPiopb for offsets in this test) ---");
    log_message("sizeof(UDPiopb) (from <MacTCP.h>) = %lu", (unsigned long)sizeof(UDPiopb));

    // *** UPDATED: Log offsets based on standard UDPiopb ***
    log_message("Offset ioCompletion (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioCompletion));
    log_message("Offset ioResult (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioResult));
    log_message("Offset ioCRefNum (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioCRefNum));
    log_message("Offset csCode (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, csCode));
    log_message("Offset udpStream (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, udpStream));
    log_message("Offset csParam (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, csParam));

    log_message("  --- csParam.create (UDPiopb) ---");
    log_message("  sizeof(UDPCreatePB) = %lu", (unsigned long)sizeof(struct UDPCreatePB));
    log_message("  Offset create.rcvBuff = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.rcvBuff));
    log_message("  Offset create.rcvBuffLen = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.rcvBuffLen));
    log_message("  Offset create.notifyProc = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.notifyProc));
    log_message("  Offset create.localPort = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.localPort));
    log_message("  Offset create.userDataPtr = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.userDataPtr));

    log_message("  --- csParam.receive (UDPiopb) ---");
    log_message("  sizeof(UDPReceivePB) = %lu", (unsigned long)sizeof(struct UDPReceivePB));
    log_message("  Offset receive.timeout = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.timeOut));
    log_message("  Offset receive.remoteHost = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.remoteHost));
    log_message("  Offset receive.remotePort = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.remotePort));
    log_message("  Offset receive.rcvBuff = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.rcvBuff));
    log_message("  Offset receive.rcvBuffLen = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.rcvBuffLen));
    log_message("  Offset receive.secondTimeStamp = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.secondTimeStamp));
    log_message("  Offset receive.userDataPtr = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.userDataPtr));

    log_message("--- End Layout ---");
}


/**
 * @brief Initializes the UDP endpoint for discovery - USES UDPiopb (58 bytes).
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    // *** USE STANDARD UDPiopb (58 bytes) ***
    UDPiopb pbCreate;

    const unsigned short specificPort = PORT_UDP;

    log_message("Initializing UDP Discovery Endpoint...");
    // *** UPDATED Log Message ***
    log_message("Using standard UDPiopb (expected 58 bytes) for ALL calls."); // Update message

    if (macTCPRefNum == 0) {
         log_message("Error (InitUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
    }

    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_message("Error (InitUDP): Failed to allocate UDP receive buffer (memFullErr).");
        return memFullErr;
    }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    // --- Prepare UDPiopb (58 bytes) for UDPCreate ---
    // *** Use sizeof(UDPiopb) ***
    memset(&pbCreate, 0, sizeof(UDPiopb)); // Use 58-byte size
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = udpCreate;
    pbCreate.udpStream = 0L;     // Output field

    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer;
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.localPort = specificPort;

    log_message("Calling PBControlSync (udpCreate) using UDPiopb (expected 58 bytes) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);

    log_message("DEBUG: After PBControlSync(udpCreate) using UDPiopb: err=%d, pbCreate.udpStream=0x%lX, pbCreate.csParam.create.localPort=%u",
        err, (unsigned long)pbCreate.udpStream, pbCreate.csParam.create.localPort);

    if (err != noErr) {
        log_message("Error (InitUDP): PBControlSync(udpCreate) failed. Error: %d", err);
        if (gUDPRecvBuffer != NULL) { DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL; }
        gUDPStream = NULL;
        return err;
    }

    // --- Store the StreamPtr directly from the struct field ---
    gUDPStream = pbCreate.udpStream;
    log_message("DEBUG: Stored StreamPtr directly from pbCreate.udpStream: 0x%lX", (unsigned long)gUDPStream);


    // Check if the stream pointer is NULL
    if (gUDPStream == NULL) {
         log_message("CRITICAL WARNING (InitUDP): StreamPtr is NULL. UDP will likely fail.");
         if (gUDPRecvBuffer != NULL) { DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL; }
         gUDPStream = NULL; // Ensure it's NULL if invalid
         return ioErr; // Indicate an internal inconsistency
    }
    // Check if it EQUALS the buffer pointer, just log a warning, don't abort yet.
    if (gUDPStream == (StreamPtr)gUDPRecvBuffer) {
        log_message("WARNING (InitUDP): StreamPtr (0x%lX) is the same as gUDPRecvBuffer (0x%lX). This might be incorrect.",
                    (unsigned long)gUDPStream, (unsigned long)gUDPRecvBuffer);
    }


    unsigned short assignedPort = pbCreate.csParam.create.localPort;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);
    gLastBroadcastTimeTicks = 0;
    return noErr;
}

/**
 * @brief Sends a UDP discovery broadcast - USES UDPiopb (58 bytes).
 */
OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2];
    // *** USE STANDARD UDPiopb (58 bytes) ***
    UDPiopb pb;
    int formatted_len;

    if (gUDPStream == NULL) {
        log_message("Error (SendUDP): Cannot send broadcast, UDP stream pointer is NULL.");
        return invalidStreamPtr;
    }
    if (macTCPRefNum == 0) {
         log_message("Error (SendUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
     }
    if (myUsername == NULL || myLocalIPStr == NULL) {
         log_message("Error (SendUDP): Missing username or IP string.");
         return paramErr;
     }

    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_message("Error (SendUDP): Failed to format discovery broadcast message.");
        return paramErr;
    }

    wds[0].length = formatted_len - 1;
    wds[0].ptr = buffer;
    wds[1].length = 0;
    wds[1].ptr = nil;

    // --- Prepare UDPiopb (58 bytes) for UDPWrite ---
    memset(&pb, 0, sizeof(UDPiopb)); // Use 58-byte size
    pb.ioCompletion = nil;
    pb.ioCRefNum = macTCPRefNum;
    pb.csCode = udpWrite;
    pb.udpStream = gUDPStream; // Use stream pointer obtained from create

    // Access parameters via csParam.send (using 58-byte struct layout)
    pb.csParam.send.remoteHost = BROADCAST_IP;
    pb.csParam.send.remotePort = PORT_UDP;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.checkSum = true;

    // log_message("Calling PBControlSync (udpWrite) using UDPiopb (58 bytes)..."); // Optional log
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err != noErr) {
        log_message("Error (SendUDP): PBControlSync(udpWrite) failed with stream 0x%lX using UDPiopb. Error: %d", (unsigned long)gUDPStream, err);
        return err;
    }

    gLastBroadcastTimeTicks = TickCount();
    return noErr;
}

/**
 * @brief Checks if it's time to send the next discovery broadcast and sends if needed.
 */
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    // *** This function remains unchanged ***
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;
    if (gUDPStream == NULL || macTCPRefNum == 0) return;
    if (currentTimeTicks < gLastBroadcastTimeTicks) { gLastBroadcastTimeTicks = currentTimeTicks; }
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        OSErr sendErr = SendDiscoveryBroadcast(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr != noErr) { log_message("Periodic broadcast failed (Error: %d)", sendErr); }
    }
}


/**
 * @brief Checks for and processes incoming UDP packets - USES UDPiopb (58 bytes) for read/return.
 *        *** Attempts to pass pbRead.csParam.receive.rcvBuff to udpBfrReturn ***
 */
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr   err;
    // *** USE STANDARD UDPiopb (58 bytes) ***
    UDPiopb pbRead;
    char    senderIPStrFromHeader[INET_ADDRSTRLEN];
    char    senderIPStrFromPayload[INET_ADDRSTRLEN];
    char    senderUsername[32];
    char    msgType[32];
    char    content[BUFFER_SIZE];
    unsigned short bytesReceived = 0;
    Ptr     receivedDataPtr = NULL; // To store the buffer pointer returned by udpRead

    // Check if UDP is initialized and stream pointer is valid
    if (gUDPStream == NULL || macTCPRefNum == 0 || gUDPRecvBuffer == NULL) {
        return; // Cannot receive if not initialized or stream invalid
    }

    // --- Prepare UDPiopb (58 bytes) for UDPRead ---
    memset(&pbRead, 0, sizeof(UDPiopb)); // Use 58-byte size
    pbRead.ioCompletion = nil;
    pbRead.ioCRefNum = macTCPRefNum;
    pbRead.csCode = udpRead;
    pbRead.udpStream = gUDPStream; // Use stream pointer obtained from create

    // Access parameters via csParam.receive (using 58-byte struct layout)
    // MacTCP *should* update pbRead.csParam.receive.rcvBuff to point to the data
    pbRead.csParam.receive.rcvBuff = gUDPRecvBuffer; // Provide base buffer
    pbRead.csParam.receive.rcvBuffLen = kMinUDPBufSize;
    pbRead.csParam.receive.timeOut = 1;
    pbRead.csParam.receive.secondTimeStamp = 0;
    pbRead.csParam.receive.remoteHost = 0;
    pbRead.csParam.receive.remotePort = 0;

    // log_message("Calling PBControlSync (udpRead) using UDPiopb (58 bytes)..."); // Optional log
    err = PBControlSync((ParmBlkPtr)&pbRead);

    // --- Handle Results ---
    if (err == noErr) {
        // --- Packet Received ---
        ip_addr senderIP = pbRead.csParam.receive.remoteHost; // Read output using 58-byte layout
        bytesReceived = pbRead.csParam.receive.rcvBuffLen;   // Read output using 58-byte layout
        // *** Store the pointer MacTCP *might* have updated ***
        receivedDataPtr = pbRead.csParam.receive.rcvBuff;

        // Only process if data was actually received
        if (bytesReceived > 0) {

            // Log the pointer returned by udpRead
            log_message("DEBUG: udpRead returned: bytes=%u, receivedDataPtr=0x%lX", bytesReceived, (unsigned long)receivedDataPtr);

            // --- Parsing logic ---
            // NOTE: We should parse from receivedDataPtr now, NOT gUDPRecvBuffer,
            // as receivedDataPtr *should* point to the actual data within the larger buffer.
            if (senderIP != myLocalIP) {
                 OSErr addrErr = AddrToStr(senderIP, senderIPStrFromHeader);
                 if (addrErr != noErr) {
                     log_message("Warning: AddrToStr failed for sender IP %lu (Error: %d). Using raw IP.", senderIP, addrErr);
                     sprintf(senderIPStrFromHeader, "%lu.%lu.%lu.%lu", (senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF, (senderIP >> 8) & 0xFF, senderIP & 0xFF);
                 }
                 // *** Parse using receivedDataPtr ***
                 if (parse_message(receivedDataPtr, bytesReceived, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
                    if (strcmp(msgType, MSG_DISCOVERY_RESPONSE) == 0) {
                        log_message("Received DISCOVERY_RESPONSE from %s@%s", senderUsername, senderIPStrFromHeader);
                        if (AddOrUpdatePeer(senderIPStrFromHeader, senderUsername) < 0) {
                            log_message("Peer list full, could not add %s@%s", senderUsername, senderIPStrFromHeader);
                        }
                    } else if (strcmp(msgType, MSG_DISCOVERY) == 0) {
                         log_message("Received DISCOVERY from %s@%s", senderUsername, senderIPStrFromHeader);
                         // TODO: Send DISCOVERY_RESPONSE back
                         // SendUDPResponse(macTCPRefNum, pbRead.csParam.receive.remoteHost, pbRead.csParam.receive.remotePort, gMyUsername, gMyLocalIPStr);
                    }
                    // else { log_message("Received Other UDP Msg Type: %s from %s", msgType, senderIPStrFromHeader); }
                 } else {
                     // Parse failed (bad magic number or format)
                     log_message("Discarding invalid/unknown UDP msg from %s (%u bytes).",
                                 senderIPStrFromHeader, bytesReceived);
                     // Log raw data (optional, for debugging)
                     if (gLogFile != NULL) {
                        fprintf(gLogFile, "    Raw Data (%u bytes): [", bytesReceived);
                        // Print hex representation for safety, limit loop to buffer size
                        // *** Use receivedDataPtr for logging raw data ***
                        for (unsigned short i = 0; i < bytesReceived && i < kMinUDPBufSize; ++i) {
                             unsigned char c = (unsigned char)receivedDataPtr[i];
                             if (c >= 32 && c <= 126) { fputc(c, gLogFile); }
                             else { fprintf(gLogFile, "\\x%02X", c); }
                        }
                        fprintf(gLogFile, "]\n");
                        fflush(gLogFile);
                     }
                 }
            } // End if (senderIP != myLocalIP)

            // --- *** Return the buffer segment using UDPiopb (58 bytes) *** ---
            // *** Pass the pointer returned by udpRead (receivedDataPtr) ***
            UDPiopb bfrReturnPB;
            OSErr returnErr;

            memset(&bfrReturnPB, 0, sizeof(UDPiopb)); // Use 58-byte size
            bfrReturnPB.ioCompletion = nil;
            bfrReturnPB.ioCRefNum = macTCPRefNum;
            bfrReturnPB.csCode = udpBfrReturn;
            bfrReturnPB.udpStream = gUDPStream; // Use stream pointer obtained from create

            // *** KEY CHANGE: Pass the pointer from the udpRead result ***
            bfrReturnPB.csParam.receive.rcvBuff = receivedDataPtr;
            // Keep setting length just in case, although it didn't help before
            bfrReturnPB.csParam.receive.rcvBuffLen = kMinUDPBufSize;

            // Log before the call, showing the pointer being passed
            log_message("DEBUG: Before udpBfrReturn: gUDPStream=0x%lX, Passing rcvBuff=0x%lX (from udpRead result), rcvBuffLen=%u",
                        (unsigned long)gUDPStream,
                        (unsigned long)bfrReturnPB.csParam.receive.rcvBuff,
                        bfrReturnPB.csParam.receive.rcvBuffLen);

            returnErr = PBControlSync((ParmBlkPtr)&bfrReturnPB);
            if (returnErr != noErr) {
                log_message("CRITICAL Error (UDP Receive): PBControlSync(udpBfrReturn) failed using UDPiopb. Error: %d.", returnErr);
            } else {
                 log_message("DEBUG: PBControlSync(udpBfrReturn) using UDPiopb succeeded."); // SUCCESS!
            }
            // --- End of buffer return ---

        } // End if (bytesReceived > 0)

    } else if (err == commandTimeout) { // Only check for the defined MacTCP timeout code
        // Timeout or expected condition - normal, no packet waiting. Nothing to do.
    } else {
        // Some other unexpected error occurred during read attempt
        log_message("Error (UDP Receive): PBControlSync(udpRead) failed using UDPiopb. Error: %d", err);
    }
}


/**
 * @brief Cleans up the UDP discovery endpoint - USES UDPiopb (58 bytes).
 */
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum) {
    // *** USE STANDARD UDPiopb (58 bytes) ***
    UDPiopb pb;
    OSErr err;

    log_message("Cleaning up UDP Discovery Endpoint...");

    // --- Release UDP Endpoint ---
    if (gUDPStream != NULL) {
        if (macTCPRefNum == 0) {
             log_message("Warning (CleanupUDP): Invalid MacTCP RefNum (%d), cannot release UDP stream.", macTCPRefNum);
        } else {
            log_message("Attempting PBControlSync (udpRelease) using UDPiopb (58 bytes) for endpoint 0x%lX...", (unsigned long)gUDPStream);
            memset(&pb, 0, sizeof(UDPiopb)); // Use 58-byte size
            pb.ioCompletion = nil;
            pb.ioCRefNum = macTCPRefNum;
            pb.csCode = udpRelease;
            pb.udpStream = gUDPStream; // Use stream pointer obtained from create

            if (gUDPRecvBuffer != NULL) {
                // Access parameters via csParam.create (using 58-byte struct layout)
                pb.csParam.create.rcvBuff = gUDPRecvBuffer;
                pb.csParam.create.rcvBuffLen = kMinUDPBufSize;

                err = PBControlSync((ParmBlkPtr)&pb);
                if (err != noErr) {
                    log_message("Warning (CleanupUDP): PBControlSync(udpRelease) failed using UDPiopb. Error: %d", err);
                } else {
                    log_message("PBControlSync(udpRelease) using UDPiopb succeeded.");
                }
            } else {
                 log_message("Warning (CleanupUDP): Cannot call udpRelease because receive buffer pointer is NULL.");
            }
        }
    } else {
        log_message("UDP Endpoint was not open or already invalid, skipping release.");
    }
    gUDPStream = NULL;

    // --- Dispose UDP Receive Buffer ---
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    log_message("UDP Discovery Endpoint cleanup finished.");
}