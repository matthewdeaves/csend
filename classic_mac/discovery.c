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
    pb.udpStream = 0L;               // Output: Will be filled by MacTCP

    // Access parameters via the csParam.create union structure
    pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pointer to allocated buffer
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Size of the buffer
    pb.csParam.create.notifyProc = nil;         // No notification routine needed
    pb.csParam.create.localPort = specificPort;  // Listen on the specific discovery port

    log_message("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        log_message("Error (InitUDP): PBControlSync(udpCreate) failed. Error: %d", err);
        if (gUDPRecvBuffer != NULL) {
            DisposePtr(gUDPRecvBuffer); // Clean up buffer if create failed
            gUDPRecvBuffer = NULL;
        }
        gUDPStream = NULL; // Ensure stream is NULL on failure
        return err;
    }

    // Retrieve the output StreamPtr from the correct field
    gUDPStream = pb.udpStream;
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

    if (gUDPStream == NULL) {
        log_message("Error (SendUDP): Cannot send broadcast, UDP endpoint not initialized.");
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

    // Call PBControlSync
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        log_message("Error (SendUDP): PBControlSync(udpWrite) failed. Error: %d", err);
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

    // Only check if the endpoint is initialized and refnum is valid
    if (gUDPStream == NULL || macTCPRefNum == 0) return;

    // Check for timer wraparound (unlikely but possible)
    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks; // Reset timer if wraparound detected
    }

    // Send immediately if never sent before, or if interval has passed
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        SendDiscoveryBroadcast(macTCPRefNum, myUsername, myLocalIPStr); // Ignore return value for simple periodic broadcast
    }
}

/**
 * @brief Checks for and processes incoming UDP packets (e.g., discovery responses).
 *        Uses PBControlSync with a short timeout for non-blocking checks.
 */
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr   err;
    UDPiopb pb;
    char    senderIPStrFromHeader[INET_ADDRSTRLEN]; // Renamed: IP from UDP header
    char    senderIPStrFromPayload[INET_ADDRSTRLEN]; // New: Buffer for parse_message output
    char    senderUsername[32];
    char    msgType[32];
    char    content[BUFFER_SIZE];
    unsigned short bytesReceived; // Store actual bytes received

    // Check if UDP is initialized
    if (gUDPStream == NULL || macTCPRefNum == 0 || gUDPRecvBuffer == NULL) {
        return; // Cannot receive if not initialized
    }

    // --- Prepare UDPiopb for UDPRead ---
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = macTCPRefNum;
    pb.csCode = udpRead;             // Correct code (21)
    pb.udpStream = gUDPStream;

    // These are treated as OUTPUT by UDPRead (csCode 21)
    // MacTCP will write into gUDPRecvBuffer up to kMinUDPBufSize bytes
    pb.csParam.receive.rcvBuff = gUDPRecvBuffer;
    pb.csParam.receive.rcvBuffLen = kMinUDPBufSize; // Max length MacTCP can write
    pb.csParam.receive.timeOut = 1; // Short timeout for non-blocking check
    pb.csParam.receive.secondTimeStamp = 0;
    pb.csParam.receive.remoteHost = 0;
    pb.csParam.receive.remotePort = 0;

    // --- Call PBControlSync to attempt reading ---
    err = PBControlSync((ParmBlkPtr)&pb);

    // --- Handle Results ---
    if (err == noErr) {
        // --- Packet Received ---
        ip_addr senderIP = pb.csParam.receive.remoteHost;
        // Get the ACTUAL number of bytes received from the output parameter
        bytesReceived = pb.csParam.receive.rcvBuffLen;

        // Ignore messages from self
        if (senderIP == myLocalIP) {
            return;
        }

        // Convert sender IP (from header) to string using DNR
        err = AddrToStr(senderIP, senderIPStrFromHeader); // Use renamed buffer
        if (err != noErr) {
            log_message("Warning: AddrToStr failed for sender IP %lu (Error: %d). Using raw IP.", senderIP, err);
            sprintf(senderIPStrFromHeader, "%lu.%lu.%lu.%lu",
                    (senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF,
                    (senderIP >> 8) & 0xFF, senderIP & 0xFF);
        }

        // Parse the message content, passing the actual received length
        // parse_message now checks the magic number first
        if (parse_message(gUDPRecvBuffer, bytesReceived, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
            // Check if it's a discovery response
            if (strcmp(msgType, MSG_DISCOVERY_RESPONSE) == 0) {
                // Use the reliable IP from the header for adding/updating peer
                log_message("Received DISCOVERY_RESPONSE from %s@%s", senderUsername, senderIPStrFromHeader);
                int addResult = AddOrUpdatePeer(senderIPStrFromHeader, senderUsername);
                if (addResult < 0) {
                     log_message("Peer list full, could not add %s@%s", senderUsername, senderIPStrFromHeader);
                }
            }
            // Handle incoming MSG_DISCOVERY requests if needed
            else if (strcmp(msgType, MSG_DISCOVERY) == 0) {
                 log_message("Received DISCOVERY from %s@%s", senderUsername, senderIPStrFromHeader);
                 // TODO: Implement sending a DISCOVERY_RESPONSE back to the sender
                 // Need a SendUDPResponse function targeting pb.csParam.receive.remoteHost/Port
                 // Example: SendUDPResponse(macTCPRefNum, pb.csParam.receive.remoteHost, pb.csParam.receive.remotePort, gMyUsername, gMyLocalIPStr);
            }
            // else { log_message("Received UDP Msg Type: %s from %s", msgType, senderIPStrFromHeader); }

        } else {
            // Parse failed - could be invalid magic number or bad format after magic number
            // Log the failure safely to GUI/main log
            log_message("Discarding invalid/unknown UDP msg from %s (%u bytes).",
                        senderIPStrFromHeader, bytesReceived);

            // Log the raw buffer *only* to the file for debugging, printing hex
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

    } else if (err == commandTimeout || err == -23018 /* unofficial readTimeout? */) {
        // Timeout occurred - this is normal, no packet waiting
    } else {
        // Some other unexpected error occurred during read attempt
        log_message("Error (UDP Receive): PBControlSync(udpRead) failed. Error: %d", err);
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
    if (gUDPStream != NULL) {
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
            pb.csParam.create.rcvBuff = gUDPRecvBuffer;
            pb.csParam.create.rcvBuffLen = kMinUDPBufSize;

            err = PBControlSync((ParmBlkPtr)&pb);
            if (err != noErr) {
                log_message("Warning (CleanupUDP): PBControlSync(udpRelease) failed. Error: %d", err);
            } else {
                log_message("PBControlSync(udpRelease) succeeded.");
            }
        }
        gUDPStream = NULL; // Mark as released
    } else {
        log_message("UDP Endpoint was not open, skipping release.");
    }

    // --- Dispose UDP Receive Buffer ---
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    log_message("UDP Discovery Endpoint cleanup finished.");
}