/*
 * Event-Driven OpenTransport Implementation for P2P Messaging
 * Based on proven minimal test architecture
 * PowerPC version with full P2P functionality
 */

#include "opentransport_impl.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
#include "peer.h"
#include "messaging.h"
#include "discovery.h"
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <OpenTptInternet.h>
#include <Memory.h>
#include <Events.h>
#include <string.h>
#include <stdio.h>

/* Global state */
static Boolean gOTInitialized = false;

/* TCP endpoints */
static EndpointRef gListenEndpoint = NULL;
static EndpointRef gSendEndpoint = NULL;

/* UDP endpoint for discovery */
static EndpointRef gDiscoveryEndpoint = NULL;

/* Current network info */
static char gLocalIPStr[16] = "0.0.0.0";
static char gUsername[32] = "OTUser";

/* Initialize Open Transport for P2P messaging */
OSErr InitOTForApp(void)
{
    OSStatus err;

    if (gOTInitialized) {
        log_debug_cat(LOG_CAT_NETWORKING, "OpenTransport already initialized");
        return noErr;
    }

    log_info_cat(LOG_CAT_NETWORKING, "Attempting to initialize OpenTransport...");

    /* Initialize OpenTransport - this will fail gracefully if OT is not available */
    err = InitOpenTransport();
    if (err != noErr) {
        /* Common OpenTransport error codes:
         * -3101 (kOTNoMemoryErr): Insufficient memory
         * -3151 (kOTNotFoundErr): OpenTransport not found/installed
         * -192: Resource not available
         */
        log_error_cat(LOG_CAT_NETWORKING, "InitOpenTransport failed with error: %ld", err);

        if (err == -3151) {
            log_error_cat(LOG_CAT_NETWORKING, "OpenTransport appears to not be installed or properly configured");
        } else if (err == -192) {
            log_error_cat(LOG_CAT_NETWORKING, "Resource not available - OpenTransport may be disabled or in use");
        } else if (err == -3101) {
            log_error_cat(LOG_CAT_NETWORKING, "Insufficient memory for OpenTransport initialization");
        }

        return (OSErr)err;
    }

    gOTInitialized = true;
    log_info_cat(LOG_CAT_NETWORKING, "OpenTransport initialized successfully");
    return noErr;
}

/* Shutdown OpenTransport */
void ShutdownOTForApp(void)
{
    if (!gOTInitialized) {
        return;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Shutting down OpenTransport");

    /* Close all endpoints */
    if (gListenEndpoint) {
        OTCloseProvider(gListenEndpoint);
        gListenEndpoint = NULL;
    }

    if (gSendEndpoint) {
        OTCloseProvider(gSendEndpoint);
        gSendEndpoint = NULL;
    }

    if (gDiscoveryEndpoint) {
        OTCloseProvider(gDiscoveryEndpoint);
        gDiscoveryEndpoint = NULL;
    }

    CloseOpenTransport();
    gOTInitialized = false;
    log_debug_cat(LOG_CAT_NETWORKING, "OpenTransport shutdown complete");
}

/* Create TCP listen endpoint for incoming connections */
OSErr CreateListenEndpoint(tcp_port localPort)
{
    OSStatus err;
    InetAddress addr;
    TBind bindReq;

    if (!gOTInitialized) {
        log_error_cat(LOG_CAT_NETWORKING, "OpenTransport not initialized");
        return -1;
    }

    if (gListenEndpoint != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "TCP listen endpoint already exists");
        return noErr;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Creating TCP listen endpoint on port %d", localPort);

    /* Create TCP endpoint with tilisten module to handle multiple simultaneous connections */
    gListenEndpoint = OTOpenEndpoint(OTCreateConfiguration("tilisten,tcp"), 0, NULL, &err);
    if (err != noErr || gListenEndpoint == NULL) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to open TCP endpoint: %ld", err);
        return (OSErr)err;
    }

    /* Set up bind address */
    memset(&addr, 0, sizeof(addr));
    addr.fAddressType = AF_INET;
    addr.fPort = localPort;
    addr.fHost = kOTAnyInetAddress; /* Listen on any interface */

    /* Set up bind request */
    memset(&bindReq, 0, sizeof(bindReq));
    bindReq.addr.len = sizeof(InetAddress);
    bindReq.addr.buf = (UInt8*)&addr;
    bindReq.addr.maxlen = sizeof(InetAddress);
    bindReq.qlen = 5; /* Listen queue depth */

    /* Bind endpoint */
    err = OTBind(gListenEndpoint, &bindReq, NULL);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to bind TCP endpoint: %ld", err);
        OTCloseProvider(gListenEndpoint);
        gListenEndpoint = NULL;
        return (OSErr)err;
    }

    log_info_cat(LOG_CAT_NETWORKING, "TCP listen endpoint created on port %d", localPort);
    return noErr;
}

