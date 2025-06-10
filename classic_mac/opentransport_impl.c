//====================================
// FILE: ./classic_mac/opentransport_impl.c
//====================================

#include "opentransport_impl.h"
#include "../shared/logging.h"
#include "messaging.h" /* For ASR handler declarations */
#include <string.h>
#include <Errors.h>
#include <Gestalt.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>

/********************************************************************************
 *                                                                              *
 *   OpenTransport Notifier Implementation                                      *
 *                                                                              *
 ********************************************************************************/

/*
 * Global variables for Universal Procedure Pointers (UPPs) for our notifiers.
 * These are necessary so we can install them when creating endpoints
 * and remove them cleanly at shutdown.
 */
static OTNotifyUPP gOTNotifierUPP = NULL;
static OTNotifyUPP gOTPersistentListenerUPP = NULL;
static OTNotifyUPP gOTDataEndpointUPP = NULL;

/**
 * @brief The application's global OpenTransport notifier procedure.
 *
 * @param contextPtr    A context pointer provided by our app during installation. Unused for now.
 * @param code          The event code from OpenTransport (e.g., T_LISTEN, T_DATA).
 * @param result        The result of the event (e.g., kOTNoError).
 * @param cookie        A value specific to the event, often the EndpointRef that sourced it.
 *
 * This function is the heart of OpenTransport's asynchronous model. It is called by
 * the system at deferred task time to notify our application of a network event.
 *
 * Inside Macintosh Best Practice:
 * This function must be extremely fast and non-blocking. It must not call any
 * Toolbox function that could move or purge memory, as it runs in a restricted
 * context. Its primary job is to record the event and signal the main event
 * loop to process it later.
 */
pascal void OTNotifierProc(void *contextPtr, OTEventCode code, OTResult result, void *cookie)
{
    /* These parameters are unused in this basic implementation, but are essential. */
    (void)contextPtr;

    /*
     * For now, we will simply log the event. This is an essential debugging aid.
     * In a more advanced implementation, this notifier would set flags or post
     * custom events to the main event loop.
     */
    log_debug_cat(LOG_CAT_NETWORKING, "OT Notifier: code=0x%lX, result=0x%lX, cookie=0x%lX",
                  (unsigned long)code, (unsigned long)result, (unsigned long)cookie);

    /*
     * A T_UDERR_IND event is an unsolicited "unrecoverable error" indication.
     * It means an endpoint has experienced a fatal error and is now unusable.
     * It is critical to be aware of this possibility.
     */
    if (code == T_UDERR) {
        if ((EndpointRef)cookie != kOTInvalidEndpointRef) {
            log_warning_cat(LOG_CAT_NETWORKING, "OT Notifier: Received T_UDERR for endpoint 0x%lX. It is now idle due to a fatal error.", (unsigned long)cookie);
        }
    }
}

/* External declarations for TCP stream variables */
extern NetworkStreamRef gTCPListenStream;
extern NetworkStreamRef gTCPSendStream;

/* Global OpenTransport connection buffers - MUST use heap allocation per Apple docs */
static TCall *gOTRcvConnectCall = NULL;
static InetAddress *gOTRemoteAddr = NULL;
static Boolean gOTGlobalBuffersInitialized = false;

/* Asynchronous Factory Pattern: Connection Queue Management */
#define MAX_PENDING_CONNECTIONS 8
#define MAX_DATA_ENDPOINTS 4
#define CONNECTION_TIMEOUT_TICKS 1800  /* 30 seconds */

typedef enum {
    FACTORY_STATE_IDLE,
    FACTORY_STATE_CREATING_ENDPOINT,
    FACTORY_STATE_ACCEPTING_CONNECTION,
    FACTORY_STATE_READY,
    FACTORY_STATE_ERROR
} FactoryState;

typedef struct {
    TCall call;
    InetAddress clientAddr;
    Boolean isValid;
    UInt32 timestamp;
    short targetDataSlot;
} PendingConnection;

typedef struct {
    EndpointRef endpoint;
    Boolean isInUse;
    Boolean isConnected;
    ip_addr remoteHost;
    tcp_port remotePort;
    FactoryState state;
    UInt32 stateTimestamp;
    short connectionIndex;  /* Which pending connection this serves */
} DataEndpointSlot;

/* OpenTransport Factory Pattern: Persistent Listener + Managed Data Endpoints */
static EndpointRef gPersistentListener = kOTInvalidEndpointRef;
static PendingConnection gPendingConnections[MAX_PENDING_CONNECTIONS];
static DataEndpointSlot gDataEndpoints[MAX_DATA_ENDPOINTS];
static short gNextConnectionIndex = 0;
static short gActiveDataEndpoints = 0;
static Boolean gPersistentListenerInitialized = false;
static Boolean gFactoryInitialized = false;
static tcp_port gListenPort = 0;

/* Forward declarations for buffer management */
static OSErr InitializeOTGlobalBuffers(void);
static void CleanupOTGlobalBuffers(void);

/* Forward declarations for Asynchronous Factory Pattern */
static OSErr InitializePersistentListener(tcp_port localPort);
static OSErr InitializeAsyncFactory(void);
static void CleanupAsyncFactory(void);
static OSErr QueuePendingConnection(const TCall *call);
static OSErr CreateDataEndpointAsync(short slotIndex);
static short FindAvailableDataSlot(void);
static OSErr AcceptQueuedConnection(short dataSlotIndex);
static void CleanupDataEndpointSlot(short slotIndex);
static OSErr ProcessPendingConnections(void);
static void CleanupPersistentListener(void);
static void TimeoutOldConnections(void);
static pascal void OTPersistentListenerNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie);
static pascal void OTDataEndpointNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie);

/* OpenTransport async operation types for UDP */
typedef enum {
    OT_ASYNC_UDP_SEND,
    OT_ASYNC_UDP_RECEIVE
} OTAsyncOpType;

/* OpenTransport async operation types for TCP */
typedef enum {
    OT_ASYNC_TCP_CONNECT,
    OT_ASYNC_TCP_LISTEN,
    OT_ASYNC_TCP_SEND,
    OT_ASYNC_TCP_RECEIVE,
    OT_ASYNC_TCP_CLOSE
} OTTCPAsyncOpType;

/* OpenTransport UDP async operation tracking */
typedef struct {
    Boolean inUse;
    EndpointRef endpoint;
    OTAsyncOpType opType;
    OSStatus result;
    Boolean completed;
    void *userData;
} OTAsyncOperation;

/* OpenTransport TCP async operation tracking */
typedef struct {
    Boolean inUse;
    EndpointRef endpoint;
    OTTCPAsyncOpType opType;
    OSStatus result;
    Boolean completed;
    void *userData;
    NetworkStreamRef stream;
    Ptr dataBuffer;           /* For send operations */
    unsigned short dataLength;
} OTTCPAsyncOperation;

#define MAX_OT_ASYNC_OPS 32
#define MAX_OT_TCP_ASYNC_OPS 16
static OTAsyncOperation gOTAsyncOps[MAX_OT_ASYNC_OPS];  /* UDP operations */
static OTTCPAsyncOperation gOTTCPAsyncOps[MAX_OT_TCP_ASYNC_OPS];  /* TCP operations */
static Boolean gOTAsyncOpsInitialized = false;
static Boolean gOTTCPAsyncOpsInitialized = false;

/* OpenTransport endpoint structures for TCP and UDP */
typedef struct {
    EndpointRef endpoint;
    Boolean isConnected;
    Boolean isListening;
    ip_addr remoteHost;
    tcp_port remotePort;
    tcp_port localPort;
    Ptr receiveBuffer;
    unsigned long bufferSize;
} OTTCPEndpoint;

typedef struct {
    EndpointRef endpoint;
    udp_port localPort;
    Ptr receiveBuffer;
    unsigned short bufferSize;
    Boolean isCreated;
} OTUDPEndpoint;

/* Global OpenTransport state */
static Boolean gOTInitialized = false;

/* Async operation management - UDP */
static void InitializeOTAsyncOps(void);
static NetworkAsyncHandle AllocateOTAsyncHandle(void);
static void FreeOTAsyncHandle(NetworkAsyncHandle handle);
static void CleanupCompletedAsyncOps(void);

/* Async operation management - TCP */
static void InitializeOTTCPAsyncOps(void);
static NetworkAsyncHandle AllocateOTTCPAsyncHandle(void);
static void FreeOTTCPAsyncHandle(NetworkAsyncHandle handle);

/* OpenTransport notifier procedures */
static pascal void OTTCPNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie);
static pascal void OTUDPNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie);

/* Forward declarations for all OpenTransport implementation functions */
static OSErr OTImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr);
static void OTImpl_Shutdown(short refNum);

/* TCP Operations */
static OSErr OTImpl_TCPCreate(short refNum, NetworkStreamRef *streamRef,
                              unsigned long rcvBufferSize, Ptr rcvBuffer,
                              NetworkNotifyProcPtr notifyProc);
static OSErr OTImpl_TCPRelease(short refNum, NetworkStreamRef streamRef);
static OSErr OTImpl_TCPListen(NetworkStreamRef streamRef, tcp_port localPort,
                              Byte timeout, Boolean async);
static OSErr OTImpl_TCPAcceptConnection(NetworkStreamRef listenerRef, NetworkStreamRef *dataStreamRef,
                                        ip_addr *remoteHost, tcp_port *remotePort);
static OSErr OTImpl_TCPConnect(NetworkStreamRef streamRef, ip_addr remoteHost,
                               tcp_port remotePort, Byte timeout,
                               NetworkGiveTimeProcPtr giveTime);
static OSErr OTImpl_TCPSend(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                            Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime);
static OSErr OTImpl_TCPReceiveNoCopy(NetworkStreamRef streamRef, Ptr rdsPtr,
                                     short maxEntries, Byte timeout,
                                     Boolean *urgent, Boolean *mark,
                                     NetworkGiveTimeProcPtr giveTime);
static OSErr OTImpl_TCPReturnBuffer(NetworkStreamRef streamRef, Ptr rdsPtr,
                                    NetworkGiveTimeProcPtr giveTime);
static OSErr OTImpl_TCPClose(NetworkStreamRef streamRef, Byte timeout,
                             NetworkGiveTimeProcPtr giveTime);
static OSErr OTImpl_TCPAbort(NetworkStreamRef streamRef);
static OSErr OTImpl_TCPStatus(NetworkStreamRef streamRef, NetworkTCPInfo *info);

