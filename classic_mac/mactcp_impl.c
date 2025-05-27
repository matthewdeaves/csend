//====================================
// FILE: ./classic_mac/mactcp_impl.c
//====================================

#include "mactcp_impl.h"
#include "network_init.h"
#include "../shared/logging.h"
#include <MacTCP.h>
#include <Devices.h>
#include <Errors.h>
#include <Files.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */
struct hostInfo;

/* DNR function declarations */
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);
extern OSErr StrToAddr(char *hostName, struct hostInfo *rtnStruct,
                       long resultProc, char *userData);

/* UDP implementation structures */
typedef struct {
    StreamPtr stream;
    udp_port localPort;
    Ptr recvBuffer;
    unsigned short bufferSize;
    Boolean isCreated;
} MacTCPUDPEndpoint;

/* Async operation tracking */
typedef struct {
    UDPiopb pb;
    Boolean inUse;
    NetworkEndpointRef endpoint;
    Boolean isReturnBuffer;  /* true for buffer return, false for receive */
    Boolean isSend;          /* true for send operation */
    wdsEntry *wdsArray;      /* For send operations */
} MacTCPAsyncOp;

/* TCP async operation tracking */
typedef enum {
    TCP_ASYNC_CONNECT,
    TCP_ASYNC_SEND,
    TCP_ASYNC_RECEIVE,
    TCP_ASYNC_CLOSE,
    TCP_ASYNC_LISTEN
} TCPAsyncOpType;

typedef struct {
    TCPiopb pb;
    Boolean inUse;
    NetworkStreamRef stream;
    TCPAsyncOpType opType;
    Ptr dataBuffer;           /* For send/receive operations */
    unsigned short dataLength;
    rdsEntry *rdsArray;       /* For receive operations */
    short rdsCount;
} TCPAsyncOp;

#define MAX_ASYNC_OPS 4
#define MAX_TCP_ASYNC_OPS 8
static MacTCPAsyncOp gAsyncOps[MAX_ASYNC_OPS];
static TCPAsyncOp gTCPAsyncOps[MAX_TCP_ASYNC_OPS];
static Boolean gAsyncOpsInitialized = false;
static Boolean gTCPAsyncOpsInitialized = false;

/* Forward declarations for all functions */
static OSErr MacTCPImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr);
static void MacTCPImpl_Shutdown(short refNum);
static OSErr MacTCPImpl_TCPCreate(short refNum, NetworkStreamRef *streamRef,
                                  unsigned long rcvBufferSize, Ptr rcvBuffer,
                                  NetworkNotifyProcPtr notifyProc);
static OSErr MacTCPImpl_TCPRelease(short refNum, NetworkStreamRef streamRef);
static OSErr MacTCPImpl_TCPListen(NetworkStreamRef streamRef, tcp_port localPort,
                                  Byte timeout, Boolean async);
static OSErr MacTCPImpl_TCPConnect(NetworkStreamRef streamRef, ip_addr remoteHost,
                                   tcp_port remotePort, Byte timeout,
                                   NetworkGiveTimeProcPtr giveTime);
static OSErr MacTCPImpl_TCPSend(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime);
static OSErr MacTCPImpl_TCPReceiveNoCopy(NetworkStreamRef streamRef, Ptr rdsPtr,
        short maxEntries, Byte timeout,
        Boolean *urgent, Boolean *mark,
        NetworkGiveTimeProcPtr giveTime);
static OSErr MacTCPImpl_TCPReturnBuffer(NetworkStreamRef streamRef, Ptr rdsPtr,
                                        NetworkGiveTimeProcPtr giveTime);
static OSErr MacTCPImpl_TCPClose(NetworkStreamRef streamRef, Byte timeout,
                                 NetworkGiveTimeProcPtr giveTime);
static OSErr MacTCPImpl_TCPAbort(NetworkStreamRef streamRef);
static OSErr MacTCPImpl_TCPStatus(NetworkStreamRef streamRef, NetworkTCPInfo *info);

