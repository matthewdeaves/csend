/*
 * Event-Driven OpenTransport Implementation for P2P Messaging
 * Based on proven minimal test architecture
 * PowerPC version with full P2P functionality
 */

#include "opentransport_impl.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
#include "../shared/peer_wrapper.h"
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

/* Endpoint pool for handling multiple concurrent connections */
typedef struct {
    EndpointRef endpoint;
    Boolean inUse;
    Boolean bound;
} PooledEndpoint;

static PooledEndpoint gEndpointPool[MAX_CACHED_ENDPOINTS];
static int gPoolSize = 0;
static Boolean gPoolInitialized = false;

/* Active connection tracking - endpoints awaiting data */
typedef struct {
    EndpointRef endpoint;
    Boolean active;
    unsigned long acceptTime; /* TickCount when accepted */
} ActiveConnection;

#define MAX_ACTIVE_CONNECTIONS 10
static ActiveConnection gActiveConnections[MAX_ACTIVE_CONNECTIONS];
static Boolean gActiveConnectionsInitialized = false;

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

    /* Shutdown endpoint pool first */
    ShutdownEndpointPool();

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

/* Initialize endpoint pool for handling multiple concurrent connections */
OSErr InitializeEndpointPool(void)
{
    int i;
    OSStatus err;

    if (gPoolInitialized) {
        log_debug_cat(LOG_CAT_NETWORKING, "Endpoint pool already initialized");
        return noErr;
    }

    log_info_cat(LOG_CAT_NETWORKING, "Initializing endpoint pool with %d endpoints", INITIAL_CACHED_ENDPOINTS);

    /* Initialize pool structure */
    memset(gEndpointPool, 0, sizeof(gEndpointPool));
    gPoolSize = 0;

    /* Pre-create initial endpoints */
    for (i = 0; i < INITIAL_CACHED_ENDPOINTS; i++) {
        EndpointRef ep = OTOpenEndpoint(OTCreateConfiguration("tcp"), 0, NULL, &err);
        if (err != noErr || ep == kOTInvalidEndpointRef) {
            log_error_cat(LOG_CAT_NETWORKING, "Failed to create pooled endpoint %d: %ld", i, err);
            continue;
        }

        /* Set non-blocking mode for all pooled endpoints */
        OTSetNonBlocking(ep);

        gEndpointPool[gPoolSize].endpoint = ep;
        gEndpointPool[gPoolSize].inUse = false;
        gEndpointPool[gPoolSize].bound = false;
        gPoolSize++;

        log_debug_cat(LOG_CAT_NETWORKING, "Created pooled endpoint %d: %ld", gPoolSize, (long)ep);
    }

    gPoolInitialized = true;
    log_info_cat(LOG_CAT_NETWORKING, "Endpoint pool initialized with %d/%d endpoints", gPoolSize, INITIAL_CACHED_ENDPOINTS);
    return noErr;
}

