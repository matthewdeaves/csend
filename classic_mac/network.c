// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h"   // For LogToDialog
#include "dialog.h"    // For gMyUsername
#include "protocol.h"  // For format_message, MSG_DISCOVERY

#include <Devices.h>   // For PBControlSync, PBOpenSync, PBCloseSync, TickCount, CntrlParam, ParmBlkPtr
#include <string.h>    // For memset, strlen, strcmp
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For BlockMoveData if needed

// --- Global Variable Definitions ---
short   gMacTCPRefNum = 0;    // Driver reference number for MacTCP
ip_addr gMyLocalIP = 0;       // Our local IP address (network byte order)
char    gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Our local IP as string
unsigned long gLastBroadcastTimeTicks = 0; // Time in Ticks

// --- Static Global for UDP Endpoint ---
static StreamPtr sUDPEndpoint = NULL; // UDP Endpoint

/**
 * @brief Initializes MacTCP networking (Driver, IP Address, DNR).
 */
OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb;
    CntrlParam cntrlPB;

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
        // Don't call CleanupNetworking here, just mark driver refnum as invalid
        gMacTCPRefNum = 0;
        return err;
    }
    LogToDialog("PBControlSync(ipctlGetAddr) succeeded.");
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0]));

    // --- Initialize DNR FIRST ---
    LogToDialog("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        LogToDialog("Error: OpenResolver failed. Error: %d", err);
        // Don't call CleanupNetworking here, just mark driver refnum as invalid
        gMacTCPRefNum = 0;
        return err; // Return the OpenResolver error
    } else {
        LogToDialog("OpenResolver succeeded.");
    }

    // --- Convert Local IP to String AFTER DNR is open ---
    LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         // This might still return an error if the IP can't be resolved to a name
         // or if AddrToStr itself has issues, but it shouldn't be notOpenErr (-28) now.
         LogToDialog("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         // Decide if this is fatal or not. For now, we continue.
         // If the string is still 0.0.0.0, that's a problem for identifying the user.
         if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0) {
             LogToDialog("Error: AddrToStr failed to get a valid IP string.");
             // Potentially cleanup and return error here if IP string is critical
             // For now, let initialization succeed but log the error.
         }
    } else {
        LogToDialog("AddrToStr finished. Result string: '%s'", gMyLocalIPStr);
    }

    LogToDialog("Networking initialization complete.");
    return noErr; // Return noErr if we got this far
}


/**
 * @brief Initializes the UDP endpoint for discovery broadcasts using PBControl.
 */
OSErr InitUDPDiscovery(void) {
    OSErr err;
    CntrlParam udpPB;
    const unsigned short specificPort = 0; // Request dynamic port assignment

    LogToDialog("Initializing UDP Discovery Endpoint...");

    memset(&udpPB, 0, sizeof(CntrlParam));
    udpPB.ioCRefNum = gMacTCPRefNum;
    udpPB.csCode = udpCreate; // Use correct constant

    // Map parameters to csParam based on byte offsets from MacTCP Guide (relative to csCode start at byte 26)
    // csParam starts at byte 28. Offsets relative to csParam start:
    // Output StreamPtr: Offset 28 -> csParam[0] (long)
    // Input rcvBuff:    Offset 32 -> csParam[4] (long)
    // Input rcvBuffLen: Offset 36 -> csParam[8] (long)
    // Input notifyProc: Offset 40 -> csParam[12] (long)
    // Input localPort:  Offset 44 -> csParam[16] (word)
    // Input userDataPtr:Offset 46 -> csParam[18] (long)

    *((Ptr*)         (&udpPB.csParam[4])) = NULL;         // rcvBuff = NULL
    *((unsigned long*)(&udpPB.csParam[8])) = 0L;           // rcvBuffLen = 0 (No receive buffer needed for sending only)
    *((ProcPtr*)     (&udpPB.csParam[12])) = NULL;         // notifyProc = NULL
    *((udp_port*)    (&udpPB.csParam[16])) = specificPort; // localPort = 0 (request dynamic)
    *((Ptr*)         (&udpPB.csParam[18])) = NULL;         // userDataPtr = NULL

    LogToDialog("Calling PBControlSync (udpCreate) for port %u with rcvBufLen=0...", specificPort);
    err = PBControlSync((ParmBlkPtr)&udpPB);

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpCreate) failed. Error: %d", err);
        sUDPEndpoint = NULL;
        return err;
    }

    // Retrieve the output StreamPtr from csParam[0]
    sUDPEndpoint = *((StreamPtr*)(&udpPB.csParam[0]));
    // Retrieve the *actual* port assigned by MacTCP from csParam[16]
    unsigned short assignedPort = *((udp_port*)(&udpPB.csParam[16]));
    LogToDialog("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)sUDPEndpoint, assignedPort);
    gLastBroadcastTimeTicks = 0;
    return noErr;
}

/**
 * @brief Sends a UDP discovery broadcast message using PBControl.
 */
