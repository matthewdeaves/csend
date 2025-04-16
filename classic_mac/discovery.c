// FILE: ./classic_mac/discovery.c
#include "discovery.h"
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

// --- Global Variable Definitions ---
StreamPtr gUDPStream = NULL; // Pointer to our UDP stream for discovery (initialize to NULL)
Ptr     gUDPRecvBuffer = NULL; // Pointer to the buffer allocated for UDPCreate (initialize to NULL)
unsigned long gLastBroadcastTimeTicks = 0; // Time in Ticks

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts and receives using PBControl.
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    UDPiopb pb; // Use the specific UDP parameter block structure
    const unsigned short specificPort = PORT_UDP; // Listen on the standard discovery port

    log_message("Initializing UDP Discovery Endpoint...");

    if (macTCPRefNum == 0) {
         log_message("Error (InitUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr; // Or another appropriate error
    }

    // Allocate buffer needed by UDPCreate for receiving data
    // Using NewPtrClear is fine, ensures buffer is zeroed initially.
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_message("Error (InitUDP): Failed to allocate UDP receive buffer (memFullErr).");
        return memFullErr;
    }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    // --- Prepare UDPiopb for UDPCreate ---
    memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
    pb.ioCompletion = nil;           // No completion routine for sync call
    pb.ioCRefNum = macTCPRefNum;     // Use passed-in Driver reference number
    pb.csCode = udpCreate;           // Command code for creating UDP stream
    pb.udpStream = 0L;               // IMPORTANT: MacTCP writes the output StreamPtr here

    // Access parameters via the csParam.create union structure
    pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pointer to allocated buffer
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Size of the buffer
    pb.csParam.create.notifyProc = nil;         // No notification routine needed for sync operation
    pb.csParam.create.localPort = specificPort;  // Listen on the specific discovery port
    // pb.csParam.create.userDataPtr is not needed for nil notifyProc

    log_message("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    // Log the state *after* the call
    log_message("DEBUG: After PBControlSync(udpCreate): err=%d, pb.udpStream=0x%lX, pb.csParam.create.localPort=%u",
        err, (unsigned long)pb.udpStream, pb.csParam.create.localPort);

    if (err != noErr) {
        log_message("Error (InitUDP): PBControlSync(udpCreate) failed. Error: %d", err);
        if (gUDPRecvBuffer != NULL) {
            DisposePtr(gUDPRecvBuffer); // Clean up buffer if create failed
            gUDPRecvBuffer = NULL;
        }
        gUDPStream = NULL; // Ensure stream is NULL on failure
        return err;
    }

    // --- Retrieve the output StreamPtr from the correct field ---
    // This is where the core problem likely lies if the log shows the wrong value here.
    gUDPStream = pb.udpStream;

    // Check if the returned stream pointer seems invalid (e.g., NULL or matches buffer)
    if (gUDPStream == NULL || gUDPStream == (StreamPtr)gUDPRecvBuffer) {
         log_message("CRITICAL WARNING (InitUDP): udpCreate returned potentially invalid StreamPtr (0x%lX). UDP may fail.", (unsigned long)gUDPStream);
         // Consider returning an error here? Or proceed cautiously?
         // For now, let's proceed but the warning is important.
         // return internalErr; // Example of returning an error
    }


    // Retrieve the *actual* port assigned by MacTCP from the localPort field
    unsigned short assignedPort = pb.csParam.create.localPort;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);
    gLastBroadcastTimeTicks = 0; // Reset broadcast timer
    return noErr;
}

/**
 * @brief Sends a UDP discovery broadcast message using PBControl.
 */
OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2]; // Need 2 entries for the terminating zero entry
    UDPiopb pb; // Use the specific UDP parameter block structure
    int formatted_len; // To store the length returned by format_message

    // Check for the invalid stream pointer *before* attempting to use it
    if (gUDPStream == NULL || gUDPStream == (StreamPtr)gUDPRecvBuffer) {
        log_message("Error (SendUDP): Cannot send broadcast, UDP stream pointer is invalid (0x%lX).", (unsigned long)gUDPStream);
        return invalidStreamPtr; // Or another appropriate error
    }
     if (macTCPRefNum == 0) {
         log_message("Error (SendUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
     }
     if (myUsername == NULL || myLocalIPStr == NULL) {
         log_message("Error (SendUDP): Missing username or IP string.");
         return paramErr;
     }

    // Format the message using the shared protocol function (includes magic number)
    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) { // format_message returns 0 on error, >0 on success
        log_message("Error (SendUDP): Failed to format discovery broadcast message.");
        return paramErr; // Or another suitable error code
    }

    // Prepare the Write Data Structure (WDS)
    // The length for WDS is the actual data length, EXCLUDING the null terminator.
    // format_message returns total bytes including null term, so subtract 1.
    wds[0].length = formatted_len - 1;
    wds[0].ptr = buffer;
    wds[1].length = 0; // Terminating entry
    wds[1].ptr = nil;

    // --- Prepare UDPiopb for UDPWrite ---
    memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
    pb.ioCompletion = nil;           // No completion routine for sync call
    pb.ioCRefNum = macTCPRefNum;     // Use passed-in Driver reference number
    pb.csCode = udpWrite;            // Command code for writing UDP data
    pb.udpStream = gUDPStream;       // Input: The stream pointer obtained from UDPCreate

    // Access parameters via the csParam.send union structure
    pb.csParam.send.remoteHost = BROADCAST_IP;   // Destination IP (broadcast)
    pb.csParam.send.remotePort = PORT_UDP;       // Destination port
    pb.csParam.send.wdsPtr = (Ptr)wds;           // Pointer to the WDS array
    pb.csParam.send.checkSum = true;             // Calculate and send checksum
    // pb.csParam.send.sendLength is output only for UDPWrite

    // Call PBControlSync
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        log_message("Error (SendUDP): PBControlSync(udpWrite) failed with stream 0x%lX. Error: %d", (unsigned long)gUDPStream, err);
        return err;
    }

    gLastBroadcastTimeTicks = TickCount(); // Update last broadcast time
    return noErr;
}