/* Async TCP Operations */
static OSErr OTImpl_TCPListenAsync(NetworkStreamRef streamRef, tcp_port localPort,
                                   NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_TCPConnectAsync(NetworkStreamRef streamRef, ip_addr remoteHost,
                                    tcp_port remotePort, NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_TCPSendAsync(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                 Boolean push, NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_TCPReceiveAsync(NetworkStreamRef streamRef, Ptr rdsPtr,
                                    short maxEntries, NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_TCPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
                                        OSErr *operationResult, void **resultData);
static void OTImpl_TCPCancelAsync(NetworkAsyncHandle asyncHandle);

/* UDP Operations */
static OSErr OTImpl_UDPCreate(short refNum, NetworkEndpointRef *endpointRef,
                              udp_port localPort, Ptr recvBuffer,
                              unsigned short bufferSize);
static OSErr OTImpl_UDPRelease(short refNum, NetworkEndpointRef endpointRef);
static OSErr OTImpl_UDPSend(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                            udp_port remotePort, Ptr data, unsigned short length);
static OSErr OTImpl_UDPReceive(NetworkEndpointRef endpointRef, ip_addr *remoteHost,
                               udp_port *remotePort, Ptr buffer,
                               unsigned short *length, Boolean async);
static OSErr OTImpl_UDPReturnBuffer(NetworkEndpointRef endpointRef, Ptr buffer,
                                    unsigned short bufferSize, Boolean async);

/* Async UDP Operations */
static OSErr OTImpl_UDPSendAsync(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                 udp_port remotePort, Ptr data, unsigned short length,
                                 NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_UDPCheckSendStatus(NetworkAsyncHandle asyncHandle);
static OSErr OTImpl_UDPReceiveAsync(NetworkEndpointRef endpointRef,
                                    NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_UDPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
                                        ip_addr *remoteHost, udp_port *remotePort,
                                        Ptr *dataPtr, unsigned short *dataLength);
static OSErr OTImpl_UDPReturnBufferAsync(NetworkEndpointRef endpointRef,
        Ptr buffer, unsigned short bufferSize,
        NetworkAsyncHandle *asyncHandle);
static OSErr OTImpl_UDPCheckReturnStatus(NetworkAsyncHandle asyncHandle);
static void OTImpl_UDPCancelAsync(NetworkAsyncHandle asyncHandle);
static void OTImpl_FreeAsyncHandle(NetworkAsyncHandle asyncHandle);

/* Utility Operations */
static OSErr OTImpl_ResolveAddress(const char *hostname, ip_addr *address);
static OSErr OTImpl_AddressToString(ip_addr address, char *addressStr);
static const char *OTImpl_GetImplementationName(void);
static Boolean OTImpl_IsAvailable(void);

/* Implementation functions */

/* Async operation management - implementation */
static void InitializeOTAsyncOps(void)
{
    int i;

    if (gOTAsyncOpsInitialized) {
        return;
    }

    for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
        gOTAsyncOps[i].inUse = false;
        gOTAsyncOps[i].endpoint = kOTInvalidEndpointRef;
        gOTAsyncOps[i].completed = false;
        gOTAsyncOps[i].result = noErr;
        gOTAsyncOps[i].userData = NULL;
    }

    gOTAsyncOpsInitialized = true;
}

static NetworkAsyncHandle AllocateOTAsyncHandle(void)
{
    int i;

    InitializeOTAsyncOps();

    for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
        if (!gOTAsyncOps[i].inUse) {
            gOTAsyncOps[i].inUse = true;
            gOTAsyncOps[i].completed = false;
            gOTAsyncOps[i].result = noErr;
            return (NetworkAsyncHandle)&gOTAsyncOps[i];
        }
    }

    /* Emergency cleanup and retry once */
    log_debug_cat(LOG_CAT_NETWORKING, "AllocateOTAsyncHandle: No free async operation slots, attempting cleanup");
    CleanupCompletedAsyncOps();

    for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
        if (!gOTAsyncOps[i].inUse) {
            gOTAsyncOps[i].inUse = true;
            gOTAsyncOps[i].completed = false;
            gOTAsyncOps[i].result = noErr;
            log_debug_cat(LOG_CAT_NETWORKING, "AllocateOTAsyncHandle: Recovered slot %d after cleanup", i);
            return (NetworkAsyncHandle)&gOTAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateOTAsyncHandle: No free async operation slots");
    return NULL;
}

static void FreeOTAsyncHandle(NetworkAsyncHandle handle)
{
    OTAsyncOperation *op = (OTAsyncOperation *)handle;

    if (op >= &gOTAsyncOps[0] && op < &gOTAsyncOps[MAX_OT_ASYNC_OPS]) {
        op->inUse = false;
        op->endpoint = kOTInvalidEndpointRef;
        op->completed = false;
        op->result = noErr;
        op->userData = NULL;
    }
}

/* TCP Async operation management - implementation */
static void InitializeOTTCPAsyncOps(void)
{
    int i;

    if (gOTTCPAsyncOpsInitialized) {
        return;
    }

    for (i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
        gOTTCPAsyncOps[i].inUse = false;
        gOTTCPAsyncOps[i].endpoint = kOTInvalidEndpointRef;
        gOTTCPAsyncOps[i].completed = false;
        gOTTCPAsyncOps[i].result = noErr;
        gOTTCPAsyncOps[i].userData = NULL;
        gOTTCPAsyncOps[i].stream = NULL;
        gOTTCPAsyncOps[i].dataBuffer = NULL;
        gOTTCPAsyncOps[i].dataLength = 0;
    }

    gOTTCPAsyncOpsInitialized = true;
}

static NetworkAsyncHandle AllocateOTTCPAsyncHandle(void)
{
    int i;

    InitializeOTTCPAsyncOps();

    for (i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
        if (!gOTTCPAsyncOps[i].inUse) {
            gOTTCPAsyncOps[i].inUse = true;
            gOTTCPAsyncOps[i].completed = false;
            gOTTCPAsyncOps[i].result = noErr;
            return (NetworkAsyncHandle)&gOTTCPAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateOTTCPAsyncHandle: No free TCP async operation slots");
    return NULL;
}

static void FreeOTTCPAsyncHandle(NetworkAsyncHandle handle)
{
    OTTCPAsyncOperation *op = (OTTCPAsyncOperation *)handle;

    if (op >= &gOTTCPAsyncOps[0] && op < &gOTTCPAsyncOps[MAX_OT_TCP_ASYNC_OPS]) {
        op->inUse = false;
        op->endpoint = kOTInvalidEndpointRef;
        op->completed = false;
        op->result = noErr;
        op->userData = NULL;
        op->stream = NULL;
        op->dataBuffer = NULL;
        op->dataLength = 0;
    }
}

/* OpenTransport notifier for TCP endpoints */
static pascal void OTTCPNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)contextPtr;
    int i;
    (void)cookie; /* Unused */

    if (tcpEp == NULL) {
        return;
    }

    switch (code) {
    case T_CONNECT:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_CONNECT result=%d", result);
        if (result == kOTNoError) {
            /* CRITICAL FIX: Call OTRcvConnect to complete the connection establishment.
             * This transitions the endpoint from T_OUTCON to T_DATAXFER state.
             * Without this call, the endpoint remains stuck in T_OUTCON and cannot send data.
             * APPLE DOCS: "For asynchronous endpoints, the OTRcvConnect function must be called to complete the handshake."
             */
            OSStatus rcvErr;

            /* Use global heap-allocated buffers per Apple documentation.
             * Stack variables in notifier context cause memory corruption. */
            if (gOTGlobalBuffersInitialized && gOTRcvConnectCall != NULL) {
                /* Reset buffer length for reuse */
                gOTRcvConnectCall->addr.len = 0;
                gOTRcvConnectCall->opt.len = 0;
                gOTRcvConnectCall->udata.len = 0;

                rcvErr = OTRcvConnect(tcpEp->endpoint, gOTRcvConnectCall);
            } else {
                log_error_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Global buffers not initialized, cannot call OTRcvConnect");
                rcvErr = kOTBadSequenceErr;
            }
            if (rcvErr == noErr) {
                tcpEp->isConnected = true;
                log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: OTRcvConnect successful, endpoint ready for data transfer");
            } else {
                log_error_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: OTRcvConnect failed: %d", rcvErr);
                tcpEp->isConnected = false;
                result = rcvErr; /* Propagate the error to async operation */
            }
        }
        /* Complete any pending connect operations */
        for (i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
            if (gOTTCPAsyncOps[i].inUse &&
                    gOTTCPAsyncOps[i].endpoint == tcpEp->endpoint &&
                    gOTTCPAsyncOps[i].opType == OT_ASYNC_TCP_CONNECT &&
                    !gOTTCPAsyncOps[i].completed) {
                gOTTCPAsyncOps[i].completed = true;
                gOTTCPAsyncOps[i].result = result;
                break;
            }
        }
        break;

    case T_LISTEN:
        /* DEPRECATED: This case is no longer used with factory pattern */
        /* All T_LISTEN events are now handled by OTPersistentListenerNotifier */
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_LISTEN on old endpoint (should not happen with factory pattern)");
        break;

    case T_DATA:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_DATA - data available");
        /* Complete any pending receive operations */
        for (i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
            if (gOTTCPAsyncOps[i].inUse &&
                    gOTTCPAsyncOps[i].endpoint == tcpEp->endpoint &&
                    gOTTCPAsyncOps[i].opType == OT_ASYNC_TCP_RECEIVE &&
                    !gOTTCPAsyncOps[i].completed) {
                gOTTCPAsyncOps[i].completed = true;
                gOTTCPAsyncOps[i].result = result;
                break;
            }
        }
        break;

    case T_GODATA:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_GODATA - can send data");
        break;

    case T_DISCONNECT:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_DISCONNECT");
        tcpEp->isConnected = false;
        break;

    case T_ORDREL:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_ORDREL - orderly release");
        {
            /* Handle orderly release from peer */
            OSStatus rcvDisconnErr, sndDisconnErr;

            /* Acknowledge the orderly release from peer */
            rcvDisconnErr = OTRcvOrderlyDisconnect(tcpEp->endpoint);
            if (rcvDisconnErr == noErr) {
                log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Acknowledged peer's orderly release");

                /* Send our own orderly release to complete the handshake */
                sndDisconnErr = OTSndOrderlyDisconnect(tcpEp->endpoint);
                if (sndDisconnErr == noErr) {
                    log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Sent orderly release to peer");
                } else {
                    log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Failed to send orderly release: %d", sndDisconnErr);
                }
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Failed to acknowledge orderly release: %d", rcvDisconnErr);
            }

            /* Mark connection as closed */
            tcpEp->isConnected = false;

            /* For OpenTransport, we need to simulate the MacTCP ASR event */
            /* Call the appropriate ASR handler directly since we're in the same address space */
            if (tcpEp == (OTTCPEndpoint *)gTCPListenStream) {
                /* This is the listen stream - generate TCPClosing event */
                TCP_Listen_ASR_Handler((StreamPtr)tcpEp, TCPClosing, NULL, 0, NULL);
            } else if (tcpEp == (OTTCPEndpoint *)gTCPSendStream) {
                /* This is the send stream - generate TCPTerminate event */
                TCP_Send_ASR_Handler((StreamPtr)tcpEp, TCPTerminate, NULL, 0, NULL);
            }
        }
        break;

    default:
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Unhandled event 0x%08lX", code);
        break;
    }
}

/* OpenTransport notifier for UDP endpoints */
static pascal void OTUDPNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)contextPtr;
    int i;
    (void)cookie; /* Unused */

    if (udpEp == NULL) {
        return;
    }

    switch (code) {
    case T_DATA:
        log_debug_cat(LOG_CAT_NETWORKING, "OTUDPNotifier: T_DATA - datagram available, result=%d", result);
        /* Complete any pending receive operations */
        for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
            if (gOTAsyncOps[i].inUse &&
                    gOTAsyncOps[i].endpoint == udpEp->endpoint &&
                    gOTAsyncOps[i].opType == OT_ASYNC_UDP_RECEIVE &&
                    !gOTAsyncOps[i].completed) {
                gOTAsyncOps[i].completed = true;
                gOTAsyncOps[i].result = result;
                log_debug_cat(LOG_CAT_NETWORKING, "OTUDPNotifier: Marked async receive operation %d as completed", i);
                break;
            }
        }
        break;

    case T_UDERR:
        log_debug_cat(LOG_CAT_NETWORKING, "OTUDPNotifier: T_UDERR result=%d", result);
        break;

    default:
        log_debug_cat(LOG_CAT_NETWORKING, "OTUDPNotifier: Unhandled event 0x%08lX", code);
        break;
    }
}

/* Asynchronous Factory Pattern: Persistent Listener Notifier */
static pascal void OTPersistentListenerNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie)
{
    (void)contextPtr;
    (void)result;
    (void)cookie; /* Unused */

    switch (code) {
    case T_LISTEN:
        log_debug_cat(LOG_CAT_NETWORKING, "OTPersistentListenerNotifier: T_LISTEN - incoming connection");
        {
            /* Asynchronous Factory Pattern: Queue connection and process asynchronously */
            TCall call;
            InetAddress clientAddr;
            OSStatus listenErr;

            /* Set up call structure to receive connection details */
            call.addr.buf = (UInt8 *)&clientAddr;
            call.addr.maxlen = sizeof(clientAddr);
            call.opt.buf = NULL;
            call.opt.maxlen = 0;
            call.udata.buf = NULL;
            call.udata.maxlen = 0;

            /* Get the connection request details from persistent listener */
            listenErr = OTListen(gPersistentListener, &call);
            if (listenErr == noErr) {
                /* CRITICAL FIX: Queue connection instead of creating endpoint synchronously */
                OSErr queueErr = QueuePendingConnection(&call);
                if (queueErr == noErr) {
                    log_info_cat(LOG_CAT_NETWORKING, "OTPersistentListenerNotifier: Queued connection from %d.%d.%d.%d:%d",
                                 (clientAddr.fHost >> 24) & 0xFF,
                                 (clientAddr.fHost >> 16) & 0xFF,
                                 (clientAddr.fHost >> 8) & 0xFF,
                                 clientAddr.fHost & 0xFF,
                                 clientAddr.fPort);

                    /* Process pending connections asynchronously */
                    ProcessPendingConnections();
                } else {
                    log_error_cat(LOG_CAT_NETWORKING, "OTPersistentListenerNotifier: Failed to queue connection: %d", queueErr);
                    /* Reject connection if queue is full */
                    OTSndDisconnect(gPersistentListener, &call);
                }
            } else {
                log_error_cat(LOG_CAT_NETWORKING, "OTPersistentListenerNotifier: OTListen failed: %d", listenErr);
            }

            /* Timeout old connections periodically */
            TimeoutOldConnections();

            /* CRITICAL: Persistent listener remains in T_LISTEN state, ready for next connection */
        }
        break;

    default:
        log_debug_cat(LOG_CAT_NETWORKING, "OTPersistentListenerNotifier: Unhandled event 0x%08lX", code);
        break;
    }
}

/* Asynchronous Factory Pattern: Data Endpoint Notifier */
static pascal void OTDataEndpointNotifier(void *contextPtr, OTEventCode code, OTResult result, void *cookie)
{
    short slotIndex = (short)(long)cookie; /* Slot index passed during OTAsyncOpenEndpoint */
    (void)contextPtr; /* Unused */

    switch (code) {
    case T_OPENCOMPLETE:
        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: T_OPENCOMPLETE - async endpoint creation completed");
        {
            /* CRITICAL: This is the completion of OTAsyncOpenEndpoint */
            EndpointRef newEndpoint = (EndpointRef)result;

            if (slotIndex >= 0 && slotIndex < MAX_DATA_ENDPOINTS) {
                DataEndpointSlot *slot = &gDataEndpoints[slotIndex];

                if (newEndpoint != kOTInvalidEndpointRef && result >= 0) {
                    /* Endpoint creation succeeded */
                    slot->endpoint = newEndpoint;
                    slot->state = FACTORY_STATE_READY;
                    slot->stateTimestamp = TickCount();

                    log_info_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Data endpoint created successfully in slot %d", slotIndex);

                    /* Set endpoint to asynchronous mode */
                    OSErr asyncErr = OTSetAsynchronous(newEndpoint);
                    if (asyncErr != noErr) {
                        log_error_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: OTSetAsynchronous failed for slot %d: %d", slotIndex, asyncErr);
                        slot->state = FACTORY_STATE_ERROR;
                    } else {
                        /* Now we can accept a queued connection on this endpoint */
                        OSErr acceptErr = AcceptQueuedConnection(slotIndex);
                        if (acceptErr == noErr) {
                            log_info_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Connection accepted on slot %d", slotIndex);

                            /* Generate Listen ASR event to signal connection availability */
                            if (gTCPListenStream != NULL) {
                                OTTCPEndpoint *listenerEp = (OTTCPEndpoint *)gTCPListenStream;
                                /* Update endpoint reference for legacy compatibility */
                                EndpointRef oldEndpoint = listenerEp->endpoint;
                                listenerEp->endpoint = slot->endpoint;
                                listenerEp->isConnected = true;

                                /* Signal connection arrival to messaging layer */
                                TCP_Listen_ASR_Handler((StreamPtr)listenerEp, TCPDataArrival, NULL, 0, NULL);

                                /* Restore listener endpoint for future operations */
                                listenerEp->endpoint = oldEndpoint;
                            }
                        } else if (acceptErr != kOTNoDataErr) {
                            log_error_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Failed to accept connection on slot %d: %d", slotIndex, acceptErr);
                            slot->state = FACTORY_STATE_ERROR;
                        }
                    }
                } else {
                    /* Endpoint creation failed */
                    log_error_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Async endpoint creation failed for slot %d: %d", slotIndex, result);
                    slot->state = FACTORY_STATE_ERROR;
                    slot->endpoint = kOTInvalidEndpointRef;
                }
            } else {
                log_error_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Invalid slot index %d in T_OPENCOMPLETE", slotIndex);
            }
        }
        break;

    case T_DATA:
        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: T_DATA - data available on data endpoint");
        /* Forward to messaging layer for data processing */
        if (slotIndex >= 0 && slotIndex < MAX_DATA_ENDPOINTS && gTCPListenStream != NULL) {
            DataEndpointSlot *slot = &gDataEndpoints[slotIndex];
            OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)gTCPListenStream;

            /* Temporarily switch to data endpoint for message processing */
            EndpointRef oldEndpoint = tcpEp->endpoint;
            tcpEp->endpoint = slot->endpoint;
            tcpEp->isConnected = slot->isConnected;

            /* Process incoming data */
            TCP_Listen_ASR_Handler((StreamPtr)tcpEp, TCPDataArrival, NULL, 0, NULL);

            /* Restore listener endpoint */
            tcpEp->endpoint = oldEndpoint;
        }
        break;

    case T_ORDREL:
        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: T_ORDREL - orderly release on slot %d", slotIndex);
        /* Handle connection close on data endpoint */
        if (slotIndex >= 0 && slotIndex < MAX_DATA_ENDPOINTS) {
            DataEndpointSlot *slot = &gDataEndpoints[slotIndex];

            if (slot->endpoint != kOTInvalidEndpointRef) {
                OSErr rcvDisconnErr = OTRcvOrderlyDisconnect(slot->endpoint);
                if (rcvDisconnErr == noErr) {
                    log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Acknowledged peer's orderly release on slot %d", slotIndex);
                    OSErr sndDisconnErr = OTSndOrderlyDisconnect(slot->endpoint);
                    if (sndDisconnErr == noErr) {
                        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Sent orderly release to peer on slot %d", slotIndex);
                    } else {
                        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Failed to send orderly release on slot %d: %d", slotIndex, sndDisconnErr);
                    }
                } else {
                    log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Failed to acknowledge orderly release on slot %d: %d", slotIndex, rcvDisconnErr);
                }

                /* Generate ASR event for messaging layer */
                if (gTCPListenStream != NULL) {
                    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)gTCPListenStream;
                    EndpointRef oldEndpoint = tcpEp->endpoint;

                    /* Temporarily switch to data endpoint */
                    tcpEp->endpoint = slot->endpoint;
                    tcpEp->isConnected = false;

                    /* Signal connection close */
                    TCP_Listen_ASR_Handler((StreamPtr)tcpEp, TCPClosing, NULL, 0, NULL);

                    /* Restore listener endpoint */
                    tcpEp->endpoint = oldEndpoint;
                }

                /* Clean up this specific data endpoint slot */
                CleanupDataEndpointSlot(slotIndex);
            }
        }
        break;

    case T_DISCONNECT:
        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: T_DISCONNECT on slot %d", slotIndex);
        if (slotIndex >= 0 && slotIndex < MAX_DATA_ENDPOINTS) {
            CleanupDataEndpointSlot(slotIndex);
        }
        break;

    default:
        log_debug_cat(LOG_CAT_NETWORKING, "OTDataEndpointNotifier: Unhandled event 0x%08lX on slot %d", code, slotIndex);
        break;
    }
}

