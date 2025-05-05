//====================================
// FILE: ./classic_mac/network.c
//====================================

#include "network.h"
#include "logging.h"
#include "discovery.h" // For Init/Cleanup UDP
#include "tcp.h"       // For InitTCP, CleanupTCP
#include <Devices.h>   // For PBOpenSync, PBCloseSync, PBControlSync
#include <Errors.h>
#include <string.h>    // For memset, strcpy, strncpy, strtok_r
#include <stdlib.h>    // For strtoul
#include <Memory.h>    // For NewPtrClear, DisposePtr
#include <Events.h>    // For WaitNextEvent

// DNR Function Prototypes (assuming DNR.c provides these)
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);

short gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
char gMyUsername[32] = "MacUser"; // Default username

OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb;
    CntrlParam cntrlPB;

    log_message("Initializing Networking...");

    // 1. Open MacTCP Driver
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm; // Read/write permission
    log_message("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        log_message("Error: PBOpenSync failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum;
    log_message("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // 2. Get Local IP Address
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    log_message("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_message("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        PBCloseSync(&pb); // Close driver on error
        gMacTCPRefNum = 0;
        return err;
    }
    log_message("PBControlSync(ipctlGetAddr) succeeded.");
    // Extract the IP address from the control parameter block
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0]));

    // 3. Initialize DNR (Domain Name Resolver)
    log_message("Attempting OpenResolver...");
    err = OpenResolver(NULL); // Pass NULL for default resolver configuration
    if (err != noErr) {
        log_message("Error: OpenResolver failed. Error: %d", err);
        PBCloseSync(&pb); // Close driver on error
        gMacTCPRefNum = 0;
        return err;
    } else {
        log_message("OpenResolver succeeded.");
    }

    // 4. Convert Local IP to String using DNR
    log_message("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         // Log warning but don't necessarily fail; maybe IP is 0.0.0.0 initially
         log_message("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         // Fallback if IP is 0 or string conversion failed badly
         if (gMyLocalIP == 0 || gMyLocalIPStr[0] == '\0' || strcmp(gMyLocalIPStr, "0.0.0.0") == 0) {
             log_message("Error: AddrToStr failed to get a valid IP string. Using fallback 127.0.0.1 for display/formatting.");
             strcpy(gMyLocalIPStr, "127.0.0.1");
             // Optionally, try parsing 127.0.0.1 back to gMyLocalIP if gMyLocalIP was 0
             if (gMyLocalIP == 0) {
                ParseIPv4("127.0.0.1", &gMyLocalIP);
             }
         }
    } else {
        log_message("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }

    // 5. Initialize UDP Discovery Endpoint
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
     if (err != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (%d). Cleaning up.", err);
        CloseResolver();
        PBCloseSync(&pb);
        gMacTCPRefNum = 0;
        return err;
    }

    // 6. Initialize TCP Listener and Sender Streams
    err = InitTCP(gMacTCPRefNum); // <<< UPDATED CALL
    if (err != noErr) {
        log_message("Fatal: TCP initialization failed (%d). Cleaning up.", err);
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum); // Clean up UDP if TCP fails
        CloseResolver();
        PBCloseSync(&pb);
        gMacTCPRefNum = 0;
        return err;
    }

    log_message("Networking initialization complete.");
    return noErr;
}

void CleanupNetworking(void) {
    OSErr err;
    ParamBlockRec pb;

    log_message("Cleaning up Networking (Streams, DNR, Driver)...");

    // 1. Cleanup TCP Listener and Sender Streams
    CleanupTCP(gMacTCPRefNum); // <<< UPDATED CALL

    // 2. Cleanup UDP Endpoint (handles its own stream release)
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);

    // 3. Close DNR
    log_message("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        log_message("Warning: CloseResolver failed. Error: %d", err);
    } else {
        log_message("CloseResolver succeeded.");
    }

    // 4. Close MacTCP Driver
    if (gMacTCPRefNum != 0) {
         log_message("Closing MacTCP driver (RefNum: %d)...", gMacTCPRefNum);
         pb.ioParam.ioRefNum = gMacTCPRefNum;
         err = PBCloseSync(&pb);
         if (err != noErr) {
             // This error (-24, fcbNotFound) is common if streams weren't released properly,
             // but we attempted release in the cleanup functions. Log it but continue.
             log_message("Warning: PBCloseSync failed for MacTCP driver. Error: %d", err);
         } else {
             log_message("MacTCP driver closed.");
         }
        gMacTCPRefNum = 0; // Mark as closed regardless of error
    } else {
        log_message("MacTCP driver was not open.");
    }

    log_message("Networking cleanup complete.");
}

// Basic yield function for cooperative multitasking
void YieldTimeToSystem(void) {
    EventRecord event; // Dummy event record
    // WaitNextEvent with minimal sleep allows other processes (including MacTCP background tasks) to run.
    // Using 0 for sleep can starve other processes on some systems, 1 is safer.
    WaitNextEvent(0, &event, 1L, NULL);
}

// Helper to parse IPv4 string "a.b.c.d" into network byte order ip_addr
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr) {
    unsigned long parts[4];
    int i = 0;
    char *token;
    char *rest;
    char buffer[INET_ADDRSTRLEN + 1]; // +1 for null terminator

    if (ip_str == NULL || out_addr == NULL) {
        return paramErr;
    }

    // Copy to a temporary buffer because strtok_r modifies the string
    strncpy(buffer, ip_str, INET_ADDRSTRLEN);
    buffer[INET_ADDRSTRLEN] = '\0'; // Ensure null termination

    rest = buffer; // Initialize rest for strtok_r

    // Parse up to 4 parts separated by '.'
    while ((token = strtok_r(rest, ".", &rest)) != NULL && i < 4) {
        char *endptr;
        parts[i] = strtoul(token, &endptr, 10); // Base 10 conversion

        // Check for conversion errors or out-of-range values
        if (*endptr != '\0' || parts[i] > 255) {
            log_message("ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0;
            return paramErr; // Return error code
        }
        i++;
    }

    // Check if exactly 4 parts were found
    if (i != 4) {
        log_message("ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0;
        return paramErr; // Return error code
    }

    // Combine parts into a 32-bit ip_addr (network byte order assumed by MacTCP)
    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr; // Success
}