/**
 * @brief Checks if it's time to send the next discovery broadcast and sends if needed.
 */
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    unsigned long currentTimeTicks = TickCount();
    // DISCOVERY_INTERVAL is in seconds, TickCount() is 1/60th seconds
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;

    // Only check if the endpoint appears initialized (stream pointer not NULL)
    // Add check for potentially invalid stream pointer as well
    if (gUDPStream == NULL || gUDPStream == (StreamPtr)gUDPRecvBuffer || macTCPRefNum == 0) return;

    // Check for timer wraparound (unlikely but possible)
    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks; // Reset timer if wraparound detected
    }

    // Send immediately if never sent before, or if interval has passed
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        OSErr sendErr = SendDiscoveryBroadcast(macTCPRefNum, myUsername, myLocalIPStr);
        // Optionally log if send failed, but don't stop periodic checks
        if (sendErr != noErr) {
             log_message("Periodic broadcast failed (Error: %d)", sendErr);
        }
    }
}

/**
 * @brief Checks for and processes incoming UDP packets (e.g., discovery responses).
 *        Uses PBControlSync with a short timeout for non-blocking checks.
 *        Includes fix to call UDPBfrReturn after successful read.
 */
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr   err;
    UDPiopb pbRead; // Use a distinct PB for the read operation
    char    senderIPStrFromHeader[INET_ADDRSTRLEN];
    char    senderIPStrFromPayload[INET_ADDRSTRLEN];
    char    senderUsername[32];
    char    msgType[32];
    char    content[BUFFER_SIZE];
    unsigned short bytesReceived = 0; // Initialize to 0

    // Check if UDP is initialized and stream pointer seems valid
    if (gUDPStream == NULL || gUDPStream == (StreamPtr)gUDPRecvBuffer || macTCPRefNum == 0 || gUDPRecvBuffer == NULL) {
        return; // Cannot receive if not initialized or stream invalid
    }

    // --- Prepare UDPiopb for UDPRead ---
    memset(&pbRead, 0, sizeof(UDPiopb));
    pbRead.ioCompletion = nil;
    pbRead.ioCRefNum = macTCPRefNum;
    pbRead.csCode = udpRead;             // Correct code (21)
    pbRead.udpStream = gUDPStream;       // Use the potentially invalid stream pointer

    // These are treated as OUTPUT by UDPRead (csCode 21)
    pbRead.csParam.receive.rcvBuff = gUDPRecvBuffer;    // Buffer MacTCP writes into
    pbRead.csParam.receive.rcvBuffLen = kMinUDPBufSize; // Max length MacTCP can write
    pbRead.csParam.receive.timeOut = 1;                 // Short timeout (in seconds) for non-blocking check
    pbRead.csParam.receive.secondTimeStamp = 0;         // Not used here
    // Output fields filled by MacTCP on success:
    pbRead.csParam.receive.remoteHost = 0;
    pbRead.csParam.receive.remotePort = 0;
    // pbRead.csParam.receive.rcvBuffLen is also output

    // --- Call PBControlSync to attempt reading ---
    err = PBControlSync((ParmBlkPtr)&pbRead);

    // --- Handle Results ---
    if (err == noErr) {
        // --- Packet Received ---
        ip_addr senderIP = pbRead.csParam.receive.remoteHost;
        // Get the ACTUAL number of bytes received from the output parameter
        bytesReceived = pbRead.csParam.receive.rcvBuffLen;

        // Only process if data was actually received
        if (bytesReceived > 0) {
            // Ignore messages from self
            if (senderIP != myLocalIP) {
                // Convert sender IP (from header) to string using DNR
                OSErr addrErr = AddrToStr(senderIP, senderIPStrFromHeader);
                if (addrErr != noErr) {
                    log_message("Warning: AddrToStr failed for sender IP %lu (Error: %d). Using raw IP.", senderIP, addrErr);
                    sprintf(senderIPStrFromHeader, "%lu.%lu.%lu.%lu",
                            (senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF,
                            (senderIP >> 8) & 0xFF, senderIP & 0xFF);
                }

                // Parse the message content, passing the actual received length
                if (parse_message(gUDPRecvBuffer, bytesReceived, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
                    // Handle valid messages (DISCOVERY_RESPONSE, DISCOVERY)
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
                        for (unsigned short i = 0; i < bytesReceived && i < kMinUDPBufSize; ++i) {
                             unsigned char c = (unsigned char)gUDPRecvBuffer[i];
                             if (c >= 32 && c <= 126) {
                                 fputc(c, gLogFile);
                             } else {
                                 fprintf(gLogFile, "\\x%02X", c);
                             }
                        }
                        fprintf(gLogFile, "]\n");
                        fflush(gLogFile);
                    }
                }
            } // End if (senderIP != myLocalIP)

            // --- *** CRITICAL: Return the buffer segment to MacTCP *** ---
            // This MUST be done after EVERY successful UDPRead that returns data (bytesReceived > 0),
            // even if the data was ignored (from self) or failed parsing.
            UDPiopb bfrReturnPB; // Use a separate PB for clarity
            OSErr returnErr;

            memset(&bfrReturnPB, 0, sizeof(UDPiopb));
            bfrReturnPB.ioCompletion = nil;
            bfrReturnPB.ioCRefNum = macTCPRefNum;
            bfrReturnPB.csCode = udpBfrReturn; // Correct code (22)
            bfrReturnPB.udpStream = gUDPStream; // Use the (potentially invalid) stream ptr

            // UDPBfrReturn uses the 'rcvBuff' field within the 'receive' union member
            // to identify the buffer segment being returned. Point it to the start
            // of the buffer that MacTCP just wrote into.
            bfrReturnPB.csParam.receive.rcvBuff = gUDPRecvBuffer;

            returnErr = PBControlSync((ParmBlkPtr)&bfrReturnPB);
            if (returnErr != noErr) {
                // Log the error, potentially including the invalid gUDPStream value
                log_message("CRITICAL Error (UDP Receive): PBControlSync(udpBfrReturn) failed with stream 0x%lX. Error: %d. UDP receive may stop working.", (unsigned long)gUDPStream, returnErr);
                // This is where the -23013 error was likely happening before.
                // If gUDPStream is invalid, this call will fail.
            }
            // --- End of buffer return ---

        } // End if (bytesReceived > 0)

    } else if (err == commandTimeout || err == -23018 /* readTimeout? */ || err == -23015 /* connectionDoesntExist? */) {
        // Timeout or expected condition - normal, no packet waiting. Nothing to do.
    } else {
        // Some other unexpected error occurred during read attempt
        // Log the error, including the stream pointer used
        log_message("Error (UDP Receive): PBControlSync(udpRead) failed with stream 0x%lX. Error: %d", (unsigned long)gUDPStream, err);
        // No buffer to return in this case, as the read itself failed.
    }
}


