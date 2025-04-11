// FILE: ./classic_mac/discovery.c
#include "discovery.h"
#include "logging.h"   // For LogToDialog
#include "protocol.h"  // For format_message, MSG_DISCOVERY

#include <Devices.h>   // For PBControlSync, TickCount, CntrlParam, ParmBlkPtr
#include <Errors.h>    // Include for error codes like memFullErr, invalidStreamPtr etc.
#include <string.h>    // For memset, strlen
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For NewPtrClear, DisposePtr

// --- Global Variable Definitions ---
StreamPtr gUDPStream = NULL; // Pointer to our UDP stream for broadcasting (initialize to NULL)
Ptr     gUDPRecvBuffer = NULL; // Pointer to the buffer allocated for UDPCreate (initialize to NULL)
unsigned long gLastBroadcastTimeTicks = 0; // Time in Ticks

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts using PBControl.
 */
OSErr InitUDPBroadcastEndpoint(short macTCPRefNum) {
    OSErr err;
    UDPiopb pb; // Use the specific UDP parameter block structure
    const unsigned short specificPort = 0; // Request dynamic port assignment

    LogToDialog("Initializing UDP Broadcast Endpoint...");

    if (macTCPRefNum == 0) {
         LogToDialog("Error (InitUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr; // Or another appropriate error
    }

    // Allocate buffer needed by UDPCreate, even if we don't plan to receive
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        LogToDialog("Error (InitUDP): Failed to allocate UDP receive buffer (memFullErr).");
        return memFullErr;
    }
    LogToDialog("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

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
    pb.csParam.create.localPort = specificPort;  // Request dynamic port (0)

    LogToDialog("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        LogToDialog("Error (InitUDP): PBControlSync(udpCreate) failed. Error: %d", err);
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
    LogToDialog("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);
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

    if (gUDPStream == NULL) {
        LogToDialog("Error (SendUDP): Cannot send broadcast, UDP endpoint not initialized.");
        return invalidStreamPtr; // Or another appropriate error
    }
     if (macTCPRefNum == 0) {
         LogToDialog("Error (SendUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
     }
     if (myUsername == NULL || myLocalIPStr == NULL) {
         LogToDialog("Error (SendUDP): Missing username or IP string.");
         return paramErr;
     }

    // Format the message using the shared protocol function
    err = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, myUsername, myLocalIPStr, "");
    if (err != 0) {
        LogToDialog("Error (SendUDP): Failed to format discovery broadcast message.");
        return paramErr; // Or another suitable error code
    }

    // Prepare the Write Data Structure (WDS)
    wds[0].length = strlen(buffer);
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
    // LogToDialog("Sending broadcast: %s", buffer); // Debug log
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        LogToDialog("Error (SendUDP): PBControlSync(udpWrite) failed. Error: %d", err);
        return err;
    }

    // LogToDialog("Discovery broadcast sent."); // Optional success log
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
        // LogToDialog("CheckSendBroadcast: Time to send."); // Debug log
        SendDiscoveryBroadcast(macTCPRefNum, myUsername, myLocalIPStr); // Ignore return value for simple periodic broadcast
    }
}

/**
 * @brief Cleans up the UDP broadcast endpoint resources.
 */
void CleanupUDPBroadcastEndpoint(short macTCPRefNum) {
    UDPiopb pb; // Use the specific UDP parameter block structure
    OSErr err;

    LogToDialog("Cleaning up UDP Broadcast Endpoint...");

    // --- Release UDP Endpoint ---
    if (gUDPStream != NULL) {
        if (macTCPRefNum == 0) {
             LogToDialog("Warning (CleanupUDP): Invalid MacTCP RefNum (%d), cannot release UDP stream.", macTCPRefNum);
        } else {
            LogToDialog("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)gUDPStream);
            memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
            pb.ioCompletion = nil;           // No completion routine for sync call
            pb.ioCRefNum = macTCPRefNum;     // Use passed-in Driver reference number
            pb.csCode = udpRelease;          // Command code for releasing UDP stream
            pb.udpStream = gUDPStream;       // Input: The stream to release

            // Set the parameters required by UDPRelease
            pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pass back original buffer pointer
            pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Pass back original buffer size

            err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr
            if (err != noErr) {
                LogToDialog("Warning (CleanupUDP): PBControlSync(udpRelease) failed. Error: %d", err);
                // Continue cleanup even if release fails
            } else {
                LogToDialog("PBControlSync(udpRelease) succeeded.");
            }
        }
        gUDPStream = NULL; // Mark as released (or intended to be released)
    } else {
        LogToDialog("UDP Endpoint was not open, skipping release.");
    }

    // --- Dispose UDP Receive Buffer ---
    // Always try to dispose the buffer if the pointer is not NULL,
    // as the memory was allocated separately.
    if (gUDPRecvBuffer != NULL) {
         LogToDialog("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    LogToDialog("UDP Broadcast Endpoint cleanup finished.");
}
