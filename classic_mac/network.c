// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h"   // For LogToDialog
#include "discovery.h" // Include discovery.h to call CleanupUDPBroadcastEndpoint

#include <Devices.h>   // For PBControlSync, PBOpenSync, CntrlParam, ParmBlkPtr
#include <Errors.h>    // For error codes
#include <string.h>    // For memset, strlen, strcmp
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For memory management (though less needed here now)

// --- Global Variable Definitions ---
short   gMacTCPRefNum = 0;    // Driver reference number for MacTCP
ip_addr gMyLocalIP = 0;       // Our local IP address (network byte order)
char    gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Our local IP as string
// UDP globals moved to discovery.c

/**
 * @brief Initializes MacTCP networking (Driver, IP Address, DNR).
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
        // Don't close driver here, let caller handle cleanup
        gMacTCPRefNum = 0; // Reset ref num on error
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
        // Don't close driver here, let caller handle cleanup
        gMacTCPRefNum = 0; // Reset ref num on error
        return err;
    } else {
        LogToDialog("OpenResolver succeeded.");
    }

    // --- Convert Local IP to String AFTER DNR is open ---
    LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         LogToDialog("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         // Use a reasonable default if conversion fails badly
         if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0 || gMyLocalIPStr[0] == '\0' || gMyLocalIP == 0) {
             LogToDialog("Error: AddrToStr failed to get a valid IP string. Using fallback 127.0.0.1.");
             strcpy(gMyLocalIPStr, "127.0.0.1"); // Fallback if needed
             // Optionally return an error here if a valid IP is critical?
             // return err;
         }
    } else {
        LogToDialog("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }

    LogToDialog("Networking initialization complete.");
    return noErr;
}

// UDP Init/Send/Check functions moved to discovery.c

/**
 * @brief Cleans up general networking resources (DNR, Driver).
 *        Calls UDP cleanup internally.
 */
void CleanupNetworking(void) {
    OSErr err;

    LogToDialog("Cleaning up Networking...");

    // --- Clean up UDP Endpoint FIRST ---
    // Call the dedicated cleanup function from discovery.c
    CleanupUDPBroadcastEndpoint(gMacTCPRefNum);

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
    // It's a shared resource. Just reset our reference number.
    if (gMacTCPRefNum != 0) {
         LogToDialog("MacTCP driver (RefNum: %d) remains open by design.", gMacTCPRefNum);
        gMacTCPRefNum = 0; // Reset our global reference number variable
    } else {
        LogToDialog("MacTCP driver was not open.");
    }

    LogToDialog("Networking cleanup complete.");
}