/* Async TCP operations */
static OSErr MacTCPImpl_TCPListenAsync(NetworkStreamRef streamRef, tcp_port localPort,
                                       NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_TCPConnectAsync(NetworkStreamRef streamRef, ip_addr remoteHost,
                                        tcp_port remotePort, NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_TCPSendAsync(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                     Boolean push, NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_TCPReceiveAsync(NetworkStreamRef streamRef, Ptr rdsPtr,
                                        short maxEntries, NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_TCPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
        OSErr *operationResult, void **resultData);
static void MacTCPImpl_TCPCancelAsync(NetworkAsyncHandle asyncHandle);

static OSErr MacTCPImpl_UDPCreate(short refNum, NetworkEndpointRef *endpointRef,
                                  udp_port localPort, Ptr recvBuffer,
                                  unsigned short bufferSize);
static OSErr MacTCPImpl_UDPRelease(short refNum, NetworkEndpointRef endpointRef);
static OSErr MacTCPImpl_UDPSend(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                udp_port remotePort, Ptr data, unsigned short length);
static OSErr MacTCPImpl_UDPReceive(NetworkEndpointRef endpointRef, ip_addr *remoteHost,
                                   udp_port *remotePort, Ptr buffer,
                                   unsigned short *length, Boolean async);
static OSErr MacTCPImpl_UDPReturnBuffer(NetworkEndpointRef endpointRef, Ptr buffer,
                                        unsigned short bufferSize, Boolean async);
static OSErr MacTCPImpl_UDPSendAsync(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                     udp_port remotePort, Ptr data, unsigned short length,
                                     NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_UDPCheckSendStatus(NetworkAsyncHandle asyncHandle);
static OSErr MacTCPImpl_UDPReceiveAsync(NetworkEndpointRef endpointRef,
                                        NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_UDPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
        ip_addr *remoteHost, udp_port *remotePort,
        Ptr *dataPtr, unsigned short *dataLength);
static OSErr MacTCPImpl_UDPReturnBufferAsync(NetworkEndpointRef endpointRef,
        Ptr buffer, unsigned short bufferSize,
        NetworkAsyncHandle *asyncHandle);
static OSErr MacTCPImpl_UDPCheckReturnStatus(NetworkAsyncHandle asyncHandle);
static void MacTCPImpl_UDPCancelAsync(NetworkAsyncHandle asyncHandle);
static OSErr MacTCPImpl_ResolveAddress(const char *hostname, ip_addr *address);
static OSErr MacTCPImpl_AddressToString(ip_addr address, char *addressStr);
static const char *MacTCPImpl_GetImplementationName(void);
static Boolean MacTCPImpl_IsAvailable(void);

/* Helper functions */
static void InitializeAsyncOps(void)
{
    if (!gAsyncOpsInitialized) {
        memset(gAsyncOps, 0, sizeof(gAsyncOps));
        gAsyncOpsInitialized = true;
    }
}

static void InitializeTCPAsyncOps(void)
{
    if (!gTCPAsyncOpsInitialized) {
        memset(gTCPAsyncOps, 0, sizeof(gTCPAsyncOps));
        gTCPAsyncOpsInitialized = true;
    }
}

static NetworkAsyncHandle AllocateAsyncHandle(void)
{
    int i;

    InitializeAsyncOps();

    for (i = 0; i < MAX_ASYNC_OPS; i++) {
        if (!gAsyncOps[i].inUse) {
            gAsyncOps[i].inUse = true;
            return (NetworkAsyncHandle)&gAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateAsyncHandle: No free async operation slots");
    return NULL;
}

static void FreeAsyncHandle(NetworkAsyncHandle handle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)handle;

    if (op >= &gAsyncOps[0] && op < &gAsyncOps[MAX_ASYNC_OPS]) {
        /* Free any allocated WDS for send operations */
        if (op->isSend && op->wdsArray != NULL) {
            DisposePtr((Ptr)op->wdsArray);
            op->wdsArray = NULL;
        }
        op->inUse = false;
        op->endpoint = NULL;
        op->isReturnBuffer = false;
        op->isSend = false;
    }
}

static NetworkAsyncHandle AllocateTCPAsyncHandle(void)
{
    int i;

    InitializeTCPAsyncOps();

    for (i = 0; i < MAX_TCP_ASYNC_OPS; i++) {
        if (!gTCPAsyncOps[i].inUse) {
            gTCPAsyncOps[i].inUse = true;
            return (NetworkAsyncHandle)&gTCPAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateTCPAsyncHandle: No free TCP async operation slots");
    return NULL;
}

static void FreeTCPAsyncHandle(NetworkAsyncHandle handle)
{
    TCPAsyncOp *op = (TCPAsyncOp *)handle;

    if (op >= &gTCPAsyncOps[0] && op < &gTCPAsyncOps[MAX_TCP_ASYNC_OPS]) {
        op->inUse = false;
        op->stream = NULL;
        op->dataBuffer = NULL;
        op->dataLength = 0;
        op->rdsArray = NULL;
        op->rdsCount = 0;
    }
}

static MacTCPUDPEndpoint *AllocateUDPEndpoint(void)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)NewPtrClear(sizeof(MacTCPUDPEndpoint));
    if (endpoint == NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "AllocateUDPEndpoint: Failed to allocate memory");
    }
    return endpoint;
}

static void FreeUDPEndpoint(MacTCPUDPEndpoint *endpoint)
{
    if (endpoint != NULL) {
        if (endpoint->recvBuffer != NULL) {
            DisposePtr(endpoint->recvBuffer);
        }
        DisposePtr((Ptr)endpoint);
    }
}

/* Implementation of network operations for MacTCP */

static OSErr MacTCPImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr)
{
    OSErr err;
    ParamBlockRec pbOpen;
    CntrlParam cntrlPB;

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: Opening MacTCP driver");

    /* Open MacTCP driver */
    memset(&pbOpen, 0, sizeof(ParamBlockRec));
    pbOpen.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pbOpen.ioParam.ioPermssn = fsCurPerm;

    err = PBOpenSync(&pbOpen);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open MacTCP driver: %d", err);
        return err;
    }

    *refNum = pbOpen.ioParam.ioRefNum;
    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: MacTCP driver opened, refNum: %d", *refNum);

    /* Get local IP address */
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = *refNum;
    cntrlPB.csCode = ipctlGetAddr;

    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to get IP address: %d", err);
        return err;
    }

    BlockMoveData(&cntrlPB.csParam[0], localIP, sizeof(ip_addr));

    /* Initialize DNR */
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open resolver: %d", err);
        return err;
    }

    /* Convert IP to string */
    err = AddrToStr(*localIP, localIPStr);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: AddrToStr failed: %d", err);
        /* Fallback formatting */
        sprintf(localIPStr, "%lu.%lu.%lu.%lu",
                (*localIP >> 24) & 0xFF,
                (*localIP >> 16) & 0xFF,
                (*localIP >> 8) & 0xFF,
                *localIP & 0xFF);
    }

    /* Initialize async operations */
    InitializeAsyncOps();
    InitializeTCPAsyncOps();

    log_app_event("MacTCPImpl_Initialize: Success. Local IP: %s", localIPStr);
    return noErr;
}