OSStatus InitializeOpenTransport(void)
{
    OSStatus err;

    log_info_cat(LOG_CAT_NETWORKING, "OT: Initializing OpenTransport stack...");

    /* Initialize OpenTransport if not already done */
    if (!gOTInitialized) {
        err = InitOpenTransport();
        if (err != kOTNoError) {
            log_error_cat(LOG_CAT_NETWORKING, "FATAL: InitOpenTransport() failed with error: %ld", (long)err);
            return err;
        }
        gOTInitialized = true;
        log_debug_cat(LOG_CAT_NETWORKING, "OT: InitOpenTransport() successful.");
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "OT: OpenTransport already initialized.");
    }

    /*
     * CRITICAL STEP 1: Create the Universal Procedure Pointers (UPPs).
     * These create callable pointers that OpenTransport can use to execute our notifiers.
     */
    if (gOTNotifierUPP == NULL) {
        gOTNotifierUPP = NewOTNotifyUPP(OTNotifierProc);
        if (gOTNotifierUPP == NULL) {
            log_error_cat(LOG_CAT_NETWORKING, "FATAL: NewOTNotifyUPP(OTNotifierProc) failed. Out of memory.");
            CloseOpenTransport(); /* Attempt to clean up */
            return memFullErr;
        }
    }

    if (gOTPersistentListenerUPP == NULL) {
        gOTPersistentListenerUPP = NewOTNotifyUPP(OTPersistentListenerNotifier);
        if (gOTPersistentListenerUPP == NULL) {
            log_error_cat(LOG_CAT_NETWORKING, "FATAL: NewOTNotifyUPP(OTPersistentListenerNotifier) failed. Out of memory.");
            if (gOTNotifierUPP) {
                DisposeOTNotifyUPP(gOTNotifierUPP);
            }
            gOTNotifierUPP = NULL;
            CloseOpenTransport(); /* Attempt to clean up */
            return memFullErr;
        }
    }

    if (gOTDataEndpointUPP == NULL) {
        gOTDataEndpointUPP = NewOTNotifyUPP(OTDataEndpointNotifier);
        if (gOTDataEndpointUPP == NULL) {
            log_error_cat(LOG_CAT_NETWORKING, "FATAL: NewOTNotifyUPP(OTDataEndpointNotifier) failed. Out of memory.");
            if (gOTNotifierUPP) {
                DisposeOTNotifyUPP(gOTNotifierUPP);
            }
            if (gOTPersistentListenerUPP) {
                DisposeOTNotifyUPP(gOTPersistentListenerUPP);
            }
            gOTNotifierUPP = NULL;
            gOTPersistentListenerUPP = NULL;
            CloseOpenTransport(); /* Attempt to clean up */
            return memFullErr;
        }
    }

    /*
     * Note: The notifier UPP is created here but will be installed per-endpoint
     * when endpoints are created. OpenTransport notifiers are installed per
     * provider/endpoint, not globally.
     */

    InitializeOTAsyncOps();
    InitializeOTTCPAsyncOps();

    log_info_cat(LOG_CAT_NETWORKING, "OT: Notifier UPPs created successfully. OpenTransport is ready.");
    return kOTNoError;
}

void ShutdownOpenTransport(void)
{
    if (gOTInitialized) {
        log_info_cat(LOG_CAT_NETWORKING, "OT: Shutting down OpenTransport stack...");

        /* Cancel all pending async operations before shutdown */
        InitializeOTAsyncOps();
        InitializeOTTCPAsyncOps();
        for (int i = 0; i < MAX_OT_ASYNC_OPS; i++) {
            if (gOTAsyncOps[i].inUse) {
                gOTAsyncOps[i].inUse = false;
                gOTAsyncOps[i].completed = true;
                gOTAsyncOps[i].result = kOTCanceledErr;
            }
        }
        for (int i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
            if (gOTTCPAsyncOps[i].inUse) {
                gOTTCPAsyncOps[i].inUse = false;
                gOTTCPAsyncOps[i].completed = true;
                gOTTCPAsyncOps[i].result = kOTCanceledErr;
            }
        }

        /* Reset initialization flags */
        gOTAsyncOpsInitialized = false;
        gOTTCPAsyncOpsInitialized = false;

        /*
         * Clean up the notifier UPPs.
         * Individual endpoints should have removed their notifiers when closed.
         */
        if (gOTDataEndpointUPP != NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "OT: Disposing Data Endpoint Notifier UPP...");
            DisposeOTNotifyUPP(gOTDataEndpointUPP);
            gOTDataEndpointUPP = NULL;
        }

        if (gOTPersistentListenerUPP != NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "OT: Disposing Persistent Listener Notifier UPP...");
            DisposeOTNotifyUPP(gOTPersistentListenerUPP);
            gOTPersistentListenerUPP = NULL;
        }

        if (gOTNotifierUPP != NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "OT: Disposing General Notifier UPP...");
            DisposeOTNotifyUPP(gOTNotifierUPP);
            gOTNotifierUPP = NULL;
        }

        /* Finally, close the OpenTransport library itself. */
        log_debug_cat(LOG_CAT_NETWORKING, "OT: Calling CloseOpenTransport()...");
        CloseOpenTransport();
        gOTInitialized = false;

        log_info_cat(LOG_CAT_NETWORKING, "OT: Shutdown complete.");
    }
}

static OSErr OTImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr)
{
    OSStatus err;
    InetInterfaceInfo info;

    err = InitializeOpenTransport();
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: OpenTransport initialization failed: %d", err);
        return err;
    }

    /* Get local IP address */
    /* Note: OTInetGetInterfaceInfo may fail if TCP/IP stack isn't loaded yet.
     * According to Apple docs: "If Open Transport TCP/IP has not yet been loaded into memory,
     * the OTInetGetInterfaceInfo function returns no valid interfaces."
     * Solution: Force TCP/IP loading by creating a dummy endpoint first.
     */

    err = OTInetGetInterfaceInfo(&info, kDefaultInetInterface);
    if (err != noErr || info.fAddress == 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: First OTInetGetInterfaceInfo failed (%d) or returned 0.0.0.0, forcing TCP/IP stack load", err);

        /* Force TCP/IP stack loading by creating a dummy endpoint */
        OTConfigurationRef config = OTCreateConfiguration(kTCPName);
        if (config != kOTInvalidConfigurationPtr && config != kOTNoMemoryConfigurationPtr && config != NULL) {
            TEndpointInfo endpointInfo;
            OSStatus dummyErr;
            EndpointRef dummyEndpoint = OTOpenEndpoint(config, 0, &endpointInfo, &dummyErr);
            if (dummyEndpoint != kOTInvalidEndpointRef) {
                /* TCP/IP stack should now be loaded, close the dummy endpoint */
                OTCloseProvider(dummyEndpoint);
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: TCP/IP stack loaded via dummy endpoint");

                /* Try getting interface info again */
                err = OTInetGetInterfaceInfo(&info, kDefaultInetInterface);
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Second OTInetGetInterfaceInfo attempt: err=%d, address=0x%08lX", err, (unsigned long)info.fAddress);
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Failed to create dummy endpoint for TCP/IP loading: %d", dummyErr);
            }
        } else {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Failed to create TCP configuration for stack loading");
        }
    }

    if (err == noErr && info.fAddress != 0 && localIP != NULL) {
        *localIP = info.fAddress;
        if (localIPStr != NULL) {
            OTInetHostToString(info.fAddress, localIPStr);
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Successfully got local IP: 0x%08lX", (unsigned long)info.fAddress);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Final OTInetGetInterfaceInfo failed: err=%d, address=0x%08lX", err, (unsigned long)info.fAddress);
        if (localIP != NULL) {
            *localIP = 0;
        }
        if (localIPStr != NULL) {
            strcpy(localIPStr, "0.0.0.0");
        }
    }

    if (refNum != NULL) {
        *refNum = 1; /* Dummy ref num for OpenTransport */
    }

    /* Initialize global buffers for OTRcvConnect operations */
    err = InitializeOTGlobalBuffers();
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: Failed to initialize global buffers: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: OpenTransport initialized");
    return noErr;
}

/* Initialize global OTRcvConnect buffers using heap allocation per Apple docs */
static OSErr InitializeOTGlobalBuffers(void)
{
    if (gOTGlobalBuffersInitialized) {
        return noErr; /* Already initialized */
    }

    /* Allocate TCall structure using OTAllocMem for notifier safety */
    gOTRcvConnectCall = (TCall *)OTAllocMem(sizeof(TCall));
    if (gOTRcvConnectCall == NULL) {
        log_error_cat(LOG_CAT_NETWORKING, "InitializeOTGlobalBuffers: Failed to allocate TCall structure");
        return memFullErr;
    }

    /* Allocate InetAddress buffer using OTAllocMem for notifier safety */
    gOTRemoteAddr = (InetAddress *)OTAllocMem(sizeof(InetAddress));
    if (gOTRemoteAddr == NULL) {
        log_error_cat(LOG_CAT_NETWORKING, "InitializeOTGlobalBuffers: Failed to allocate InetAddress buffer");
        OTFreeMem(gOTRcvConnectCall);
        gOTRcvConnectCall = NULL;
        return memFullErr;
    }

    /* Initialize TCall structure with proper buffer pointers */
    gOTRcvConnectCall->addr.buf = (UInt8 *)gOTRemoteAddr;
    gOTRcvConnectCall->addr.maxlen = sizeof(InetAddress);
    gOTRcvConnectCall->addr.len = 0;

    /* No options or user data for TCP connections per Apple docs */
    gOTRcvConnectCall->opt.buf = NULL;
    gOTRcvConnectCall->opt.maxlen = 0;
    gOTRcvConnectCall->opt.len = 0;

    gOTRcvConnectCall->udata.buf = NULL;    /* TCP doesn't support user data during connect */
    gOTRcvConnectCall->udata.maxlen = 0;    /* Must be 0 for TCP per OpenTransport spec */
    gOTRcvConnectCall->udata.len = 0;

    gOTGlobalBuffersInitialized = true;
    log_debug_cat(LOG_CAT_NETWORKING, "InitializeOTGlobalBuffers: Global OTRcvConnect buffers initialized using OTAllocMem");
    return noErr;
}

/* Cleanup global OTRcvConnect buffers */
static void CleanupOTGlobalBuffers(void)
{
    if (!gOTGlobalBuffersInitialized) {
        return; /* Not initialized */
    }

    if (gOTRemoteAddr != NULL) {
        OTFreeMem(gOTRemoteAddr);
        gOTRemoteAddr = NULL;
    }

    if (gOTRcvConnectCall != NULL) {
        OTFreeMem(gOTRcvConnectCall);
        gOTRcvConnectCall = NULL;
    }

    gOTGlobalBuffersInitialized = false;
    log_debug_cat(LOG_CAT_NETWORKING, "CleanupOTGlobalBuffers: Global OTRcvConnect buffers cleaned up");
}

/* OpenTransport Factory Pattern Implementation */

