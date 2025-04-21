// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h"   // For log_message
#include "discovery.h" // Include discovery.h to call CleanupUDPDiscoveryEndpoint // <-- Updated comment

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

    log_message("Initializing Networking...");

    // --- Open MacTCP Driver ---
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm;
    log_message("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        log_message("Error: PBOpenSync failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum;
    log_message("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // --- Get Local IP Address ---
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    log_message("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_message("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        // Don't close driver here, let caller handle cleanup
        gMacTCPRefNum = 0; // Reset ref num on error
        return err;
    }
    log_message("PBControlSync(ipctlGetAddr) succeeded.");
    // Correctly access the IP address from the parameter block
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0])); // csParam[0] holds the IP address

    // --- Initialize DNR FIRST ---
    log_message("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_message("Error: OpenResolver failed. Error: %d", err);
        // Don't close driver here, let caller handle cleanup
        gMacTCPRefNum = 0; // Reset ref num on error
        return err;
    } else {
        log_message("OpenResolver succeeded.");
    }

    // --- Convert Local IP to String AFTER DNR is open ---
    log_message("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         log_message("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         // Use a reasonable default if conversion fails badly
         if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0 || gMyLocalIPStr[0] == '\0' || gMyLocalIP == 0) {
             log_message("Error: AddrToStr failed to get a valid IP string. Using fallback 127.0.0.1.");
             strcpy(gMyLocalIPStr, "127.0.0.1"); // Fallback if needed
             // Optionally return an error here if a valid IP is critical?
             // return err;
         }
    } else {
        log_message("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }

    log_message("Networking initialization complete.");
    return noErr;
}

// UDP Init/Send/Check functions moved to discovery.c

/**
 * @brief Cleans up general networking resources (DNR, Driver).
 *        Calls UDP cleanup internally.
 */
void CleanupNetworking(void) {
    OSErr err;

    log_message("Cleaning up Networking...");

    // --- Clean up UDP Endpoint FIRST ---
    // Call the dedicated cleanup function from discovery.c
    // Use the RENAMED function here:
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);

    // --- Close DNR ---
    log_message("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        log_message("Warning: CloseResolver failed. Error: %d", err);
    } else {
        log_message("CloseResolver succeeded.");
    }

    // --- Close MacTCP Driver ---
    // DO NOT CALL PBCloseSync for the MacTCP driver (.IPP)
    // It's a shared resource. Just reset our reference number.
    if (gMacTCPRefNum != 0) {
         log_message("MacTCP driver (RefNum: %d) remains open by design.", gMacTCPRefNum);
        gMacTCPRefNum = 0; // Reset our global reference number variable
    } else {
        log_message("MacTCP driver was not open.");
    }

    log_message("Networking cleanup complete.");
}