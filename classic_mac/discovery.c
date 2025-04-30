// FILE: ./classic_mac/discovery.c
#include "discovery.h" // Includes MyUDPiopb definition
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

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts and receives using PBControl.
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    MyUDPiopb pb; // <--- CHANGE: Use OUR corrected UDP parameter block structure
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

    // --- Prepare MyUDPiopb for UDPCreate ---
    memset(&pb, 0, sizeof(MyUDPiopb)); // <--- CHANGE: Use corrected struct size
    pb.ioCompletion = nil;             // No completion routine for sync call
    pb.ioCRefNum = macTCPRefNum;       // Use passed-in Driver reference number
    pb.csCode = udpCreate;             // Command code (uses #define from discovery.h)
    pb.udpStream = 0L;                 // IMPORTANT: MacTCP writes the output StreamPtr here

    // Access parameters via the csParam.create union structure
    pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pointer to allocated buffer
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Size of the buffer
    pb.csParam.create.notifyProc = nil;         // No notification routine needed for sync operation
    pb.csParam.create.localPort = specificPort;  // Listen on the specific discovery port
    // pb.csParam.create.userDataPtr is not needed for nil notifyProc

    log_message("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pb); // Cast MyUDPiopb to generic ParmBlkPtr (should be safe)

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
    gUDPStream = pb.udpStream;

    // Check if the returned stream pointer is NULL, even though noErr was returned.
    if (gUDPStream == NULL) {
         log_message("CRITICAL WARNING (InitUDP): udpCreate returned noErr but StreamPtr is NULL. UDP may fail.");
         // Clean up buffer as the stream is unusable
         if (gUDPRecvBuffer != NULL) {
             DisposePtr(gUDPRecvBuffer);
             gUDPRecvBuffer = NULL;
         }
         return ioErr; // Indicate an internal inconsistency (general I/O error)
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
    MyUDPiopb pb; // <--- CHANGE: Use OUR Ccorrected UDP parameter block structure
    int formatted_len; // To store the length returned by format_message

    // Check if the stream pointer is NULL before attempting to use it
    if (gUDPStream == NULL) {
        log_message("Error (SendUDP): Cannot send broadcast, UDP stream pointer is NULL.");
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
    wds[0].length = formatted_len - 1; // format_message returns total bytes incl. null term
    wds[0].ptr = buffer;
    wds[1].length = 0; // Terminating entry
    wds[1].ptr = nil;

    // --- Prepare MyUDPiopb for UDPWrite ---
    memset(&pb, 0, sizeof(MyUDPiopb)); // <--- CHANGE: Use corrected struct size
    pb.ioCompletion = nil;             // No completion routine for sync call
    pb.ioCRefNum = macTCPRefNum;       // Use passed-in Driver reference number
    pb.csCode = udpWrite;              // Command code (uses #define)
    pb.udpStream = gUDPStream;         // Input: The stream pointer obtained from UDPCreate

    // Access parameters via the csParam.send union structure
    pb.csParam.send.remoteHost = BROADCAST_IP;   // Destination IP (broadcast)
    pb.csParam.send.remotePort = PORT_UDP;       // Destination port
    pb.csParam.send.wdsPtr = (Ptr)wds;           // Pointer to the WDS array
    pb.csParam.send.checkSum = true;             // Calculate and send checksum
    // pb.csParam.send.sendLength is output only for UDPWrite

    // Call PBControlSync
    err = PBControlSync((ParmBlkPtr)&pb); // Cast MyUDPiopb to generic ParmBlkPtr

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
    if (gUDPStream == NULL || macTCPRefNum == 0) return;

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
    MyUDPiopb pbRead; // <--- CHANGE: Use OUR corrected structure for the read operation
    char    senderIPStrFromHeader[INET_ADDRSTRLEN];
    char    senderIPStrFromPayload[INET_ADDRSTRLEN];
    char    senderUsername[32];
    char    msgType[32];
    char    content[BUFFER_SIZE];
    unsigned short bytesReceived = 0; // Initialize to 0

    // Check if UDP is initialized and stream pointer is valid
    if (gUDPStream == NULL || macTCPRefNum == 0 || gUDPRecvBuffer == NULL) {
        return; // Cannot receive if not initialized or stream invalid
    }

    // --- Prepare MyUDPiopb for UDPRead ---
    memset(&pbRead, 0, sizeof(MyUDPiopb)); // <--- CHANGE: Use corrected struct size
    pbRead.ioCompletion = nil;
    pbRead.ioCRefNum = macTCPRefNum;
    pbRead.csCode = udpRead;             // Command code (uses #define)
    pbRead.udpStream = gUDPStream;       // Use the valid stream pointer

    // These are treated as OUTPUT by UDPRead
    pbRead.csParam.receive.rcvBuff = gUDPRecvBuffer;    // Buffer MacTCP writes into
    pbRead.csParam.receive.rcvBuffLen = kMinUDPBufSize; // Max length MacTCP can write
    pbRead.csParam.receive.timeOut = 1;                 // Short timeout (in seconds) for non-blocking check
    pbRead.csParam.receive.secondTimeStamp = 0;         // Not used here
    // Output fields filled by MacTCP on success:
    pbRead.csParam.receive.remoteHost = 0;
    pbRead.csParam.receive.remotePort = 0;
    // pbRead.csParam.receive.rcvBuffLen is also output (actual bytes received)

    // --- Call PBControlSync to attempt reading ---
    err = PBControlSync((ParmBlkPtr)&pbRead); // Cast is safe

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
            MyUDPiopb bfrReturnPB; // <--- CHANGE: Use OUR corrected structure for clarity
            OSErr returnErr;

            memset(&bfrReturnPB, 0, sizeof(MyUDPiopb)); // <--- CHANGE: Use corrected struct size
            bfrReturnPB.ioCompletion = nil;
            bfrReturnPB.ioCRefNum = macTCPRefNum;
            bfrReturnPB.csCode = udpBfrReturn; // Command code (uses #define)
            bfrReturnPB.udpStream = gUDPStream; // Use the valid stream ptr

            // UDPBfrReturn uses the 'rcvBuff' field within the 'receive' union member
            // to identify the buffer segment being returned. Point it to the start
            // of the buffer that MacTCP just wrote into.
            bfrReturnPB.csParam.receive.rcvBuff = gUDPRecvBuffer;

            returnErr = PBControlSync((ParmBlkPtr)&bfrReturnPB); // Cast is safe
            if (returnErr != noErr) {
                // Log the error, potentially including the invalid gUDPStream value
                log_message("CRITICAL Error (UDP Receive): PBControlSync(udpBfrReturn) failed with stream 0x%lX. Error: %d. UDP receive may stop working.", (unsigned long)gUDPStream, returnErr);
                // If this fails now, it's less likely to be a struct layout issue,
                // potentially an invalid gUDPStream value from udpCreate or memory corruption.
            }
            // --- End of buffer return ---

        } // End if (bytesReceived > 0)

    } else if (err == commandTimeout) { // Only check for the defined MacTCP timeout code
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
    MyUDPiopb pb; // <--- CHANGE: Use OUR corrected UDP parameter block structure
    OSErr err;

    log_message("Cleaning up UDP Discovery Endpoint...");

    // --- Release UDP Endpoint ---
    // Check if stream pointer is valid before attempting release
    if (gUDPStream != NULL) {
        if (macTCPRefNum == 0) {
             log_message("Warning (CleanupUDP): Invalid MacTCP RefNum (%d), cannot release UDP stream.", macTCPRefNum);
        } else {
            log_message("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)gUDPStream);
            memset(&pb, 0, sizeof(MyUDPiopb)); // <--- CHANGE: Use corrected struct size
            pb.ioCompletion = nil;
            pb.ioCRefNum = macTCPRefNum;
            pb.csCode = udpRelease; // Command code (uses #define)
            pb.udpStream = gUDPStream;

            // Set the parameters required by UDPRelease (which uses UDPCreatePB layout)
            // These identify the buffer associated with the stream being released.
            if (gUDPRecvBuffer != NULL) {
                pb.csParam.create.rcvBuff = gUDPRecvBuffer;
                pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Provide the original size

                err = PBControlSync((ParmBlkPtr)&pb); // Cast is safe
                if (err != noErr) {
                    log_message("Warning (CleanupUDP): PBControlSync(udpRelease) failed with stream 0x%lX. Error: %d", (unsigned long)gUDPStream, err);
                } else {
                    log_message("PBControlSync(udpRelease) succeeded.");
                }
            } else {
                 log_message("Warning (CleanupUDP): Cannot call udpRelease because receive buffer pointer is NULL.");
            }
        }
    } else {
        log_message("UDP Endpoint was not open or already invalid, skipping release.");
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

// Update the debug function to use the new structure type
void PrintUDPiopbLayout() {
    log_message("--- MyUDPiopb Layout (Corrected for Driver) ---");
    log_message("sizeof(MyUDPiopb) = %lu", (unsigned long)sizeof(MyUDPiopb)); // <--- CHANGE

    // Use MyUDPiopb for offset calculation
    log_message("Offset ioCompletion = %lu", (unsigned long)offsetof(MyUDPiopb, ioCompletion)); // <--- CHANGE
    log_message("Offset ioResult = %lu", (unsigned long)offsetof(MyUDPiopb, ioResult));       // <--- CHANGE
    log_message("Offset ioCRefNum = %lu", (unsigned long)offsetof(MyUDPiopb, ioCRefNum));      // <--- CHANGE
    log_message("Offset csCode = %lu", (unsigned long)offsetof(MyUDPiopb, csCode));           // <--- CHANGE (Should be 28 now)
    log_message("Offset udpStream = %lu", (unsigned long)offsetof(MyUDPiopb, udpStream));      // <--- CHANGE (Should be 30 now)
    log_message("Offset csParam = %lu", (unsigned long)offsetof(MyUDPiopb, csParam));         // <--- CHANGE (Should be 34 now)

    log_message("  --- csParam.create ---");
    log_message("  sizeof(UDPCreatePB) = %lu", (unsigned long)sizeof(struct UDPCreatePB)); // No change, uses type from MacTCP.h
    log_message("  Offset create.rcvBuff = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.create.rcvBuff));       // <--- CHANGE
    log_message("  Offset create.rcvBuffLen = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.create.rcvBuffLen));  // <--- CHANGE
    log_message("  Offset create.notifyProc = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.create.notifyProc));  // <--- CHANGE
    log_message("  Offset create.localPort = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.create.localPort));   // <--- CHANGE
    log_message("  Offset create.userDataPtr = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.create.userDataPtr)); // <--- CHANGE

    log_message("  --- csParam.receive ---");
    log_message("  sizeof(UDPReceivePB) = %lu", (unsigned long)sizeof(struct UDPReceivePB)); // No change, uses type from MacTCP.h
    // Renamed field in official MacTCP.h struct
    log_message("  Offset receive.timeout = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.timeOut));      // <--- CHANGE
    log_message("  Offset receive.remoteHost = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.remoteHost));   // <--- CHANGE
    log_message("  Offset receive.remotePort = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.remotePort));   // <--- CHANGE
    log_message("  Offset receive.rcvBuff = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.rcvBuff));      // <--- CHANGE
    log_message("  Offset receive.rcvBuffLen = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.rcvBuffLen)); // <--- CHANGE
    log_message("  Offset receive.secondTimeStamp = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.secondTimeStamp)); // <--- CHANGE
    log_message("  Offset receive.userDataPtr = %lu", (unsigned long)offsetof(MyUDPiopb, csParam.receive.userDataPtr)); // <--- CHANGE

    log_message("--- End Layout ---");
}