/* Initialize persistent listener that lives forever */
static OSErr InitializePersistentListener(tcp_port localPort)
{
    OSStatus err;
    InetAddress addr;
    TBind reqAddr, retAddr;
    TCall call;

    if (gPersistentListenerInitialized) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: Already initialized");
        return noErr;
    }

    /* Create TCP endpoint for persistent listener */
    {
        OTConfigurationRef config = OTCreateConfiguration(kTCPName);
        if (config == kOTInvalidConfigurationPtr || config == kOTNoMemoryConfigurationPtr || config == NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: Failed to create TCP configuration");
            return kOTBadConfigurationErr;
        }

        gPersistentListener = OTOpenEndpoint(config, 0, NULL, &err);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: OTOpenEndpoint failed: %d", err);
            return err;
        }
    }

    /* Install persistent listener notifier */
    err = OTInstallNotifier(gPersistentListener, gOTPersistentListenerUPP, NULL);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: OTInstallNotifier failed: %d", err);
        OTCloseProvider(gPersistentListener);
        gPersistentListener = kOTInvalidEndpointRef;
        return err;
    }

    /* Set endpoint to async mode */
    err = OTSetAsynchronous(gPersistentListener);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: OTSetAsynchronous failed: %d", err);
        OTCloseProvider(gPersistentListener);
        gPersistentListener = kOTInvalidEndpointRef;
        return err;
    }

    /* Set up local address for binding */
    OTInitInetAddress(&addr, localPort, kOTAnyInetAddress);

    /* Set up bind request */
    reqAddr.addr.buf = (UInt8 *)&addr;
    reqAddr.addr.len = sizeof(addr);
    reqAddr.qlen = 1; /* Listen queue length */

    retAddr.addr.buf = (UInt8 *)&addr;
    retAddr.addr.maxlen = sizeof(addr);

    /* Bind persistent listener to local address */
    err = OTBind(gPersistentListener, &reqAddr, &retAddr);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: OTBind failed: %d", err);
        OTCloseProvider(gPersistentListener);
        gPersistentListener = kOTInvalidEndpointRef;
        return err;
    }

    /* Set up call structure for OTListen */
    call.addr.buf = (UInt8 *)&addr;
    call.addr.maxlen = sizeof(addr);
    call.opt.buf = NULL;
    call.opt.maxlen = 0;
    call.udata.buf = NULL;
    call.udata.maxlen = 0;

    /* Start persistent listening - this endpoint will NEVER be destroyed */
    err = OTListen(gPersistentListener, &call);
    if (err != noErr && err != kOTNoDataErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: OTListen failed: %d", err);
        OTUnbind(gPersistentListener);
        OTCloseProvider(gPersistentListener);
        gPersistentListener = kOTInvalidEndpointRef;
        return err;
    }

    gListenPort = localPort;
    gPersistentListenerInitialized = true;

    /* Initialize the asynchronous factory pattern */
    OSErr factoryErr = InitializeAsyncFactory();
    if (factoryErr != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: Failed to initialize async factory: %d", factoryErr);
        CleanupPersistentListener();
        return factoryErr;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "InitializePersistentListener: Persistent listener created and listening on port %d", localPort);
    return noErr;
}

/* Initialize the Asynchronous Factory Pattern */
static OSErr InitializeAsyncFactory(void)
{
    short i;

    if (gFactoryInitialized) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializeAsyncFactory: Already initialized");
        return noErr;
    }

    /* Initialize pending connections queue */
    for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
        gPendingConnections[i].isValid = false;
        gPendingConnections[i].timestamp = 0;
        gPendingConnections[i].targetDataSlot = -1;
        gPendingConnections[i].call.addr.buf = (UInt8 *)&gPendingConnections[i].clientAddr;
        gPendingConnections[i].call.addr.maxlen = sizeof(InetAddress);
        gPendingConnections[i].call.opt.buf = NULL;
        gPendingConnections[i].call.opt.maxlen = 0;
        gPendingConnections[i].call.udata.buf = NULL;
        gPendingConnections[i].call.udata.maxlen = 0;
    }

    /* Initialize data endpoint slots */
    for (i = 0; i < MAX_DATA_ENDPOINTS; i++) {
        gDataEndpoints[i].endpoint = kOTInvalidEndpointRef;
        gDataEndpoints[i].isInUse = false;
        gDataEndpoints[i].isConnected = false;
        gDataEndpoints[i].remoteHost = 0;
        gDataEndpoints[i].remotePort = 0;
        gDataEndpoints[i].state = FACTORY_STATE_IDLE;
        gDataEndpoints[i].stateTimestamp = 0;
        gDataEndpoints[i].connectionIndex = -1;
    }

    gNextConnectionIndex = 0;
    gActiveDataEndpoints = 0;
    gFactoryInitialized = true;

    log_info_cat(LOG_CAT_NETWORKING, "Asynchronous Factory Pattern initialized successfully");
    return noErr;
}

/* Find an available data endpoint slot */
static short FindAvailableDataSlot(void)
{
    short i;

    for (i = 0; i < MAX_DATA_ENDPOINTS; i++) {
        if (!gDataEndpoints[i].isInUse) {
            return i;
        }
    }
    return -1; /* No available slots */
}


/* Queue an incoming connection for asynchronous processing */
static OSErr QueuePendingConnection(const TCall *call)
{
    short i;
    UInt32 currentTime = TickCount();

    /* Find available slot in pending connections queue */
    for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
        if (!gPendingConnections[i].isValid) {
            /* Copy connection information */
            gPendingConnections[i].call = *call;
            gPendingConnections[i].clientAddr = *(InetAddress *)call->addr.buf;
            gPendingConnections[i].isValid = true;
            gPendingConnections[i].timestamp = currentTime;
            gPendingConnections[i].targetDataSlot = -1;

            log_debug_cat(LOG_CAT_NETWORKING, "QueuePendingConnection: Queued connection from %d.%d.%d.%d:%d at slot %d",
                          (gPendingConnections[i].clientAddr.fHost >> 24) & 0xFF,
                          (gPendingConnections[i].clientAddr.fHost >> 16) & 0xFF,
                          (gPendingConnections[i].clientAddr.fHost >> 8) & 0xFF,
                          gPendingConnections[i].clientAddr.fHost & 0xFF,
                          gPendingConnections[i].clientAddr.fPort, i);

            return noErr;
        }
    }

    log_error_cat(LOG_CAT_NETWORKING, "QueuePendingConnection: Connection queue full - rejecting connection");
    return kOTQFullErr;
}

/* Create a data endpoint asynchronously */
static OSErr CreateDataEndpointAsync(short slotIndex)
{
    OSStatus err;
    DataEndpointSlot *slot;
    if (slotIndex < 0 || slotIndex >= MAX_DATA_ENDPOINTS) {
        return paramErr;
    }

    slot = &gDataEndpoints[slotIndex];

    /* Clean up any existing endpoint in this slot */
    if (slot->endpoint != kOTInvalidEndpointRef) {
        CleanupDataEndpointSlot(slotIndex);
    }

    /* Mark slot as in use and creating */
    slot->isInUse = true;
    slot->state = FACTORY_STATE_CREATING_ENDPOINT;
    slot->stateTimestamp = TickCount();

    /* Create TCP configuration */
    {
        OTConfigurationRef config = OTCreateConfiguration(kTCPName);
        if (config == kOTInvalidConfigurationPtr || config == kOTNoMemoryConfigurationPtr || config == NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "CreateDataEndpointAsync: Failed to create TCP configuration for slot %d", slotIndex);
                          slot->isInUse = false;
                          slot->state = FACTORY_STATE_ERROR;
                          return kOTBadConfigurationErr;
        }

        /* CRITICAL: Use OTAsyncOpenEndpoint instead of synchronous OTOpenEndpoint */
        /* This is the key fix - no more synchronous calls in notifier context! */
        err = OTAsyncOpenEndpoint(config, 0, NULL, gOTDataEndpointUPP, (void *)(long)slotIndex);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "CreateDataEndpointAsync: OTAsyncOpenEndpoint failed for slot %d: %d", slotIndex, err);
            slot->isInUse = false;
            slot->state = FACTORY_STATE_ERROR;
            return err;
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "CreateDataEndpointAsync: Initiated async endpoint creation for slot %d", slotIndex);
    return noErr;
}

/* Accept a queued connection on the specified data endpoint */
static OSErr AcceptQueuedConnection(short dataSlotIndex)
{
    DataEndpointSlot *slot;
    short connectionIndex;
    PendingConnection *connection;
    OSStatus err;

    if (dataSlotIndex < 0 || dataSlotIndex >= MAX_DATA_ENDPOINTS) {
        return paramErr;
    }

    slot = &gDataEndpoints[dataSlotIndex];

    if (slot->endpoint == kOTInvalidEndpointRef || slot->state != FACTORY_STATE_READY) {
        log_error_cat(LOG_CAT_NETWORKING, "AcceptQueuedConnection: Slot %d not ready for connection", dataSlotIndex);
        return kOTStateChangeErr;
    }

    /* Find the oldest pending connection */
    connectionIndex = -1;
    {
        UInt32 oldestTime = 0xFFFFFFFF;
        short i;

        for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
            if (gPendingConnections[i].isValid && gPendingConnections[i].targetDataSlot == -1) {
                if (gPendingConnections[i].timestamp < oldestTime) {
                    oldestTime = gPendingConnections[i].timestamp;
                    connectionIndex = i;
                }
            }
        }
    }

    if (connectionIndex == -1) {
        log_debug_cat(LOG_CAT_NETWORKING, "AcceptQueuedConnection: No pending connections to accept");
        return kOTNoDataErr;
    }

    connection = &gPendingConnections[connectionIndex];

    /* Accept the connection on the data endpoint */
    err = OTAccept(gPersistentListener, slot->endpoint, &connection->call);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "AcceptQueuedConnection: OTAccept failed for slot %d: %d", dataSlotIndex, err);
        return err;
    }

    /* Update slot and connection state */
    slot->state = FACTORY_STATE_ACCEPTING_CONNECTION;
    slot->isConnected = true;
    slot->remoteHost = connection->clientAddr.fHost;
    slot->remotePort = connection->clientAddr.fPort;
    slot->connectionIndex = connectionIndex;
    connection->targetDataSlot = dataSlotIndex;

    log_info_cat(LOG_CAT_NETWORKING, "AcceptQueuedConnection: Accepted connection from %d.%d.%d.%d:%d on slot %d",
                 (connection->clientAddr.fHost >> 24) & 0xFF,
                 (connection->clientAddr.fHost >> 16) & 0xFF,
                 (connection->clientAddr.fHost >> 8) & 0xFF,
                 connection->clientAddr.fHost & 0xFF,
                 connection->clientAddr.fPort, dataSlotIndex);

    /* Clear the pending connection */
    connection->isValid = false;
    connection->targetDataSlot = -1;

    return noErr;
}

/* Process all pending connections */
static OSErr ProcessPendingConnections(void)
{
    short availableSlot;
    OSErr err;

    if (!gFactoryInitialized) {
        return kOTStateChangeErr;
    }

    /* Check if we have pending connections and available slots */
    while ((availableSlot = FindAvailableDataSlot()) != -1) {
        Boolean hasPendingConnection = false;
        short i;

        /* Check for pending connections */
        for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
            if (gPendingConnections[i].isValid && gPendingConnections[i].targetDataSlot == -1) {
                hasPendingConnection = true;
                break;
            }
        }

        if (!hasPendingConnection) {
            break; /* No pending connections */
        }

        /* Create data endpoint asynchronously for this connection */
        err = CreateDataEndpointAsync(availableSlot);
        if (err != noErr) {
            log_error_cat(LOG_CAT_NETWORKING, "ProcessPendingConnections: Failed to create endpoint for slot %d: %d", availableSlot, err);
            break;
        }

        gActiveDataEndpoints++;
    }

    return noErr;
}

/* Clean up a specific data endpoint slot */
static void CleanupDataEndpointSlot(short slotIndex)
{
    DataEndpointSlot *slot;

    if (slotIndex < 0 || slotIndex >= MAX_DATA_ENDPOINTS) {
        return;
    }

    slot = &gDataEndpoints[slotIndex];

    if (slot->endpoint != kOTInvalidEndpointRef) {
        log_debug_cat(LOG_CAT_NETWORKING, "CleanupDataEndpointSlot: Cleaning up slot %d", slotIndex);

        /* Close the endpoint */
        OTCloseProvider(slot->endpoint);
        slot->endpoint = kOTInvalidEndpointRef;

        /* Clear associated pending connection if any */
        if (slot->connectionIndex >= 0 && slot->connectionIndex < MAX_PENDING_CONNECTIONS) {
            gPendingConnections[slot->connectionIndex].isValid = false;
            gPendingConnections[slot->connectionIndex].targetDataSlot = -1;
        }

        if (slot->isInUse && gActiveDataEndpoints > 0) {
            gActiveDataEndpoints--;
        }
    }

    /* Reset slot */
    slot->isInUse = false;
    slot->isConnected = false;
    slot->remoteHost = 0;
    slot->remotePort = 0;
    slot->state = FACTORY_STATE_IDLE;
    slot->stateTimestamp = 0;
    slot->connectionIndex = -1;
}

/* Timeout old connections */
static void TimeoutOldConnections(void)
{
    UInt32 currentTime = TickCount();
    short i;

    for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
        if (gPendingConnections[i].isValid) {
            if (currentTime - gPendingConnections[i].timestamp > CONNECTION_TIMEOUT_TICKS) {
                log_debug_cat(LOG_CAT_NETWORKING, "TimeoutOldConnections: Timing out connection at slot %d", i);
                gPendingConnections[i].isValid = false;
                gPendingConnections[i].targetDataSlot = -1;
            }
        }
    }

    /* Also timeout data endpoints that are stuck in creating state */
    for (i = 0; i < MAX_DATA_ENDPOINTS; i++) {
        if (gDataEndpoints[i].isInUse && gDataEndpoints[i].state == FACTORY_STATE_CREATING_ENDPOINT) {
            if (currentTime - gDataEndpoints[i].stateTimestamp > CONNECTION_TIMEOUT_TICKS) {
                log_error_cat(LOG_CAT_NETWORKING, "TimeoutOldConnections: Timing out stuck endpoint creation at slot %d", i);
                CleanupDataEndpointSlot(i);
            }
        }
    }
}

/* Clean up the entire asynchronous factory */
static void CleanupAsyncFactory(void)
{
    short i;

    if (!gFactoryInitialized) {
        return;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "CleanupAsyncFactory: Cleaning up asynchronous factory pattern");

    /* Clean up all data endpoints */
    for (i = 0; i < MAX_DATA_ENDPOINTS; i++) {
        CleanupDataEndpointSlot(i);
    }

    /* Clear all pending connections */
    for (i = 0; i < MAX_PENDING_CONNECTIONS; i++) {
        gPendingConnections[i].isValid = false;
        gPendingConnections[i].targetDataSlot = -1;
    }

    gActiveDataEndpoints = 0;
    gNextConnectionIndex = 0;
    gFactoryInitialized = false;

    log_debug_cat(LOG_CAT_NETWORKING, "CleanupAsyncFactory: Factory cleanup complete");
}

