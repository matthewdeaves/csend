// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h"   // For LogToDialog
#include "dialog.h"    // For gMyUsername
#include "protocol.h"  // For format_message, MSG_DISCOVERY

#include <Devices.h>   // For PBControlSync, PBOpenSync, PBCloseSync, TickCount, CntrlParam, ParmBlkPtr
#include <string.h>    // For memset, strlen, strcmp
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For NewPtrClear, DisposePtr, HLock, HUnlock, HSetState, HGetState

// --- Global Variable Definitions ---
short   gMacTCPRefNum = 0;    // Driver reference number for MacTCP
ip_addr gMyLocalIP = 0;       // Our local IP address (network byte order)
char    gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Our local IP as string
StreamPtr gUDPStream = NULL; // Pointer to our UDP stream for broadcasting (initialize to NULL)
Ptr     gUDPRecvBuffer = NULL; // Pointer to the buffer allocated for UDPCreate (initialize to NULL)
unsigned long gLastBroadcastTimeTicks = 0; // Time in Ticks

/**
 * @brief Initializes MacTCP networking (Driver, IP Address, DNR).
 * (No changes needed in this function)
 */
OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb;
    CntrlParam cntrlPB; // Use generic CntrlParam for ipctlGetAddr

    LogToDialog("Initializing Networking...");

    // --- Open MacTCP Driver ---
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm;
    LogToDialog("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        LogToDialog("Error: PBOpenSync failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum;
    LogToDialog("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // --- Get Local IP Address ---
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    LogToDialog("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        LogToDialog("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    LogToDialog("PBControlSync(ipctlGetAddr) succeeded.");
    // Correctly access the IP address from the parameter block
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0])); // csParam[0] holds the IP address

    // --- Initialize DNR FIRST ---
    LogToDialog("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        LogToDialog("Error: OpenResolver failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    } else {
        LogToDialog("OpenResolver succeeded.");
    }

    // --- Convert Local IP to String AFTER DNR is open ---
    LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         LogToDialog("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0 || gMyLocalIPStr[0] == '\0') {
             LogToDialog("Error: AddrToStr failed to get a valid IP string. Using default.");
             strcpy(gMyLocalIPStr, "127.0.0.1"); // Fallback if needed
         }
    } else {
        LogToDialog("AddrToStr finished. Result string: '%s'", gMyLocalIPStr);
    }

    LogToDialog("Networking initialization complete.");
    return noErr;
}


/**
 * @brief Initializes the UDP endpoint for discovery broadcasts using PBControl.
 * @details Matches the parameter block setup style from the example code.
 */
OSErr InitUDPBroadcastEndpoint(void) {
    OSErr err;
    UDPiopb pb; // Use the specific UDP parameter block structure
    const unsigned short specificPort = 0; // Request dynamic port assignment

    LogToDialog("Initializing UDP Broadcast Endpoint...");

    // Allocate buffer needed by UDPCreate, even if we don't plan to receive
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        LogToDialog("Error: Failed to allocate UDP receive buffer (memFullErr).");
        return memFullErr;
    }
    LogToDialog("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    // --- Prepare UDPiopb for UDPCreate (Matching example style) ---
    memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
    pb.ioCompletion = nil;           // No completion routine for sync call
    pb.ioCRefNum = gMacTCPRefNum;    // Driver reference number
    pb.csCode = udpCreate;           // Command code for creating UDP stream
    pb.udpStream = 0L;               // Output: Will be filled by MacTCP (Use 0L like example)

    // Access parameters via the csParam.create union structure
    pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pointer to allocated buffer
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Size of the buffer
    pb.csParam.create.notifyProc = nil;         // No notification routine needed
    pb.csParam.create.localPort = specificPort;  // Request dynamic port (0)
    // pb.csParam.create.userDataPtr = NULL; // Not explicitly set in example, covered by memset

    LogToDialog("Calling PBControlSync (udpCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpCreate) failed. Error: %d", err);
        if (gUDPRecvBuffer != NULL) {
            DisposePtr(gUDPRecvBuffer); // Clean up buffer if create failed
            gUDPRecvBuffer = NULL;
        }
        gUDPStream = 0L; // Use 0L like example
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
 * @details Matches the parameter block setup style from the example code.
 */
OSErr SendDiscoveryBroadcast(void) {
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2]; // Need 2 entries for the terminating zero entry
    UDPiopb pb; // Use the specific UDP parameter block structure

    if (gUDPStream == 0L) { // Check against 0L like example
        LogToDialog("Error: Cannot send broadcast, UDP endpoint not initialized.");
        return invalidStreamPtr; // Or another appropriate error
    }

    // Format the message using the shared protocol function
    err = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, gMyUsername, gMyLocalIPStr, "");
    if (err != 0) {
        LogToDialog("Error: Failed to format discovery broadcast message.");
        return paramErr; // Or another suitable error code
    }

    // Prepare the Write Data Structure (WDS)
    wds[0].length = strlen(buffer);
    wds[0].ptr = buffer;
    wds[1].length = 0; // Terminating entry
    wds[1].ptr = nil;  // Use nil like example

    // --- Prepare UDPiopb for UDPWrite (Matching example style) ---
    memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
    pb.ioCompletion = nil;           // No completion routine for sync call
    pb.ioCRefNum = gMacTCPRefNum;    // Driver reference number
    pb.csCode = udpWrite;            // Command code for writing UDP data
    pb.udpStream = gUDPStream;       // Input: The stream pointer obtained from UDPCreate

    // Access parameters via the csParam.send union structure
    pb.csParam.send.remoteHost = BROADCAST_IP;   // Destination IP (broadcast)
    pb.csParam.send.remotePort = PORT_UDP;       // Destination port
    pb.csParam.send.wdsPtr = (Ptr)wds;           // Pointer to the WDS array
    pb.csParam.send.checkSum = true;             // Calculate and send checksum
    // pb.csParam.send.userDataPtr = NULL; // Not explicitly set in example, covered by memset
    // pb.csParam.send.sendLength = 0; // Not used by UDPWrite according to docs

    // Call PBControlSync
    // LogToDialog("Sending broadcast: %s", buffer); // Debug log
    err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpWrite) failed. Error: %d", err);
        return err;
    }

    // LogToDialog("Discovery broadcast sent."); // Optional success log
    gLastBroadcastTimeTicks = TickCount(); // Update last broadcast time
    return noErr;
}