/* Shutdown endpoint pool */
void ShutdownEndpointPool(void)
{
    int i;

    if (!gPoolInitialized) {
        return;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Shutting down endpoint pool");

    for (i = 0; i < gPoolSize; i++) {
        if (gEndpointPool[i].endpoint != kOTInvalidEndpointRef) {
            if (gEndpointPool[i].bound) {
                OTUnbind(gEndpointPool[i].endpoint);
            }
            OTCloseProvider(gEndpointPool[i].endpoint);
            gEndpointPool[i].endpoint = kOTInvalidEndpointRef;
        }
    }

    gPoolSize = 0;
    gPoolInitialized = false;
    log_debug_cat(LOG_CAT_NETWORKING, "Endpoint pool shutdown complete");
}

/* Acquire an endpoint from the pool */
EndpointRef AcquireEndpointFromPool(void)
{
    int i;
    OSStatus err;

    /* Try to find an unused endpoint in the pool */
    for (i = 0; i < gPoolSize; i++) {
        if (!gEndpointPool[i].inUse) {
            gEndpointPool[i].inUse = true;
            log_debug_cat(LOG_CAT_NETWORKING, "Acquired pooled endpoint %d: %ld", i, (long)gEndpointPool[i].endpoint);
            return gEndpointPool[i].endpoint;
        }
    }

    /* Pool exhausted - create a new endpoint if we have room */
    if (gPoolSize < MAX_CACHED_ENDPOINTS) {
        EndpointRef ep = OTOpenEndpoint(OTCreateConfiguration("tcp"), 0, NULL, &err);
        if (err != noErr || ep == kOTInvalidEndpointRef) {
            log_error_cat(LOG_CAT_NETWORKING, "Failed to create new pooled endpoint: %ld", err);
            return kOTInvalidEndpointRef;
        }

        /* Set non-blocking mode */
        OTSetNonBlocking(ep);

        gEndpointPool[gPoolSize].endpoint = ep;
        gEndpointPool[gPoolSize].inUse = true;
        gEndpointPool[gPoolSize].bound = false;
        gPoolSize++;

        log_info_cat(LOG_CAT_NETWORKING, "Created new pooled endpoint %d: %ld (pool now %d/%d)",
                     gPoolSize - 1, (long)ep, gPoolSize, MAX_CACHED_ENDPOINTS);
        return ep;
    }

    /* No endpoints available */
    log_error_cat(LOG_CAT_NETWORKING, "Endpoint pool exhausted! All %d endpoints in use", MAX_CACHED_ENDPOINTS);
    return kOTInvalidEndpointRef;
}

/* Release an endpoint back to the pool */
void ReleaseEndpointToPool(EndpointRef endpoint)
{
    int i;
    OTResult state;

    for (i = 0; i < gPoolSize; i++) {
        if (gEndpointPool[i].endpoint == endpoint) {
            /* Check endpoint state */
            state = OTGetEndpointState(endpoint);
            log_debug_cat(LOG_CAT_NETWORKING, "Releasing endpoint %d (state %ld)", i, state);

            /* If endpoint is connected, disconnect it first */
            if (state == T_DATAXFER || state == T_INREL || state == T_OUTREL || state == T_INCON || state == T_OUTCON) {
                /* Use abortive disconnect - it's immediate and returns endpoint to T_IDLE */
                OSStatus err = OTSndDisconnect(endpoint, NULL);
                if (err == noErr) {
                    /* OTSndDisconnect immediately places endpoint in T_IDLE state */
                    state = OTGetEndpointState(endpoint);
                    log_debug_cat(LOG_CAT_NETWORKING, "Disconnected endpoint %d, now in state %ld", i, state);
                } else if (err == kOTLookErr) {
                    /* There's a pending event - clear it first */
                    OTResult lookResult = OTLook(endpoint);
                    log_debug_cat(LOG_CAT_NETWORKING, "Pending event on endpoint %d: %ld", i, lookResult);

                    if (lookResult == T_DISCONNECT) {
                        OTRcvDisconnect(endpoint, NULL);
                        log_debug_cat(LOG_CAT_NETWORKING, "Cleared T_DISCONNECT event on endpoint %d", i);
                    } else if (lookResult == T_ORDREL) {
                        OTRcvOrderlyDisconnect(endpoint);
                        /* Send our own orderly disconnect to complete the sequence */
                        OTSndOrderlyDisconnect(endpoint);
                        log_debug_cat(LOG_CAT_NETWORKING, "Completed orderly disconnect on endpoint %d", i);
                    }

                    /* Try disconnect again */
                    state = OTGetEndpointState(endpoint);
                    if (state == T_DATAXFER || state == T_INREL || state == T_OUTREL) {
                        OTSndDisconnect(endpoint, NULL);
                    }
                } else {
                    log_error_cat(LOG_CAT_NETWORKING, "OTSndDisconnect failed for endpoint %d: %ld", i, err);
                }
            }

            /* Per NetworkingOpenTransport.txt: Endpoints for OTAccept should be "open, unbound".
             * After disconnect, endpoint is in T_IDLE. Unbind it to return to T_UNBND for reuse. */
            state = OTGetEndpointState(endpoint);
            if (state == T_IDLE) {
                OSStatus err = OTUnbind(endpoint);
                if (err == noErr) {
                    state = OTGetEndpointState(endpoint);
                    log_debug_cat(LOG_CAT_NETWORKING, "Unbound endpoint %d, now in state %ld", i, state);
                } else {
                    log_error_cat(LOG_CAT_NETWORKING, "OTUnbind failed for endpoint %d: %ld", i, err);
                }
            }

            /* Verify endpoint is in a reusable state (T_UNBND preferred for OTAccept) */
            state = OTGetEndpointState(endpoint);
            if (state != T_IDLE && state != T_UNBND && state != T_UNINIT) {
                log_error_cat(LOG_CAT_NETWORKING, "WARNING: Endpoint %d still in bad state %ld after release attempt", i, state);
            }

            gEndpointPool[i].inUse = false;
            gEndpointPool[i].bound = false; /* Mark as unbound for proper tracking */
            log_debug_cat(LOG_CAT_NETWORKING, "Released endpoint %d back to pool (final state %ld)", i, state);
            return;
        }
    }

    /* Endpoint not in pool - close it */
    log_debug_cat(LOG_CAT_NETWORKING, "Endpoint %ld not in pool, closing directly", (long)endpoint);
    OTCloseProvider(endpoint);
}

/* Initialize active connections tracking */
static void InitializeActiveConnections(void)
{
    int i;
    if (gActiveConnectionsInitialized) {
        return;
    }

    for (i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        gActiveConnections[i].endpoint = kOTInvalidEndpointRef;
        gActiveConnections[i].active = false;
        gActiveConnections[i].acceptTime = 0;
    }

    gActiveConnectionsInitialized = true;
    log_debug_cat(LOG_CAT_NETWORKING, "Initialized active connections tracking");
}

/* Add connection to active tracking */
static Boolean AddActiveConnection(EndpointRef endpoint)
{
    int i;

    if (!gActiveConnectionsInitialized) {
        InitializeActiveConnections();
    }

    for (i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        if (!gActiveConnections[i].active) {
            gActiveConnections[i].endpoint = endpoint;
            gActiveConnections[i].active = true;
            gActiveConnections[i].acceptTime = TickCount();
            log_debug_cat(LOG_CAT_NETWORKING, "Added endpoint %ld to active connections slot %d", (long)endpoint, i);
            return true;
        }
    }

    log_error_cat(LOG_CAT_NETWORKING, "No free slot in active connections for endpoint %ld", (long)endpoint);
    return false;
}

/* Remove connection from active tracking */
static void RemoveActiveConnection(EndpointRef endpoint)
{
    int i;

    for (i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        if (gActiveConnections[i].active && gActiveConnections[i].endpoint == endpoint) {
            gActiveConnections[i].active = false;
            gActiveConnections[i].endpoint = kOTInvalidEndpointRef;
            log_debug_cat(LOG_CAT_NETWORKING, "Removed endpoint %ld from active connections slot %d", (long)endpoint, i);
            return;
        }
    }
}

/* Poll active connections for data */
void PollActiveConnections(void)
{
    int i;
    unsigned long currentTime = TickCount();
    const unsigned long timeout = 180; /* 3 seconds at 60 ticks/sec */

    if (!gActiveConnectionsInitialized) {
        return;
    }

    for (i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        if (!gActiveConnections[i].active) {
            continue;
        }

        EndpointRef ep = gActiveConnections[i].endpoint;

        /* Per NetworkingOpenTransport.txt: OTLook prioritizes T_ORDREL over T_DATA.
         * We MUST try to receive data first before checking OTLook, otherwise we'll
         * miss data that arrived before the orderly disconnect. */

        /* Try to receive data first */
        char buffer[BUFFER_SIZE];
        UInt32 flags = 0;
        OTResult bytesReceived = OTRcv(ep, buffer, sizeof(buffer) - 1, &flags);

        if (bytesReceived > 0) {
            /* Data received! Process it */
            buffer[bytesReceived] = '\0';
            log_debug_cat(LOG_CAT_NETWORKING, "Poll: Received %ld bytes on active connection %d (endpoint %ld)", bytesReceived, i, (long)ep);

            /* Get peer IP for processing */
            TBind peerAddr;
            InetAddress peerInetAddr;
            char peerIPStr[16] = "unknown";
            peerAddr.addr.buf = (UInt8*)&peerInetAddr;
            peerAddr.addr.maxlen = sizeof(InetAddress);

            OSStatus err = OTGetProtAddress(ep, NULL, &peerAddr);
            if (err == noErr && peerAddr.addr.len > 0) {
                OTInetHostToString(peerInetAddr.fHost, peerIPStr);
            }

            ProcessIncomingMessage(buffer, peerIPStr);

            /* After receiving data, release the endpoint */
            RemoveActiveConnection(ep);
            ReleaseEndpointToPool(ep);
        } else if (bytesReceived == kOTNoDataErr || bytesReceived == -3158) {
            /* No data yet - check for disconnect events */
            OTResult lookResult = OTLook(ep);

            if (lookResult == T_DISCONNECT) {
                log_debug_cat(LOG_CAT_NETWORKING, "Poll: Abortive disconnect on active connection %d (endpoint %ld)", i, (long)ep);
                RemoveActiveConnection(ep);
                ReleaseEndpointToPool(ep);
            } else if (lookResult == T_ORDREL) {
                /* Orderly disconnect received, but data might still be in the buffer.
                 * Per NetworkingOpenTransport.txt: "receive all data remaining" before disconnect.
                 * Try one more OTRcv to catch any data that arrived with the T_ORDREL. */
                log_debug_cat(LOG_CAT_NETWORKING, "Poll: T_ORDREL on active connection %d, checking for buffered data", i);

                char finalBuffer[BUFFER_SIZE];
                UInt32 finalFlags = 0;
                OTResult finalBytes = OTRcv(ep, finalBuffer, sizeof(finalBuffer) - 1, &finalFlags);

                if (finalBytes > 0) {
                    /* Data was still in buffer! Process it before disconnect */
                    finalBuffer[finalBytes] = '\0';
                    log_debug_cat(LOG_CAT_NETWORKING, "Poll: Received %ld bytes from orderly disconnect (endpoint %ld)", finalBytes, (long)ep);

                    /* Get peer IP */
                    TBind peerAddr;
                    InetAddress peerInetAddr;
                    char peerIPStr[16] = "unknown";
                    peerAddr.addr.buf = (UInt8*)&peerInetAddr;
                    peerAddr.addr.maxlen = sizeof(InetAddress);

                    OSStatus err = OTGetProtAddress(ep, NULL, &peerAddr);
                    if (err == noErr && peerAddr.addr.len > 0) {
                        OTInetHostToString(peerInetAddr.fHost, peerIPStr);
                    }

                    ProcessIncomingMessage(finalBuffer, peerIPStr);
                } else {
                    log_debug_cat(LOG_CAT_NETWORKING, "Poll: Orderly disconnect on active connection %d (endpoint %ld) - no data received", i, (long)ep);
                }

                OTRcvOrderlyDisconnect(ep); /* Acknowledge the orderly release */
                RemoveActiveConnection(ep);
                ReleaseEndpointToPool(ep);
            } else if (lookResult < 0) {
                log_error_cat(LOG_CAT_NETWORKING, "Poll: OTLook error on active connection %d (endpoint %ld): %ld", i, (long)ep, lookResult);
                RemoveActiveConnection(ep);
                ReleaseEndpointToPool(ep);
            } else if (currentTime - gActiveConnections[i].acceptTime > timeout) {
                log_debug_cat(LOG_CAT_NETWORKING, "Poll: Timeout on active connection %d (endpoint %ld)", i, (long)ep);
                RemoveActiveConnection(ep);
                ReleaseEndpointToPool(ep);
            }
            /* else: No data, no events - keep waiting */
        } else if (bytesReceived < 0) {
            /* Other OTRcv error (not kOTNoDataErr) */
            log_error_cat(LOG_CAT_NETWORKING, "Poll: OTRcv error on active connection %d (endpoint %ld): %ld", i, (long)ep, bytesReceived);
            RemoveActiveConnection(ep);
            ReleaseEndpointToPool(ep);
        }
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

    /* Acquire an endpoint from the pool for this connection */
    connEp = AcquireEndpointFromPool();
    if (connEp == kOTInvalidEndpointRef) {
        log_error_cat(LOG_CAT_NETWORKING, "No endpoint available from pool, rejecting connection");
        OTSndDisconnect(listener, &call);
        return;
    }

    /* Endpoint from pool is already in non-blocking mode */

    /* Accept the connection on the pooled endpoint */
    err = OTAccept(listener, connEp, &call);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTAccept failed: %ld", err);
        /* Release endpoint back to pool */
        ReleaseEndpointToPool(connEp);
        return;
    }

    log_info_cat(LOG_CAT_NETWORKING, "Accepted connection, sequence %ld. Using pooled endpoint %ld", call.sequence, (long)connEp);

    /* Check if data is immediately available */
    OTResult lookResult = OTLook(connEp);
    if (lookResult == T_DATA) {
        /* Data is available immediately, handle it now */
        log_debug_cat(LOG_CAT_NETWORKING, "Data immediately available on new connection");
        HandleIncomingTCPData(connEp);
        /* Release endpoint after handling data */
        ReleaseEndpointToPool(connEp);
        log_debug_cat(LOG_CAT_NETWORKING, "Released connection endpoint back to pool");
    } else {
        /* No immediate data - add to active connections for polling */
        log_debug_cat(LOG_CAT_NETWORKING, "No immediate data, adding connection to active tracking");
        if (!AddActiveConnection(connEp)) {
            /* Failed to add to active connections - release immediately */
            log_error_cat(LOG_CAT_NETWORKING, "Failed to track connection, releasing endpoint");
            ReleaseEndpointToPool(connEp);
        }
    }
}

/* Handle incoming TCP data
 * Per NetworkingOpenTransport.txt p.7439: "Until you get a T_DATA event, you should
 * continue calling OTRcv until it returns kOTNoDataErr" to fully clear the T_DATA event.
 */
void HandleIncomingTCPData(EndpointRef endpoint)
{
    OSStatus err;
    UInt32 flags;
    OTResult bytesReceived;
    char buffer[BUFFER_SIZE];
    char peerIPStr[16] = "unknown";
    TBind peerAddr;
    InetAddress peerInetAddr;
    int totalBytesReceived = 0;
    int receiveCount = 0;

    log_debug_cat(LOG_CAT_NETWORKING, "Handling incoming TCP data");

    /* Get peer IP address once */
    peerAddr.addr.buf = (UInt8*)&peerInetAddr;
    peerAddr.addr.maxlen = sizeof(InetAddress);
    err = OTGetProtAddress(endpoint, NULL, &peerAddr);
    if (err == noErr && peerAddr.addr.len > 0) {
        OTInetHostToString(peerInetAddr.fHost, peerIPStr);
    }

    /* Per NetworkingOpenTransport.txt: OTLook prioritizes T_ORDREL over T_DATA.
     * When sender uses OTSndOrderlyDisconnect after sending data, OTLook returns
     * T_ORDREL first, even if data is pending. We must try to receive data FIRST
     * before checking for disconnect events. */

    /* CRITICAL: Read until kOTNoDataErr to clear T_DATA event */
    do {
        bytesReceived = OTRcv(endpoint, buffer, sizeof(buffer) - 1, &flags);

        if (bytesReceived > 0) {
            receiveCount++;
            totalBytesReceived += bytesReceived;
            buffer[bytesReceived] = '\0'; /* Null terminate */
            log_debug_cat(LOG_CAT_MESSAGING, "Received TCP data chunk %d (%ld bytes): %s",
                          receiveCount, bytesReceived, buffer);

            /* Parse and process the message */
            ProcessIncomingMessage(buffer, peerIPStr);

        } else if (bytesReceived == kOTLookErr) {
            /* Async event pending - check what it is */
            OTResult lookResult = OTLook(endpoint);

            if (lookResult == T_DISCONNECT) {
                log_debug_cat(LOG_CAT_NETWORKING, "Connection aborted while receiving data");
                return;
            } else if (lookResult == T_ORDREL) {
                /* Orderly disconnect - sender finished sending.
                 * We should have already received all data, but check once more */
                log_debug_cat(LOG_CAT_NETWORKING, "Orderly disconnect received after %d bytes", totalBytesReceived);
                OTRcvOrderlyDisconnect(endpoint);
                return;
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "OTLook returned unexpected event: %ld", lookResult);
            }
            break;
        } else if (bytesReceived < 0 && bytesReceived != kOTNoDataErr) {
            log_error_cat(LOG_CAT_NETWORKING, "OTRcv failed: %ld", bytesReceived);
            break;
        }
    } while (bytesReceived != kOTNoDataErr);

    if (receiveCount > 1) {
        log_debug_cat(LOG_CAT_NETWORKING, "Received %d TCP chunks (%d total bytes) in one T_DATA event",
                      receiveCount, totalBytesReceived);
    }
}