/* Create UDP endpoint for discovery */
OSErr CreateDiscoveryEndpoint(udp_port localPort)
{
    OSStatus err;
    InetAddress addr;
    TBind bindReq;

    if (!gOTInitialized) {
        log_error_cat(LOG_CAT_NETWORKING, "OpenTransport not initialized");
        return -1;
    }

    if (gDiscoveryEndpoint != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "UDP discovery endpoint already exists");
        return noErr;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Creating UDP discovery endpoint on port %d", localPort);

    /* Create UDP endpoint */
    gDiscoveryEndpoint = OTOpenEndpoint(OTCreateConfiguration(kUDPName), 0, NULL, &err);
    if (err != noErr || gDiscoveryEndpoint == NULL) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to open UDP endpoint: %ld", err);
        return (OSErr)err;
    }

    /* Set up bind address */
    memset(&addr, 0, sizeof(addr));
    addr.fAddressType = AF_INET;
    addr.fPort = localPort;
    addr.fHost = kOTAnyInetAddress;

    /* Set up bind request */
    memset(&bindReq, 0, sizeof(bindReq));
    bindReq.addr.len = sizeof(InetAddress);
    bindReq.addr.buf = (UInt8*)&addr;
    bindReq.addr.maxlen = sizeof(InetAddress);
    bindReq.qlen = 0; /* No listen queue for UDP */

    /* Bind endpoint */
    err = OTBind(gDiscoveryEndpoint, &bindReq, NULL);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to bind UDP endpoint: %ld", err);
        OTCloseProvider(gDiscoveryEndpoint);
        gDiscoveryEndpoint = NULL;
        return (OSErr)err;
    }

    /* Enable IP_BROADCAST option to allow sending broadcast datagrams */
    TOptMgmt optReq;
    TOption optBuf;

    memset(&optReq, 0, sizeof(optReq));
    memset(&optBuf, 0, sizeof(optBuf));

    /* Set up the IP_BROADCAST option */
    optBuf.len = kOTFourByteOptionSize;
    optBuf.level = INET_IP;
    optBuf.name = IP_BROADCAST;
    optBuf.status = 0;
    optBuf.value[0] = T_YES;

    /* Set up request parameter for OTOptionManagement */
    optReq.opt.buf = (UInt8*)&optBuf;
    optReq.opt.len = sizeof(optBuf);
    optReq.opt.maxlen = sizeof(optBuf);
    optReq.flags = T_NEGOTIATE;

    err = OTOptionManagement(gDiscoveryEndpoint, &optReq, &optReq);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to enable IP_BROADCAST option: %ld", err);
        /* Don't fail - try to continue anyway */
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "IP_BROADCAST option enabled for UDP endpoint");
    }

    log_info_cat(LOG_CAT_NETWORKING, "UDP discovery endpoint created on port %d", localPort);
    return noErr;
}