/**
 * @brief Cleans up the UDP discovery endpoint resources.
 */
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum) {
    UDPiopb pb; // Use the specific UDP parameter block structure
    OSErr err;

    log_message("Cleaning up UDP Discovery Endpoint...");

    // --- Release UDP Endpoint ---
    // Check if stream pointer looks valid before attempting release
    if (gUDPStream != NULL && gUDPStream != (StreamPtr)gUDPRecvBuffer) {
        if (macTCPRefNum == 0) {
             log_message("Warning (CleanupUDP): Invalid MacTCP RefNum (%d), cannot release UDP stream.", macTCPRefNum);
        } else {
            log_message("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)gUDPStream);
            memset(&pb, 0, sizeof(UDPiopb));
            pb.ioCompletion = nil;
            pb.ioCRefNum = macTCPRefNum;
            pb.csCode = udpRelease;
            pb.udpStream = gUDPStream;

            // Set the parameters required by UDPRelease
            // These identify the buffer associated with the stream being released.
            pb.csParam.create.rcvBuff = gUDPRecvBuffer;
            pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Provide the original size

            err = PBControlSync((ParmBlkPtr)&pb);
            if (err != noErr) {
                log_message("Warning (CleanupUDP): PBControlSync(udpRelease) failed with stream 0x%lX. Error: %d", (unsigned long)gUDPStream, err);
            } else {
                log_message("PBControlSync(udpRelease) succeeded.");
            }
        }
    } else if (gUDPStream == (StreamPtr)gUDPRecvBuffer) {
         log_message("Skipping udpRelease because stream pointer appears invalid (0x%lX).", (unsigned long)gUDPStream);
    } else {
        log_message("UDP Endpoint was not open, skipping release.");
    }
    gUDPStream = NULL; // Mark as released or invalid regardless

    // --- Dispose UDP Receive Buffer ---
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    log_message("UDP Discovery Endpoint cleanup finished.");
}