static void MacTCPImpl_Shutdown(short refNum)
{
    (void)refNum; /* Unused parameter */

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Shutdown: Closing resolver");
    CloseResolver();

    /* Note: We don't close the MacTCP driver as other apps may be using it */
    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Shutdown: Complete (driver remains open for system)");
}

/* Global storage for the notify proc wrapper */
static NetworkNotifyProcPtr gStoredNotifyProc = NULL;

/* Pascal wrapper that calls the C notify proc */
static pascal void MacTCPNotifyWrapper(StreamPtr tcpStream, unsigned short eventCode,
                                       Ptr userDataPtr, unsigned short terminReason,
                                       ICMPReport *icmpMsg)
{
    if (gStoredNotifyProc) {
        gStoredNotifyProc((void *)tcpStream, eventCode, userDataPtr, terminReason,
                          (struct ICMPReport *)icmpMsg);
    }
}

static OSErr MacTCPImpl_TCPCreate(short refNum, NetworkStreamRef *streamRef,
                                  unsigned long rcvBufferSize, Ptr rcvBuffer,
                                  NetworkNotifyProcPtr notifyProc)
{
    TCPiopb pb;
    OSErr err;

    /* Store the C notify proc for the wrapper to call */
    gStoredNotifyProc = notifyProc;

    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = rcvBuffer;
    pb.csParam.create.rcvBuffLen = rcvBufferSize;
    pb.csParam.create.notifyProc = MacTCPNotifyWrapper;

    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr) {
        *streamRef = (NetworkStreamRef)pb.tcpStream;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCreate: Created stream 0x%lX", (unsigned long)*streamRef);
    } else {
        *streamRef = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCreate: Failed: %d", err);
    }

    return err;
}