/* Legacy functions removed - replaced by asynchronous factory pattern
* All endpoint creation is now handled by CreateDataEndpointAsync()
* All endpoint cleanup is now handled by CleanupDataEndpointSlot()
*/

/* Cleanup persistent listener (only called during shutdown) */
static void CleanupPersistentListener(void)
{
    /* Clean up the asynchronous factory first */
    CleanupAsyncFactory();

    if (gPersistentListener != kOTInvalidEndpointRef) {
        /* Shutdown the persistent listener */
        OTUnbind(gPersistentListener);
        OTCloseProvider(gPersistentListener);
        gPersistentListener = kOTInvalidEndpointRef;

        gPersistentListenerInitialized = false;
        log_debug_cat(LOG_CAT_NETWORKING, "CleanupPersistentListener: Persistent listener cleaned up");
    }
}

static void OTImpl_Shutdown(short refNum)
{
    (void)refNum; /* Unused */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Shutdown: OpenTransport shutdown requested");

    /* Cleanup factory pattern endpoints */
    CleanupPersistentListener();

    /* Cleanup global buffers before shutdown */
    CleanupOTGlobalBuffers();

    /* Actually shutdown OpenTransport */
    ShutdownOpenTransport();
}

/* Stub implementations for all required functions */
/* These would be implemented fully when adding complete OpenTransport support */

static OSErr OTImpl_TCPCreate(short refNum, NetworkStreamRef *streamRef,
                              unsigned long rcvBufferSize, Ptr rcvBuffer,
                              NetworkNotifyProcPtr notifyProc)
{
    OSStatus err;
    EndpointRef endpoint;
    OTTCPEndpoint *tcpEp;
    TEndpointInfo info;

    (void)refNum;
    (void)notifyProc; /* Unused */

    if (streamRef == NULL) {
        return paramErr;
    }

    /* Create TCP endpoint */
    {
        OTConfigurationRef config = OTCreateConfiguration(kTCPName);
        if (config == kOTInvalidConfigurationPtr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: Invalid TCP configuration");
            return kOTBadConfigurationErr;
        } else if (config == kOTNoMemoryConfigurationPtr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: Out of memory creating TCP configuration");
            return memFullErr;
        } else if (config == NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: Failed to create TCP configuration");
            return kOTBadConfigurationErr;
        }

        endpoint = OTOpenEndpoint(config, 0, &info, &err);
        /* NOTE: OTOpenEndpoint automatically disposes of the configuration - do NOT call OTDestroyConfiguration */

        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: OTOpenEndpoint failed: %d", err);
            return err;
        }
    }

    /* Allocate TCP endpoint structure */
    tcpEp = (OTTCPEndpoint *)NewPtr(sizeof(OTTCPEndpoint));
    if (tcpEp == NULL) {
        OTCloseProvider(endpoint);
        return memFullErr;
    }

    /* Initialize TCP endpoint */
    tcpEp->endpoint = endpoint;
    tcpEp->isConnected = false;
    tcpEp->isListening = false;
    tcpEp->remoteHost = 0;
    tcpEp->remotePort = 0;
    tcpEp->localPort = 0;
    tcpEp->receiveBuffer = rcvBuffer;
    tcpEp->bufferSize = rcvBufferSize;

    /* Install notifier */
    err = OTInstallNotifier(endpoint, OTTCPNotifier, tcpEp);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: OTInstallNotifier failed: %d", err);
        DisposePtr((Ptr)tcpEp);
        OTCloseProvider(endpoint);
        return err;
    }


    /* Set endpoint to async mode */
    err = OTSetAsynchronous(endpoint);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: OTSetAsynchronous failed: %d", err);
        DisposePtr((Ptr)tcpEp);
        OTCloseProvider(endpoint);
        return err;
    }

    *streamRef = (NetworkStreamRef)tcpEp;
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: TCP endpoint created successfully");
    return noErr;
}

static OSErr OTImpl_TCPRelease(short refNum, NetworkStreamRef streamRef)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err = noErr;

    (void)refNum; /* Unused */

    if (tcpEp == NULL) {
        return paramErr;
    }

    /* Close the endpoint */
    if (tcpEp->endpoint != kOTInvalidEndpointRef) {
        err = OTCloseProvider(tcpEp->endpoint);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPRelease: OTCloseProvider failed: %d", err);
        }
    }

    /* Free the endpoint structure */
    DisposePtr((Ptr)tcpEp);

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPRelease: TCP endpoint released");
    return err;
}

static OSErr OTImpl_TCPListen(NetworkStreamRef streamRef, tcp_port localPort,
                              Byte timeout, Boolean async)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;

    (void)timeout;
    (void)async; /* Always async in OpenTransport */

    if (tcpEp == NULL) {
        return paramErr;
    }

    /* OpenTransport Factory Pattern: Use persistent listener instead of per-stream listening */
    /* This is the key architectural change that eliminates -3155 errors */

    if (!gPersistentListenerInitialized) {
        /* Initialize persistent listener on first call */
        err = InitializePersistentListener(localPort);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListen: Failed to initialize persistent listener: %d", err);
            return err;
        }
    }

    /* Mark the stream as listening for compatibility with existing code */
    tcpEp->localPort = localPort;
    tcpEp->isListening = true;
    tcpEp->isConnected = false;

    /* The persistent listener handles all actual listening operations */
    /* When connections arrive, they will be accepted on new data endpoints */
    /* This stream object is just a placeholder for messaging compatibility */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListen: Factory pattern listening active on port %d", localPort);
    return noErr;
}

/* GEMINI'S ARCHITECTURAL IMPROVEMENT: Separate listener and data connection handling */
static OSErr OTImpl_TCPAcceptConnection(NetworkStreamRef listenerRef, NetworkStreamRef *dataStreamRef,
                                        ip_addr *remoteHost, tcp_port *remotePort)
{
    OTTCPEndpoint *listenerEp = (OTTCPEndpoint *)listenerRef;
    OSStatus err;

    if (listenerEp == NULL || dataStreamRef == NULL || remoteHost == NULL || remotePort == NULL) {
        return paramErr;
    }

    /* Verify this is actually a listener stream */
    if (!listenerEp->isListening) {
        log_error_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAcceptConnection: Stream is not in listening state");
        return kOTBadSequenceErr;
    }

    /* Check if we have any connected data endpoints available */
    short connectedSlot = -1;
    short i;

    for (i = 0; i < MAX_DATA_ENDPOINTS; i++) {
        if (gDataEndpoints[i].isInUse && gDataEndpoints[i].isConnected &&
                gDataEndpoints[i].state == FACTORY_STATE_ACCEPTING_CONNECTION) {
            connectedSlot = i;
            break;
        }
    }

    if (connectedSlot == -1) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAcceptConnection: No connected data endpoints available");
        return kOTNoDataErr;
    }

    /* Create a NEW NetworkStreamRef for the data connection */
    err = OTImpl_TCPCreate(0, dataStreamRef, listenerEp->bufferSize, listenerEp->receiveBuffer, NULL);
    if (err != noErr) {
        log_error_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAcceptConnection: Failed to create data stream: %d", err);
        return err;
    }

    /* Configure the new data stream to use the accepted endpoint */
    OTTCPEndpoint *dataEp = (OTTCPEndpoint *)*dataStreamRef;
    if (dataEp->endpoint != kOTInvalidEndpointRef) {
        /* Release the auto-created endpoint since we'll use the accepted one */
        OTCloseProvider(dataEp->endpoint);
    }

    /* Transfer the accepted endpoint to the new data stream */
    DataEndpointSlot *slot = &gDataEndpoints[connectedSlot];
    dataEp->endpoint = slot->endpoint;
    dataEp->isConnected = true;
    dataEp->isListening = false;

    /* Extract connection info from the data endpoint slot */
    *remoteHost = slot->remoteHost;
    *remotePort = slot->remotePort;
    dataEp->remoteHost = slot->remoteHost;
    dataEp->remotePort = slot->remotePort;

    /* Mark the slot as transferred (endpoint now owned by the data stream) */
    slot->endpoint = kOTInvalidEndpointRef;
    CleanupDataEndpointSlot(connectedSlot);

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAcceptConnection: Created separate data stream for connection");
    return noErr;
}

static OSErr OTImpl_TCPConnect(NetworkStreamRef streamRef, ip_addr remoteHost,
                               tcp_port remotePort, Byte timeout,
                               NetworkGiveTimeProcPtr giveTime)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    InetAddress addr;
    TCall sndCall;
    OTResult state;

    (void)timeout;
    (void)giveTime; /* Async operation, timeout handled by caller */

    if (tcpEp == NULL) {
        return paramErr;
    }

    /* Check endpoint state and reset if needed */
    state = OTGetEndpointState(tcpEp->endpoint);
    if (state != T_IDLE) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnect: Endpoint in state %d, resetting to T_IDLE", state);
        if (state == T_DATAXFER || state == T_OUTCON || state == T_INCON) {
            OTSndDisconnect(tcpEp->endpoint, NULL);
        }
        if (state >= T_IDLE) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnect: Unbinding endpoint from state %d", state);
            OTUnbind(tcpEp->endpoint);
        }
        tcpEp->isConnected = false;
        tcpEp->isListening = false;
    }

    /* Bind with any local address for outgoing connection */
    if (OTGetEndpointState(tcpEp->endpoint) == T_UNBND) {
        InetAddress localAddr;
        TBind reqAddr, retAddr;

        /* Set IP_REUSEADDR option before binding for client endpoints */
        /* This allows rapid reconnection in broadcast scenarios */
        {
            TOption option;
            TOptMgmt request;

            option.len    = kOTFourByteOptionSize;
            option.level  = INET_IP;
            option.name   = IP_REUSEADDR;
            option.status = 0;
            *(UInt32 *)option.value = T_YES; /* Enable address reuse */

            request.opt.buf = (UInt8 *)&option;
            request.opt.len = sizeof(option);
            request.flags   = T_NEGOTIATE;

            err = OTOptionManagement(tcpEp->endpoint, &request, &request);
            if (err != noErr || option.status != T_SUCCESS) {
                log_warning_cat(LOG_CAT_NETWORKING, "Could not set IP_REUSEADDR on client endpoint. Err: %d, Status: %d", err, option.status);
                /* Continue without IP_REUSEADDR - not critical but may affect broadcast performance */
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "IP_REUSEADDR option set successfully on client endpoint");
            }
        }

        OTInitInetAddress(&localAddr, 0, kOTAnyInetAddress); /* Any local port */

        reqAddr.addr.buf = (UInt8 *)&localAddr;
        reqAddr.addr.len = sizeof(localAddr);
        reqAddr.qlen = 0; /* No listen queue for client */

        retAddr.addr.buf = (UInt8 *)&localAddr;
        retAddr.addr.maxlen = sizeof(localAddr);

        err = OTBind(tcpEp->endpoint, &reqAddr, &retAddr);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnect: OTBind failed: %d", err);
            return err;
        }
    }

    /* Set up remote address */
    OTInitInetAddress(&addr, remotePort, remoteHost);

    /* Set up call structure */
    sndCall.addr.buf = (UInt8 *)&addr;
    sndCall.addr.len = sizeof(addr);
    sndCall.opt.buf = NULL;
    sndCall.opt.len = 0;
    sndCall.udata.buf = NULL;
    sndCall.udata.len = 0;

    /* Initiate connection */
    err = OTConnect(tcpEp->endpoint, &sndCall, NULL);
    if (err != noErr && err != kOTNoDataErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnect: OTConnect failed: %d", err);
        return err;
    }

    tcpEp->remoteHost = remoteHost;
    tcpEp->remotePort = remotePort;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnect: Connection initiated to %08lX:%d", remoteHost, remotePort);
    return noErr;
}

static OSErr OTImpl_TCPSend(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                            Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    OTFlags flags = 0;

    (void)timeout;
    (void)giveTime; /* Async operation */

    if (tcpEp == NULL || data == NULL) {
        return paramErr;
    }

    if (!tcpEp->isConnected) {
        return connectionDoesntExist;
    }

    if (push) {
        flags |= T_MORE; /* Use T_MORE instead of T_PUSH in OpenTransport */
    }

    err = OTSnd(tcpEp->endpoint, data, length, flags);
    if (err < 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPSend: OTSnd failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPSend: Sent %d bytes", length);
    return noErr;
}

static OSErr OTImpl_TCPReceiveNoCopy(NetworkStreamRef streamRef, Ptr rdsPtr,
                                     short maxEntries, Byte timeout,
                                     Boolean *urgent, Boolean *mark,
                                     NetworkGiveTimeProcPtr giveTime)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    OTFlags flags = 0;

    (void)timeout;
    (void)giveTime; /* Simplified for OpenTransport */

    if (tcpEp == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    if (!tcpEp->isConnected) {
        return connectionDoesntExist;
    }

    /*
     * FIX: Use OpenTransport's proper no-copy receive mechanism with OTBuffer chains.
     * The previous approach was fundamentally flawed - it used copying OTRcv and pointed
     * to potentially stale buffer data. This caused protocol parsing to read garbage.
     */
    OTBuffer *otBufferChain;

    /* Use OT's native no-copy receive to get OTBuffer chain */
    err = OTRcv(tcpEp->endpoint, &otBufferChain, kOTNetbufDataIsOTBufferStar, &flags);
    if (err < 0) {
        if (err == kOTNoDataErr) {
            return commandTimeout; /* No data available */
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: OTRcv no-copy failed: %d", err);
        return err;
    }

    /* Convert OTBuffer chain to rdsEntry array format expected by the application */
    {
        wdsEntry *rdsArray = (wdsEntry *)rdsPtr;
        OTBuffer *currentBuffer = otBufferChain;
        int entriesCreated = 0;

        /* Convert each OTBuffer to an rdsEntry */
        while (currentBuffer != NULL && entriesCreated < (maxEntries - 1)) {
            rdsArray[entriesCreated].length = currentBuffer->fLen;
            rdsArray[entriesCreated].ptr = (Ptr)currentBuffer->fData;

            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: RDS entry %d: length=%d, ptr=0x%08lX",
                          entriesCreated, rdsArray[entriesCreated].length, (unsigned long)rdsArray[entriesCreated].ptr);

            /* Debug dump first buffer's content */
            if (entriesCreated == 0 && currentBuffer->fLen > 0 && currentBuffer->fData) {
                unsigned char *data = (unsigned char *)currentBuffer->fData;
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                              data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
                              data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
            }

            currentBuffer = currentBuffer->fNext;
            entriesCreated++;
        }

        /* Terminate the RDS array with a null entry */
        rdsArray[entriesCreated].length = 0;
        rdsArray[entriesCreated].ptr = NULL;

        /* FIX: Store the OTBuffer chain head in the entry after the null terminator for later release */
        /* This ensures the caller allocates maxEntries + 2 entries: data + null terminator + buffer chain storage */
        if (entriesCreated < maxEntries - 2) {
            rdsArray[entriesCreated + 1].length = 0;
            rdsArray[entriesCreated + 1].ptr = (Ptr)otBufferChain; /* Store chain head for cleanup */
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: Stored OTBuffer chain 0x%08lX at rdsArray[%d]",
                          (unsigned long)otBufferChain, entriesCreated + 1);
        } else {
            log_warning_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: Not enough RDS entries to store buffer chain - memory leak possible");
        }

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveNoCopy: Converted %d OTBuffer(s) to RDS array", entriesCreated);
    }

    if (urgent != NULL) {
        *urgent = (flags & T_EXPEDITED) != 0;
    }
    if (mark != NULL) {
        *mark = false; /* Not supported in this simplified implementation */
    }

    return noErr;
}

