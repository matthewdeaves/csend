//====================================
// FILE: ./classic_mac/opentransport_impl.c
//====================================

#include "opentransport_impl.h"
#include "../shared/logging.h"
#include "messaging.h" /* For ASR handler declarations */
#include <string.h>
#include <Errors.h>
#include <Gestalt.h>

/* External declarations for TCP stream variables */
extern NetworkStreamRef gTCPListenStream;
extern NetworkStreamRef gTCPSendStream;

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

#define MAX_OT_ASYNC_OPS 16
#define MAX_OT_TCP_ASYNC_OPS 8
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
            tcpEp->isConnected = true;
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
        log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: T_LISTEN - incoming connection");
        {
            /* Handle incoming connection request */
            TCall call;
            InetAddress clientAddr;
            OSStatus acceptErr;
            
            /* Set up call structure to receive connection details */
            call.addr.buf = (UInt8 *)&clientAddr;
            call.addr.maxlen = sizeof(clientAddr);
            call.opt.buf = NULL;
            call.opt.maxlen = 0;
            call.udata.buf = NULL;
            call.udata.maxlen = 0;
            
            /* Get the connection request details */
            acceptErr = OTListen(tcpEp->endpoint, &call);
            if (acceptErr == noErr) {
                /* Accept the connection */
                acceptErr = OTAccept(tcpEp->endpoint, tcpEp->endpoint, &call);
                if (acceptErr == noErr) {
                    /* Connection accepted successfully */
                    tcpEp->isConnected = true;
                    /* Keep isListening true for the listen stream to handle more connections */
                    tcpEp->remoteHost = clientAddr.fHost;
                    tcpEp->remotePort = clientAddr.fPort;
                    log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: Connection accepted from %d.%d.%d.%d:%d", 
                                  (tcpEp->remoteHost >> 24) & 0xFF,
                                  (tcpEp->remoteHost >> 16) & 0xFF, 
                                  (tcpEp->remoteHost >> 8) & 0xFF,
                                  tcpEp->remoteHost & 0xFF,
                                  tcpEp->remotePort);
                } else {
                    log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: OTAccept failed: %d", acceptErr);
                }
            } else {
                log_debug_cat(LOG_CAT_NETWORKING, "OTTCPNotifier: OTListen failed: %d", acceptErr);
            }
            
            /* Complete any pending listen operations */
            for (i = 0; i < MAX_OT_TCP_ASYNC_OPS; i++) {
                if (gOTTCPAsyncOps[i].inUse && 
                    gOTTCPAsyncOps[i].endpoint == tcpEp->endpoint &&
                    gOTTCPAsyncOps[i].opType == OT_ASYNC_TCP_LISTEN &&
                    !gOTTCPAsyncOps[i].completed) {
                    gOTTCPAsyncOps[i].completed = true;
                    gOTTCPAsyncOps[i].result = (acceptErr == noErr) ? result : acceptErr;
                    break;
                }
            }
        }
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

OSStatus InitializeOpenTransport(void)
{
    OSStatus err;
    
    if (gOTInitialized) {
        return noErr;
    }
    
    log_debug_cat(LOG_CAT_NETWORKING, "InitializeOpenTransport: Initializing OpenTransport");
    
    err = InitOpenTransport();
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitializeOpenTransport: InitOpenTransport failed: %d", err);
        return err;
    }
    
    InitializeOTAsyncOps();
    gOTInitialized = true;
    
    log_debug_cat(LOG_CAT_NETWORKING, "InitializeOpenTransport: OpenTransport initialized successfully");
    return noErr;
}

