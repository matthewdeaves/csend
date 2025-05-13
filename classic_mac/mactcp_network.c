#include "mactcp_network.h"
#include "logging.h"
#include "../shared/logging.h"
#include "mactcp_discovery.h"
#include "mactcp_messaging.h"
#include "common_defs.h"
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdlib.h>
#include <Memory.h>
#include <Events.h>
#include <Resources.h>
#include <OSUtils.h>
#include <MixedMode.h>
#ifndef kTCPDriverName
const unsigned char kTCPDriverName[] = "\p.IPP";
#endif
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);
short gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
char gMyUsername[GLOBAL_USERNAME_BUFFER_SIZE] = "MacUser";
static TCPNotifyUPP gTCPASR_UPP = NULL;
OSErr InitializeNetworking(void)
{
    OSErr err;
    ParamBlockRec pbOpen;
    CntrlParam cntrlPB;
    unsigned long tcpStreamBufferSize;
    log_debug("Initializing Networking...");
    memset(&pbOpen, 0, sizeof(ParamBlockRec));
    pbOpen.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pbOpen.ioParam.ioPermssn = fsCurPerm;
    log_debug("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pbOpen);
    if (err != noErr) {
        log_app_event("Fatal Error: PBOpenSync for MacTCP driver failed. Error: %d. MacTCP cannot be used.", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pbOpen.ioParam.ioRefNum;
    log_debug("PBOpenSync succeeded (MacTCP RefNum: %d). Driver is now open for system use.", gMacTCPRefNum);
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    log_debug("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_app_event("Error: PBControlSync(ipctlGetAddr) failed. Error: %d. Cannot determine local IP.", err);
        gMacTCPRefNum = 0;
        return err;
    }
    log_debug("PBControlSync(ipctlGetAddr) succeeded.");
    BlockMoveData(&cntrlPB.csParam[0], &gMyLocalIP, sizeof(ip_addr));
    log_debug("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_app_event("Error: OpenResolver failed. Error: %d. DNS resolution will not work.", err);
        gMacTCPRefNum = 0;
        return err;
    }
    log_debug("OpenResolver succeeded.");
    log_debug("Attempting AddrToStr for IP: %lu (0x%lX)...", gMyLocalIP, gMyLocalIP);
    err = AddrToStr(gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
        log_debug("Warning: AddrToStr returned error %d for IP %lu. Result IP string: '%s'", err, gMyLocalIP, gMyLocalIPStr);
        if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0 || gMyLocalIPStr[0] == '\0') {
            log_app_event("Warning/Error: AddrToStr suggests local IP is 0.0.0.0 or DNR could not convert. Connectivity may fail.");
        }
    } else {
        log_debug("AddrToStr finished. Local IP: '%s'", gMyLocalIPStr);
    }
    if (gMyLocalIP == 0) {
        log_app_event("Critical Warning: Local IP address is 0.0.0.0. Check MacTCP configuration. Application may not function correctly.");
    }
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (err != noErr) {
        log_app_event("Fatal: UDP Discovery initialization failed (%d).", err);
        CloseResolver();
        gMacTCPRefNum = 0;
        return err;
    }
    log_debug("UDP Discovery Endpoint Initialized.");
    tcpStreamBufferSize = PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE;
    if (tcpStreamBufferSize < MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE) {
        tcpStreamBufferSize = MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE;
    }
    log_debug("Initializing TCP with stream receive buffer size: %lu bytes.", tcpStreamBufferSize);
    if (gTCPASR_UPP == NULL) {
        gTCPASR_UPP = NewTCPNotifyUPP(TCP_ASR_Handler);
        if (gTCPASR_UPP == NULL) {
            log_app_event("Fatal: Failed to create UPP for TCP_ASR_Handler.");
            CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
            CloseResolver();
            gMacTCPRefNum = 0;
            return memFullErr;
        }
        log_debug("TCP ASR UPP created at 0x%lX.", (unsigned long)gTCPASR_UPP);
    }
    err = InitTCP(gMacTCPRefNum, tcpStreamBufferSize, gTCPASR_UPP);
    if (err != noErr) {
        log_app_event("Fatal: TCP messaging initialization failed (%d).", err);
        if (gTCPASR_UPP != NULL) {
            log_debug("Disposing TCP ASR UPP due to InitTCP failure.");
            DisposeRoutineDescriptor(gTCPASR_UPP);
            gTCPASR_UPP = NULL;
        }
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
        CloseResolver();
        gMacTCPRefNum = 0;
        return err;
    }
    log_debug("TCP Messaging Initialized.");
    log_app_event("Networking initialization complete. Local IP: %s", gMyLocalIPStr);
    return noErr;
}
void CleanupNetworking(void)
{
    OSErr err;
    log_app_event("Cleaning up Networking...");
    CleanupTCP(gMacTCPRefNum);
    log_debug("TCP Messaging Cleaned up.");
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
    log_debug("UDP Discovery Cleaned up.");
    if (gTCPASR_UPP != NULL) {
        log_debug("Disposing TCP ASR UPP at 0x%lX.", (unsigned long)gTCPASR_UPP);
        DisposeRoutineDescriptor(gTCPASR_UPP);
        gTCPASR_UPP = NULL;
    }
    log_debug("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        log_debug("Warning: CloseResolver failed. Error: %d", err);
    } else {
        log_debug("CloseResolver succeeded.");
    }
    if (gMacTCPRefNum != 0) {
        log_debug("Application releasing its use of MacTCP driver (RefNum: %d). Driver remains open in system.", gMacTCPRefNum);
        gMacTCPRefNum = 0;
    } else {
        log_debug("MacTCP driver was not actively used by this application instance or already marked as released by app.");
    }
    gMyLocalIP = 0;
    gMyLocalIPStr[0] = '\0';
    log_app_event("Networking cleanup complete.");
}
void YieldTimeToSystem(void)
{
    EventRecord event;
    (void)WaitNextEvent(0, &event, 1L, NULL);
}
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr)
{
    unsigned long parts[4];
    int i = 0;
    char *token;
    char *rest_of_string;
    char buffer[INET_ADDRSTRLEN];
    if (ip_str == NULL || out_addr == NULL) {
        return paramErr;
    }
    strncpy(buffer, ip_str, INET_ADDRSTRLEN - 1);
    buffer[INET_ADDRSTRLEN - 1] = '\0';
    rest_of_string = buffer;
    while ((token = strtok_r(rest_of_string, ".", &rest_of_string)) != NULL && i < 4) {
        char *endptr;
        parts[i] = strtoul(token, &endptr, 10);
        if (*endptr != '\0' || parts[i] > 255) {
            log_debug("ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0;
            return paramErr;
        }
        i++;
    }
    if (i != 4) {
        log_debug("ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0;
        return paramErr;
    }
    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr;
}