static OSErr MacTCPImpl_TCPRelease(short refNum, NetworkStreamRef streamRef)
{
    TCPiopb pb;

    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPRelease;
    pb.tcpStream = (StreamPtr)streamRef;

    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_TCPListen(NetworkStreamRef streamRef, tcp_port localPort,
                                  Byte timeout, Boolean async)
{
    TCPiopb pb;
    OSErr err;

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPPassiveOpen;
    pb.csParam.open.ulpTimeoutValue = 20; /* Default ULP timeout */
    pb.csParam.open.ulpTimeoutAction = 1;
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = timeout;
    pb.csParam.open.localPort = localPort;
    pb.csParam.open.localHost = 0;
    pb.csParam.open.remoteHost = 0;
    pb.csParam.open.remotePort = 0;

    if (async) {
        pb.ioCompletion = nil;
        pb.ioCRefNum = gMacTCPRefNum;
        pb.ioResult = 1;
        err = PBControlAsync((ParmBlkPtr)&pb);
    } else {
        err = PBControlSync((ParmBlkPtr)&pb);
    }

    return err;
}

/* Async TCP Listen implementation */
static OSErr MacTCPImpl_TCPListenAsync(NetworkStreamRef streamRef, tcp_port localPort,
                                       NetworkAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_LISTEN;

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPPassiveOpen;
    op->pb.csParam.open.ulpTimeoutValue = 20; /* Default ULP timeout */
    op->pb.csParam.open.ulpTimeoutAction = 1;
    op->pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.open.commandTimeoutValue = 0; /* Non-blocking */
    op->pb.csParam.open.localPort = localPort;
    op->pb.csParam.open.localHost = 0;
    op->pb.csParam.open.remoteHost = 0;
    op->pb.csParam.open.remotePort = 0;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPListenAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPListenAsync: Started async listen on port %u", localPort);
    return noErr;
}

static OSErr MacTCPImpl_TCPConnect(NetworkStreamRef streamRef, ip_addr remoteHost,
                                   tcp_port remotePort, Byte timeout,
                                   NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPActiveOpen;
    pb.csParam.open.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.open.ulpTimeoutAction = 1;
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.remoteHost = remoteHost;
    pb.csParam.open.remotePort = remotePort;
    pb.csParam.open.localPort = 0; /* Let MacTCP assign a random ephemeral port */
    pb.csParam.open.localHost = 0;
    pb.csParam.open.commandTimeoutValue = timeout;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

/* Async TCP Connect implementation */
static OSErr MacTCPImpl_TCPConnectAsync(NetworkStreamRef streamRef, ip_addr remoteHost,
                                        tcp_port remotePort, NetworkAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_CONNECT;

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPActiveOpen;
    op->pb.csParam.open.ulpTimeoutValue = 30;  /* Default timeout */
    op->pb.csParam.open.ulpTimeoutAction = 1;
    op->pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.open.remoteHost = remoteHost;
    op->pb.csParam.open.remotePort = remotePort;
    op->pb.csParam.open.localPort = 0; /* Let MacTCP assign port */
    op->pb.csParam.open.localHost = 0;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPConnectAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPConnectAsync: Started async connect to %lu:%u",
                  remoteHost, remotePort);
    return noErr;
}

static OSErr MacTCPImpl_TCPSend(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    wdsEntry wds[2];
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || data == NULL) {
        return paramErr;
    }

    /* Set up WDS */
    wds[0].length = length;
    wds[0].ptr = data;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPSend;
    pb.csParam.send.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag = push;
    pb.csParam.send.urgentFlag = false;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

/* Async TCP Send implementation */
static OSErr MacTCPImpl_TCPSendAsync(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                                     Boolean push, NetworkAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;
    wdsEntry *wds;

    if (streamRef == NULL || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_SEND;
    op->dataBuffer = data;
    op->dataLength = length;

    /* Allocate WDS array - needs to persist for async operation */
    wds = (wdsEntry *)NewPtrClear(sizeof(wdsEntry) * 2);
    if (wds == NULL) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        return memFullErr;
    }

    /* Set up WDS */
    wds[0].length = length;
    wds[0].ptr = data;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPSend;
    op->pb.csParam.send.ulpTimeoutValue = 30;  /* Default timeout */
    op->pb.csParam.send.ulpTimeoutAction = 1;
    op->pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.send.pushFlag = push;
    op->pb.csParam.send.urgentFlag = false;
    op->pb.csParam.send.wdsPtr = (Ptr)wds;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Store WDS pointer for cleanup */
    op->rdsArray = (rdsEntry *)wds;  /* Reuse rdsArray field */

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        DisposePtr((Ptr)wds);
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPSendAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPSendAsync: Started async send of %u bytes", length);
    return noErr;
}

static OSErr MacTCPImpl_TCPReceiveNoCopy(NetworkStreamRef streamRef, Ptr rdsPtr,
        short maxEntries, Byte timeout,
        Boolean *urgent, Boolean *mark,
        NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    OSErr err;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPNoCopyRcv;
    pb.csParam.receive.commandTimeoutValue = timeout;
    pb.csParam.receive.rdsPtr = rdsPtr;
    pb.csParam.receive.rdsLength = maxEntries;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        if (urgent) *urgent = pb.csParam.receive.urgentFlag;
        if (mark) *mark = pb.csParam.receive.markFlag;
    }

    return err;
}

