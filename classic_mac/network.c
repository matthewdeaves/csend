// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h" // For LogToDialog

#include <Devices.h>   // For PBControlSync, PBOpenSync, PBCloseSync
#include <string.h>    // For memset

// --- Global Variable Definitions ---
short   gMacTCPRefNum = 0;    // Driver reference number for MacTCP
ip_addr gMyLocalIP = 0;       // Our local IP address (network byte order)
char    gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Our local IP as string

/**
 * @brief Initializes MacTCP networking.
 */
OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb; // Use ParamBlockRec for PBOpenSync/PBCloseSync
    CntrlParam cntrlPB; // Use CntrlParam for PBControlSync

    LogToDialog("Initializing Networking...");

    // --- Open MacTCP Driver ---
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm; // Request current permission
    LogToDialog("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        LogToDialog("Error: PBOpenSync failed. Error: %d", err);
        gMacTCPRefNum = 0; // Ensure ref num is 0 on failure
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum; // Store the driver reference number
    LogToDialog("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // --- Get Local IP Address ---
    memset(&cntrlPB, 0, sizeof(CntrlParam)); // Zero out the control parameter block
    cntrlPB.ioCRefNum = gMacTCPRefNum;       // Set the driver reference number
    cntrlPB.csCode = ipctlGetAddr;           // Set the control code to get IP address
    LogToDialog("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB); // Cast CntrlParam to generic ParmBlkPtr
    if (err != noErr) {
        LogToDialog("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        // Close the driver if getting the address failed
        CleanupNetworking(); // Call cleanup to close the driver
        return err;
    }
    LogToDialog("PBControlSync(ipctlGetAddr) succeeded.");

    // Extract the IP address from the parameter block's csParam field
    // The IP address is stored starting at csParam1 (offset 32 in CntrlParam)
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0]));

    // --- Convert Local IP to String ---
    LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    // Use the DNR function to convert the binary IP to dotted-decimal string
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         // Although AddrToStr is declared to return OSErr, the DNR.c implementation
         // doesn't actually return errors for this specific call.
         // We'll log anyway, but likely it always "succeeds".
         LogToDialog("Warning: AddrToStr returned error %d (might be harmless). Result string: '%s'", err, gMyLocalIPStr);
         // Continue even if AddrToStr reports an error, as gMyLocalIPStr might still be usable.
    } else {
        LogToDialog("AddrToStr finished. Result string: '%s'", gMyLocalIPStr);
    }


    // --- Initialize DNR ---
    // Note: DNR.c's OpenResolver/CloseResolver manage their own state.
    // We need to call OpenResolver here.
    LogToDialog("Attempting OpenResolver...");
    err = OpenResolver(NULL); // Use default Hosts file path
    if (err != noErr) {
        LogToDialog("Error: OpenResolver failed. Error: %d", err);
        // This might not be fatal, depending on whether DNS is needed.
        // Continue for now, but log the error.
        // Consider closing the driver if DNR is essential?
    } else {
        LogToDialog("OpenResolver succeeded.");
    }


    // TODO: Initialize UDP socket for discovery later
    // TODO: Initialize TCP listener socket later

    LogToDialog("Networking initialization complete.");
    return noErr; // Return success
}

/**
 * @brief Cleans up networking resources.
 */
void CleanupNetworking(void) {
    ParamBlockRec pb; // Use ParamBlockRec for PBCloseSync
    OSErr err;

    LogToDialog("Cleaning up Networking...");

    // --- Close DNR ---
    // Call CloseResolver before closing the driver.
    LogToDialog("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        LogToDialog("Warning: CloseResolver failed. Error: %d", err);
        // Continue cleanup even if CloseResolver fails.
    } else {
        LogToDialog("CloseResolver succeeded.");
    }


    // --- Close MacTCP Driver ---
    if (gMacTCPRefNum != 0) {
        LogToDialog("Attempting PBCloseSync for driver RefNum: %d...", gMacTCPRefNum);
        pb.ioParam.ioRefNum = gMacTCPRefNum;
        err = PBCloseSync(&pb);
        if (err != noErr) {
            // Log the error, but we can't do much else at this point.
            LogToDialog("Error: PBCloseSync failed. Error: %d", err);
        } else {
            LogToDialog("PBCloseSync succeeded.");
        }
        gMacTCPRefNum = 0; // Mark driver as closed
    } else {
        LogToDialog("MacTCP driver was not open, skipping PBCloseSync.");
    }

    // TODO: Close UDP/TCP sockets later

    LogToDialog("Networking cleanup complete.");
}