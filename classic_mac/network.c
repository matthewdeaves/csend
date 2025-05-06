//====================================
// FILE: ./classic_mac/network.c
//====================================

#include "network.h"
#include "logging.h"
#include "discovery.h"
#include "tcp.h"
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdlib.h>
#include <Memory.h>
#include <Events.h>

// External DNR function prototypes (from DNR.c or similar)
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);

// Globals
short gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
char gMyUsername[32] = "MacUser"; // Default username


OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pbOpen; // For PBOpen of the driver
    CntrlParam cntrlPB;   // For ipctlGetAddr
    
    log_message("Initializing Networking...");

    // Initialize the ParamBlockRec for PBOpen
    memset(&pbOpen, 0, sizeof(ParamBlockRec));
    pbOpen.ioParam.ioNamePtr = (StringPtr)kTCPDriverName; // MacTCP driver name: ".IPP"
    pbOpen.ioParam.ioPermssn = fsCurPerm; // Read/write permission (standard for drivers)

    log_message("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pbOpen); // Synchronously open the MacTCP driver
    if (err != noErr) {
        log_message("Error: PBOpenSync for MacTCP driver failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pbOpen.ioParam.ioRefNum; // Store the driver reference number
    log_message("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // Get local IP address
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr; // Control call to get IP address
    log_message("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB); // Synchronously get IP
    if (err != noErr) {
        log_message("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        // Note: Driver is open, but we failed to get IP. 
        // For robust cleanup, one might consider closing the driver here,
        // but since we plan to quit if this fails, system restart will handle it.
        gMacTCPRefNum = 0; // Invalidate refnum as networking is not fully up
        return err;
    }
    log_message("PBControlSync(ipctlGetAddr) succeeded.");
    BlockMoveData(&cntrlPB.csParam[0], &gMyLocalIP, sizeof(ip_addr)); // Copy IP address

    // Initialize DNR (Domain Name Resolver)
    log_message("Attempting OpenResolver...");
    err = OpenResolver(NULL); // Use default "Hosts" file
    if (err != noErr) {
        log_message("Error: OpenResolver failed. Error: %d", err);
        // No need to explicitly close driver here on failure if app exits
        gMacTCPRefNum = 0;
        return err;
    } else {
        log_message("OpenResolver succeeded.");
    }

    // Convert local IP to string
    log_message("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         log_message("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         // Fallback if AddrToStr fails or returns an unusable IP
         if (gMyLocalIP == 0 || gMyLocalIPStr[0] == '\0' || strcmp(gMyLocalIPStr, "0.0.0.0") == 0) {
             log_message("Error: AddrToStr failed to get a valid IP string. Using fallback 127.0.0.1 for display/formatting.");
             strcpy(gMyLocalIPStr, "127.0.0.1");
             if (gMyLocalIP == 0) { // If gMyLocalIP was also 0, parse the fallback
                ParseIPv4("127.0.0.1", &gMyLocalIP); 
             }
         }
    } else {
        log_message("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }

    // Initialize UDP Discovery
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
     if (err != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (%d). Cleaning up.", err);
        CloseResolver();
        // No driver close here
        gMacTCPRefNum = 0;
        return err;
    }

    // Initialize TCP
    err = InitTCP(gMacTCPRefNum);
    if (err != noErr) {
        log_message("Fatal: TCP initialization failed (%d). Cleaning up.", err);
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum); // Cleanup what was initialized
        CloseResolver();
        // No driver close here
        gMacTCPRefNum = 0;
        return err;
    }

    log_message("Networking initialization complete.");
    return noErr;
}

void CleanupNetworking(void) {
    OSErr err;
    // ParamBlockRec pbCloseDriver; // No longer needed for closing the driver

    log_message("Cleaning up Networking (Streams, DNR, Driver)...");

    // Cleanup TCP and UDP streams first
    CleanupTCP(gMacTCPRefNum);
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);

    // Close DNR
    log_message("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        log_message("Warning: CloseResolver failed. Error: %d", err);
    } else {
        log_message("CloseResolver succeeded.");
    }

    // Do NOT close the MacTCP driver itself. It's a shared system resource.
    // The PBOpen call only gets a reference to it.
    if (gMacTCPRefNum != 0) {
         log_message("MacTCP driver (RefNum: %d) was opened by this application. It will remain open for the system.", gMacTCPRefNum);
        // memset(&pbCloseDriver, 0, sizeof(ParamBlockRec));
        // pbCloseDriver.ioParam.ioRefNum = gMacTCPRefNum;
        // log_message("Attempting PBCloseSync for MacTCP driver (RefNum: %d)...", gMacTCPRefNum);
        // err = PBCloseSync(&pbCloseDriver); // THIS IS THE LINE TO REMOVE / COMMENT OUT
        // if (err != noErr) {
        //     log_message("Warning: PBCloseSync failed for MacTCP driver. Error: %d. This is problematic.", err);
        // } else {
        //     log_message("MacTCP driver (RefNum: %d) closed via PBCloseSync.", gMacTCPRefNum);
        // }
        gMacTCPRefNum = 0; // Invalidate our reference
    } else {
        log_message("MacTCP driver was not opened by this application or already cleaned up.");
    }
    
    log_message("Networking cleanup complete.");
}

// YieldTimeToSystem: Gives time back to the OS, allowing other processes
// (including MacTCP background tasks) to run.
void YieldTimeToSystem(void) {
    EventRecord event; // Dummy event record
    // Minimal wait to cede time. The 1L is a short duration.
    // We're not actually interested in the event, just yielding.
    WaitNextEvent(0, &event, 1L, NULL); 
}


// ParseIPv4: Converts a dotted-decimal IP string to a 32-bit MacTCP ip_addr.
// Note: MacTCP ip_addr is typically network byte order, but since we get it
// from MacTCP calls and pass it back to MacTCP calls, we generally don't
// need to swap bytes for MacTCP's own use. However, if displaying parts
// or comparing with standard host order numbers, be mindful.
// This function produces a host-order equivalent from the string for internal logic,
// which is then implicitly treated as network order by MacTCP functions.
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr) {
    unsigned long parts[4];
    int i = 0;
    char *token;
    char *rest_of_string; // Renamed to avoid conflict with standard 'rest'
    char buffer[INET_ADDRSTRLEN + 1]; // +1 for null terminator

    if (ip_str == NULL || out_addr == NULL) {
        return paramErr;
    }

    // Make a writable copy of the IP string for strtok_r
    strncpy(buffer, ip_str, INET_ADDRSTRLEN);
    buffer[INET_ADDRSTRLEN] = '\0'; // Ensure null termination

    rest_of_string = buffer;
    while ((token = strtok_r(rest_of_string, ".", &rest_of_string)) != NULL && i < 4) {
        char *endptr;
        parts[i] = strtoul(token, &endptr, 10); // Base 10
        
        // Check for conversion errors or out-of-range values
        if (*endptr != '\0' || parts[i] > 255) {
            log_message("ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0; // Indicate error
            return paramErr; 
        }
        i++;
    }

    if (i != 4) {
        log_message("ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0; // Indicate error
        return paramErr;
    }

    // Assemble the ip_addr in network byte order (big-endian)
    // which is MacTCP's native format for ip_addr.
    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr;
}