/* Async TCP Receive implementation */
static OSErr MacTCPImpl_TCPReceiveAsync(NetworkStreamRef streamRef, Ptr rdsPtr,
                                        short maxEntries, NetworkAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || rdsPtr == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_RECEIVE;
    op->rdsArray = (rdsEntry *)rdsPtr;
    op->rdsCount = maxEntries;

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPNoCopyRcv;
    op->pb.csParam.receive.commandTimeoutValue = 0;  /* Non-blocking */
    op->pb.csParam.receive.rdsPtr = rdsPtr;
    op->pb.csParam.receive.rdsLength = maxEntries;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPReceiveAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPReceiveAsync: Started async receive");
    return noErr;
}

static OSErr MacTCPImpl_TCPReturnBuffer(NetworkStreamRef streamRef, Ptr rdsPtr,
                                        NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPRcvBfrReturn;
    pb.csParam.receive.rdsPtr = rdsPtr;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call */
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_TCPClose(NetworkStreamRef streamRef, Byte timeout,
                                 NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPClose;
    pb.csParam.close.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.close.ulpTimeoutAction = 1;
    pb.csParam.close.validityFlags = timeoutValue | timeoutAction;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_TCPAbort(NetworkStreamRef streamRef)
{
    TCPiopb pb;

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPAbort;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* For abort, we always use sync to ensure immediate effect */
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_TCPStatus(NetworkStreamRef streamRef, NetworkTCPInfo *info)
{
    TCPiopb pb;
    OSErr err;

    if (streamRef == NULL || info == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPStatus;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call */
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        info->localHost = pb.csParam.status.localHost;
        info->localPort = pb.csParam.status.localPort;
        info->remoteHost = pb.csParam.status.remoteHost;
        info->remotePort = pb.csParam.status.remotePort;
        info->isConnected = (pb.csParam.status.connectionState >= 8); /* established */
        info->isListening = (pb.csParam.status.connectionState == 2); /* listening */
    }

    return err;
}

/* UDP Operations */

static OSErr MacTCPImpl_UDPCreate(short refNum, NetworkEndpointRef *endpointRef,
                                  udp_port localPort, Ptr recvBuffer,
                                  unsigned short bufferSize)
{
    OSErr err;
    UDPiopb pb;
    MacTCPUDPEndpoint *endpoint;

    if (endpointRef == NULL) {
        return paramErr;
    }

    /* Allocate endpoint structure */
    endpoint = AllocateUDPEndpoint();
    if (endpoint == NULL) {
        return memFullErr;
    }

    /* If no buffer provided, allocate one */
    if (recvBuffer == NULL) {
        endpoint->recvBuffer = NewPtrClear(bufferSize);
        if (endpoint->recvBuffer == NULL) {
            FreeUDPEndpoint(endpoint);
            return memFullErr;
        }
    } else {
        endpoint->recvBuffer = recvBuffer;
    }

    endpoint->bufferSize = bufferSize;
    endpoint->localPort = localPort;

    /* Create UDP endpoint */
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = refNum;
    pb.csCode = UDPCreate;
    pb.csParam.create.rcvBuff = endpoint->recvBuffer;
    pb.csParam.create.rcvBuffLen = bufferSize;
    pb.csParam.create.notifyProc = nil;
    pb.csParam.create.localPort = localPort;

    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCreate: Failed: %d", err);
        FreeUDPEndpoint(endpoint);
        return err;
    }

    /* Store the actual stream pointer returned by MacTCP */
    endpoint->stream = pb.udpStream;
    endpoint->isCreated = true;
    *endpointRef = (NetworkEndpointRef)endpoint;

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCreate: Success. Stream: 0x%lX (endpoint: 0x%lX), Port: %u",
                  (unsigned long)endpoint->stream, (unsigned long)endpoint, pb.csParam.create.localPort);

    return noErr;
}

static OSErr MacTCPImpl_UDPRelease(short refNum, NetworkEndpointRef endpointRef)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    OSErr err;

    if (endpoint == NULL || !endpoint->isCreated) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = refNum;
    pb.csCode = UDPRelease;
    pb.udpStream = endpoint->stream;

    err = PBControlSync((ParmBlkPtr)&pb);

    /* Free endpoint regardless of error */
    endpoint->isCreated = false;
    FreeUDPEndpoint(endpoint);

    return err;
}