/* Handle incoming UDP data
 * Per NetworkingOpenTransport.txt p.7439: "An endpoint does not receive any more
 * T_DATA events until its current T_DATA event is cleared." We MUST call OTRcvUData
 * in a loop until it returns kOTNoDataErr to fully clear the T_DATA event.
 */
void HandleIncomingUDPData(EndpointRef endpoint)
{
    OSStatus err;
    UInt32 flags;
    TUnitData unitData;
    InetAddress peerAddr;
    char buffer[BUFFER_SIZE];
    char peerIPStr[16];
    char myLocalIP[16];
    int datagramCount = 0;

    log_debug_cat(LOG_CAT_NETWORKING, "Handling incoming UDP discovery data");

    /* Get our local IP once to filter out our own messages */
    GetLocalIPAddress(myLocalIP, sizeof(myLocalIP));

    /* CRITICAL: Read until kOTNoDataErr to clear T_DATA event */
    do {
        /* Set up unit data structure */
        memset(&unitData, 0, sizeof(unitData));
        unitData.addr.buf = (UInt8*)&peerAddr;
        unitData.addr.maxlen = sizeof(InetAddress);
        unitData.udata.buf = (UInt8*)buffer;
        unitData.udata.maxlen = sizeof(buffer) - 1;

        /* Receive UDP data */
        err = OTRcvUData(endpoint, &unitData, &flags);

        if (err == noErr && unitData.udata.len > 0) {
            datagramCount++;
            buffer[unitData.udata.len] = '\0'; /* Null terminate */

            /* Convert peer address to string using OTInetHostToString */
            OTInetHostToString(peerAddr.fHost, peerIPStr);

            /* Ignore messages from ourselves */
            if (strcmp(peerIPStr, myLocalIP) == 0) {
                log_debug_cat(LOG_CAT_DISCOVERY, "Ignored UDP message from self (%s)", peerIPStr);
                continue;  /* Process next datagram */
            }

            log_debug_cat(LOG_CAT_DISCOVERY, "Received UDP data from %s (%ld bytes): %s",
                          peerIPStr, unitData.udata.len, buffer);

            /* Process discovery message using shared logic */
            ProcessIncomingUDPMessage(buffer, unitData.udata.len, peerIPStr, peerAddr.fHost, peerAddr.fPort);
        } else if (err != noErr && err != kOTNoDataErr) {
            log_error_cat(LOG_CAT_NETWORKING, "OTRcvUData failed: %ld", err);
            break;  /* Exit on error */
        }
    } while (err != kOTNoDataErr);

    if (datagramCount > 1) {
        log_debug_cat(LOG_CAT_NETWORKING, "Processed %d UDP datagrams in one T_DATA event", datagramCount);
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
        if (err == kOTFlowErr || err == kOTNoDataErr || err == -3158) {
            /* Flow control or buffer full - not a critical error, data will be dropped */
            log_debug_cat(LOG_CAT_NETWORKING, "OTSndUData flow-controlled (data dropped): %ld", err);
        } else {
            log_error_cat(LOG_CAT_NETWORKING, "OTSndUData failed: %ld", err);
        }
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

    /* Use orderly disconnect to ensure data is sent before closing connection
     * Per NetworkingOpenTransport.txt: "An orderly disconnect allows an endpoint
     * to send all data remaining in its send buffer before it breaks a connection."
     * OTSndDisconnect (abortive) would immediately tear down the connection,
     * potentially discarding unsent buffered data. */
    OTSndOrderlyDisconnect(ep);
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