static OSErr OTImpl_TCPReturnBuffer(NetworkStreamRef streamRef, Ptr rdsPtr,
                                    NetworkGiveTimeProcPtr giveTime)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;

    (void)giveTime; /* Unused */

    if (tcpEp == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    /*
     * FIX: Release the OTBuffer chain that was stored during the no-copy receive.
     * Find the end of the RDS array and retrieve the stored OTBuffer chain head.
     */
    {
        wdsEntry *rdsArray = (wdsEntry *)rdsPtr;
        OTBuffer *otBufferChain = NULL;
        int entryIndex = 0;

        /* Find the first terminating entry (null terminator for valid data) */
        while (rdsArray[entryIndex].length != 0) {
            entryIndex++;
        }

        /* The entry after the null terminator should contain the stored OTBuffer chain */
        entryIndex++;
        if (rdsArray[entryIndex].length == 0 && rdsArray[entryIndex].ptr != NULL) {
            otBufferChain = (OTBuffer *)rdsArray[entryIndex].ptr;
            rdsArray[entryIndex].ptr = NULL; /* Clear the stored pointer to prevent double-free */

            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReturnBuffer: Found stored OTBuffer chain 0x%08lX at rdsArray[%d]",
                          (unsigned long)otBufferChain, entryIndex);
        } else {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReturnBuffer: No OTBuffer chain found at rdsArray[%d] (length=%d, ptr=0x%08lX)",
                          entryIndex, rdsArray[entryIndex].length, (unsigned long)rdsArray[entryIndex].ptr);
        }

        /* Release the OTBuffer chain if found */
        if (otBufferChain != NULL) {
            OTReleaseBuffer(otBufferChain);
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReturnBuffer: Successfully released OTBuffer chain 0x%08lX",
                          (unsigned long)otBufferChain);
        } else {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReturnBuffer: No OTBuffer chain to release (expected for copy-based receives)");
        }
    }

    return noErr;
}

static OSErr OTImpl_TCPClose(NetworkStreamRef streamRef, Byte timeout,
                             NetworkGiveTimeProcPtr giveTime)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;

    (void)timeout;
    (void)giveTime; /* OpenTransport handles timing internally */

    if (tcpEp == NULL) {
        return paramErr;
    }

    if (!tcpEp->isConnected) {
        return connectionDoesntExist;
    }

    /* Initiate orderly disconnect */
    err = OTSndOrderlyDisconnect(tcpEp->endpoint);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPClose: OTSndOrderlyDisconnect failed: %d", err);
        /* Try abortive close */
        err = OTSndDisconnect(tcpEp->endpoint, NULL);
    }

    tcpEp->isConnected = false;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPClose: Connection closed");
    return err;
}

static OSErr OTImpl_TCPAbort(NetworkStreamRef streamRef)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err = noErr;

    if (tcpEp == NULL) {
        return paramErr;
    }

    /* Factory Pattern: Check if this is a listen stream or send stream */
    if (tcpEp == (OTTCPEndpoint *)gTCPListenStream) {
        /* CRITICAL: For listen streams, clean up data endpoint, NOT the persistent listener */
        /* This is the key fix that eliminates -3155 errors */

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Cleaning up data endpoint (not persistent listener)");
        /* Use async factory to clean up data endpoints */
        CleanupAsyncFactory();

        tcpEp->isConnected = false;

        /* Generate TCPTerminate event for messaging layer */
        TCP_Listen_ASR_Handler((StreamPtr)tcpEp, TCPTerminate, NULL, 0, NULL);

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Listen stream abort complete (persistent listener remains active)");

    } else if (tcpEp == (OTTCPEndpoint *)gTCPSendStream) {
        /* For send streams, apply proper state validation as Gemini recommended */
        OTResult state = OTGetEndpointState(tcpEp->endpoint);

        /* Only disconnect if actually connected */
        if (state == T_DATAXFER || state == T_OUTCON || state == T_INCON) {
            err = OTSndDisconnect(tcpEp->endpoint, NULL);
            if (err != noErr) {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: OTSndDisconnect failed: %d", err);
            }
        }

        tcpEp->isConnected = false;

        /* Generate TCPTerminate event for messaging layer */
        TCP_Send_ASR_Handler((StreamPtr)tcpEp, TCPTerminate, NULL, 0, NULL);

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Send stream abort complete");

    } else {
        /* Unknown stream type */
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Unknown stream type, applying generic cleanup");

        OTResult state = OTGetEndpointState(tcpEp->endpoint);
        if (state == T_DATAXFER || state == T_OUTCON || state == T_INCON) {
            err = OTSndDisconnect(tcpEp->endpoint, NULL);
            if (err != noErr) {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: OTSndDisconnect failed: %d", err);
            }
        }

        tcpEp->isConnected = false;
    }

    return err;
}

static OSErr OTImpl_TCPStatus(NetworkStreamRef streamRef, NetworkTCPInfo *info)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    TEndpointInfo epInfo;

    if (tcpEp == NULL || info == NULL) {
        return paramErr;
    }

    /* Get endpoint information */
    err = OTGetEndpointInfo(tcpEp->endpoint, &epInfo);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPStatus: OTGetEndpointInfo failed: %d", err);
        return err;
    }

    /* Fill in status information */
    info->localHost = 0; /* Would need to get from binding info */
    info->localPort = tcpEp->localPort;
    info->remoteHost = tcpEp->remoteHost;
    info->remotePort = tcpEp->remotePort;
    info->isConnected = tcpEp->isConnected;
    info->isListening = tcpEp->isListening;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPStatus: Status retrieved");
    return noErr;
}

static OSErr OTImpl_TCPUnbind(NetworkStreamRef streamRef)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err = noErr;
    OTResult state;

    if (tcpEp == NULL) {
        return paramErr;
    }

    state = OTGetEndpointState(tcpEp->endpoint);
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: Current endpoint state: %d", state);

    /* FIX: Per OpenTransport docs, OTUnbind must only be called from T_IDLE.
       If the endpoint is in any other active or bound state, it must be
       transitioned to T_IDLE first. An abortive disconnect is the most
       reliable way to do this. */
    if (state > T_IDLE) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: Endpoint is in an active state (%d), aborting to reach T_IDLE.", state);
        OTSndDisconnect(tcpEp->endpoint, NULL);
        /* Note: OTSndDisconnect is asynchronous. A truly robust implementation
           would need to poll OTGetEndpointState until it becomes T_IDLE.
           However, for this application's flow, we proceed and attempt the unbind. */
        state = OTGetEndpointState(tcpEp->endpoint);
    }

    if (state == T_IDLE) {
        err = OTUnbind(tcpEp->endpoint);
        if (err != noErr) {
            log_error_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: OTUnbind failed from T_IDLE state: %d", err);
            return err;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: Endpoint successfully unbound and reset to T_UNBND state.");
    } else if (state != T_UNBND) {
        log_warning_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: Could not unbind; endpoint is in state %d (not T_IDLE or T_UNBND).", state);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPUnbind: Endpoint already in T_UNBND state, no action needed.");
    }

    /* Reset local flags to ensure clean state */
    tcpEp->isConnected = false;
    tcpEp->isListening = false;

    return noErr;
}

/* Async TCP stubs */
static OSErr OTImpl_TCPListenAsync(NetworkStreamRef streamRef, tcp_port localPort,
                                   NetworkAsyncHandle *asyncHandle)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    OTTCPAsyncOperation *asyncOp;

    if (tcpEp == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* First bind to the port if not already listening */
    if (!tcpEp->isListening) {
        err = OTImpl_TCPListen(streamRef, localPort, 0, true);
        if (err != noErr) {
            return err;
        }
    }

    /* Allocate TCP async operation handle */
    *asyncHandle = AllocateOTTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTTCPAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = tcpEp->endpoint;
    asyncOp->opType = OT_ASYNC_TCP_LISTEN;
    asyncOp->stream = streamRef;
    asyncOp->result = noErr;

    /* The actual listening happens in the notifier when T_LISTEN event arrives */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListenAsync: Async listen initiated on port %d", localPort);
    return noErr;
}

static OSErr OTImpl_TCPConnectAsync(NetworkStreamRef streamRef, ip_addr remoteHost,
                                    tcp_port remotePort, NetworkAsyncHandle *asyncHandle)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    InetAddress addr;
    TCall sndCall;
    OTTCPAsyncOperation *asyncOp;
    OTResult state;

    if (tcpEp == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Check endpoint state and reset if needed for robust reconnection */
    state = OTGetEndpointState(tcpEp->endpoint);
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Current endpoint state: %d", state);

    /* FIXED STATE MANAGEMENT: Handle each state according to OpenTransport documentation */
    if (state != T_IDLE) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint in non-IDLE state %d. Applying proper state transition.", state);

        switch (state) {
        case T_OUTCON:
            /* T_OUTCON means previous connection attempt stuck - need to abort cleanly */
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint stuck in T_OUTCON, sending disconnect to reset");
            OTSndDisconnect(tcpEp->endpoint, NULL);
            /* Wait for T_DISCONNECT event and call OTRcvDisconnect, or just continue to unbind */
            break;

        case T_DATAXFER:
        case T_OUTREL:
        case T_INREL:
            /* Connected states - need orderly or abortive disconnect */
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint in connected state %d, disconnecting", state);
            OTSndDisconnect(tcpEp->endpoint, NULL);
            break;

        case T_INCON:
            /* Incoming connection pending - reject it */
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint has pending incoming connection, rejecting");
            OTSndDisconnect(tcpEp->endpoint, NULL);
            break;

        default:
            /* For other states, continue to unbind attempt */
            break;
        }

        /* Now try to unbind - but only if state allows it */
        state = OTGetEndpointState(tcpEp->endpoint);
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: State after disconnect: %d", state);

        if (state >= T_IDLE) { /* T_IDLE or higher can be unbound */
            OSErr unbindErr = OTUnbind(tcpEp->endpoint);
            if (unbindErr != noErr) {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: OTUnbind failed: %d", unbindErr);
                /* Don't return error - continue with current state */
            }
            /* Reset endpoint flags after unbind */
            tcpEp->isConnected = false;
            tcpEp->isListening = false;
            tcpEp->remoteHost = 0;
            tcpEp->remotePort = 0;
            tcpEp->localPort = 0;
        }

        /* Get final state after cleanup */
        state = OTGetEndpointState(tcpEp->endpoint);
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Final state after reset: %d", state);
    }

    if (state == T_UNBND) {
        InetAddress localAddr;
        TBind reqAddr, retAddr;

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint in T_UNBND state, binding to ephemeral port");

        /* Set IP_REUSEADDR option before binding for async client endpoints */
        /* This allows rapid reconnection in broadcast scenarios */
        {
            TOption option;
            TOptMgmt request;

            option.len    = kOTFourByteOptionSize;
            option.level  = INET_IP;
            option.name   = IP_REUSEADDR;
            option.status = 0;
            *(UInt32 *)option.value = T_YES; /* Enable address reuse */

            request.opt.buf = (UInt8 *)&option;
            request.opt.len = sizeof(option);
            request.flags   = T_NEGOTIATE;

            err = OTOptionManagement(tcpEp->endpoint, &request, &request);
            if (err != noErr || option.status != T_SUCCESS) {
                log_warning_cat(LOG_CAT_NETWORKING, "Could not set IP_REUSEADDR on async client endpoint. Err: %d, Status: %d", err, option.status);
                /* Continue without IP_REUSEADDR - not critical but may affect broadcast performance */
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "IP_REUSEADDR option set successfully on async client endpoint");
            }
        }

        /* Bind to any local address and port for outgoing connection */
        OTInitInetAddress(&localAddr, 0, kOTAnyInetAddress);

        reqAddr.addr.buf = (UInt8 *)&localAddr;
        reqAddr.addr.len = sizeof(localAddr);
        reqAddr.qlen = 0; /* No listen queue for client */

        retAddr.addr.buf = (UInt8 *)&localAddr;
        retAddr.addr.maxlen = sizeof(localAddr);

        err = OTBind(tcpEp->endpoint, &reqAddr, &retAddr);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: OTBind failed: %d", err);
            return err;
        }

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Successfully bound endpoint to ephemeral port");
    }
    /* Note: After our robust state handling above, endpoint should now be in T_IDLE state */

    /* Allocate TCP async operation handle (FIX: Use TCP handle pool, not UDP) */
    *asyncHandle = AllocateOTTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTTCPAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = tcpEp->endpoint;
    asyncOp->opType = OT_ASYNC_TCP_CONNECT;
    asyncOp->stream = streamRef;

    /* Set up address */
    OTInitInetAddress(&addr, remotePort, remoteHost);

    /* Set up call structure */
    sndCall.addr.buf = (UInt8 *)&addr;
    sndCall.addr.len = sizeof(addr);
    sndCall.opt.buf = NULL;
    sndCall.opt.len = 0;
    sndCall.udata.buf = NULL;
    sndCall.udata.len = 0;

    /* Initiate async connection */
    err = OTConnect(tcpEp->endpoint, &sndCall, NULL);
    if (err != noErr && err != kOTNoDataErr) {
        FreeOTTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: OTConnect failed: %d", err);
        return err;
    }

    tcpEp->remoteHost = remoteHost;
    tcpEp->remotePort = remotePort;
    asyncOp->result = err;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Async connection initiated to %08lX:%d", remoteHost, remotePort);
    return noErr;
}