static OSErr MacTCPImpl_UDPSend(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                udp_port remotePort, Ptr data, unsigned short length)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    wdsEntry wds[2];

    if (endpoint == NULL || !endpoint->isCreated || data == NULL) {
        return paramErr;
    }

    /* Set up WDS */
    wds[0].length = length;
    wds[0].ptr = data;
    wds[1].length = 0;
    wds[1].ptr = nil;

    /* Send datagram */
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum; /* Use global ref */
    pb.csCode = UDPWrite;
    pb.udpStream = endpoint->stream;
    pb.csParam.send.remoteHost = remoteHost;
    pb.csParam.send.remotePort = remotePort;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.checkSum = true;

    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_UDPReceive(NetworkEndpointRef endpointRef, ip_addr *remoteHost,
                                   udp_port *remotePort, Ptr buffer,
                                   unsigned short *length, Boolean async)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    OSErr err;

    if (endpoint == NULL || !endpoint->isCreated || buffer == NULL || length == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPRead;
    pb.udpStream = endpoint->stream;
    pb.csParam.receive.rcvBuff = buffer;
    pb.csParam.receive.rcvBuffLen = *length;
    pb.csParam.receive.timeOut = async ? 0 : 1; /* 0 for async, 1 sec for sync */

    if (async) {
        err = PBControlAsync((ParmBlkPtr)&pb);
        if (err == noErr) {
            /* For async, caller needs to poll ioResult */
            return 1; /* Indicate pending */
        }
    } else {
        err = PBControlSync((ParmBlkPtr)&pb);
    }

    if (err == noErr) {
        if (remoteHost) *remoteHost = pb.csParam.receive.remoteHost;
        if (remotePort) *remotePort = pb.csParam.receive.remotePort;
        *length = pb.csParam.receive.rcvBuffLen;
    }

    return err;
}

static OSErr MacTCPImpl_UDPReturnBuffer(NetworkEndpointRef endpointRef, Ptr buffer,
                                        unsigned short bufferSize, Boolean async)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;

    if (endpoint == NULL || !endpoint->isCreated || buffer == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPBfrReturn;
    pb.udpStream = endpoint->stream;
    pb.csParam.receive.rcvBuff = buffer;
    pb.csParam.receive.rcvBuffLen = bufferSize;

    if (async) {
        return PBControlAsync((ParmBlkPtr)&pb);
    } else {
        return PBControlSync((ParmBlkPtr)&pb);
    }
}

/* Async UDP operations */