OSErr SendDiscoveryBroadcast(void) {
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2]; // Need 2 entries for the terminating zero entry
    CntrlParam udpPB;

    if (sUDPEndpoint == NULL) {
        LogToDialog("Error: Cannot send broadcast, UDP endpoint not initialized.");
        return invalidStreamPtr; // Use a more specific error if possible
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
    wds[1].ptr = NULL;

    // Prepare the Parameter Block for udpSend
    memset(&udpPB, 0, sizeof(CntrlParam));
    udpPB.ioCRefNum = gMacTCPRefNum;
    udpPB.csCode = udpSend; // Use correct constant

    // Map parameters to csParam based on byte offsets from MacTCP Guide (relative to csCode start at byte 26)
    // csParam starts at byte 28. Offsets relative to csParam start:
    // Input streamPtr:   Offset 28 -> csParam[0] (long)
    // Input remoteHost:  Offset 34 -> csParam[6] (long)
    // Input remotePort:  Offset 38 -> csParam[10] (word)
    // Input wdsPtr:      Offset 40 -> csParam[12] (long)
    // Input checkSum:    Offset 44 -> csParam[16] (byte) - Use byte access or careful word access
    // Input userDataPtr: Offset 48 -> csParam[20] (long)

    *((StreamPtr*)(&udpPB.csParam[0])) = sUDPEndpoint;   // streamPtr
    *((ip_addr*)  (&udpPB.csParam[6])) = BROADCAST_IP;   // remoteHost
    *((udp_port*) (&udpPB.csParam[10])) = PORT_UDP;      // remotePort
    *((Ptr*)      (&udpPB.csParam[12])) = (Ptr)wds;      // wdsPtr
    // Accessing byte at offset 44 (csParam[16] is word at 44, so access low byte)
    *((unsigned char*)(&udpPB.csParam[16])) = 1;         // checkSum = true
    *((Ptr*)      (&udpPB.csParam[20])) = NULL;         // userDataPtr = NULL

    // Call PBControlSync
    err = PBControlSync((ParmBlkPtr)&udpPB);

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpSend) failed. Error: %d", err);
        return err;
    }

    // LogToDialog("Discovery broadcast sent."); // Optional success log
    gLastBroadcastTimeTicks = TickCount(); // Update last broadcast time
    return noErr;
}

/**
 * @brief Checks if it's time to send the next discovery broadcast.
 */
void CheckSendBroadcast(void) {
    unsigned long currentTimeTicks = TickCount();
    // DISCOVERY_INTERVAL is in seconds, TickCount() is 1/60th seconds
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;

    // Only check if the endpoint is initialized
    if (sUDPEndpoint == NULL) return;

    // Send immediately if never sent before, or if interval has passed
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        SendDiscoveryBroadcast(); // Ignore return value for simple periodic broadcast
    }
}


/**
 * @brief Cleans up networking resources (DNR, Driver, UDP Endpoint).
 */
void CleanupNetworking(void) {
    // ParamBlockRec pb; // Not needed anymore as we don't close the driver
    CntrlParam udpPB;
    OSErr err;

    LogToDialog("Cleaning up Networking...");

    // --- Close DNR ---
    LogToDialog("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        LogToDialog("Warning: CloseResolver failed. Error: %d", err);
    } else {
        LogToDialog("CloseResolver succeeded.");
    }

    // --- Release UDP Endpoint ---
    if (sUDPEndpoint != NULL) {
        LogToDialog("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)sUDPEndpoint);
        memset(&udpPB, 0, sizeof(CntrlParam));
        udpPB.ioCRefNum = gMacTCPRefNum;
        udpPB.csCode = udpRelease; // Use correct constant

        // Map parameters to csParam based on byte offsets from MacTCP Guide (relative to csCode start at byte 26)
        // csParam starts at byte 28. Offsets relative to csParam start:
        // Input streamPtr:   Offset 28 -> csParam[0] (long)
        // Output rcvBuff:    Offset 32 -> csParam[4] (long) - Not used on input
        // Output rcvBuffLen: Offset 36 -> csParam[8] (long) - Not used on input
        // Input userDataPtr: Offset 44 -> csParam[16] (long) - Note: Guide says 46, but 44 fits better with structure

        *((StreamPtr*)(&udpPB.csParam[0])) = sUDPEndpoint; // streamPtr
        *((Ptr*)      (&udpPB.csParam[16])) = NULL;        // userDataPtr = NULL (Assuming offset 44)

        err = PBControlSync((ParmBlkPtr)&udpPB);
        if (err != noErr) {
            LogToDialog("Warning: PBControlSync(udpRelease) failed. Error: %d", err);
        } else {
            LogToDialog("PBControlSync(udpRelease) succeeded.");
        }
        sUDPEndpoint = NULL; // Mark as released even if call failed
    } else {
        LogToDialog("UDP Endpoint was not open, skipping release.");
    }

    // --- Close MacTCP Driver ---
    // DO NOT CALL PBCloseSync for the MacTCP driver (.IPP)
    if (gMacTCPRefNum != 0) {
         LogToDialog("MacTCP driver (RefNum: %d) remains open by design.", gMacTCPRefNum);
        // pb.ioParam.ioRefNum = gMacTCPRefNum;
        // err = PBCloseSync(&pb); // <-- REMOVED THIS CALL
        // if (err != noErr) {
        //     LogToDialog("Error: PBCloseSync failed. Error: %d", err);
        // } else {
        //     LogToDialog("PBCloseSync succeeded.");
        // }
        gMacTCPRefNum = 0; // Still reset our global reference number variable
    } else {
        LogToDialog("MacTCP driver was not open.");
    }

    LogToDialog("Networking cleanup complete.");
}