/* Poll for OpenTransport events - call this from main event loop */
void PollOTEvents(void)
{
    OTResult result;

    if (!gOTInitialized) {
        return;
    }

    /* Poll TCP listen endpoint */
    if (gListenEndpoint != NULL) {
        result = OTLook(gListenEndpoint);
        if (result > 0) {
            HandleTCPEvent(gListenEndpoint, result);
        } else if (result < 0) {
            log_error_cat(LOG_CAT_NETWORKING, "TCP OTLook error: %ld", result);
        }
    }

    /* Poll UDP discovery endpoint */
    if (gDiscoveryEndpoint != NULL) {
        result = OTLook(gDiscoveryEndpoint);
        if (result > 0) {
            HandleUDPEvent(gDiscoveryEndpoint, result);
        } else if (result < 0) {
            log_error_cat(LOG_CAT_NETWORKING, "UDP OTLook error: %ld", result);
        }
    }
}

/* Handle TCP events */
void HandleTCPEvent(EndpointRef endpoint, OTResult event)
{
    switch (event) {
        case T_LISTEN:
            log_debug_cat(LOG_CAT_NETWORKING, "T_LISTEN: Incoming connection request");
            HandleIncomingConnection(endpoint);
            break;

        case T_CONNECT:
            log_debug_cat(LOG_CAT_NETWORKING, "T_CONNECT: Connection established");
            break;

        case T_DATA:
            log_debug_cat(LOG_CAT_NETWORKING, "T_DATA: TCP data available");
            HandleIncomingTCPData(endpoint);
            break;

        case T_DISCONNECT:
            log_debug_cat(LOG_CAT_NETWORKING, "T_DISCONNECT: Connection closed");
            HandleConnectionClosed(endpoint);
            break;

        case T_ERROR:
            log_error_cat(LOG_CAT_NETWORKING, "T_ERROR: TCP error occurred");
            break;

        default:
            log_debug_cat(LOG_CAT_NETWORKING, "Unhandled TCP event: %ld", event);
            break;
    }
}

/* Handle UDP events */
void HandleUDPEvent(EndpointRef endpoint, OTResult event)
{
    switch (event) {
        case T_DATA:
            log_debug_cat(LOG_CAT_NETWORKING, "T_DATA: UDP discovery data available");
            HandleIncomingUDPData(endpoint);
            break;

        case T_ERROR:
            log_error_cat(LOG_CAT_NETWORKING, "T_ERROR: UDP error occurred");
            break;

        default:
            log_debug_cat(LOG_CAT_NETWORKING, "Unhandled UDP event: %ld", event);
            break;
    }
}

/* Handle incoming TCP connection */
void HandleIncomingConnection(EndpointRef listener)
{
    OSStatus err;
    TCall call;
    InetAddress peerAddr;
    EndpointRef connEp = kOTInvalidEndpointRef;

    log_debug_cat(LOG_CAT_NETWORKING, "Handling incoming TCP connection");

    /* Set up call structure */
    memset(&call, 0, sizeof(call));
    call.addr.buf = (UInt8*)&peerAddr;
    call.addr.maxlen = sizeof(InetAddress);

    /* Listen for the connection */
    err = OTListen(listener, &call);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTListen failed: %ld", err);
        return;
    }

    /* Create new endpoint for the connection */
    connEp = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (err != noErr || connEp == kOTInvalidEndpointRef) {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to open new endpoint for incoming connection: %ld", err);
        OTSndDisconnect(listener, &call);
        return;
    }

    /* Set the new endpoint to non-blocking mode to avoid blocking the event loop */
    OTSetNonBlocking(connEp);

    /* Accept the connection on the new endpoint */
    err = OTAccept(listener, connEp, &call);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTAccept failed: %ld", err);
        OTCloseProvider(connEp);
        return;
    }

    log_info_cat(LOG_CAT_NETWORKING, "Accepted connection, sequence %ld. New endpoint ref %ld", call.sequence, (long)connEp);

    /* Try to read data with a short timeout - don't block */
    /* Use OTRcv with non-blocking check first */
    OTResult lookResult = OTLook(connEp);
    if (lookResult == T_DATA) {
        /* Data is available, handle it */
        HandleIncomingTCPData(connEp);
    } else {
        /* No immediate data - try one receive with very short wait */
        HandleIncomingTCPData(connEp);
    }

    /* Send orderly disconnect before closing */
    OTSndOrderlyDisconnect(connEp);

    /* Give a moment for disconnect to complete */
    Delay(1, NULL); /* 1 tick = ~17ms */

    /* Close the temporary connection endpoint */
    OTCloseProvider(connEp);
    log_debug_cat(LOG_CAT_NETWORKING, "Closed incoming connection endpoint");
}