static OSErr OTImpl_TCPSendAsync(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                 Boolean push, NetworkAsyncHandle *asyncHandle)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OSStatus err;
    OTFlags flags = 0;
    OTTCPAsyncOperation *asyncOp;

    if (tcpEp == NULL || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    if (!tcpEp->isConnected) {
        return connectionDoesntExist;
    }

    /* Verify OpenTransport endpoint state before attempting send */
    {
        OTResult endpointState = OTGetEndpointState(tcpEp->endpoint);
        if (endpointState != T_DATAXFER) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPSendAsync: Endpoint not in T_DATAXFER state (current: %d)", endpointState);
            /* Return specific OpenTransport error instead of generic connectionDoesntExist */
            if (endpointState == T_OUTCON) {
                return kOTOutStateErr; /* Connection still establishing */
            } else if (endpointState == T_UNBND || endpointState == T_IDLE) {
                return connectionDoesntExist; /* Not connected */
            } else {
                return kOTOutStateErr; /* Wrong state for data transfer */
            }
        }
    }

    /* Allocate TCP async operation handle (FIX: Use TCP handle pool, not UDP) */
    *asyncHandle = AllocateOTTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTTCPAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = tcpEp->endpoint;
    asyncOp->opType = OT_ASYNC_TCP_SEND;
    asyncOp->stream = streamRef;
    asyncOp->dataBuffer = data;
    asyncOp->dataLength = length;

    if (push) {
        flags |= T_MORE; /* Use T_MORE instead of T_PUSH in OpenTransport */
    }

    /* Send data asynchronously */
    err = OTSnd(tcpEp->endpoint, data, length, flags);
    if (err < 0) {
        FreeOTTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPSendAsync: OTSnd failed: %d", err);
        return err;
    }

    asyncOp->result = err; /* Number of bytes sent or error */
    asyncOp->completed = true; /* OTSnd completes immediately */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPSendAsync: Async send completed, %d bytes", err);
    return noErr;
}

static OSErr OTImpl_TCPReceiveAsync(NetworkStreamRef streamRef, Ptr rdsPtr,
                                    short maxEntries, NetworkAsyncHandle *asyncHandle)
{
    OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)streamRef;
    OTTCPAsyncOperation *asyncOp;

    (void)maxEntries; /* Simplified for OpenTransport */

    if (tcpEp == NULL || rdsPtr == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    if (!tcpEp->isConnected) {
        return connectionDoesntExist;
    }

    /* Allocate TCP async operation handle (FIX: Use TCP handle pool, not UDP) */
    *asyncHandle = AllocateOTTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTTCPAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = tcpEp->endpoint;
    asyncOp->opType = OT_ASYNC_TCP_RECEIVE;
    asyncOp->stream = streamRef;
    asyncOp->result = noErr;
    asyncOp->userData = rdsPtr; /* Store RDS pointer for completion */

    /* The actual receive happens when T_DATA event arrives in notifier */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPReceiveAsync: Async receive initiated");
    return noErr;
}

static OSErr OTImpl_TCPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
                                        OSErr *operationResult, void **resultData)
{
    OTTCPAsyncOperation *asyncOp = (OTTCPAsyncOperation *)asyncHandle;

    if (asyncOp == NULL) {
        return paramErr;
    }

    if (!asyncOp->inUse) {
        return paramErr;
    }

    /* Check if operation completed */
    if (!asyncOp->completed) {
        return 1; /* Still in progress */
    }

    /* Operation completed */
    if (operationResult != NULL) {
        *operationResult = asyncOp->result;
    }

    if (resultData != NULL) {
        *resultData = asyncOp->userData;
    }

    /* For connect operations, validate that endpoint is actually ready for data transfer */
    if (asyncOp->opType == OT_ASYNC_TCP_CONNECT && asyncOp->result == noErr) {
        OTTCPEndpoint *tcpEp = (OTTCPEndpoint *)asyncOp->stream;
        if (tcpEp != NULL) {
            OTResult endpointState = OTGetEndpointState(tcpEp->endpoint);
            if (endpointState != T_DATAXFER) {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCheckAsyncStatus: Connect completed but endpoint not in T_DATAXFER state (%d), connection not ready", endpointState);
                if (operationResult != NULL) {
                    *operationResult = kOTOutStateErr; /* Override result to indicate not ready */
                }
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCheckAsyncStatus: Connection fully established and ready for data transfer");
            }
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCheckAsyncStatus: TCP operation completed with result %d", asyncOp->result);

    /* Free the TCP async handle (FIX: Use TCP handle pool) */
    FreeOTTCPAsyncHandle(asyncHandle);

    return noErr; /* Completed */
}

static void OTImpl_TCPCancelAsync(NetworkAsyncHandle asyncHandle)
{
    OTTCPAsyncOperation *asyncOp = (OTTCPAsyncOperation *)asyncHandle;

    if (asyncOp == NULL || !asyncOp->inUse) {
        return;
    }

    /* Cancel the operation if possible */
    if (asyncOp->endpoint != kOTInvalidEndpointRef && !asyncOp->completed) {
        /* OpenTransport doesn't have explicit cancel, just mark as completed */
        asyncOp->completed = true;
        asyncOp->result = kOTCanceledErr;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCancelAsync: TCP operation cancelled");
    FreeOTTCPAsyncHandle(asyncHandle);
}

/* UDP stubs */
static OSErr OTImpl_UDPCreate(short refNum, NetworkEndpointRef *endpointRef,
                              udp_port localPort, Ptr recvBuffer,
                              unsigned short bufferSize)
{
    OSStatus err;
    EndpointRef endpoint;
    OTUDPEndpoint *udpEp;
    TEndpointInfo info;
    InetAddress addr;
    TBind reqAddr, retAddr;

    (void)refNum; /* Unused */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Entry - port=%d, buffer=0x%08lX, size=%d",
                  localPort, (unsigned long)recvBuffer, bufferSize);

    if (endpointRef == NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: endpointRef is NULL");
        return paramErr;
    }

    /* Create UDP endpoint */
    {
        OTConfigurationRef config;

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: About to call OTCreateConfiguration with kUDPName");
        config = OTCreateConfiguration(kUDPName);
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTCreateConfiguration returned 0x%08lX", (unsigned long)config);

        if (config == kOTInvalidConfigurationPtr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Invalid UDP configuration");
            return kOTBadConfigurationErr;
        } else if (config == kOTNoMemoryConfigurationPtr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Out of memory creating UDP configuration");
            return memFullErr;
        } else if (config == NULL) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Failed to create UDP configuration");
            return kOTBadConfigurationErr;
        }

        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: About to call OTOpenEndpoint");
        endpoint = OTOpenEndpoint(config, 0, &info, &err);
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTOpenEndpoint returned endpoint=0x%08lX, err=%d", (unsigned long)endpoint, err);

        /* NOTE: OTOpenEndpoint automatically disposes of the configuration - do NOT call OTDestroyConfiguration */
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Configuration automatically disposed by OTOpenEndpoint");

        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTOpenEndpoint failed: %d", err);
            return err;
        }
    }

    /* Allocate UDP endpoint structure */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Allocating UDP endpoint structure (%ld bytes)", (long)sizeof(OTUDPEndpoint));
    udpEp = (OTUDPEndpoint *)NewPtr(sizeof(OTUDPEndpoint));
    if (udpEp == NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Failed to allocate UDP endpoint structure");
        OTCloseProvider(endpoint);
        return memFullErr;
    }
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: UDP endpoint structure allocated at 0x%08lX", (unsigned long)udpEp);

    /* Initialize UDP endpoint */
    udpEp->endpoint = endpoint;
    udpEp->localPort = localPort;
    udpEp->receiveBuffer = recvBuffer;
    udpEp->bufferSize = bufferSize;
    udpEp->isCreated = true;

    /* Install notifier */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Installing notifier");
    err = OTInstallNotifier(endpoint, OTUDPNotifier, udpEp);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTInstallNotifier failed: %d", err);
        DisposePtr((Ptr)udpEp);
        OTCloseProvider(endpoint);
        return err;
    }
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Notifier installed successfully");

    /*
     * FIX: Set the IP_BROADCAST option to allow receiving broadcast packets.
     * This is crucial for OpenTransport to receive broadcasts from other hosts.
     */
    {
        TOption option;
        TOptMgmt request, result;

        /* Prepare the option structure */
        option.len    = kOTFourByteOptionSize;
        option.level  = INET_IP;
        option.name   = IP_BROADCAST;
        option.status = 0;
        *(UInt32 *)option.value = T_YES; /* Enable broadcast reception */

        /* Prepare the request for OTOptionManagement */
        request.opt.buf = (UInt8 *)&option;
        request.opt.len = sizeof(option);
        request.flags   = T_NEGOTIATE;

        result.opt.buf = (UInt8 *)&option;
        result.opt.maxlen = sizeof(option);

        /* Set the option on the endpoint */
        err = OTOptionManagement(endpoint, &request, &result);
        if (err != noErr || option.status != T_SUCCESS) {
            log_error_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: Failed to set IP_BROADCAST option. Err: %d, Status: %d",
                          err, option.status);
            DisposePtr((Ptr)udpEp);
            OTCloseProvider(endpoint);
            return (err != noErr) ? err : kOTBadOptionErr;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: IP_BROADCAST option set successfully");
    }

    /* Set endpoint to async mode */
    err = OTSetAsynchronous(endpoint);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTSetAsynchronous failed: %d", err);
        DisposePtr((Ptr)udpEp);
        OTCloseProvider(endpoint);
        return err;
    }

    /* Bind to local port if specified */
    if (localPort != 0) {
        OTInitInetAddress(&addr, localPort, kOTAnyInetAddress);

        reqAddr.addr.buf = (UInt8 *)&addr;
        reqAddr.addr.len = sizeof(addr);
        reqAddr.qlen = 0; /* No listen queue for UDP */

        retAddr.addr.buf = (UInt8 *)&addr;
        retAddr.addr.maxlen = sizeof(addr);

        err = OTBind(endpoint, &reqAddr, &retAddr);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTBind failed: %d", err);
            DisposePtr((Ptr)udpEp);
            OTCloseProvider(endpoint);
            return err;
        }
    }

    *endpointRef = (NetworkEndpointRef)udpEp;
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: UDP endpoint created on port %d", localPort);
    return noErr;
}

static OSErr OTImpl_UDPRelease(short refNum, NetworkEndpointRef endpointRef)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OSStatus err = noErr;

    (void)refNum; /* Unused */

    if (udpEp == NULL) {
        return paramErr;
    }

    /* Close the endpoint */
    if (udpEp->endpoint != kOTInvalidEndpointRef) {
        err = OTCloseProvider(udpEp->endpoint);
        if (err != noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPRelease: OTCloseProvider failed: %d", err);
        }
    }

    /* Free the endpoint structure */
    DisposePtr((Ptr)udpEp);

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPRelease: UDP endpoint released");
    return err;
}

static OSErr OTImpl_UDPSend(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                            udp_port remotePort, Ptr data, unsigned short length)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OSStatus err;
    InetAddress addr;
    TUnitData unitData;

    if (udpEp == NULL || data == NULL) {
        return paramErr;
    }

    if (!udpEp->isCreated) {
        return notOpenErr;
    }

    /* Set up destination address */
    OTInitInetAddress(&addr, remotePort, remoteHost);

    /* Set up unit data structure */
    unitData.addr.buf = (UInt8 *)&addr;
    unitData.addr.len = sizeof(addr);
    unitData.opt.buf = NULL;
    unitData.opt.len = 0;
    unitData.udata.buf = (UInt8 *)data;
    unitData.udata.len = length;

    /* Send datagram */
    err = OTSndUData(udpEp->endpoint, &unitData);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPSend: OTSndUData failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPSend: Sent %d bytes to %08lX:%d", length, remoteHost, remotePort);
    return noErr;
}

static OSErr OTImpl_UDPReceive(NetworkEndpointRef endpointRef, ip_addr *remoteHost,
                               udp_port *remotePort, Ptr buffer,
                               unsigned short *length, Boolean async)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OSStatus err;
    InetAddress addr;
    TUnitData unitData;
    OTFlags flags = 0;

    (void)async; /* Always async in OpenTransport */

    if (udpEp == NULL || buffer == NULL || length == NULL) {
        return paramErr;
    }

    if (!udpEp->isCreated) {
        return notOpenErr;
    }

    /* Set up unit data structure */
    unitData.addr.buf = (UInt8 *)&addr;
    unitData.addr.maxlen = sizeof(addr);
    unitData.opt.buf = NULL;
    unitData.opt.maxlen = 0;
    unitData.udata.buf = (UInt8 *)buffer;
    unitData.udata.maxlen = *length;

    /* Receive datagram */
    err = OTRcvUData(udpEp->endpoint, &unitData, &flags);
    if (err != noErr) {
        if (err == kOTNoDataErr) {
            return commandTimeout; /* No data available */
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPReceive: OTRcvUData failed: %d", err);
        return err;
    }

    /* Extract sender information */
    if (remoteHost != NULL && unitData.addr.len >= sizeof(InetAddress)) {
        *remoteHost = ((InetAddress *)unitData.addr.buf)->fHost;
    }
    if (remotePort != NULL && unitData.addr.len >= sizeof(InetAddress)) {
        *remotePort = ((InetAddress *)unitData.addr.buf)->fPort;
    }

    *length = unitData.udata.len;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPReceive: Received %d bytes", *length);
    return noErr;
}

static OSErr OTImpl_UDPReturnBuffer(NetworkEndpointRef endpointRef, Ptr buffer,
                                    unsigned short bufferSize, Boolean async)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;

    (void)buffer;
    (void)bufferSize;
    (void)async; /* Unused in OpenTransport */

    if (udpEp == NULL) {
        return paramErr;
    }

    /* In OpenTransport, UDP buffer management is simpler */
    /* No explicit buffer return needed like in MacTCP */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPReturnBuffer: Buffer returned");
    return noErr;
}