static OSErr MacTCPImpl_UDPSendAsync(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                                     udp_port remotePort, Ptr data, unsigned short length,
                                     NetworkAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;
    wdsEntry *wds;

    if (endpoint == NULL || !endpoint->isCreated || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = false;
    op->isSend = true;

    /* Allocate WDS array - needs to persist for async operation */
    wds = (wdsEntry *)NewPtrClear(sizeof(wdsEntry) * 2);
    if (wds == NULL) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        return memFullErr;
    }

    /* Set up WDS */
    wds[0].length = length;
    wds[0].ptr = data;
    wds[1].length = 0;
    wds[1].ptr = nil;

    /* Store WDS for cleanup */
    op->wdsArray = wds;

    /* Set up async UDP write */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPWrite;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.send.remoteHost = remoteHost;
    op->pb.csParam.send.remotePort = remotePort;
    op->pb.csParam.send.wdsPtr = (Ptr)wds;
    op->pb.csParam.send.checkSum = true;
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        DisposePtr((Ptr)wds);
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPSendAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPSendAsync: Started async send of %u bytes to %lu:%u",
                      length, remoteHost, remotePort);
    }

    return err;
}

static OSErr MacTCPImpl_UDPCheckSendStatus(NetworkAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !op->isSend) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Operation complete - free the handle (which also frees WDS) */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

static OSErr MacTCPImpl_UDPReceiveAsync(NetworkEndpointRef endpointRef,
                                        NetworkAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;

    if (!endpoint || !endpoint->isCreated || !asyncHandle) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = false;

    /* Set up async UDP read */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPRead;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.receive.rcvBuff = endpoint->recvBuffer;
    op->pb.csParam.receive.rcvBuffLen = endpoint->bufferSize;
    op->pb.csParam.receive.timeOut = 0; /* Non-blocking */
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReceiveAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReceiveAsync: Started async read");
    }

    return err;
}

static OSErr MacTCPImpl_UDPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
        ip_addr *remoteHost, udp_port *remotePort,
        Ptr *dataPtr, unsigned short *dataLength)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    if (ioResult == noErr) {
        if (remoteHost) *remoteHost = op->pb.csParam.receive.remoteHost;
        if (remotePort) *remotePort = op->pb.csParam.receive.remotePort;
        if (dataPtr) *dataPtr = op->pb.csParam.receive.rcvBuff;
        if (dataLength) *dataLength = op->pb.csParam.receive.rcvBuffLen;
    }

    /* Mark as available */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

static OSErr MacTCPImpl_UDPReturnBufferAsync(NetworkEndpointRef endpointRef,
        Ptr buffer, unsigned short bufferSize,
        NetworkAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;

    if (!endpoint || !endpoint->isCreated || !buffer || !asyncHandle) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = true;

    /* Set up async buffer return */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPBfrReturn;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.receive.rcvBuff = buffer;
    op->pb.csParam.receive.rcvBuffLen = bufferSize;
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReturnBufferAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReturnBufferAsync: Started async buffer return");
    }

    return err;
}

static OSErr MacTCPImpl_UDPCheckReturnStatus(NetworkAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !op->isReturnBuffer) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Mark as available */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