void ShutdownOpenTransport(void)
{
    if (gOTInitialized) {
        log_debug_cat(LOG_CAT_NETWORKING, "ShutdownOpenTransport: Closing OpenTransport");
        CloseOpenTransport();
        gOTInitialized = false;
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
    err = OTInetGetInterfaceInfo(&info, kDefaultInetInterface);
    if (err == noErr && localIP != NULL) {
        *localIP = info.fAddress;
        if (localIPStr != NULL) {
            OTInetHostToString(info.fAddress, localIPStr);
        }
    } else {
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
    
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Initialize: OpenTransport initialized");
    return noErr;
}

static void OTImpl_Shutdown(short refNum)
{
    (void)refNum; /* Unused */
    /* OpenTransport cleanup is handled by ShutdownOpenTransport() */
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_Shutdown: OpenTransport shutdown requested");
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
    
    (void)refNum; (void)notifyProc; /* Unused */
    
    if (streamRef == NULL) {
        return paramErr;
    }
    
    /* Create TCP endpoint */
    endpoint = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, &info, &err);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPCreate: OTOpenEndpoint failed: %d", err);
        return err;
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
    InetAddress addr;
    TBind reqAddr, retAddr;
    TCall call;
    
    (void)timeout; (void)async; /* Always async in OpenTransport */
    
    if (tcpEp == NULL) {
        return paramErr;
    }
    
    /* First unbind if already bound */
    if (tcpEp->isListening) {
        OTUnbind(tcpEp->endpoint);
        tcpEp->isListening = false;
    }
    
    /* Set up local address */
    OTInitInetAddress(&addr, localPort, kOTAnyInetAddress);
    
    /* Set up bind request */
    reqAddr.addr.buf = (UInt8 *)&addr;
    reqAddr.addr.len = sizeof(addr);
    reqAddr.qlen = 1; /* Listen queue length */
    
    retAddr.addr.buf = (UInt8 *)&addr;
    retAddr.addr.maxlen = sizeof(addr);
    
    /* Bind to local address */
    err = OTBind(tcpEp->endpoint, &reqAddr, &retAddr);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListen: OTBind failed: %d", err);
        return err;
    }
    
    /* Set up call structure for OTListen */
    call.addr.buf = (UInt8 *)&addr;
    call.addr.maxlen = sizeof(addr);
    call.opt.buf = NULL;
    call.opt.maxlen = 0;
    call.udata.buf = NULL;
    call.udata.maxlen = 0;
    
    /* Now listen for incoming connections - this is what was missing! */
    err = OTListen(tcpEp->endpoint, &call);
    if (err != noErr && err != kOTNoDataErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListen: OTListen failed: %d", err);
        OTUnbind(tcpEp->endpoint);
        return err;
    }
    
    tcpEp->localPort = localPort;
    tcpEp->isListening = true;
    
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPListen: Successfully bound and listening on port %d", localPort);
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
    
    (void)timeout; (void)giveTime; /* Async operation, timeout handled by caller */
    
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
    
    (void)timeout; (void)giveTime; /* Async operation */
    
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
    
    (void)timeout; (void)giveTime; /* Simplified for OpenTransport */
    
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
    OTBuffer* otBufferChain;
    
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
    
    (void)timeout; (void)giveTime; /* OpenTransport handles timing internally */
    
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
    OSStatus err;
    
    if (tcpEp == NULL) {
        return paramErr;
    }
    
    /* Abort connection immediately */
    err = OTSndDisconnect(tcpEp->endpoint, NULL);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: OTSndDisconnect failed: %d", err);
    }
    
    tcpEp->isConnected = false;
    
    /* For OpenTransport abort, we need to trigger the appropriate ASR event */
    /* This ensures proper state machine transitions in the messaging layer */
    if (tcpEp == (OTTCPEndpoint *)gTCPSendStream) {
        /* This is the send stream - generate TCPTerminate event to trigger RELEASING state */
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Triggering TCPTerminate for send stream");
        TCP_Send_ASR_Handler((StreamPtr)tcpEp, TCPTerminate, NULL, 0, NULL);
    } else if (tcpEp == (OTTCPEndpoint *)gTCPListenStream) {
        /* This is the listen stream - generate TCPTerminate event */
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Triggering TCPTerminate for listen stream");
        TCP_Listen_ASR_Handler((StreamPtr)tcpEp, TCPTerminate, NULL, 0, NULL);
    }
    
    log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPAbort: Connection aborted");
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
    
    /* Check endpoint state and bind if needed (FIX: Missing OTBind for outgoing connections) */
    state = OTGetEndpointState(tcpEp->endpoint);
    if (state == T_UNBND) {
        InetAddress localAddr;
        TBind reqAddr, retAddr;
        
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint in T_UNBND state, binding to ephemeral port");
        
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
    } else if (state != T_IDLE) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_TCPConnectAsync: Endpoint in invalid state %d for connect", state);
        return kOTOutStateErr;
    }
    
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
    
    if (endpointRef == NULL) {
        return paramErr;
    }
    
    /* Create UDP endpoint */
    endpoint = OTOpenEndpoint(OTCreateConfiguration(kUDPName), 0, &info, &err);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTOpenEndpoint failed: %d", err);
        return err;
    }
    
    /* Allocate UDP endpoint structure */
    udpEp = (OTUDPEndpoint *)NewPtr(sizeof(OTUDPEndpoint));
    if (udpEp == NULL) {
        OTCloseProvider(endpoint);
        return memFullErr;
    }
    
    /* Initialize UDP endpoint */
    udpEp->endpoint = endpoint;
    udpEp->localPort = localPort;
    udpEp->receiveBuffer = recvBuffer;
    udpEp->bufferSize = bufferSize;
    udpEp->isCreated = true;
    
    /* Install notifier */
    err = OTInstallNotifier(endpoint, OTUDPNotifier, udpEp);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "OTImpl_UDPCreate: OTInstallNotifier failed: %d", err);
        DisposePtr((Ptr)udpEp);
        OTCloseProvider(endpoint);
        return err;
    }
    
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
        *(UInt32*)option.value = T_YES; /* Enable broadcast reception */

        /* Prepare the request for OTOptionManagement */
        request.opt.buf = (UInt8*)&option;
        request.opt.len = sizeof(option);
        request.flags   = T_NEGOTIATE;
        
        result.opt.buf = (UInt8*)&option;
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
    
    (void)buffer; (void)bufferSize; (void)async; /* Unused in OpenTransport */
    
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
    return noErr; /* Completed */
}

static OSErr OTImpl_UDPReturnBufferAsync(NetworkEndpointRef endpointRef,
                                         Ptr buffer, unsigned short bufferSize,
                                         NetworkAsyncHandle *asyncHandle)
{
    OTUDPEndpoint *udpEp = (OTUDPEndpoint *)endpointRef;
    OTAsyncOperation *asyncOp;
    
    (void)buffer; (void)bufferSize; /* Unused in OpenTransport */
    
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
    
    /* Convert IP address to string */
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
    OSErr err;
    long response;
    
    /* If already initialized, it's available */
    if (gOTInitialized) {
        return true;
    }
    
    /* Check if OpenTransport is available using Gestalt */
    err = Gestalt('otan', &response);
    if (err != noErr) {
        return false;
    }
    
    /* Check required bits: present, loaded, TCP present, TCP loaded */
    if ((response & ((1 << 0) | (1 << 1) | (1 << 4) | (1 << 5))) != 
        ((1 << 0) | (1 << 1) | (1 << 4) | (1 << 5))) {
        return false;
    }
    
    return true;
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
    OTImpl_TCPConnect,
    OTImpl_TCPSend,
    OTImpl_TCPReceiveNoCopy,
    OTImpl_TCPReturnBuffer,
    OTImpl_TCPClose,
    OTImpl_TCPAbort,
    OTImpl_TCPStatus,

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