/* Async UDP stubs */
static OSErr OTImpl_UDPSendAsync(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                 udp_port remotePort, Ptr data, unsigned short length,
                                 NetworkAsyncHandle *asyncHandle)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OSStatus err;
    InetAddress addr;
    TUnitData unitData;
    OTAsyncOperation *asyncOp;

    if (udpEp == NULL || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    if (!udpEp->isCreated) {
        return notOpenErr;
    }

    /* Allocate async operation handle */
    *asyncHandle = AllocateOTAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = udpEp->endpoint;
    asyncOp->opType = OT_ASYNC_UDP_SEND;

    /* Set up destination address */
    OTInitInetAddress(&addr, remotePort, remoteHost);

    /* Set up unit data structure */
    unitData.addr.buf = (UInt8 *)&addr;
    unitData.addr.len = sizeof(addr);
    unitData.opt.buf = NULL;
    unitData.opt.len = 0;
    unitData.udata.buf = (UInt8 *)data;
    unitData.udata.len = length;

    /* Send datagram asynchronously */
    err = OTSndUData(udpEp->endpoint, &unitData);
    if (err != noErr) {
        FreeOTAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPSendAsync: OTSndUData failed: %d", err);
        return err;
    }

    asyncOp->result = noErr;
    asyncOp->completed = true; /* UDP send completes immediately */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPSendAsync: Async UDP send completed");
    return noErr;
}

static OSErr OTImpl_UDPCheckSendStatus(NetworkAsyncHandle asyncHandle)
{
    OTAsyncOperation *asyncOp = (OTAsyncOperation *)asyncHandle;

    if (asyncOp == NULL) {
        return paramErr;
    }

    if (!asyncOp->inUse) {
        return paramErr;
    }

    /* Check if send operation completed */
    if (!asyncOp->completed) {
        return 1; /* Still in progress */
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCheckSendStatus: Send completed with result %d", asyncOp->result);
    return asyncOp->result;
}

static OSErr OTImpl_UDPReceiveAsync(NetworkEndpointRef endpointRef,
                                    NetworkAsyncHandle *asyncHandle)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OTAsyncOperation *asyncOp;

    if (udpEp == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    if (!udpEp->isCreated) {
        return notOpenErr;
    }

    /* Allocate async operation handle */
    *asyncHandle = AllocateOTAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = udpEp->endpoint;
    asyncOp->opType = OT_ASYNC_UDP_RECEIVE;
    asyncOp->result = noErr;
    asyncOp->userData = (void *)udpEp; /* Store UDP endpoint for later use */

    /* In OpenTransport, we rely on T_DATA notifier events to signal data availability */
    /* The receive operation is conceptually always active once the endpoint is bound */
    asyncOp->completed = false;

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPReceiveAsync: Async receive initiated, waiting for incoming UDP data");
    return noErr;
}

static OSErr OTImpl_UDPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
                                        ip_addr *remoteHost, udp_port *remotePort,
                                        Ptr *dataPtr, unsigned short *dataLength)
{
    OTAsyncOperation *asyncOp = (OTAsyncOperation *)asyncHandle;
    OTUDPEndpoint *udpEp;
    OSStatus err;
    InetAddress addr;
    TUnitData unitData;
    OTFlags flags = 0;

    if (asyncOp == NULL) {
        return paramErr;
    }

    if (!asyncOp->inUse) {
        return paramErr;
    }

    /* Check if operation completed */
    if (!asyncOp->completed) {
        return 1; /* Still in progress */
    }

    /* Get the UDP endpoint from the async operation */
    udpEp = (OTUDPEndpoint *)asyncOp->userData;
    if (udpEp == NULL || !udpEp->isCreated) {
        return paramErr;
    }

    /* Now actually receive the data using OTRcvUData */
    unitData.addr.buf = (UInt8 *)&addr;
    unitData.addr.maxlen = sizeof(addr);
    unitData.opt.buf = NULL;
    unitData.opt.maxlen = 0;
    unitData.udata.buf = (UInt8 *)udpEp->receiveBuffer;
    unitData.udata.maxlen = udpEp->bufferSize;

    err = OTRcvUData(udpEp->endpoint, &unitData, &flags);
    if (err != noErr) {
        if (err == kOTNoDataErr) {
            /* No data available - this is normal for async operations */
            return 1; /* Still pending */
        }
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCheckAsyncStatus: OTRcvUData failed: %d", err);
        return err;
    }

    /* Extract sender information and data */
    if (remoteHost != NULL && unitData.addr.len >= sizeof(InetAddress)) {
        *remoteHost = ((InetAddress *)unitData.addr.buf)->fHost;
    }
    if (remotePort != NULL && unitData.addr.len >= sizeof(InetAddress)) {
        *remotePort = ((InetAddress *)unitData.addr.buf)->fPort;
    }
    if (dataPtr != NULL) {
        *dataPtr = (Ptr)unitData.udata.buf;
    }
    if (dataLength != NULL) {
        *dataLength = unitData.udata.len;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCheckAsyncStatus: Received %d bytes from %08lX:%d",
                  unitData.udata.len,
                  (remoteHost && unitData.addr.len >= sizeof(InetAddress)) ? *remoteHost : 0,
                  (remotePort && unitData.addr.len >= sizeof(InetAddress)) ? *remotePort : 0);

    /* Free the UDP async handle - this was missing and caused handle pool exhaustion */
    FreeOTAsyncHandle(asyncHandle);

    return noErr; /* Completed */
}

static OSErr OTImpl_UDPReturnBufferAsync(NetworkEndpointRef endpointRef,
        Ptr buffer, unsigned short bufferSize,
        NetworkAsyncHandle *asyncHandle)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OTAsyncOperation *asyncOp;

    (void)buffer;
    (void)bufferSize; /* Unused in OpenTransport */

    if (udpEp == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async operation handle for consistency */
    *asyncHandle = AllocateOTAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    asyncOp = (OTAsyncOperation *)*asyncHandle;
    asyncOp->endpoint = udpEp->endpoint;
    asyncOp->opType = OT_ASYNC_UDP_RECEIVE; /* Reuse receive type */
    asyncOp->result = noErr;
    asyncOp->completed = true; /* Complete immediately */

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPReturnBufferAsync: Async buffer return completed");
    return noErr;
}

static OSErr OTImpl_UDPCheckReturnStatus(NetworkAsyncHandle asyncHandle)
{
    OTAsyncOperation *asyncOp = (OTAsyncOperation *)asyncHandle;

    if (asyncOp == NULL) {
        return paramErr;
    }

    if (!asyncOp->inUse) {
        return paramErr;
    }

    /* Check if return operation completed */
    if (!asyncOp->completed) {
        return 1; /* Still in progress */
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCheckReturnStatus: Return completed with result %d", asyncOp->result);
    return asyncOp->result;
}

static void OTImpl_UDPCancelAsync(NetworkAsyncHandle asyncHandle)
{
    OTAsyncOperation *asyncOp = (OTAsyncOperation *)asyncHandle;

    if (asyncOp == NULL || !asyncOp->inUse) {
        return;
    }

    /* Cancel the operation if possible */
    if (asyncOp->endpoint != kOTInvalidEndpointRef && !asyncOp->completed) {
        asyncOp->completed = true;
        asyncOp->result = kOTCanceledErr;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCancelAsync: Operation cancelled");
    FreeOTAsyncHandle(asyncHandle);
}

static void OTImpl_FreeAsyncHandle(NetworkAsyncHandle asyncHandle)
{
    /* This function frees an async handle without canceling the operation */
    /* Use this when an operation has completed and you want to free the handle */
    if (asyncHandle != NULL) {
        FreeOTAsyncHandle(asyncHandle);
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_FreeAsyncHandle: Handle freed");
    }
}

/* Emergency cleanup function to recover from handle exhaustion */
static void CleanupCompletedAsyncOps(void)
{
    int i, cleanedCount = 0;

    InitializeOTAsyncOps();

    for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
        if (gOTAsyncOps[i].inUse && gOTAsyncOps[i].completed) {
            /* Free completed operations that weren't properly cleaned up */
            gOTAsyncOps[i].inUse = false;
            gOTAsyncOps[i].endpoint = kOTInvalidEndpointRef;
            gOTAsyncOps[i].completed = false;
            gOTAsyncOps[i].result = noErr;
            gOTAsyncOps[i].userData = NULL;
            cleanedCount++;
        }
    }

    /* Count current pool status */
    int inUse = 0, completed = 0;
    for (i = 0; i < MAX_OT_ASYNC_OPS; i++) {
        if (gOTAsyncOps[i].inUse) {
            inUse++;
            if (gOTAsyncOps[i].completed) completed++;
        }
    }

    if (cleanedCount > 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "CleanupCompletedAsyncOps: Recovered %d stale handles, pool status: %d/%d in use (%d completed)",
                      cleanedCount, inUse, MAX_OT_ASYNC_OPS, completed);
    }
}

/* Utility functions */
static OSErr OTImpl_ResolveAddress(const char *hostname, ip_addr *address)
{
    OSStatus err;

    if (hostname == NULL || address == NULL) {
        return paramErr;
    }

    /* Try to parse as IP address first */
    err = OTInetStringToHost(hostname, (InetHost *)address);
    if (err == noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_ResolveAddress: %s resolved to %08lX (direct)", hostname, *address);
        return noErr;
    }

    /* For now, just return error for hostname resolution */
    /* Full DNS resolution would require more complex OpenTransport setup */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_ResolveAddress: DNS lookup not implemented for %s", hostname);
    return unimpErr;
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_ResolveAddress: %s resolved to %08lX", hostname, *address);
    return noErr;
}

static OSErr OTImpl_AddressToString(ip_addr address, char *addressStr)
{
    if (addressStr == NULL) {
        return paramErr;
    }

    /* CRITICAL: Add safety checks to prevent crash during factory pattern operations */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_AddressToString: Converting %08lX to string", address);

    /* Validate buffer before calling OTInetHostToString */
    if ((Ptr)addressStr < (Ptr)0x1000) {
        log_error_cat(LOG_CAT_NETWORKING, "OTImpl_AddressToString: Invalid buffer pointer %p", addressStr);
        return paramErr;
    }

    /* Convert IP address to string using Apple's function */
    OTInetHostToString(address, addressStr);

    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_AddressToString: %08lX -> %s", address, addressStr);
    return noErr;
}

static const char *OTImpl_GetImplementationName(void)
{
    return "OpenTransport";
}

static Boolean OTImpl_IsAvailable(void)
{
    OSStatus err;
    long response;

    /* If already initialized, it's available */
    if (gOTInitialized) {
        return true;
    }

    /* Check for OpenTransport availability using Gestalt without initializing */
    /* This avoids premature initialization that would bypass notifier setup */
    err = Gestalt(gestaltOpenTpt, &response);
    if (err == noErr) {
        /* Check if OpenTransport is present and TCP/IP support is available */
        if ((response & gestaltOpenTptPresentMask) && (response & gestaltOpenTptTCPPresentMask)) {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_IsAvailable: OpenTransport detected via Gestalt (flags 0x%08lX)", response);
            return true;
        } else {
            log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_IsAvailable: OpenTransport Gestalt returned insufficient features: 0x%08lX", response);
            return false;
        }
    } else {
        /* OpenTransport not available */
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_IsAvailable: Gestalt(gestaltOpenTpt) failed: %d", err);
        return false;
    }
}

/* Error translation */
NetworkError TranslateOTErrToNetworkError(OSStatus err)
{
    /* TODO: Implement proper OpenTransport error translation */
    switch (err) {
    case noErr:
        return NETWORK_SUCCESS;
    case kOTNoDataErr:
        return NETWORK_ERROR_TIMEOUT;
    case kOTOutOfMemoryErr:
        return NETWORK_ERROR_NO_MEMORY;
    case kOTBadNameErr:
    case kOTBadOptionErr:
        return NETWORK_ERROR_INVALID_PARAM;
    default:
        return NETWORK_ERROR_UNKNOWN;
    }
}

const char *GetOpenTransportErrorString(OSStatus err)
{
    /* TODO: Add comprehensive OpenTransport error strings */
    switch (err) {
    case noErr:
        return "Success";
    case kOTNoDataErr:
        return "No data available";
    case kOTOutOfMemoryErr:
        return "Out of memory";
    case kOTBadNameErr:
        return "Bad name";
    case kOTBadOptionErr:
        return "Bad option";
    default:
        return "Unknown OpenTransport error";
    }
}

/* Operations table for OpenTransport */
static NetworkOperations gOpenTransportOperations = {
    /* System operations */
    OTImpl_Initialize,
    OTImpl_Shutdown,

    /* TCP operations */
    OTImpl_TCPCreate,
    OTImpl_TCPRelease,
    OTImpl_TCPListen,
    OTImpl_TCPAcceptConnection,
    OTImpl_TCPConnect,
    OTImpl_TCPSend,
    OTImpl_TCPReceiveNoCopy,
    OTImpl_TCPReturnBuffer,
    OTImpl_TCPClose,
    OTImpl_TCPAbort,
    OTImpl_TCPStatus,
    OTImpl_TCPUnbind,

    /* Async TCP operations */
    OTImpl_TCPListenAsync,
    OTImpl_TCPConnectAsync,
    OTImpl_TCPSendAsync,
    OTImpl_TCPReceiveAsync,
    OTImpl_TCPCheckAsyncStatus,
    OTImpl_TCPCancelAsync,

    /* UDP operations */
    OTImpl_UDPCreate,
    OTImpl_UDPRelease,
    OTImpl_UDPSend,
    OTImpl_UDPReceive,
    OTImpl_UDPReturnBuffer,

    /* Async UDP operations */
    OTImpl_UDPSendAsync,
    OTImpl_UDPCheckSendStatus,
    OTImpl_UDPReceiveAsync,
    OTImpl_UDPCheckAsyncStatus,
    OTImpl_UDPReturnBufferAsync,
    OTImpl_UDPCheckReturnStatus,
    OTImpl_UDPCancelAsync,
    OTImpl_FreeAsyncHandle,

    /* Utility operations */
    OTImpl_ResolveAddress,
    OTImpl_AddressToString,

    /* Implementation info */
    OTImpl_GetImplementationName,
    OTImpl_IsAvailable
};

/* Get OpenTransport operations table */
NetworkOperations *GetOpenTransportOperations(void)
{
    return &gOpenTransportOperations;
}