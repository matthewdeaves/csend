//====================================
// FILE: ./classic_mac/network_init.c
//====================================

#include "network_init.h"
#include "network_abstraction.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "discovery.h"
#include "messaging.h"
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

/* Define MacTCP driver name here */
const unsigned char kTCPDriverName[] = "\p.IPP";

extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);

short gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
char gMyUsername[GLOBAL_USERNAME_BUFFER_SIZE] = "MacUser";

/* Separate UPPs for listen and send streams */
static TCPNotifyUPP gTCPListenASR_UPP = NULL;
static TCPNotifyUPP gTCPSendASR_UPP = NULL;

OSErr InitializeNetworking(void)
{
    OSErr err;
    unsigned long tcpStreamBufferSize;

    log_info_cat(LOG_CAT_NETWORKING, "InitializeNetworking: Starting network initialization");

    /* Initialize the network abstraction layer */
    err = InitNetworkAbstraction();
    if (err != noErr) {
        log_app_event("Fatal Error: Failed to initialize network abstraction: %d", err);
        return err;
    }

    log_info_cat(LOG_CAT_NETWORKING, "InitializeNetworking: Network abstraction initialized with %s",
                 GetNetworkImplementationName());

    /* Initialize the underlying network implementation */
    if (gNetworkOps == NULL || gNetworkOps->Initialize == NULL) {
        log_app_event("Fatal Error: Network operations not available");
        return notOpenErr;
    }

    err = gNetworkOps->Initialize(&gMacTCPRefNum, &gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
        log_app_event("Fatal Error: Network initialization failed: %d", err);
        ShutdownNetworkAbstraction();
        return err;
    }

    if (gMyLocalIP == 0) {
        log_app_event("Critical Warning: Local IP address is 0.0.0.0. Check network configuration.");
    }

    /* Initialize UDP Discovery */
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (err != noErr) {
        log_app_event("Fatal: UDP Discovery initialization failed (%d).", err);
        if (gNetworkOps && gNetworkOps->Shutdown) {
            gNetworkOps->Shutdown(gMacTCPRefNum);
        }
        ShutdownNetworkAbstraction();
        gMacTCPRefNum = 0;
        return err;
    }
    log_info_cat(LOG_CAT_DISCOVERY, "UDP Discovery Endpoint Initialized.");

    /* Initialize TCP Messaging with dual streams */
    tcpStreamBufferSize = PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE;
    if (tcpStreamBufferSize < MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE) {
        tcpStreamBufferSize = MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Initializing TCP with stream receive buffer size: %lu bytes.", tcpStreamBufferSize);

    /* Create separate UPPs for listen and send streams */
    if (gTCPListenASR_UPP == NULL) {
        gTCPListenASR_UPP = NewTCPNotifyUPP(TCP_Listen_ASR_Handler);
        if (gTCPListenASR_UPP == NULL) {
            log_app_event("Fatal: Failed to create UPP for TCP_Listen_ASR_Handler.");
            CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
            if (gNetworkOps && gNetworkOps->Shutdown) {
                gNetworkOps->Shutdown(gMacTCPRefNum);
            }
            ShutdownNetworkAbstraction();
            gMacTCPRefNum = 0;
            return memFullErr;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "TCP Listen ASR UPP created at 0x%lX.", (unsigned long)gTCPListenASR_UPP);
    }

    if (gTCPSendASR_UPP == NULL) {
        gTCPSendASR_UPP = NewTCPNotifyUPP(TCP_Send_ASR_Handler);
        if (gTCPSendASR_UPP == NULL) {
            log_app_event("Fatal: Failed to create UPP for TCP_Send_ASR_Handler.");
            DisposeRoutineDescriptor(gTCPListenASR_UPP);
            gTCPListenASR_UPP = NULL;
            CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
            if (gNetworkOps && gNetworkOps->Shutdown) {
                gNetworkOps->Shutdown(gMacTCPRefNum);
            }
            ShutdownNetworkAbstraction();
            gMacTCPRefNum = 0;
            return memFullErr;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "TCP Send ASR UPP created at 0x%lX.", (unsigned long)gTCPSendASR_UPP);
    }

    err = InitTCP(gMacTCPRefNum, tcpStreamBufferSize, gTCPListenASR_UPP, gTCPSendASR_UPP);
    if (err != noErr) {
        log_app_event("Fatal: TCP messaging initialization failed (%d).", err);
        if (gTCPListenASR_UPP != NULL) {
            DisposeRoutineDescriptor(gTCPListenASR_UPP);
            gTCPListenASR_UPP = NULL;
        }
        if (gTCPSendASR_UPP != NULL) {
            DisposeRoutineDescriptor(gTCPSendASR_UPP);
            gTCPSendASR_UPP = NULL;
        }
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
        if (gNetworkOps && gNetworkOps->Shutdown) {
            gNetworkOps->Shutdown(gMacTCPRefNum);
        }
        ShutdownNetworkAbstraction();
        gMacTCPRefNum = 0;
        return err;
    }

    log_info_cat(LOG_CAT_MESSAGING, "TCP Messaging Initialized with dual streams.");
    log_app_event("Networking initialization complete. Local IP: %s using %s",
                  gMyLocalIPStr, GetNetworkImplementationName());

    return noErr;
}

void CleanupNetworking(void)
{
    log_app_event("Cleaning up Networking...");

    /* Clean up TCP Messaging */
    CleanupTCP(gMacTCPRefNum);
    log_debug_cat(LOG_CAT_MESSAGING, "TCP Messaging Cleaned up.");

    /* Clean up UDP Discovery */
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
    log_debug_cat(LOG_CAT_DISCOVERY, "UDP Discovery Cleaned up.");

    /* Dispose of TCP ASR UPPs */
    if (gTCPListenASR_UPP != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "Disposing TCP Listen ASR UPP at 0x%lX.", (unsigned long)gTCPListenASR_UPP);
        DisposeRoutineDescriptor(gTCPListenASR_UPP);
        gTCPListenASR_UPP = NULL;
    }

    if (gTCPSendASR_UPP != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "Disposing TCP Send ASR UPP at 0x%lX.", (unsigned long)gTCPSendASR_UPP);
        DisposeRoutineDescriptor(gTCPSendASR_UPP);
        gTCPSendASR_UPP = NULL;
    }

    /* Shutdown network implementation */
    if (gNetworkOps != NULL && gNetworkOps->Shutdown != NULL) {
        gNetworkOps->Shutdown(gMacTCPRefNum);
    }

    /* Shutdown abstraction layer */
    ShutdownNetworkAbstraction();

    gMacTCPRefNum = 0;
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
            log_error_cat(LOG_CAT_NETWORKING, "ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0;
            return paramErr;
        }
        i++;
    }

    if (i != 4) {
        log_error_cat(LOG_CAT_NETWORKING, "ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0;
        return paramErr;
    }

    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr;
}