/* Handle incoming TCP data */
void HandleIncomingTCPData(EndpointRef endpoint)
{
    OSStatus err;
    UInt32 flags;
    OTResult bytesReceived;
    char buffer[BUFFER_SIZE];
    int retryCount = 0;
    const int maxRetries = 100; /* Wait up to ~1 second total */

    log_debug_cat(LOG_CAT_NETWORKING, "Handling incoming TCP data");

    /* Wait for data to arrive with timeout - endpoint is in non-blocking mode */
    while (retryCount < maxRetries) {
        OTResult lookResult = OTLook(endpoint);
        if (lookResult == T_DATA) {
            break; /* Data available */
        } else if (lookResult == T_DISCONNECT || lookResult == T_ORDREL) {
            log_debug_cat(LOG_CAT_NETWORKING, "Connection closed while waiting for data");
            return;
        } else if (lookResult < 0) {
            log_error_cat(LOG_CAT_NETWORKING, "OTLook failed while waiting for data: %ld", lookResult);
            return;
        }
        /* Wait a bit and try again */
        Delay(1, NULL); /* ~17ms per tick */
        retryCount++;
    }

    if (retryCount >= maxRetries) {
        log_debug_cat(LOG_CAT_NETWORKING, "Timeout waiting for TCP data");
        return;
    }

    /* Receive data - non-blocking mode, so won't block event loop */
    bytesReceived = OTRcv(endpoint, buffer, sizeof(buffer) - 1, &flags);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0'; /* Null terminate */
        log_debug_cat(LOG_CAT_MESSAGING, "Received TCP data (%ld bytes): %s", bytesReceived, buffer);

        /* Get peer IP address */
        TBind peerAddr;
        InetAddress peerInetAddr;
        char peerIPStr[16] = "unknown";

        peerAddr.addr.buf = (UInt8*)&peerInetAddr;
        peerAddr.addr.maxlen = sizeof(InetAddress);

        err = OTGetProtAddress(endpoint, NULL, &peerAddr);
        if (err == noErr && peerAddr.addr.len > 0) {
            /* Convert peer address to string using OTInetHostToString */
            OTInetHostToString(peerInetAddr.fHost, peerIPStr);
        }

        /* Parse and process the message */
        ProcessIncomingMessage(buffer, peerIPStr);
    } else if (bytesReceived < 0) {
        err = (OSStatus)bytesReceived;
        log_error_cat(LOG_CAT_NETWORKING, "OTRcv failed: %ld", err);
    }
}

/* Handle incoming UDP data */
void HandleIncomingUDPData(EndpointRef endpoint)
{
    OSStatus err;
    UInt32 flags;
    TUnitData unitData;
    InetAddress peerAddr;
    char buffer[BUFFER_SIZE];
    char peerIPStr[16];
    char myLocalIP[16];

    log_debug_cat(LOG_CAT_NETWORKING, "Handling incoming UDP discovery data");

    /* Set up unit data structure */
    memset(&unitData, 0, sizeof(unitData));
    unitData.addr.buf = (UInt8*)&peerAddr;
    unitData.addr.maxlen = sizeof(InetAddress);
    unitData.udata.buf = (UInt8*)buffer;
    unitData.udata.maxlen = sizeof(buffer) - 1;

    /* Receive UDP data */
    err = OTRcvUData(endpoint, &unitData, &flags);
    if (err == noErr && unitData.udata.len > 0) {
        buffer[unitData.udata.len] = '\0'; /* Null terminate */

        /* Convert peer address to string using OTInetHostToString */
        OTInetHostToString(peerAddr.fHost, peerIPStr);

        log_debug_cat(LOG_CAT_DISCOVERY, "Received UDP data from %s (%ld bytes): %s",
                      peerIPStr, unitData.udata.len, buffer);

        /* Get our local IP to filter out our own messages */
        GetLocalIPAddress(myLocalIP, sizeof(myLocalIP));

        /* Ignore messages from ourselves */
        if (strcmp(peerIPStr, myLocalIP) == 0) {
            log_debug_cat(LOG_CAT_DISCOVERY, "Ignored UDP message from self (%s)", peerIPStr);
            return;
        }

        /* Process discovery message using shared logic */
        ProcessIncomingUDPMessage(buffer, unitData.udata.len, peerIPStr, peerAddr.fHost, peerAddr.fPort);
    } else if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTRcvUData failed: %ld", err);
    }
}

