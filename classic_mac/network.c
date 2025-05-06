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
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);
short gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
char gMyUsername[32] = "MacUser";
OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb;
    CntrlParam cntrlPB;
    log_message("Initializing Networking...");
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
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    log_message("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_message("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        PBCloseSync(&pb);
        gMacTCPRefNum = 0;
        return err;
    }
    log_message("PBControlSync(ipctlGetAddr) succeeded.");
    BlockMoveData(&cntrlPB.csParam[0], &gMyLocalIP, sizeof(ip_addr));
    log_message("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_message("Error: OpenResolver failed. Error: %d", err);
        PBCloseSync(&pb);
        gMacTCPRefNum = 0;
        return err;
    } else {
        log_message("OpenResolver succeeded.");
    }
    log_message("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
         log_message("Warning: AddrToStr returned error %d. Result string: '%s'", err, gMyLocalIPStr);
         if (gMyLocalIP == 0 || gMyLocalIPStr[0] == '\0' || strcmp(gMyLocalIPStr, "0.0.0.0") == 0) {
             log_message("Error: AddrToStr failed to get a valid IP string. Using fallback 127.0.0.1 for display/formatting.");
             strcpy(gMyLocalIPStr, "127.0.0.1");
             if (gMyLocalIP == 0) {
                ParseIPv4("127.0.0.1", &gMyLocalIP);
             }
         }
    } else {
        log_message("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
     if (err != noErr) {
        log_message("Fatal: UDP Discovery initialization failed (%d). Cleaning up.", err);
        CloseResolver();
        PBCloseSync(&pb);
        gMacTCPRefNum = 0;
        return err;
    }
    err = InitTCP(gMacTCPRefNum);
    if (err != noErr) {
        log_message("Fatal: TCP initialization failed (%d). Cleaning up.", err);
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
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
    CleanupTCP(gMacTCPRefNum);
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
    log_message("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        log_message("Warning: CloseResolver failed. Error: %d", err);
    } else {
        log_message("CloseResolver succeeded.");
    }
    if (gMacTCPRefNum != 0) {
         log_message("Closing MacTCP driver (RefNum: %d)...", gMacTCPRefNum);
         pb.ioParam.ioRefNum = gMacTCPRefNum;
         err = PBCloseSync(&pb);
         if (err != noErr) {
             log_message("Warning: PBCloseSync failed for MacTCP driver. Error: %d", err);
         } else {
             log_message("MacTCP driver closed.");
         }
        gMacTCPRefNum = 0;
    } else {
        log_message("MacTCP driver was not open.");
    }
    log_message("Networking cleanup complete.");
}
void YieldTimeToSystem(void) {
    EventRecord event;
    WaitNextEvent(0, &event, 1L, NULL);
}
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr) {
    unsigned long parts[4];
    int i = 0;
    char *token;
    char *rest;
    char buffer[INET_ADDRSTRLEN + 1];
    if (ip_str == NULL || out_addr == NULL) {
        return paramErr;
    }
    strncpy(buffer, ip_str, INET_ADDRSTRLEN);
    buffer[INET_ADDRSTRLEN] = '\0';
    rest = buffer;
    while ((token = strtok_r(rest, ".", &rest)) != NULL && i < 4) {
        char *endptr;
        parts[i] = strtoul(token, &endptr, 10);
        if (*endptr != '\0' || parts[i] > 255) {
            log_message("ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0;
            return paramErr;
        }
        i++;
    }
    if (i != 4) {
        log_message("ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0;
        return paramErr;
    }
    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr;
}