static void MacTCPImpl_UDPCancelAsync(NetworkAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;

    if (op && op->inUse) {
        /* Note: MacTCP doesn't provide a way to cancel async operations */
        /* We just mark it as free and let it complete in the background */
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCancelAsync: Marking handle as free (can't cancel MacTCP async)");
        FreeAsyncHandle(asyncHandle);
    }
}

static OSErr MacTCPImpl_ResolveAddress(const char *hostname, ip_addr *address)
{
    /* For now, just try to parse as IP address */
    return ParseIPv4(hostname, address);

    /* TODO: Implement actual DNS resolution using StrToAddr */
}

static OSErr MacTCPImpl_AddressToString(ip_addr address, char *addressStr)
{
    return AddrToStr(address, addressStr);
}

static const char *MacTCPImpl_GetImplementationName(void)
{
    return "MacTCP";
}

static Boolean MacTCPImpl_IsAvailable(void)
{
    /* Check if MacTCP driver can be opened */
    ParamBlockRec pb;
    OSErr err;

    memset(&pb, 0, sizeof(ParamBlockRec));
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm;

    err = PBOpenSync(&pb);
    if (err == noErr) {
        /* Driver opened, close it immediately */
        pb.ioParam.ioRefNum = pb.ioParam.ioRefNum;
        PBCloseSync(&pb);
        return true;
    }

    /* If already open, that's OK too */
    return (err == opWrErr);
}

/* TCP Async Status Checking */
static OSErr MacTCPImpl_TCPCheckAsyncStatus(NetworkAsyncHandle asyncHandle,
        OSErr *operationResult, void **resultData)
{
    TCPAsyncOp *op = (TCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !operationResult) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Operation complete */
    *operationResult = ioResult;

    /* Return operation-specific data if requested */
    if (resultData) {
        switch (op->opType) {
        case TCP_ASYNC_CONNECT:
            /* For connect, no additional data */
            *resultData = NULL;
            break;

        case TCP_ASYNC_SEND:
            /* For send, return bytes sent (in csParam.send.sendLength) */
            *resultData = (void *)(unsigned long)op->pb.csParam.send.sendLength;
            break;

        case TCP_ASYNC_RECEIVE:
            /* For receive, resultData should point to a structure with urgent/mark flags */
            /* Caller should interpret based on context */
            *resultData = (void *)&op->pb.csParam.receive;
            break;

        case TCP_ASYNC_CLOSE:
            /* For close, no additional data */
            *resultData = NULL;
            break;

        case TCP_ASYNC_LISTEN:
            /* For listen, no additional data */
            *resultData = NULL;
            break;

        default:
            *resultData = NULL;
            break;
        }
    }

    /* Clean up any allocated resources */
    if (op->opType == TCP_ASYNC_SEND && op->rdsArray) {
        /* Free the WDS array we allocated */
        DisposePtr((Ptr)op->rdsArray);
        op->rdsArray = NULL;
    }

    /* Free the async handle */
    FreeTCPAsyncHandle(asyncHandle);

    return noErr;
}

/* TCP Async Cancel */
static void MacTCPImpl_TCPCancelAsync(NetworkAsyncHandle asyncHandle)
{
    TCPAsyncOp *op = (TCPAsyncOp *)asyncHandle;

    if (op && op->inUse) {
        /* Note: MacTCP doesn't provide a way to cancel async operations */
        /* We just mark it as free and let it complete in the background */
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCancelAsync: Marking handle as free (can't cancel MacTCP async)");

        /* Clean up any allocated resources */
        if (op->opType == TCP_ASYNC_SEND && op->rdsArray) {
            /* Don't free WDS until operation completes */
            /* This is a memory leak risk, but safer than crashing */
        }

        /* Mark as not in use but don't clear the pb - let it complete */
        op->inUse = false;
    }
}

/* Static operations table for MacTCP */
static NetworkOperations gMacTCPOperations = {
    /* System operations */
    MacTCPImpl_Initialize,
    MacTCPImpl_Shutdown,

    /* TCP operations */
    MacTCPImpl_TCPCreate,
    MacTCPImpl_TCPRelease,
    MacTCPImpl_TCPListen,
    MacTCPImpl_TCPConnect,
    MacTCPImpl_TCPSend,
    MacTCPImpl_TCPReceiveNoCopy,
    MacTCPImpl_TCPReturnBuffer,
    MacTCPImpl_TCPClose,
    MacTCPImpl_TCPAbort,
    MacTCPImpl_TCPStatus,

    /* Async TCP operations */
    MacTCPImpl_TCPListenAsync,
    MacTCPImpl_TCPConnectAsync,
    MacTCPImpl_TCPSendAsync,
    MacTCPImpl_TCPReceiveAsync,
    MacTCPImpl_TCPCheckAsyncStatus,
    MacTCPImpl_TCPCancelAsync,

    /* UDP operations */
    MacTCPImpl_UDPCreate,
    MacTCPImpl_UDPRelease,
    MacTCPImpl_UDPSend,
    MacTCPImpl_UDPReceive,
    MacTCPImpl_UDPReturnBuffer,

    /* Async UDP operations */
    MacTCPImpl_UDPSendAsync,
    MacTCPImpl_UDPCheckSendStatus,
    MacTCPImpl_UDPReceiveAsync,
    MacTCPImpl_UDPCheckAsyncStatus,
    MacTCPImpl_UDPReturnBufferAsync,
    MacTCPImpl_UDPCheckReturnStatus,
    MacTCPImpl_UDPCancelAsync,

    /* Utility operations */
    MacTCPImpl_ResolveAddress,
    MacTCPImpl_AddressToString,

    /* Implementation info */
    MacTCPImpl_GetImplementationName,
    MacTCPImpl_IsAvailable
};

/* Get MacTCP operations table */
NetworkOperations *GetMacTCPOperations(void)
{
    return &gMacTCPOperations;
}