/* Handle connection closed */
void HandleConnectionClosed(EndpointRef endpoint)
{
    OSStatus err;
    TDiscon discon;

    log_debug_cat(LOG_CAT_NETWORKING, "TCP connection closed");

    /* Receive the disconnect indication to clean up properly */
    memset(&discon, 0, sizeof(discon));
    err = OTRcvDisconnect(endpoint, &discon);
    if (err == noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "Disconnect reason: %ld", discon.reason);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "OTRcvDisconnect failed: %ld", err);
    }

    /* Note: For the listen endpoint, we don't close it as it should remain open */
    /* Temporary connection endpoints are closed by their respective handlers */
}

/* Send UDP discovery message */
OSErr SendUDPMessage(const char* message, const char* targetIP, udp_port targetPort)
{
    OSStatus err;
    TUnitData unitData;
    InetAddress targetAddr;

    if (!gOTInitialized || gDiscoveryEndpoint == NULL) {
        log_error_cat(LOG_CAT_NETWORKING, "UDP endpoint not ready for sending");
        return -1;
    }

    /* Parse target IP address */
    int a, b, c, d;
    if (sscanf(targetIP, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        log_error_cat(LOG_CAT_NETWORKING, "Invalid IP address: %s", targetIP);
        return -1;
    }

    /* Set up target address - convert IP string to host format */
    memset(&targetAddr, 0, sizeof(targetAddr));
    targetAddr.fAddressType = AF_INET;
    targetAddr.fPort = targetPort;
    /* Manual conversion: OpenTransport expects host byte order for fHost */
    targetAddr.fHost = (a << 24) | (b << 16) | (c << 8) | d;

    /* Set up unit data */
    memset(&unitData, 0, sizeof(unitData));
    unitData.addr.buf = (UInt8*)&targetAddr;
    unitData.addr.len = sizeof(InetAddress);
    unitData.udata.buf = (UInt8*)message;
    unitData.udata.len = strlen(message);

    /* Send UDP data */
    err = OTSndUData(gDiscoveryEndpoint, &unitData);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTSndUData failed: %ld", err);
        return (OSErr)err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Sent UDP message to %s:%d", targetIP, targetPort);
    return noErr;
}

/* Send TCP message to peer */
OSErr SendTCPMessage(const char* message, const char* targetIP, tcp_port targetPort)
{
    OSStatus err;
    EndpointRef ep = kOTInvalidEndpointRef;
    TCall sndCall;
    InetAddress targetAddr;
    OTResult lookResult;
    EventRecord event;
    int retryCount = 0;
    const int maxRetries = 50; /* ~5 seconds at 100ms per retry */

    log_debug_cat(LOG_CAT_MESSAGING, "Sending TCP message to %s:%d: %s", targetIP, targetPort, message);

    /* Create endpoint for sending */
    ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (err != noErr || ep == kOTInvalidEndpointRef) {
        log_error_cat(LOG_CAT_MESSAGING, "Failed to open endpoint for sending: %ld", err);
        return (OSErr)err;
    }

    /* Bind to any available port */
    err = OTBind(ep, NULL, NULL);
    if (err != noErr) {
        log_error_cat(LOG_CAT_MESSAGING, "Failed to bind sending endpoint: %ld", err);
        OTCloseProvider(ep);
        return (OSErr)err;
    }

    /* Set up target address */
    memset(&targetAddr, 0, sizeof(targetAddr));
    targetAddr.fAddressType = AF_INET;
    targetAddr.fPort = targetPort;
    err = OTInetStringToHost(targetIP, &targetAddr.fHost);
    if (err != noErr) {
        log_error_cat(LOG_CAT_MESSAGING, "Invalid IP address %s: %ld", targetIP, err);
        OTCloseProvider(ep);
        return (OSErr)err;
    }

    /* Set up call structure */
    memset(&sndCall, 0, sizeof(sndCall));
    sndCall.addr.buf = (UInt8*)&targetAddr;
    sndCall.addr.len = sizeof(targetAddr);

    /* Connect to target */
    err = OTConnect(ep, &sndCall, NULL);
    if (err != noErr) {
        if (err == kOTLookErr) {
            /* Connection is proceeding asynchronously, wait for T_CONNECT */
            while (retryCount < maxRetries) {
                lookResult = OTLook(ep);
                if (lookResult == T_CONNECT) {
                    err = OTRcvConnect(ep, NULL);
                    if (err == noErr) {
                        break; /* Connected successfully */
                    } else {
                        log_error_cat(LOG_CAT_MESSAGING, "OTRcvConnect failed: %ld", err);
                        OTCloseProvider(ep);
                        return (OSErr)err;
                    }
                } else if (lookResult < 0) {
                    log_error_cat(LOG_CAT_MESSAGING, "OTLook error during connect: %ld", lookResult);
                    OTCloseProvider(ep);
                    return (OSErr)lookResult;
                }

                /* Yield time to system and retry */
                WaitNextEvent(0, &event, 6, NULL); /* ~100ms delay */
                retryCount++;
            }

            if (retryCount >= maxRetries) {
                log_error_cat(LOG_CAT_MESSAGING, "Connection timeout to %s:%d", targetIP, targetPort);
                OTCloseProvider(ep);
                return paramErr;
            }
        } else {
            log_error_cat(LOG_CAT_MESSAGING, "Failed to connect to %s:%d: %ld", targetIP, targetPort, err);
            OTCloseProvider(ep);
            return (OSErr)err;
        }
    }

    /* Send the message */
    OTResult bytesSent = OTSnd(ep, (void*)message, strlen(message), 0);
    if (bytesSent < 0) {
        log_error_cat(LOG_CAT_MESSAGING, "OTSnd failed: %ld", bytesSent);
        err = (OSStatus)bytesSent;
    } else {
        log_debug_cat(LOG_CAT_MESSAGING, "Sent %ld bytes to %s:%d", bytesSent, targetIP, targetPort);
        err = noErr;
    }

    /* Close connection gracefully */
    OTSndDisconnect(ep, NULL);
    OTCloseProvider(ep);

    return (OSErr)err;
}

/* Get local IP address */
OSErr GetLocalIPAddress(char* ipStr, size_t ipStrSize)
{
    InetInterfaceInfo ifaceInfo;
    OSStatus err = OTInetGetInterfaceInfo(&ifaceInfo, kDefaultInetInterface);

    if (err == noErr) {
        OTInetHostToString(ifaceInfo.fAddress, ipStr);
        /* Cache the IP address */
        strncpy(gLocalIPStr, ipStr, sizeof(gLocalIPStr) - 1);
        gLocalIPStr[sizeof(gLocalIPStr) - 1] = '\0';
        log_info_cat(LOG_CAT_NETWORKING, "Local IP address: %s", ipStr);
        return noErr;
    } else {
        log_error_cat(LOG_CAT_NETWORKING, "Failed to get local IP address: %ld", err);
        strncpy(ipStr, "127.0.0.1", ipStrSize - 1);
        ipStr[ipStrSize - 1] = '\0';
        return (OSErr)err;
    }
}

/* Set username for P2P messaging */
void SetUsername(const char* username)
{
    strncpy(gUsername, username, sizeof(gUsername) - 1);
    gUsername[sizeof(gUsername) - 1] = '\0';
    log_debug_cat(LOG_CAT_SYSTEM, "Username set to: %s", gUsername);
}

/* Get current username */
const char* GetUsername(void)
{
    return gUsername;
}