/**
 * @brief Checks if it's time to send the next discovery broadcast.
 * (No changes needed in this function)
 */
void CheckSendBroadcast(void) {
    unsigned long currentTimeTicks = TickCount();
    // DISCOVERY_INTERVAL is in seconds, TickCount() is 1/60th seconds
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;

    // Only check if the endpoint is initialized
    if (gUDPStream == 0L) return; // Check against 0L like example

    // Check for timer wraparound (unlikely but possible)
    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks; // Reset timer if wraparound detected
    }

    // Send immediately if never sent before, or if interval has passed
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        // LogToDialog("CheckSendBroadcast: Time to send."); // Debug log
        SendDiscoveryBroadcast(); // Ignore return value for simple periodic broadcast
    }
}


/**
 * @brief Cleans up networking resources (DNR, Driver, UDP Endpoint).
 * @details Matches the parameter block setup style from the example code.
 */
void CleanupNetworking(void) {
    UDPiopb pb; // Use the specific UDP parameter block structure
    OSErr err;

    LogToDialog("Cleaning up Networking...");

    // --- Release UDP Endpoint ---
    if (gUDPStream != 0L) { // Check against 0L like example
        LogToDialog("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)gUDPStream);
        memset(&pb, 0, sizeof(UDPiopb)); // Zero out the parameter block
        pb.ioCompletion = nil;           // No completion routine for sync call
        pb.ioCRefNum = gMacTCPRefNum;    // Driver reference number
        pb.csCode = udpRelease;          // Command code for releasing UDP stream
        pb.udpStream = gUDPStream;       // Input: The stream to release

        // Set the parameters exactly as in the example's Cleanup for UDPRelease
        pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Pass back original buffer pointer
        pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Pass back original buffer size

        err = PBControlSync((ParmBlkPtr)&pb); // Cast UDPiopb to generic ParmBlkPtr
        if (err != noErr) {
            LogToDialog("Warning: PBControlSync(udpRelease) failed. Error: %d", err);
        } else {
            LogToDialog("PBControlSync(udpRelease) succeeded.");
        }
        gUDPStream = 0L; // Mark as released even if call failed (Use 0L like example)
    } else {
        LogToDialog("UDP Endpoint was not open, skipping release.");
    }

    // --- Dispose UDP Receive Buffer ---
    // Always try to dispose the buffer if the pointer is not NULL,
    // even if UDPRelease failed, as the memory was allocated separately.
    if (gUDPRecvBuffer != nil) { // Use nil for Ptr check
         LogToDialog("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = nil; // Use nil for Ptr
    }

    // --- Close DNR ---
    LogToDialog("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        LogToDialog("Warning: CloseResolver failed. Error: %d", err);
    } else {
        LogToDialog("CloseResolver succeeded.");
    }

    // --- Close MacTCP Driver ---
    // DO NOT CALL PBCloseSync for the MacTCP driver (.IPP)
    if (gMacTCPRefNum != 0) {
         LogToDialog("MacTCP driver (RefNum: %d) remains open by design.", gMacTCPRefNum);
        gMacTCPRefNum = 0; // Still reset our global reference number variable
    } else {
        LogToDialog("MacTCP driver was not open.");
    }

    LogToDialog("Networking cleanup complete.");
}